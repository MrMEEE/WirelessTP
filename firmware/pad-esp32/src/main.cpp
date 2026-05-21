// pad-esp32/src/main.cpp
// ESP32-S2 Mini — Physical Toy Pad wireless bridge
//
// Connects to the physical Toy Pad as a USB host and bridges over WiFi:
//   Physical tag events  -> LP TAG_SET/TAG_CLEAR -> console-esp32
//   LP LED_CMD from console -> USB HID OUT c0    -> physical Toy Pad LEDs
//
// Connection flow:
//   Searching (unpaired) : blink all pads yellow
//   Pairing succeeded    : blink all pads green x3, then enter PAIRED state
//   Pairing timed out    : blink all pads red x3,   retry search
//   PAIRED + connected   : mirror physical figures and LED colours

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <lwip/sockets.h>  // for direct send() — bypasses WiFiClient::_connected stale-errno
#include <esp_system.h>        // for esp_reset_reason()
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_log.h>

#if PAD_USB_HOST
extern "C" {
#include "usb/usb_host.h"
}
#endif

#include "link_protocol.h"
#include "ld_catalog_data.h"

// ─── Constants ───────────────────────────────────────────────────────────────
static const char*    kSetupApSsid      = "ToyPad-Setup";
static const char*    kSetupApPass      = "toypadsetup";
static const uint16_t kSetupPort        = 80;
static const uint16_t kDnsPort          = 53;
static const int      kResetPin         = 0;
static const uint32_t kResetHoldMs     = 3000;
static const uint16_t kConsolePort     = 25100;

// Forward declaration — defined after startPadWeb().
static void serviceFactoryReset();
static const uint32_t kRetryMs         = 50;
static const uint8_t  kMaxRetries      = 3;
static const uint32_t kHelloMs         = 2000;   // HELLO interval while searching
static const uint32_t kPairTimeoutMs   = 15000;  // give up pairing after 15 s
static const uint32_t kHeartbeatMs     = 5000;   // HELLO interval while paired
static const uint32_t kLostMs          = 30000;  // assume console gone after 30 s
static const uint32_t kStateSyncMs     = 1000;   // full-state re-broadcast interval

static const uint16_t kToypadVid       = 0x0e6f;
static const uint16_t kToypadPid       = 0x0241;
static const uint8_t  kToypadIntfNum   = 0;
static const uint8_t  kEndpointIn      = 0x81;
static const uint8_t  kEndpointOut     = 0x01;
static const uint8_t  kReportSize      = 32;

// Physical LED feedback colours.
// Direct PWM values sent to the toypad's own LEDs.  Green/blue diodes are more
// efficient so lower values give perceptually balanced colours.
static const uint8_t kYR = 200, kYG = 60,  kYB = 0;    // yellow (searching)
static const uint8_t kGR = 0,   kGG = 160, kGB = 0;    // green  (paired OK)
static const uint8_t kRR = 200, kRG = 0,   kRB = 0;    // red    (pair fail)
static const uint8_t kOR = 0,   kOG = 0,   kOB = 0;    // off

static const uint8_t  kBlinkCount   = 3;
static const uint32_t kBlinkOnMs    = 200;
static const uint32_t kBlinkOffMs   = 200;
static const uint32_t kSearchOnMs   = 400;
static const uint32_t kSearchOffMs  = 400;
static const uint32_t kPhysSyncMs   = 8000; // physical zone TAG_SET/TAG_CLEAR re-sync period

#if PAD_USB_HOST
// Startup B0 command: "55 0f b0 01 (c) LEGO 2014 <checksum>"
// checksum = sum(buf[0..16]) & 0xff = 0xf7 (verified)
static const uint8_t kToypadB0Cmd[32] = {
  0x55, 0x0f, 0xb0, 0x01,
  0x28, 0x63, 0x29, 0x20, 0x4c, 0x45, 0x47, 0x4f,
  0x20, 0x32, 0x30, 0x31, 0x34, 0xf7
  // bytes [18..31] zero-initialised
};
#endif  // PAD_USB_HOST

// ─── Enums ────────────────────────────────────────────────────────────────────
enum ConnState : uint8_t {
  CONN_SEARCHING = 0,
  CONN_BLINK_OK,
  CONN_BLINK_FAIL,
  CONN_PAIRED,
};

// ─── Structs ──────────────────────────────────────────────────────────────────
struct PadConfig {
  String ssid;
  String pass;
  bool   valid;
};

#if PAD_USB_HOST
// Tag event decoded from a USB IN report (passed via FreeRTOS queue)
struct TagEvent {
  uint8_t lpZone;   // physical pad zone 0-2 (from d[2]-1): 0=center, 1=left, 2=right
  bool    placed;   // true=placed, false=removed
  uint8_t uid[7];   // 7-byte NFC UID — unique per toy, used as key for slot assignment
};
#endif  // PAD_USB_HOST

struct PendingTx {
  bool     active;
  uint8_t  wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen;
  uint8_t  seq;
  uint8_t  retries;
  uint32_t lastSentMs;
};

// ─── USB host globals ─────────────────────────────────────────────────────────
#if PAD_USB_HOST
static usb_host_client_handle_t sUsbClient  = NULL;
static usb_device_handle_t      sUsbDev     = NULL;
static usb_transfer_t*          sUsbInXfer  = NULL;
static usb_transfer_t*          sUsbOutXfer = NULL;
static volatile bool            sUsbDevOpen  = false;
static volatile bool            sUsbOutBusy  = false;
static volatile uint8_t         sUsbNewDevCnt = 0;  // lifetime count of NEW_DEV events
static volatile uint8_t         sUsbOpenOk    = 0;  // set to 1 when open succeeds
static QueueHandle_t            sTagQueue   = NULL;
static uint8_t                  sMsgNum     = 1;  // toypad OUT message counter
// Per-slot occupancy for physical pad.  Index = slot-1 (0-6).
// Slots are assigned from per-zone pools by forwardTagEvent, keyed by NFC UID.
static uint32_t sSlotToyId[7]   = {};
static uint8_t  sSlotUid[7][7]  = {};
static bool     sSlotOccupied[7]= {};
#endif

// ─── WiFi / LP globals ────────────────────────────────────────────────────────
static WiFiClient      client;
static lp_stream_parser_t tcpParser;
static WebServer web(kSetupPort);
static DNSServer dns;
static bool      apActive = false;  // true while provisioning AP is up
static Preferences prefs;
static PadConfig cfg;
static uint8_t   seqCounter   = 0;
static IPAddress consoleIp(192, 168, 4, 1);
static ConnState connState    = CONN_SEARCHING;
static bool      paired       = false;
static uint32_t  sharedSecret = 0;
static PendingTx pending      = {};

static uint32_t lastHelloMs     = 0;
static uint32_t helloStartMs    = 0;
static uint32_t blinkLastMs     = 0;
static uint8_t  blinkRemaining  = 0;
static bool     blinkOn         = false;
static uint32_t lastHeartbeatMs = 0;
static uint32_t lastPeerMs      = 0;
static int           sLastDiscErrno  = 0;   // errno from last TCP write failure (104=RST, 0=no-fd)
static uint8_t       sDiscCnt        = 0;   // write failures since last pad-dbg report
static uint8_t       sSearchCnt      = 0;   // CONN_SEARCHING entries since last pad-dbg
static uint8_t       sConnCnt        = 0;   // client.connect() successes since last pad-dbg
static uint8_t       sBootReason     = 0;   // esp_reset_reason() captured at startup
static uint8_t       sBlinkWhy       = 0;   // 1=kLostMs 2=kPairTimeout triggered last CONN_BLINK_FAIL
static uint32_t      sBlinkWhyPeerMs = 0;   // lastPeerMs snapshot when CONN_BLINK_FAIL triggered
static volatile uint32_t sWifiDrops     = 0;   // WiFi STA_DISCONNECTED event count since last report
static volatile uint8_t  sWifiDropRsn   = 0;   // reason code from last WiFi drop

// ─── Virtual toy state (serial debug menu) ────────────────────────────────────
// Slots 1-7, index = slot-1.  For USB-host builds slots 1-3 map to physical
// zones; for no-USB builds all seven slots are virtual-only.
static bool     sVirtualOccupied[7] = {};
static uint32_t sVirtualToyId[7]    = {};

// ─── Serial debug menu ────────────────────────────────────────────────────────
struct CatalogMatch { uint16_t id; char name[40]; };
enum MenuState : uint8_t { MENU_IDLE, MENU_SEARCH, MENU_SELECT };
static MenuState menuState      = MENU_IDLE;
static uint8_t   menuSlot       = 0;         // slot being placed
static char      menuLine[64]   = {};
static uint8_t   menuLineLen    = 0;
static CatalogMatch menuMatches[20];
static uint8_t   menuMatchCount = 0;
static bool      menuStarted    = false;

#if PAD_USB_HOST
// ─── Toypad OUT report helpers ────────────────────────────────────────────────
// Packet checksum: sum of all bytes from index 0 up to (but not including) the
// checksum byte itself, mod 256.  Placed at buf[dataLen].
static uint8_t toypadChecksum(const uint8_t* buf, uint8_t dataLen) {
  uint8_t sum = 0;
  for (uint8_t i = 0; i < dataLen; i++) sum += buf[i];
  return sum;
}

// Build a c0 (immediate colour) OUT report.
// padIdx: 0=all, 1=center, 2=left, 3=right
static void buildC0(uint8_t* out, uint8_t padIdx, uint8_t r, uint8_t g, uint8_t b) {
  memset(out, 0, kReportSize);
  out[0] = 0x55;
  out[1] = 0x06;       // length
  out[2] = 0xc0;       // command
  out[3] = sMsgNum++;
  out[4] = padIdx;
  out[5] = r;
  out[6] = g;
  out[7] = b;
  out[8] = toypadChecksum(out, 8);
}

// LP zone 0-2 → toypad pad index 1-3
static inline uint8_t lpZoneToPadIdx(uint8_t z) { return z + 1; }

// ─── Submit OUT transfer ───────────────────────────────────────────────────────
static bool sendToToypad(const uint8_t* cmd) {
  if (!sUsbDevOpen || sUsbOutBusy || !sUsbOutXfer) return false;
  sUsbOutBusy = true;
  memcpy(sUsbOutXfer->data_buffer, cmd, kReportSize);
  sUsbOutXfer->num_bytes = kReportSize;
  if (usb_host_transfer_submit(sUsbOutXfer) != ESP_OK) {
    sUsbOutBusy = false;
    return false;
  }
  return true;
}

static void toypadLedAll(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t buf[kReportSize];
  buildC0(buf, 0, r, g, b);
  sendToToypad(buf);
}

static void toypadLedZone(uint8_t lpZone, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t buf[kReportSize];
  buildC0(buf, lpZoneToPadIdx(lpZone), r, g, b);
  sendToToypad(buf);
}

// ─── Toypad B1/B3 authentication (TEA) ───────────────────────────────────────
// The physical toypad ignores colour commands until a valid B0→B1→B1→B3
// handshake has been completed.  Algorithm from RPCS3 Dimensions.cpp.
static const uint8_t kPadCommandKey[16] = {
  0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
  0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B,
};

static void padTeaEncrypt(const uint8_t* in, uint8_t* out) {
  static const uint32_t kD = 0x9E3779B9u;
  uint32_t v0 = (uint32_t)in[0] | ((uint32_t)in[1]<<8) | ((uint32_t)in[2]<<16) | ((uint32_t)in[3]<<24);
  uint32_t v1 = (uint32_t)in[4] | ((uint32_t)in[5]<<8) | ((uint32_t)in[6]<<16) | ((uint32_t)in[7]<<24);
  uint32_t k0 = (uint32_t)kPadCommandKey[0]  | ((uint32_t)kPadCommandKey[1] <<8) | ((uint32_t)kPadCommandKey[2] <<16) | ((uint32_t)kPadCommandKey[3] <<24);
  uint32_t k1 = (uint32_t)kPadCommandKey[4]  | ((uint32_t)kPadCommandKey[5] <<8) | ((uint32_t)kPadCommandKey[6] <<16) | ((uint32_t)kPadCommandKey[7] <<24);
  uint32_t k2 = (uint32_t)kPadCommandKey[8]  | ((uint32_t)kPadCommandKey[9] <<8) | ((uint32_t)kPadCommandKey[10]<<16) | ((uint32_t)kPadCommandKey[11]<<24);
  uint32_t k3 = (uint32_t)kPadCommandKey[12] | ((uint32_t)kPadCommandKey[13]<<8) | ((uint32_t)kPadCommandKey[14]<<16) | ((uint32_t)kPadCommandKey[15]<<24);
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += kD;
    v0 += (((v1<<4)+k0) ^ (v1+sum) ^ ((v1>>5)+k1));
    v1 += (((v0<<4)+k2) ^ (v0+sum) ^ ((v0>>5)+k3));
  }
  out[0]=(uint8_t)v0; out[1]=(uint8_t)(v0>>8); out[2]=(uint8_t)(v0>>16); out[3]=(uint8_t)(v0>>24);
  out[4]=(uint8_t)v1; out[5]=(uint8_t)(v1>>8); out[6]=(uint8_t)(v1>>16); out[7]=(uint8_t)(v1>>24);
}

// B1 (Seed): TEA([seed_LE | conf_BE]).  Initialises the pad's RNG.
static void sendToypadB1(uint32_t seed, uint32_t conf) {
  uint8_t plain[8];
  plain[0]=(uint8_t)seed;       plain[1]=(uint8_t)(seed>>8);
  plain[2]=(uint8_t)(seed>>16); plain[3]=(uint8_t)(seed>>24);
  plain[4]=(uint8_t)(conf>>24); plain[5]=(uint8_t)(conf>>16);
  plain[6]=(uint8_t)(conf>>8);  plain[7]=(uint8_t)conf;
  uint8_t enc[8]; padTeaEncrypt(plain, enc);
  uint8_t buf[kReportSize] = {};
  buf[0]=0x55; buf[1]=0x09; buf[2]=0xb1; buf[3]=sMsgNum++;
  memcpy(&buf[4], enc, 8);
  buf[12] = toypadChecksum(buf, 12);
  sendToToypad(buf);
}

// B3 (Challenge): TEA([conf_BE | 00000000]).  Pad replies with next RNG value.
static void sendToypadB3(uint32_t conf) {
  uint8_t plain[8] = {};
  plain[0]=(uint8_t)(conf>>24); plain[1]=(uint8_t)(conf>>16);
  plain[2]=(uint8_t)(conf>>8);  plain[3]=(uint8_t)conf;
  uint8_t enc[8]; padTeaEncrypt(plain, enc);
  uint8_t buf[kReportSize] = {};
  buf[0]=0x55; buf[1]=0x09; buf[2]=0xb3; buf[3]=sMsgNum++;
  memcpy(&buf[4], enc, 8);
  buf[12] = toypadChecksum(buf, 12);
  sendToToypad(buf);
}

// ─── Physical toy ID resolution (NFC page 36 read + figure-key decrypt) ───────────
// The LEGO Dimensions character ID lives encrypted in NFC page 36 of the figure’s
// NTAG213 tag.  We send a D2 (block-read) command to the physical pad to fetch
// it, then decrypt with the per-figure key derived from the real NFC UID.
// Algorithm ported from RPCS3 Dimensions.cpp (CHAR_CONSTANT / scramble /
// generate_figure_key).  Single-core ESP32-S2: volatile flag is sufficient.

// D2 response state — written by usbInCallback (USB task), read by main loop.
static volatile uint8_t sD2WaitCounter  = 0;    // counter we sent; 0 = not waiting
static volatile bool    sD2ReplyReady   = false;
static          uint8_t sD2PageData[16] = {};   // 4 pages of NFC data from D2 reply

// From RPCS3 Dimensions.cpp CHAR_CONSTANT.
static const uint8_t kCharConstant[17] = {
  0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8,
  0xD8, 0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA
};

static uint32_t padRotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static uint32_t padDimRandomize(const uint8_t* key, uint8_t count) {
  uint32_t s = 0;
  for (uint8_t i = 0; i < count; i++) {
    const uint32_t b = (uint32_t)key[i*4] | ((uint32_t)key[i*4+1]<<8) |
                       ((uint32_t)key[i*4+2]<<16) | ((uint32_t)key[i*4+3]<<24);
    s = b + padRotr32(s,25) + padRotr32(s,10) - s;
  }
  return s;
}

static uint32_t padFigScramble(const uint8_t uid[7], uint8_t count) {
  uint8_t buf[24];  // 7 uid + 17 kCharConstant
  memcpy(buf, uid, 7);
  memcpy(buf + 7, kCharConstant, 17);
  buf[count * 4 - 1] ^= count;
  return padDimRandomize(buf, count);
}

static void padGenerateFigureKey(const uint8_t uid[7], uint8_t key[16]) {
  const uint32_t s3 = padFigScramble(uid, 3);
  const uint32_t s4 = padFigScramble(uid, 4);
  const uint32_t s5 = padFigScramble(uid, 5);
  const uint32_t s6 = padFigScramble(uid, 6);
  key[0]=(s3>>24); key[1]=(s3>>16); key[2]=(s3>>8);  key[3]=s3;
  key[4]=(s4>>24); key[5]=(s4>>16); key[6]=(s4>>8);  key[7]=s4;
  key[8]=(s5>>24); key[9]=(s5>>16); key[10]=(s5>>8); key[11]=s5;
  key[12]=(s6>>24); key[13]=(s6>>16); key[14]=(s6>>8); key[15]=s6;
}

// TEA decrypt with an arbitrary 16-byte key — inverse of padTeaEncrypt/teaEncryptWithKey.
static void padTeaDecryptWithKey(const uint8_t* in, uint8_t* out, const uint8_t key[16]) {
  static const uint32_t kD = 0x9E3779B9u;
  uint32_t v0 = (uint32_t)in[0] | ((uint32_t)in[1]<<8) | ((uint32_t)in[2]<<16) | ((uint32_t)in[3]<<24);
  uint32_t v1 = (uint32_t)in[4] | ((uint32_t)in[5]<<8) | ((uint32_t)in[6]<<16) | ((uint32_t)in[7]<<24);
  const uint32_t k0=(uint32_t)key[0] |((uint32_t)key[1]<<8)|((uint32_t)key[2]<<16)|((uint32_t)key[3]<<24);
  const uint32_t k1=(uint32_t)key[4] |((uint32_t)key[5]<<8)|((uint32_t)key[6]<<16)|((uint32_t)key[7]<<24);
  const uint32_t k2=(uint32_t)key[8] |((uint32_t)key[9]<<8)|((uint32_t)key[10]<<16)|((uint32_t)key[11]<<24);
  const uint32_t k3=(uint32_t)key[12]|((uint32_t)key[13]<<8)|((uint32_t)key[14]<<16)|((uint32_t)key[15]<<24);
  uint32_t sum = kD * 32u;  // 0xC6EF3720
  for (int i = 0; i < 32; i++) {
    v1 -= (((v0<<4)+k2)^(v0+sum)^((v0>>5)+k3));
    v0 -= (((v1<<4)+k0)^(v1+sum)^((v1>>5)+k1));
    sum -= kD;
  }
  out[0]=(uint8_t)v0; out[1]=(uint8_t)(v0>>8); out[2]=(uint8_t)(v0>>16); out[3]=(uint8_t)(v0>>24);
  out[4]=(uint8_t)v1; out[5]=(uint8_t)(v1>>8); out[6]=(uint8_t)(v1>>16); out[7]=(uint8_t)(v1>>24);
}

// Send D2 (block read) to the physical pad: read 4 pages starting at `page`.
// Returns the counter byte used, which is stored in sD2WaitCounter by the caller.
static uint8_t sendToypadD2(uint8_t figIndex, uint8_t page) {
  const uint8_t ctr = sMsgNum++;
  uint8_t buf[kReportSize] = {};
  buf[0]=0x55; buf[1]=0x04; buf[2]=0xD2; buf[3]=ctr;
  buf[4]=figIndex; buf[5]=page;
  buf[6] = toypadChecksum(buf, 6);
  sendToToypad(buf);
  return ctr;
}

// Fetch NFC page 36 from the physical pad and decrypt with the per-figure key
// to get the real LEGO Dimensions character ID.  Blocks up to 80 ms.
// Returns 0 if the read fails or the decrypted ID is 0.
static uint32_t readPhysicalToyId(const uint8_t uid[7]) {
  if (!sUsbDevOpen) return 0;
  sD2ReplyReady  = false;
  sD2WaitCounter = sendToypadD2(0, 36);  // figIndex=0, start page 36
  const uint32_t deadline = millis() + 80;
  while (!sD2ReplyReady && (millis() < deadline)) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  if (!sD2ReplyReady) {
    sD2WaitCounter = 0;
    Serial.println("[pad] D2 page36 timeout");
    return 0;
  }
  // sD2PageData[0..7] = pages 36 and 37 (8 bytes, TEA-encrypted with figure key)
  uint8_t figKey[16];
  padGenerateFigureKey(uid, figKey);
  uint8_t dec[8];
  padTeaDecryptWithKey(sD2PageData, dec, figKey);
  const uint32_t toyId = (uint32_t)dec[0] | ((uint32_t)dec[1]<<8) |
                         ((uint32_t)dec[2]<<16) | ((uint32_t)dec[3]<<24);
  Serial.printf("[pad] D2 ok toyId=0x%04lx\n", (unsigned long)toyId);
  return toyId;
}

#else  // !PAD_USB_HOST — stub LED functions for no-USB builds
static void toypadLedAll(uint8_t r, uint8_t g, uint8_t b) {
  Serial.printf("[pad-stub] led all r=%u g=%u b=%u\n", r, g, b);
}
static void toypadLedZone(uint8_t lpZone, uint8_t r, uint8_t g, uint8_t b) {
  Serial.printf("[pad-stub] led z=%u r=%u g=%u b=%u\n", lpZone, r, g, b);
}
#endif  // PAD_USB_HOST

#if PAD_USB_HOST
// ─── USB transfer callbacks ───────────────────────────────────────────────────
static void usbOutCallback(usb_transfer_t* xfer) {
  (void)xfer;
  sUsbOutBusy = false;
}

static void usbInCallback(usb_transfer_t* xfer) {
  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED
      && xfer->actual_num_bytes >= 14) {
    const uint8_t* d = xfer->data_buffer;
    if (d[0] == 0x56 && d[1] == 0x0b) {
      // Physical toypad IN report (verified from session logs):
      //  [0]=0x56 magic  [1]=0x0b length
      //  [2]=pad zone (1=center, 2=left, 3=right)
      //  [3]=0x00  [4]=0x00  [5]=action (0=placed, 1=removed)
      //  [6..12]=NFC UID (7 bytes)  [13]=checksum
      const uint8_t padZone = d[2];
      const uint8_t action  = d[5];  // 0=placed, 1=removed
      if (padZone >= 1 && padZone <= 3) {
        TagEvent evt;
        evt.lpZone = padZone - 1;  // 0=center, 1=left, 2=right
        evt.placed = (action == 0);
        memcpy(evt.uid, &d[6], 7);
        xQueueSend(sTagQueue, &evt, 0);
      }
    } else if (d[0] == 0x55 && d[1] == 0x12 && d[3] == 0x00) {
      // D2 page-read response: [0x55, 0x12, counter, 0x00, <16 bytes NFC data>]
      const uint8_t ctr = d[2];
      if (sD2WaitCounter != 0 && ctr == sD2WaitCounter) {
        memcpy(sD2PageData, &d[4], 16);
        sD2WaitCounter = 0;   // clear before ready flag (ordering on single-core)
        sD2ReplyReady  = true;
      }
    }
    // All other 0x55 responses (B0, B1, B3 replies) are silently consumed.
  }
  // Keep polling as long as the device is open.
  if (sUsbDevOpen) {
    usb_host_transfer_submit(xfer);
  }
}

// ─── Device open / close ──────────────────────────────────────────────────────
static void openToypadDevice(uint8_t devAddr) {
  if (usb_host_device_open(sUsbClient, devAddr, &sUsbDev) != ESP_OK) {
    Serial.println("[pad-usb] device open failed");
    return;
  }

  const usb_device_desc_t* desc = NULL;
  if (usb_host_get_device_descriptor(sUsbDev, &desc) != ESP_OK) {
    usb_host_device_close(sUsbClient, sUsbDev);
    sUsbDev = NULL;
    return;
  }

  if (desc->idVendor != kToypadVid || desc->idProduct != kToypadPid) {
    Serial.printf("[pad-usb] ignoring %04x:%04x\n",
                  desc->idVendor, desc->idProduct);
    usb_host_device_close(sUsbClient, sUsbDev);
    sUsbDev = NULL;
    return;
  }
  Serial.printf("[pad-usb] toypad %04x:%04x\n", kToypadVid, kToypadPid);

  if (usb_host_interface_claim(sUsbClient, sUsbDev, kToypadIntfNum, 0) != ESP_OK) {
    Serial.println("[pad-usb] claim intf failed");
    usb_host_device_close(sUsbClient, sUsbDev);
    sUsbDev = NULL;
    return;
  }

  if (usb_host_transfer_alloc(kReportSize, 0, &sUsbInXfer)  != ESP_OK ||
      usb_host_transfer_alloc(kReportSize, 0, &sUsbOutXfer) != ESP_OK) {
    Serial.println("[pad-usb] alloc xfer failed");
    if (sUsbInXfer)  { usb_host_transfer_free(sUsbInXfer);  sUsbInXfer  = NULL; }
    if (sUsbOutXfer) { usb_host_transfer_free(sUsbOutXfer); sUsbOutXfer = NULL; }
    usb_host_interface_release(sUsbClient, sUsbDev, kToypadIntfNum);
    usb_host_device_close(sUsbClient, sUsbDev);
    sUsbDev = NULL;
    return;
  }

  sUsbInXfer->device_handle    = sUsbDev;
  sUsbInXfer->bEndpointAddress = kEndpointIn;
  sUsbInXfer->num_bytes        = kReportSize;
  sUsbInXfer->callback         = usbInCallback;
  sUsbInXfer->context          = NULL;
  sUsbInXfer->timeout_ms       = 0;  // no timeout — keep polling

  sUsbOutXfer->device_handle    = sUsbDev;
  sUsbOutXfer->bEndpointAddress = kEndpointOut;
  sUsbOutXfer->num_bytes        = kReportSize;
  sUsbOutXfer->callback         = usbOutCallback;
  sUsbOutXfer->context          = NULL;
  sUsbOutXfer->timeout_ms       = 1000;

  sUsbDevOpen = true;
  sUsbOpenOk   = 1;
  usb_host_transfer_submit(sUsbInXfer);  // begin polling

  // Initialise: B0 wake command, then B1×2 + B3 auth so the pad accepts LEDs.
  vTaskDelay(pdMS_TO_TICKS(50));
  sendToToypad(kToypadB0Cmd);
  vTaskDelay(pdMS_TO_TICKS(20));
  sendToypadB1(esp_random(), esp_random());  // seed 1
  vTaskDelay(pdMS_TO_TICKS(20));
  sendToypadB1(esp_random(), esp_random());  // seed 2
  vTaskDelay(pdMS_TO_TICKS(20));
  sendToypadB3(esp_random());                // challenge
  vTaskDelay(pdMS_TO_TICKS(20));
  Serial.println("[pad-usb] toypad ready");

  // If already paired, restore standby green (covers USB reconnect after pairing).
  if (connState == CONN_PAIRED) {
    vTaskDelay(pdMS_TO_TICKS(20));
    toypadLedAll(kGR, kGG, kGB);
  }
}

static void closeToypadDevice() {
  sUsbDevOpen = false;
  sUsbOutBusy = false;
  // Give in-flight transfers a moment to complete with error status before freeing.
  vTaskDelay(pdMS_TO_TICKS(20));
  if (sUsbInXfer)  { usb_host_transfer_free(sUsbInXfer);  sUsbInXfer  = NULL; }
  if (sUsbOutXfer) { usb_host_transfer_free(sUsbOutXfer); sUsbOutXfer = NULL; }
  if (sUsbDev) {
    usb_host_interface_release(sUsbClient, sUsbDev, kToypadIntfNum);
    usb_host_device_close(sUsbClient, sUsbDev);
    sUsbDev = NULL;
  }
  Serial.println("[pad-usb] toypad disconnected");
}

// ─── USB client event callback ────────────────────────────────────────────────
static void usbClientEventCb(const usb_host_client_event_msg_t* msg, void* /*arg*/) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    sUsbNewDevCnt++;
    openToypadDevice(msg->new_dev.address);
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    closeToypadDevice();
  }
}

// ─── FreeRTOS USB tasks ───────────────────────────────────────────────────────
// USB lib daemon task — must call usb_host_lib_handle_events() continuously.
static void usbLibTask(void* arg) {
  usb_host_config_t hostCfg = {};
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  ESP_ERROR_CHECK(usb_host_install(&hostCfg));
  xTaskNotifyGive(static_cast<TaskHandle_t>(arg));  // signal init done

  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

// USB client task — drives client event loop and transfer callbacks.
static void usbClientTask(void* /*arg*/) {
  usb_host_client_config_t clientCfg = {};
  clientCfg.is_synchronous         = false;
  clientCfg.max_num_event_msg      = 5;
  clientCfg.async.client_event_callback = usbClientEventCb;
  clientCfg.async.callback_arg     = NULL;
  ESP_ERROR_CHECK(usb_host_client_register(&clientCfg, &sUsbClient));

  while (true) {
    usb_host_client_handle_events(sUsbClient, portMAX_DELAY);
  }
}
#endif  // PAD_USB_HOST

// ─── LP send helpers ──────────────────────────────────────────────────────────
static bool tcpSendWire(const uint8_t* data, uint16_t len) {
  // Use send() directly on the socket fd instead of client.write().
  // WiFiClient::write() checks the stored _connected member which is set to
  // false by connected() via a len=0 recv() that reads stale errno — causing
  // spurious disconnects.  send() bypasses that flag and reflects true socket
  // state.  client.fd() is safe: it just checks clientSocketHandle != NULL.
  const int fd = client.fd();
  if (fd < 0) {
    // No socket handle at all — client was stopped or never connected.
    sLastDiscErrno = 0;
    sDiscCnt++;
    sSearchCnt++;
    Serial.println("[pad] tcpSendWire: no fd — disconnecting");
    paired         = false;
    lastPeerMs     = 0;
    pending.active = false;
    helloStartMs   = millis();
    lastHelloMs    = 0;
    connState      = CONN_SEARCHING;
    return false;
  }
  int n = ::send(fd, data, len, MSG_DONTWAIT);
  if (n == (int)len) return true;  // fast path — buffer had space
  if (n < 0 && errno == EAGAIN) {
    // Send buffer momentarily full — give it 200 ms then retry once.
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv = {0, 200000};  // 200 ms
    if (select(fd + 1, NULL, &wset, NULL, &tv) > 0 && FD_ISSET(fd, &wset)) {
      n = ::send(fd, data, len, MSG_DONTWAIT);
      if (n == (int)len) return true;
    }
  }
  sLastDiscErrno = (n < 0) ? errno : 0;
  sDiscCnt++;
  sSearchCnt++;
  Serial.printf("[pad] tcpSendWire: send failed n=%d/%u errno=%d — disconnecting\n",
                n, (unsigned)len, sLastDiscErrno);
  client.stop();
  paired         = false;
  lastPeerMs     = 0;
  pending.active = false;
  helloStartMs   = millis();
  lastHelloMs    = 0;
  connState      = CONN_SEARCHING;
  return false;
}

static bool sendFrame(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                      bool trackAck) {
  uint8_t  wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen = 0;
  const uint8_t seq = seqCounter++;

  if (!lp_encode_frame(type, seq, payload, payloadLen, wire, sizeof(wire), &wireLen)) {
    return false;
  }
  if (!tcpSendWire(wire, wireLen)) {
    return false;
  }
  if (trackAck) {
    pending.active      = true;
    pending.wireLen     = wireLen;
    pending.seq         = seq;
    pending.retries     = 0;
    pending.lastSentMs  = millis();
    memcpy(pending.wire, wire, wireLen);
  }
  return true;
}

static void sendAck(uint8_t seq) {
  sendFrame(LP_MSG_ACK, &seq, 1, false);
}

static void sendHello() {
  if (sharedSecret == 0) {
    const uint8_t p[1] = {0xa1};
    sendFrame(LP_MSG_HELLO, p, 1, true);
  } else {
    uint8_t p[5];
    p[0] = 0xa1;
    p[1] = (uint8_t)(sharedSecret);
    p[2] = (uint8_t)(sharedSecret >> 8);
    p[3] = (uint8_t)(sharedSecret >> 16);
    p[4] = (uint8_t)(sharedSecret >> 24);
    sendFrame(LP_MSG_HELLO, p, 5, true);
  }
}

// ─── Tag forwarding ───────────────────────────────────────────────────────────
#if PAD_USB_HOST
// LP slot pools per zone.  The physical pad sends zone + UID; we assign the
// first free slot from the zone's pool and track toys by UID.
// Matches slotToLpZone() in console-rp2040: slot2=center, {1,4,5}=left, {3,6,7}=right.
// padZone: 1=center (lpZone 0), 2=left (lpZone 1), 3=right (lpZone 2).
static const uint8_t kZoneSlots[3][3] = {
  {2, 0, 0},    // lpZone 0 (center): slot 2 only
  {1, 4, 5},    // lpZone 1 (left):   slots 1, 4, 5
  {3, 6, 7},    // lpZone 2 (right):  slots 3, 6, 7
};
static const uint8_t kZoneSlotCount[3] = {1, 3, 3};

static void forwardTagEvent(const TagEvent& evt) {
  if (!paired) return;
  const uint8_t z      = evt.lpZone;
  const uint8_t nSlots = kZoneSlotCount[z];

  if (evt.placed) {
    // Ignore if this UID is already tracked in this zone (duplicate event).
    for (uint8_t i = 0; i < nSlots; i++) {
      const uint8_t s = kZoneSlots[z][i];
      if (s && sSlotOccupied[s-1] && memcmp(sSlotUid[s-1], evt.uid, 7) == 0) return;
    }
    // Assign the first free slot in the zone's pool.
    uint8_t slot = 0;
    for (uint8_t i = 0; i < nSlots; i++) {
      const uint8_t s = kZoneSlots[z][i];
      if (s && !sSlotOccupied[s-1]) { slot = s; break; }
    }
    if (slot == 0) {
      Serial.printf("[pad] zone %u full, dropping uid=%02x%02x%02x%02x\n",
                    z, evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
      return;
    }
    // Read real LEGO character ID from NFC page 36 of the physical figure.
    uint32_t toyId = readPhysicalToyId(evt.uid);
    if (toyId == 0) {
      // Fallback: UID bytes — game won’t recognise the toy but at least shows it.
      toyId = (uint32_t)evt.uid[0] | ((uint32_t)evt.uid[1]<<8) |
              ((uint32_t)evt.uid[2]<<16) | ((uint32_t)evt.uid[3]<<24);
      Serial.printf("[pad] placed z=%u slot=%u uid=%02x%02x%02x%02x (UID fallback)\n",
                    z, slot, evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
    } else {
      Serial.printf("[pad] placed z=%u slot=%u uid=%02x%02x%02x%02x toyId=0x%04lx\n",
                    z, slot, evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3],
                    (unsigned long)toyId);
    }
    sSlotOccupied[slot-1] = true;
    memcpy(sSlotUid[slot-1], evt.uid, 7);
    sSlotToyId[slot-1] = toyId;
    uint8_t payload[5];
    payload[0]=slot; payload[1]=(uint8_t)toyId; payload[2]=(uint8_t)(toyId>>8);
    payload[3]=(uint8_t)(toyId>>16); payload[4]=(uint8_t)(toyId>>24);
    sendFrame(LP_MSG_TAG_SET, payload, 5, true);
  } else {
    // Find the slot holding this UID in this zone.
    uint8_t slot = 0;
    for (uint8_t i = 0; i < nSlots; i++) {
      const uint8_t s = kZoneSlots[z][i];
      if (s && sSlotOccupied[s-1] && memcmp(sSlotUid[s-1], evt.uid, 7) == 0) {
        slot = s; break;
      }
    }
    if (slot == 0) return;  // not tracked — ignore spurious removal
    sSlotOccupied[slot-1] = false;
    const uint8_t payload[1] = {slot};
    sendFrame(LP_MSG_TAG_CLEAR, payload, 1, true);
    Serial.printf("[pad] removed z=%u slot=%u uid=%02x%02x%02x%02x\n",
                  z, slot, evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
  }
}
#endif  // PAD_USB_HOST

// ─── Blink state machine ──────────────────────────────────────────────────────
// Non-blocking helpers; called every loop() iteration.

static void startBlink(uint8_t count) {
  blinkRemaining = count;
  blinkOn        = false;
  blinkLastMs    = 0;
}

// Drive a fixed-count blink pattern.  Returns true when the sequence finishes.
static bool updateBlink(uint8_t onR, uint8_t onG, uint8_t onB) {
  if (blinkRemaining == 0) return true;
  const uint32_t now = millis();
  const bool expire = (blinkLastMs == 0)
                      || (blinkOn  && (now - blinkLastMs) >= kBlinkOnMs)
                      || (!blinkOn && (now - blinkLastMs) >= kBlinkOffMs);
  if (expire) {
    blinkOn     = !blinkOn;
    blinkLastMs = now;
    if (blinkOn) {
      toypadLedAll(onR, onG, onB);
    } else {
      toypadLedAll(kOR, kOG, kOB);
      blinkRemaining--;
    }
  }
  return (blinkRemaining == 0);
}

// Drive the continuous searching blink (yellow pulse).
static void updateSearchBlink() {
  const uint32_t now = millis();
  const bool expire = (blinkLastMs == 0)
                      || (blinkOn  && (now - blinkLastMs) >= kSearchOnMs)
                      || (!blinkOn && (now - blinkLastMs) >= kSearchOffMs);
  if (expire) {
    blinkOn     = !blinkOn;
    blinkLastMs = now;
    toypadLedAll(blinkOn ? kYR : kOR,
                 blinkOn ? kYG : kOG,
                 blinkOn ? kYB : kOB);
  }
}

// ─── Provisioning portal ──────────────────────────────────────────────────────
static void clearConfig() {
  prefs.begin("toypad", false);
  prefs.clear();
  prefs.end();
}

static PadConfig loadConfig() {
  PadConfig out = {};
  prefs.begin("toypad", true);
  out.ssid     = prefs.getString("ssid", "");
  out.pass     = prefs.getString("pass", "");
  sharedSecret = prefs.getUInt("secret", 0);
  prefs.end();
  out.valid = (out.ssid.length() > 0);
  return out;
}

static void saveWifiConfig(const String& ssid, const String& pass) {
  prefs.begin("toypad", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putUInt("secret", 0);
  prefs.end();
  sharedSecret = 0;
}

static void saveSharedSecret(uint32_t secret) {
  prefs.begin("toypad", false);
  prefs.putUInt("secret", secret);
  prefs.end();
  sharedSecret = secret;
}

// ─── Web UI (status page only — debug via USB serial) ────────────────────────
static const char kPadPortalPage[] = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#101827">
  <title>Toy Pad Bridge</title>
  <style>
    :root { --bg:#111827; --panel:#1f2937; --muted:#93a0ba; --ink:#e5e7eb; --accent:#4f46e5; }
    * { box-sizing: border-box; }
    body { margin:0; background:linear-gradient(170deg,#101827,#1b2a40); color:var(--ink); font-family:"Trebuchet MS","Segoe UI",sans-serif; }
    .app { max-width:640px; margin:0 auto; padding:14px; display:grid; gap:10px; }
    .card { background:rgba(25,34,52,.88); border:1px solid rgba(255,255,255,.14); border-radius:12px; padding:16px; }
    .badge { background:#273552; border:1px solid rgba(255,255,255,.2); border-radius:999px; padding:5px 10px; font-size:.84rem; display:inline-block; margin:4px 4px 4px 0; }
    .status-row { display:flex; flex-wrap:wrap; gap:4px; }
    h2 { margin:0 0 4px; font-size:1.1rem; }
    .muted { color:var(--muted); font-size:.85rem; }
    .hint { background:rgba(79,70,229,.15); border:1px solid rgba(79,70,229,.4); border-radius:8px; padding:10px 14px; font-size:.88rem; color:#a5b4fc; }
    code { background:#1a2540; border-radius:4px; padding:1px 5px; font-size:.85rem; }
  </style>
</head>
<body>
  <div class="app">
    <div class="card">
      <h2>Toy Pad Bridge</h2>
      <div class="muted" id="subtitle">Loading...</div>
    </div>
    <div class="card">
      <h3 style="margin-top:0">Status</h3>
      <div class="status-row" id="statusRow"></div>
    </div>
    <div class="card hint">
      Debug actions are available over the USB serial port.<br>
      Connect via a terminal at 115200 baud and press any key to open the menu.<br>
      Commands: <code>s</code> state &nbsp; <code>p &lt;slot&gt;</code> place &nbsp; <code>r &lt;slot&gt;</code> remove &nbsp; <code>f &lt;text&gt;</code> search
    </div>
    <div class="card">
      <h3 style="margin-top:0">WiFi</h3>
      <div id="wifiStatus" class="muted">Loading...</div>
    </div>
  </div>
  <script>
    async function refresh() {
      try {
        const r = await fetch('/api/state', { cache:'no-store' });
        const d = await r.json();
        document.getElementById('subtitle').textContent = d.consoleIp ? ('Connected to ' + d.consoleIp) : 'Searching for console...';
        document.getElementById('statusRow').innerHTML = [
          '<span class="badge">State: ' + d.state + '</span>',
          '<span class="badge">Console: ' + (d.consoleIp || '-') + '</span>',
          '<span class="badge">Secret: ' + (d.hasSecret ? 'yes' : 'no') + '</span>',
          '<span class="badge">WiFi: ' + d.ssid + '</span>',
        ].join('');
        document.getElementById('wifiStatus').textContent = 'SSID: ' + d.ssid + ' | IP: ' + d.ip;
      } catch (e) { document.getElementById('subtitle').textContent = 'Error: ' + e.message; }
    }
    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>
)HTML";

static void handlePadRoot() {
  web.send(200, "text/html", kPadPortalPage);
}

static void handlePadState() {
  String json = "{";
  json += "\"state\":\"";
  switch (connState) {
    case CONN_SEARCHING:  json += "searching"; break;
    case CONN_BLINK_OK:   json += "pairing";   break;
    case CONN_BLINK_FAIL: json += "fail";      break;
    case CONN_PAIRED:     json += "paired";    break;
  }
  json += "\",";
  json += "\"consoleIp\":\"";
  json += (paired ? consoleIp.toString() : "");
  json += "\",";
  json += "\"hasSecret\":";
  json += (sharedSecret != 0 ? "true" : "false");
  json += ",\"ssid\":\"";
  json += cfg.ssid;
  json += "\",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\"";
  json += "}";
  web.send(200, "application/json", json);
}

static void redirectToRoot() {
  web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  web.send(302, "text/plain", "");
}

static void handleSave() {
  const String ssid = web.arg("ssid");
  if (ssid.length() == 0) { web.send(400, "text/plain", "invalid"); return; }
  saveWifiConfig(ssid, web.arg("pass"));
  web.send(200, "text/plain", "saved, rebooting");
  delay(500);
  ESP.restart();
}

static void handleSetupRoot() {
  const int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n; i++) {
    const String s = WiFi.SSID(i);
    String esc = s;
    esc.replace("&", "&amp;"); esc.replace("<", "&lt;"); esc.replace("\"", "&quot;");
    opts += "<option value=\"" + esc + "\">" + esc + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  web.send(200, "text/html",
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<meta name='theme-color' content='#101827'>"
    "<title>Toy Pad Setup</title>"
    "<style>"
    ":root{--bg:#111827;--panel:#1f2937;--muted:#93a0ba;--ink:#e5e7eb;}"
    "*{box-sizing:border-box;}"
    "body{margin:0;background:linear-gradient(170deg,#101827,#1b2a40);color:var(--ink);font-family:'Trebuchet MS','Segoe UI',sans-serif;}"
    ".app{max-width:480px;margin:0 auto;padding:14px;display:grid;gap:10px;}"
    ".card{background:rgba(25,34,52,.88);border:1px solid rgba(255,255,255,.14);border-radius:12px;padding:16px;}"
    "h2{margin:0 0 4px;font-size:1.1rem;}"
    ".muted{color:var(--muted);font-size:.85rem;margin-bottom:12px;}"
    "label{display:block;margin-bottom:4px;font-size:.9rem;}"
    "select,input{width:100%;background:#192438;color:var(--ink);border:1px solid rgba(255,255,255,.22);border-radius:8px;padding:8px;margin-bottom:10px;font-size:.95rem;}"
    "button{background:#4f46e5;color:#fff;border:0;border-radius:8px;padding:10px 18px;font-weight:700;width:100%;}"
    "button:hover{background:#4338ca;}"
    "</style></head><body>"
    "<div class='app'><div class='card'>"
    "<h2>Toy Pad Bridge Setup</h2>"
    "<p class='muted'>Select the console AP (hosted by console-esp32) and enter the password if set.</p>"
    "<form method='POST' action='/save'>"
    "<label>Network</label><select name='ssid'>" + opts + "</select>"
    "<label>Password</label><input name='pass' type='password' placeholder='Leave blank if open'>"
    "<button type='submit'>Save &amp; Reboot</button>"
    "</form></div></div></body></html>");
}

static void runProvisioningPortal() {
  Serial.println("[pad] setup portal");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kSetupApSsid, kSetupApPass);
  apActive = true;
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(kDnsPort, "*", WiFi.softAPIP());
  web.on("/",                   HTTP_GET,  handleSetupRoot);
  web.on("/save",               HTTP_POST, handleSave);
  web.on("/generate_204",       HTTP_GET,  redirectToRoot);
  web.on("/hotspot-detect.html",HTTP_GET,  redirectToRoot);
  web.on("/connecttest.txt",    HTTP_GET,  redirectToRoot);
  web.on("/ncsi.txt",           HTTP_GET,  redirectToRoot);
  web.onNotFound(redirectToRoot);
  web.begin();
  while (true) {
    dns.processNextRequest();
    web.handleClient();
    updateSearchBlink();
    serviceFactoryReset();
    delay(5);
  }
}

// Start WiFi in the background — does not block.  consoleIp is resolved in
// CONN_SEARCHING once the interface comes up.
static void connectToConsoleAp() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // disable modem sleep for stable TCP connections
  // Count WiFi-layer disconnect events so we can correlate them with TCP drops
  // in the pad-dbg stream (drops=N rsn=reason in pad-dbg output).
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    sWifiDrops++;
    sWifiDropRsn = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (cfg.pass.length() == 0) {
    WiFi.begin(cfg.ssid.c_str());
  } else {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  }
  Serial.printf("[pad] WiFi.begin → %s\n", cfg.ssid.c_str());
}

// ─── Toy catalog search ───────────────────────────────────────────────────────
// Case-insensitive substring check (avoids pulling in <strings.h> strncasecmp).
static bool containsCI(const char* hay, const char* needle) {
  const size_t nl = strlen(needle);
  if (nl == 0) return true;
  for (; *hay; hay++) {
    if (strncasecmp(hay, needle, nl) == 0) return true;
  }
  return false;
}

// Search kLdCatalogJson for entries whose name contains `query` (case-insens.).
// Returns number of matches (up to maxOut).
static uint8_t searchCatalog(const char* query, CatalogMatch* out, uint8_t maxOut) {
  uint8_t count = 0;
  const char* p = kLdCatalogJson;
  while (count < maxOut) {
    const char* idTag = strstr(p, "\"id\":");
    if (!idTag) break;
    p = idTag + 5;
    // Parse id
    uint16_t id = 0;
    while (*p >= '0' && *p <= '9') { id = id * 10 + (uint16_t)(*p - '0'); p++; }
    // Find name value
    const char* nameTag = strstr(p, "\"name\":\"");
    if (!nameTag) break;
    const char* ns = nameTag + 8;
    // Copy name until closing quote
    char nameBuf[48]; uint8_t nl = 0;
    while (ns[nl] && ns[nl] != '"' && nl < 47) { nameBuf[nl] = ns[nl]; nl++; }
    nameBuf[nl] = '\0';
    p = ns + nl;  // advance past name
    // Skip non-specific entries
    if (strcmp(nameBuf, "Unknown") == 0) continue;
    if (strncmp(nameBuf, "Future Update", 13) == 0) continue;
    if (strncmp(nameBuf, "World 1", 7) == 0) continue;
    if (strncmp(nameBuf, "Lord Vortech", 12) == 0) continue;
    if (strncmp(nameBuf, "* ", 2) == 0) continue;  // skip upgrade variants
    // Match
    if (containsCI(nameBuf, query)) {
      out[count].id = id;
      strncpy(out[count].name, nameBuf, 39);
      out[count].name[39] = '\0';
      count++;
    }
  }
  return count;
}

// ─── Virtual toy helpers ──────────────────────────────────────────────────────
static void virtualPlace(uint8_t slot, uint32_t toyId) {
  if (slot < 1 || slot > 7) return;
  sVirtualOccupied[slot - 1] = true;
  sVirtualToyId[slot - 1]    = toyId;
  if (!paired) {
    Serial.println("  [not paired] cached — will sync once paired");
    return;
  }
  uint8_t p[5] = {slot,
    (uint8_t)toyId, (uint8_t)(toyId >> 8),
    (uint8_t)(toyId >> 16), (uint8_t)(toyId >> 24)};
  sendFrame(LP_MSG_TAG_SET, p, 5, true);
  Serial.printf("  Placed ID 0x%04X on slot %u\n", toyId, slot);
}

static void virtualRemove(uint8_t slot) {
  if (slot < 1 || slot > 7) return;
  sVirtualOccupied[slot - 1] = false;
  sVirtualToyId[slot - 1]    = 0;
  if (!paired) {
    Serial.printf("  [not paired] slot %u cached as empty\n", slot);
    return;
  }
  const uint8_t p[1] = {slot};
  sendFrame(LP_MSG_TAG_CLEAR, p, 1, true);
  Serial.printf("  Removed slot %u\n", slot);
}

static void virtualRemoveAll() {
  for (uint8_t s = 1; s <= 7; s++) virtualRemove(s);
}

// ─── Serial debug menu ────────────────────────────────────────────────────────
static void menuPrintState() {
  Serial.println();
  Serial.print("  State  : ");
  switch (connState) {
    case CONN_SEARCHING:  Serial.println("searching"); break;
    case CONN_BLINK_OK:   Serial.println("pairing");   break;
    case CONN_BLINK_FAIL: Serial.println("fail");      break;
    case CONN_PAIRED:     Serial.println("paired");    break;
  }
  Serial.printf("  Console: %s\n", paired ? consoleIp.toString().c_str() : "-");
  Serial.printf("  WiFi   : %s / %s\n", cfg.ssid.c_str(), WiFi.localIP().toString().c_str());
  Serial.println("  Slots:");
  for (uint8_t s = 1; s <= 7; s++) {
    if (sVirtualOccupied[s - 1]) {
      Serial.printf("    [%u] virtual ID 0x%04X\n", s, sVirtualToyId[s - 1]);
    } else {
      Serial.printf("    [%u] empty\n", s);
    }
  }
#if PAD_USB_HOST
  bool anyPhys = false;
  for (uint8_t si = 0; si < 7; si++) { if (sSlotOccupied[si]) { anyPhys = true; break; } }
  if (anyPhys) {
    Serial.println("  Physical slots (USB):");
    for (uint8_t si = 0; si < 7; si++) {
      if (sSlotOccupied[si]) {
        Serial.printf("    slot%u  uid=%02X%02X%02X%02X\n",
                      si + 1,
                      sSlotUid[si][0], sSlotUid[si][1], sSlotUid[si][2], sSlotUid[si][3]);
      }
    }
  }
#endif
}

static void menuPrintHelp() {
  Serial.println("\r\n=== Toy Pad Debug Menu ===");
  Serial.println("  s              Show current state");
  Serial.println("  p <slot>       Place virtual toy on slot 1-7");
  Serial.println("                 (then type search text or a raw decimal ID)");
  Serial.println("  r <slot>       Remove virtual toy from slot 1-7");
  Serial.println("  r *            Remove all virtual toys");
  Serial.println("  f <text>       Search toy catalog (characters only)");
  Serial.println("  h              This help");
  Serial.print("> ");
}

static void menuPrompt() { Serial.print("\r\n> "); }

static void processMenuLine() {
  const char* line = menuLine;
  while (*line == ' ') line++;

  switch (menuState) {

    case MENU_IDLE: {
      if (*line == '\0' || *line == 's') {
        menuPrintState();
        menuPrompt();
        break;
      }
      if (*line == 'h' || *line == '?') {
        menuPrintHelp();
        break;
      }
      if (*line == 'p') {
        const char* sp = line + 1;
        while (*sp == ' ') sp++;
        const uint8_t slot = (uint8_t)atoi(sp);
        if (slot < 1 || slot > 7) {
          Serial.println("  Usage: p <slot>  (1-7)");
          menuPrompt();
          break;
        }
        menuSlot  = slot;
        menuState = MENU_SEARCH;
        Serial.printf("  Slot %u — search (name, partial) or enter raw ID: ", slot);
        break;
      }
      if (*line == 'r') {
        const char* sp = line + 1;
        while (*sp == ' ') sp++;
        if (*sp == '*' || *sp == '0') {
          virtualRemoveAll();
          Serial.println("  All slots cleared");
        } else {
          const uint8_t slot = (uint8_t)atoi(sp);
          if (slot < 1 || slot > 7) {
            Serial.println("  Usage: r <slot>  (1-7) or r *");
            menuPrompt();
            break;
          }
          virtualRemove(slot);
        }
        menuPrompt();
        break;
      }
      if (*line == 'f') {
        const char* q = line + 1;
        while (*q == ' ') q++;
        if (*q == '\0') { Serial.println("  Usage: f <text>"); menuPrompt(); break; }
        const uint8_t n = searchCatalog(q, menuMatches, 20);
        if (n == 0) { Serial.println("  No matches."); menuPrompt(); break; }
        Serial.printf("  %u match(es):\n", n);
        for (uint8_t i = 0; i < n; i++) {
          Serial.printf("  [%2u] %-38s  ID %u\n", i + 1, menuMatches[i].name, menuMatches[i].id);
        }
        menuPrompt();
        break;
      }
      Serial.println("  Unknown. Type h for help.");
      menuPrompt();
      break;
    }

    case MENU_SEARCH: {
      const char* q = line;
      while (*q == ' ') q++;
      if (*q == '\0') {
        // Cancel
        menuState = MENU_IDLE;
        Serial.println("  Cancelled.");
        menuPrompt();
        break;
      }
      // Pure digits → treat as raw ID
      bool isDigits = true;
      for (const char* c = q; *c; c++) if (*c < '0' || *c > '9') { isDigits = false; break; }
      if (isDigits) {
        virtualPlace(menuSlot, (uint32_t)strtoul(q, nullptr, 10));
        menuState = MENU_IDLE;
        menuPrompt();
        break;
      }
      // Search catalog
      menuMatchCount = searchCatalog(q, menuMatches, 20);
      if (menuMatchCount == 0) {
        Serial.printf("  No matches for '%s'. Search again or enter raw ID: ", q);
        break;  // stay in MENU_SEARCH
      }
      if (menuMatchCount == 1) {
        Serial.printf("  → %s  (ID %u)\n", menuMatches[0].name, menuMatches[0].id);
        virtualPlace(menuSlot, menuMatches[0].id);
        menuState = MENU_IDLE;
        menuPrompt();
        break;
      }
      Serial.printf("  %u matches:\n", menuMatchCount);
      for (uint8_t i = 0; i < menuMatchCount; i++) {
        Serial.printf("  [%2u] %-38s  ID %u\n", i + 1, menuMatches[i].name, menuMatches[i].id);
      }
      Serial.printf("  Select 1-%u, blank=cancel: ", menuMatchCount);
      menuState = MENU_SELECT;
      break;
    }

    case MENU_SELECT: {
      const char* q = line;
      while (*q == ' ') q++;
      if (*q == '\0') {
        menuState = MENU_IDLE;
        Serial.println("  Cancelled.");
        menuPrompt();
        break;
      }
      const int sel = atoi(q);
      if (sel < 1 || sel > (int)menuMatchCount) {
        Serial.printf("  Enter 1-%u or blank to cancel: ", menuMatchCount);
        break;
      }
      const CatalogMatch& m = menuMatches[sel - 1];
      Serial.printf("  → %s  (ID %u)\n", m.name, m.id);
      virtualPlace(menuSlot, m.id);
      menuState = MENU_IDLE;
      menuPrompt();
      break;
    }
  }
}

static void serviceSerialMenu() {
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (!menuStarted) {
      menuStarted  = true;
      menuLineLen  = 0;
      menuState    = MENU_IDLE;
      menuPrintHelp();
      continue;
    }
    if (c == '\n') {
      menuLine[menuLineLen] = '\0';
      Serial.println();
      processMenuLine();
      menuLineLen = 0;
      continue;
    }
    if ((c == '\b' || c == 127) && menuLineLen > 0) {
      menuLineLen--;
      Serial.print("\b \b");
      continue;
    }
    if (menuLineLen < (uint8_t)(sizeof(menuLine) - 1)) {
      menuLine[menuLineLen++] = c;
      Serial.print(c);
    }
  }
}

// ─── STA web server ───────────────────────────────────────────────────────────
static void startPadWeb() {
  web.on("/",          HTTP_GET, handlePadRoot);
  web.on("/api/state", HTTP_GET, handlePadState);
  web.begin();
}

// Non-blocking factory reset: call every loop iteration.
// Holding the BOOT button (GPIO0) for kResetHoldMs triggers NVS wipe + reboot.
static uint32_t sResetHoldStart = 0;
static void serviceFactoryReset() {
  if (digitalRead(kResetPin) == LOW) {
    if (sResetHoldStart == 0) sResetHoldStart = millis();
    if (millis() - sResetHoldStart >= kResetHoldMs) {
      Serial.println("[pad] factory reset (BOOT held)");
      clearConfig();
      delay(200);
      ESP.restart();
    }
  } else {
    sResetHoldStart = 0;
  }
}

// ─── TCP receive ──────────────────────────────────────────────────────────────
static void processTcpIn(uint32_t now) {
  // No client.connected() check — available() returns 0 when not connected so
  // the loop is a safe no-op, and connected() has stale-errno false positives.
  // Use the caller's `now` (captured before this call) so that lastPeerMs never
  // exceeds `now`.  If we used millis() here, lastPeerMs could be slightly
  // greater than `now`, causing (now - lastPeerMs) to wrap to ~UINT32_MAX and
  // trigger kLostMs on every frame received.
  while (client.available()) {
    const int c = client.read();
    if (c < 0) break;

    lp_frame_t frame;
    const lp_parse_result_t res = lp_stream_push(&tcpParser, (uint8_t)c, &frame);
    if (res == LP_PARSE_FRAME_BAD_CRC) continue;
    if (res != LP_PARSE_FRAME_OK) continue;

    lastPeerMs = now;

    // ACK
    if (frame.header.type == LP_MSG_ACK && frame.header.length == 1) {
      if (pending.active && frame.payload[0] == pending.seq) pending.active = false;
      continue;
    }

    // PAIR_SET — console accepted us
    if (frame.header.type == LP_MSG_PAIR_SET && frame.header.length == 4) {
      const uint32_t secret =
          (uint32_t)frame.payload[0]         |
          ((uint32_t)frame.payload[1] << 8)  |
          ((uint32_t)frame.payload[2] << 16) |
          ((uint32_t)frame.payload[3] << 24);
      saveSharedSecret(secret);
      sendAck(frame.header.seq);
      if (client.fd() < 0) {
        // sendAck failed — tcpSendWire already called client.stop() and set
        // connState=CONN_SEARCHING.  Don't overwrite with CONN_BLINK_OK.
        continue;
      }
      pending.active = false;
      paired     = true;
      // Discard any USB events that queued up before we had a console link.
      // They are stale initialisation events that would generate spurious
      // TAG_CLEARs for zones that were never occupied this session.
#if PAD_USB_HOST
      if (sTagQueue) {
        TagEvent dummy;
        while (xQueueReceive(sTagQueue, &dummy, 0) == pdTRUE) {}
        Serial.println("[pad] flushed pre-pair tag queue");
      }
#endif
      connState  = CONN_BLINK_OK;
      startBlink(kBlinkCount);
      Serial.println("[pad] paired");
      continue;
    }

    // LED_CMD — forward to physical toypad
    if (frame.header.type == LP_MSG_LED_CMD && frame.header.length >= 4) {
      sendAck(frame.header.seq);
      const uint8_t zone = frame.payload[0];
      const uint8_t r    = frame.payload[1];
      const uint8_t g    = frame.payload[2];
      const uint8_t b    = frame.payload[3];
      if (zone == 0xff) {
        toypadLedAll(r, g, b);
      } else {
        toypadLedZone(zone, r, g, b);
      }
      continue;
    }

    // Everything else: ack and ignore
    sendAck(frame.header.seq);
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  // On S2 Mini with USB CDC disabled, Serial routes to UART0 (GPIO 43/44).
  Serial.begin(115200);
  delay(300);
  sBootReason = (uint8_t)esp_reset_reason();
  Serial.printf("[pad] boot, reset reason: %u\n", sBootReason);

  pinMode(kResetPin, INPUT_PULLUP);  // BOOT button — monitored by serviceFactoryReset()

  cfg = loadConfig();
  if (!cfg.valid) runProvisioningPortal();  // blocks until saved+reboot
  connectToConsoleAp();  // start WiFi in background; does not block

  // Start the debug/status web UI (reachable once WiFi connects).
  startPadWeb();

  lp_stream_init(&tcpParser);

#if PAD_USB_HOST
  sTagQueue = xQueueCreate(8, sizeof(TagEvent));

  // USB lib task must start first; notify this task when ready.
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  // USB tasks: priority BELOW the WiFi task (23 on ESP32-S2 single-core) so
  // USB event processing never starves the TCP/IP stack.
  xTaskCreate(usbLibTask, "usb_lib", 4096, self, 5, NULL);
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));  // wait for USB host install

  xTaskCreate(usbClientTask, "usb_cli", 4096, NULL, 5, NULL);
#endif

  helloStartMs = millis();
  blinkLastMs  = 0;
  Serial.println("[pad] searching for console");
}

// ─── Main loop ────────────────────────────────────────────────────────────────
void loop() {
  const uint32_t now = millis();

  serviceFactoryReset();
  processTcpIn(now);

  // Retry pending LP frame
  if (pending.active && (now - pending.lastSentMs) >= kRetryMs) {
    if (pending.retries >= kMaxRetries) {
      pending.active = false;
    } else {
      pending.retries++;
      pending.lastSentMs = now;
      tcpSendWire(pending.wire, pending.wireLen);
    }
  }

  // Connection state machine
  switch (connState) {

    case CONN_SEARCHING:
      updateSearchBlink();
      // Resolve console IP from the gateway the first time WiFi comes up.
      if (WiFi.status() == WL_CONNECTED && consoleIp == IPAddress(192, 168, 4, 1)) {
        IPAddress gw = WiFi.gatewayIP();
        if (gw != IPAddress((uint32_t)0)) {
          consoleIp = gw;
          Serial.printf("[pad] console ip: %s\n", consoleIp.toString().c_str());
        }
      }
      // Maintain TCP connection to console (only while WiFi is up).
      // Use client.fd() < 0 instead of !client.connected(): connected() does a
      // len=0 recv() which always returns 0 but leaves errno stale from the
      // previous syscall.  If that stale errno is ECONNRESET (104), connected()
      // sets _connected=false, and the subsequent write() returns 0 immediately
      // → false disconnect.  client.fd() < 0 just checks clientSocketHandle and
      // has no errno side-effects.
      if (WiFi.status() == WL_CONNECTED && client.fd() < 0) {
        if (client.connect(consoleIp, kConsolePort)) {
          sConnCnt++;
          client.setNoDelay(true);  // send LP frames immediately, no Nagle buffering
          lp_stream_reset(&tcpParser);
          pending.active = false;
          Serial.println("[pad] tcp connected");
        }
      }
      if (client.fd() >= 0 && !pending.active && (now - lastHelloMs) >= kHelloMs) {
        lastHelloMs = now;
        sendHello();
      }
      // Give up after kPairTimeoutMs, blink red, then retry
      if ((now - helloStartMs) >= kPairTimeoutMs) {
        helloStartMs = now;
        blinkLastMs  = 0;
        sBlinkWhy = 2;
        sBlinkWhyPeerMs = lastPeerMs;
        client.stop();
        connState    = CONN_BLINK_FAIL;
        startBlink(kBlinkCount);
      }
      break;

    case CONN_BLINK_OK:
      if (updateBlink(kGR, kGG, kGB)) {
        connState = CONN_PAIRED;
        lastHeartbeatMs = now;
        lastPeerMs      = now;
        // Hold green on all zones as a standby indicator until the game sends
        // its first LED_CMD.  The game will override this on initialisation.
        toypadLedAll(kGR, kGG, kGB);
        Serial.println("[pad] paired and ready");
        // Shut down provisioning AP now that we have a console link.
        // (apActive is false when we were already paired on boot.)
        if (apActive) {
          apActive = false;
          dns.stop();
          WiFi.softAPdisconnect(true);
          Serial.println("[pad] AP shut down");
        }
      }
      break;

    case CONN_BLINK_FAIL:
      if (updateBlink(kRR, kRG, kRB)) {
        blinkLastMs  = 0;
        helloStartMs = now;
        sSearchCnt++;
        connState    = CONN_SEARCHING;
      }
      break;

    case CONN_PAIRED:
      // client.connected() is NOT used for disconnect detection here — on ESP32
      // lwIP it reads stale errno and causes false positives every ~1 s.
      // Disconnects are detected via write failure (tcpSendWire) and kLostMs timeout.
      // Periodic heartbeat
      if ((now - lastHeartbeatMs) >= kHeartbeatMs) {
        lastHeartbeatMs = now;
        if (!pending.active) sendHello();
      }
      // Detect console lost (no response to heartbeats)
      if (lastPeerMs > 0 && (now - lastPeerMs) >= kLostMs) {
        Serial.println("[pad] console lost");
        sBlinkWhy = 1;
        sBlinkWhyPeerMs = lastPeerMs;
        client.stop();
        paired     = false;
        lastPeerMs = 0;
        blinkLastMs = 0;
        helloStartMs = now;
        connState  = CONN_BLINK_FAIL;
        startBlink(kBlinkCount);
      }
#if PAD_USB_HOST
      // Drain physical tag event queue
      {
        TagEvent evt;
        while (xQueueReceive(sTagQueue, &evt, 0) == pdTRUE) {
          forwardTagEvent(evt);
        }
      }
      // Periodic re-sync: broadcast full physical pad state every kPhysSyncMs.
      // TAG_SET for occupied slots, TAG_CLEAR for empty — lets the RP2040 correct
      // its state within one sync period even if a removal event was missed.
      // Covers all 7 physical slots.
      {
        static uint32_t lastPadSyncMs = 0;
        if ((now - lastPadSyncMs) >= kPhysSyncMs) {
          lastPadSyncMs = now;
          for (uint8_t si = 0; si < 7; si++) {
            const uint8_t s = si + 1;
            if (sSlotOccupied[si]) {
              uint8_t p[5] = {s,
                (uint8_t)sSlotToyId[si],        (uint8_t)(sSlotToyId[si] >> 8),
                (uint8_t)(sSlotToyId[si] >> 16), (uint8_t)(sSlotToyId[si] >> 24)};
              sendFrame(LP_MSG_TAG_SET, p, 5, false);
            } else {
              const uint8_t p[1] = {s};
              sendFrame(LP_MSG_TAG_CLEAR, p, 1, false);
            }
          }
        }
      }
#endif
      // Periodic re-sync of virtual toy slots.
      // USB host: skip any slot that has a physical toy present.
      // No-USB:   covers all slots 1-7.
      {
        static uint32_t lastVirtSyncMs = 0;
        if ((now - lastVirtSyncMs) >= kStateSyncMs) {
          lastVirtSyncMs = now;
          for (uint8_t s = 1; s <= 7; s++) {
#if PAD_USB_HOST
            if (sSlotOccupied[s - 1]) continue;  // physical toy present
#endif
            if (sVirtualOccupied[s - 1]) {
              const uint32_t id = sVirtualToyId[s - 1];
              uint8_t p[5] = {s, (uint8_t)id, (uint8_t)(id >> 8),
                              (uint8_t)(id >> 16), (uint8_t)(id >> 24)};
              sendFrame(LP_MSG_TAG_SET, p, 5, false);
            }
            // Don't send TAG_CLEAR for empty virtual slots — same reason as above.
          }
        }
      }
      // Periodic WiFi diagnostics forwarded to console so we can correlate
      // signal quality with TCP disconnects without accessing the pad serial.
      // Format: rssi=<dBm> wf=<status> disc=<errno> drops=<N> rsn=<wifi_reason>
      //   drops: count of WiFi STA_DISCONNECTED events since last report
      //   rsn:   wifi_err_reason_t from last WiFi drop (200=BEACON_TIMEOUT, etc.)
      {
        static uint32_t lastPadDbgMs = 0;
        if ((now - lastPadDbgMs) >= 1000) {
          lastPadDbgMs = now;
          sWifiDrops = 0;   // reset (no longer reported — saves space)
          char dbg[96];
          // Keep under LP_MAX_PAYLOAD=64 bytes.  Drop wf/drops/rsn (always stable).
          // uev=new-dev events, uok=open-succeeded flag, usb=current open state.
          snprintf(dbg, sizeof(dbg),
                   "rssi=%d fail=%u ern=%d srch=%u conn=%u why=%u uev=%u uok=%u usb=%u",
                   (int)WiFi.RSSI(),
                   (unsigned)sDiscCnt, sLastDiscErrno,
                   (unsigned)sSearchCnt, (unsigned)sConnCnt,
                   (unsigned)sBlinkWhy,
#if PAD_USB_HOST
                   (unsigned)sUsbNewDevCnt, (unsigned)sUsbOpenOk,
                   sUsbDevOpen ? 1u : 0u
#else
                   0u, 0u, 0u
#endif
                   );
          sLastDiscErrno = 0;
          sDiscCnt   = 0;
          sSearchCnt = 0;
          sConnCnt   = 0;
          sBlinkWhy  = 0;
          sBlinkWhyPeerMs = 0;
          sendFrame(LP_MSG_DEBUG, (const uint8_t*)dbg, (uint8_t)strlen(dbg), false);
        }
      }
      break;
  }

  serviceSerialMenu();
  dns.processNextRequest();
  web.handleClient();
  delay(5);
}
