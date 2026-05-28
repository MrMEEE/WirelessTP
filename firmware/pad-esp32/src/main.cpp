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
#include <HTTPUpdate.h>
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
#include "fw_header.h"
#include "ld_catalog_data.h"

// Non-const so the linker places this string in the .data segment,
// making it scannable in the binary image for OTA target validation.
static char kFwIdent[] = FW_IDENT_STR;

// ─── Constants ───────────────────────────────────────────────────────────────
static const char*    kSetupApSsid      = "ToyPad-Setup";
static const char*    kSetupApPass      = "toypadsetup";
static const uint16_t kSetupPort        = 80;
static const uint16_t kDnsPort          = 53;
static const int      kResetPin         = 0;
static const uint32_t kResetHoldMs     = 3000;
// Batman (DC Comics) toy-ID — placing him in center during CONN_BLINK_FAIL reopens setup portal.
static const uint32_t kGestureResetToyId = 1;
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
static const uint32_t kMoveGraceMs  = 3000;  // defer TAG_CLEAR; if re-placed within this window, treat as zone-move (move itself is instant)
static const uint32_t kLedKeepaliveMs = 8000; // resend LED colour to physical pad every 8 s to suppress its idle-green timer
static const uint32_t kLedIdleMs    = 300000; // revert to standby green after 5 min of no LED cmds

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
  uint8_t lpZone;     // physical pad zone 0-2 (from d[2]-1): 0=center, 1=left, 2=right
  uint8_t padFigIdx;  // slot index assigned by the physical pad (d[4] of 0x56 event)
                      // 0=first registered toy, 1=second, etc. — NOT zone-based.
                      // This is what the pad expects back in D2 figIdx.
  bool    placed;     // true=placed, false=removed
  uint8_t uid[7];     // 7-byte NFC UID — unique per toy, used as key for slot assignment
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
// Slots 1-7 are assigned from a global pool (not per-zone); the same slot
// is reused when a toy moves between zones so the figure index stays stable.
static uint32_t sSlotToyId[7]   = {};
static uint8_t  sSlotUid[7][7]  = {};
static bool     sSlotOccupied[7]= {};
static uint8_t  sSlotZone[7]    = {};  // lpZone (0=center,1=left,2=right) of each slot
static bool     sSlotIsVehicle[7]    = {};  // true if the physical toy in this slot is a vehicle
// Move-grace: deferred TAG_CLEAR so zone-moves reuse the same slot.
static bool     sSlotPendingRemove[7] = {};  // REMOVE received but TAG_CLEAR not yet sent
static uint32_t sSlotRemoveMs[7]      = {};  // millis() when REMOVE was received
// Zone-canonical slot assignment: center=slot2; left=slots1,4,5; right=slots3,6,7.
static const uint8_t kZoneSlots[3][3]  = {{2,0,0},{1,4,5},{3,6,7}};
static const uint8_t kZoneSlotCnt[3]   = {1, 3, 3};
// Last D2 outcome — filled by readPhysicalToyId, sent as LP debug in forwardTagEvent.
static char     sLastD2Status[32] = "d2:init";
static bool     sLastD2IsVehicle  = false;  // set by readPhysicalToyId, read in forwardTagEvent
// Diagnostic: all-4-variant decryption results from last physical read.
static char     sLastD2Debug[160] = "";
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
static uint32_t          sLastLedCmdMs  = 0;   // millis() of last LP LED_CMD from game; 0 = never/idle
#if PAD_USB_HOST
// Pending LED zone colours, flushed to USB in flushPendingLed() after processTcpIn().
// Coalescing zone-0/1/2 bursts into a single toypadLedAll() eliminates USB timing skew.
static uint8_t           sLedR[3]       = {};
static uint8_t           sLedG[3]       = {};
static uint8_t           sLedB[3]       = {};
static uint8_t           sLedDirty      = 0;   // bitmask: bit z → zone z needs USB flush
// Last colour actually sent to all zones via toypadLedAll(); used to deduplicate
// the periodic state-sync frames (sent every ~1 s by both console-esp32 and
// console-rp2040) that would otherwise cancel in-flight blink animations.
static bool              sLedLastSentValid = false;
static uint8_t           sLedLastSentR     = 0;
static uint8_t           sLedLastSentG     = 0;
static uint8_t           sLedLastSentB     = 0;
#endif

// ─── Virtual toy state (serial debug menu) ────────────────────────────────────
// Slots 1-7, index = slot-1.  For USB-host builds slots 1-3 map to physical
// zones; for no-USB builds all seven slots are virtual-only.
static bool     sVirtualOccupied[7]  = {};
static uint32_t sVirtualToyId[7]     = {};
static bool     sVirtualIsVehicle[7] = {};  // type flag for virtual toys placed via serial menu

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
  // Wait for any in-flight OUT transfer so consecutive zone commands don't get dropped.
  for (int i = 0; i < 10 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(1));
  uint8_t buf[kReportSize];
  buildC0(buf, 0, r, g, b);
  sendToToypad(buf);
}

static void toypadLedZone(uint8_t lpZone, uint8_t r, uint8_t g, uint8_t b) {
  // Wait for any in-flight OUT transfer so consecutive zone commands don't get dropped.
  for (int i = 0; i < 10 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(1));
  uint8_t buf[kReportSize];
  buildC0(buf, lpZoneToPadIdx(lpZone), r, g, b);
  sendToToypad(buf);
}

// Flush buffered LED zone colours to the physical toypad.  Called from loop()
// after processTcpIn() so that all pending TCP frames are absorbed first; this
// lets us coalesce a burst of per-zone commands into a single toypadLedAll()
// call when all zones share the same colour (the common blink case).
static void flushPendingLed() {
  if (!sLedDirty) return;
  if (!sUsbDevOpen) { sLedDirty = 0; sLedLastSentValid = false; return; }

  // If all 3 zones share the same colour (dirty or not), use a single all-zones
  // command.  We check *all* zones regardless of sLedDirty so that partial dirty
  // batches (e.g. only 1–2 zones updated this cycle) still take this path and
  // avoid zone-3 sequential lag.
  if (sLedR[0]==sLedR[1] && sLedR[1]==sLedR[2] &&
      sLedG[0]==sLedG[1] && sLedG[1]==sLedG[2] &&
      sLedB[0]==sLedB[1] && sLedB[1]==sLedB[2]) {
    const uint8_t r=sLedR[0], g=sLedG[0], b=sLedB[0];
    sLedDirty = 0;
    // Skip if the physical pad already shows this colour.  Deduplicates the
    // ~1 s periodic state-sync frames from console-esp32 / console-rp2040 that
    // would otherwise re-assert the pre-blink colour mid-animation and make ON
    // or OFF blink states invisible.
    if (sLedLastSentValid && r==sLedLastSentR && g==sLedLastSentG && b==sLedLastSentB) return;
    sLedLastSentR=r; sLedLastSentG=g; sLedLastSentB=b; sLedLastSentValid=true;
    toypadLedAll(r, g, b);
    return;
  }

  // Mixed colours: send each dirty zone; toypadLedZone() waits for USB free.
  // Invalidate the dedup cache — zones are now at different colours.
  sLedLastSentValid = false;
  for (uint8_t z = 0; z < 3; z++) {
    if (sLedDirty & (1u << z)) {
      toypadLedZone(z, sLedR[z], sLedG[z], sLedB[z]);
    }
  }
  sLedDirty = 0;
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
  buf[0]=0x55; buf[1]=0x0a; buf[2]=0xb1; buf[3]=sMsgNum++;
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
  buf[0]=0x55; buf[1]=0x0a; buf[2]=0xb3; buf[3]=sMsgNum++;
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

// D3 write ACK tracking (for logging only).
static volatile uint8_t sD3LastCtr = 0;

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
  buf[count * 4 - 1] = 0xaa;  // Physical toypad scramble (Ellerbach): constant 0xaa
  return padDimRandomize(buf, count);
}

// RPCS3-style scramble: buf[count*4-1] ^= count  (vs Ellerbach's = 0xaa).
static uint32_t padFigScrambleRpcs3(const uint8_t uid[7], uint8_t count) {
  uint8_t buf[24];
  memcpy(buf, uid, 7);
  memcpy(buf + 7, kCharConstant, 17);
  buf[count * 4 - 1] ^= count;  // RPCS3 algorithm
  return padDimRandomize(buf, count);
}

static void padGenerateFigureKey(const uint8_t uid[7], uint8_t key[16]) {
  // Physical toy key derivation: 0xaa-constant scramble, little-endian key storage.
  // padTeaDecryptWithKey reads the key as LE u32 words, so storing LE gives the
  // correct k0-k3 values that match what was used to encrypt the physical tag.
  const uint32_t s3 = padFigScramble(uid, 3);
  const uint32_t s4 = padFigScramble(uid, 4);
  const uint32_t s5 = padFigScramble(uid, 5);
  const uint32_t s6 = padFigScramble(uid, 6);
  // Store as little-endian u32 words so the LE reads in padTeaDecryptWithKey are correct.
  key[0]=(uint8_t)s3;  key[1]=(uint8_t)(s3>>8);  key[2]=(uint8_t)(s3>>16); key[3]=(uint8_t)(s3>>24);
  key[4]=(uint8_t)s4;  key[5]=(uint8_t)(s4>>8);  key[6]=(uint8_t)(s4>>16); key[7]=(uint8_t)(s4>>24);
  key[8]=(uint8_t)s5;  key[9]=(uint8_t)(s5>>8);  key[10]=(uint8_t)(s5>>16); key[11]=(uint8_t)(s5>>24);
  key[12]=(uint8_t)s6; key[13]=(uint8_t)(s6>>8); key[14]=(uint8_t)(s6>>16); key[15]=(uint8_t)(s6>>24);
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
  out[0]=(uint8_t)v0;       out[1]=(uint8_t)(v0>>8);
  out[2]=(uint8_t)(v0>>16); out[3]=(uint8_t)(v0>>24);
  out[4]=(uint8_t)v1;       out[5]=(uint8_t)(v1>>8);
  out[6]=(uint8_t)(v1>>16); out[7]=(uint8_t)(v1>>24);
}

// Build a key using the specified scramble function and endianness, then TEA-decrypt
// pageData[0..7] → dec[0..7].  Returns id0 and id1 (both as LE uint32).
static void padTryVariant(const uint8_t uid[7], const uint8_t pageData[16],
                          bool useRpcs3Scramble, bool storeKeyBE,
                          uint32_t& id0, uint32_t& id1) {
  uint32_t s[4];
  for (uint8_t i = 0; i < 4; i++) {
    s[i] = useRpcs3Scramble ? padFigScrambleRpcs3(uid, (uint8_t)(3+i))
                            : padFigScramble(uid, (uint8_t)(3+i));
  }
  uint8_t key[16];
  for (uint8_t w = 0; w < 4; w++) {
    if (storeKeyBE) {
      key[w*4+0]=(uint8_t)(s[w]>>24); key[w*4+1]=(uint8_t)(s[w]>>16);
      key[w*4+2]=(uint8_t)(s[w]>>8);  key[w*4+3]=(uint8_t)s[w];
    } else {
      key[w*4+0]=(uint8_t)s[w];       key[w*4+1]=(uint8_t)(s[w]>>8);
      key[w*4+2]=(uint8_t)(s[w]>>16); key[w*4+3]=(uint8_t)(s[w]>>24);
    }
  }
  uint8_t dec[8];
  padTeaDecryptWithKey(pageData, dec, key);
  id0 = (uint32_t)dec[0]|((uint32_t)dec[1]<<8)|((uint32_t)dec[2]<<16)|((uint32_t)dec[3]<<24);
  id1 = (uint32_t)dec[4]|((uint32_t)dec[5]<<8)|((uint32_t)dec[6]<<16)|((uint32_t)dec[7]<<24);
}

// Send a D2 NFC-page-read command for reader slot 'figIdx' starting at NFC
// 'page', wait for the pad's response, and copy the 16 result bytes (4 pages)
// into outBuf.  Returns true on success, false on timeout or send failure.
// Safe to call from any task other than usbClientTask.
static bool d2ReadPages(uint8_t figIdx, uint8_t page, uint8_t outBuf[16]) {
  if (!sUsbDevOpen) return false;

  // Wait for any in-flight OUT transfer (up to 40 ms).
  for (int i = 0; i < 20 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(2));
  if (sUsbOutBusy) return false;

  sD2ReplyReady  = false;
  const uint8_t ctr = sMsgNum++;
  uint8_t buf[kReportSize] = {};
  buf[0]=0x55; buf[1]=0x04; buf[2]=0xD2; buf[3]=ctr;
  buf[4]=figIdx; buf[5]=page;
  buf[6] = toypadChecksum(buf, 6);
  Serial.printf("[pad] D2 figIdx=%u page=%u ctr=%u\n", figIdx, page, ctr);
  if (!sendToToypad(buf)) {
    Serial.println("[pad] D2 send failed");
    return false;
  }
  sD2WaitCounter = ctr;

  const uint32_t deadline = millis() + 100;
  while (sD2WaitCounter != 0 && !sD2ReplyReady && millis() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  sD2WaitCounter = 0;
  if (!sD2ReplyReady) {
    Serial.printf("[pad] D2 timeout figIdx=%u page=%u\n", figIdx, page);
    return false;
  }
  memcpy(outBuf, sD2PageData, 16);
  return true;
}

// Write 4 bytes to an NFC page on the physical toypad (fire-and-forget D3).
// figIdx: 0=center, 1=left, 2=right (lpZone, same as D2).
// page: NFC page number (e.g. 36 for vehicle ID).
// data: 4 bytes to write.
static void d3WritePages(uint8_t figIdx, uint8_t page, const uint8_t data[4]) {
  if (!sUsbDevOpen) return;

  // Wait for any in-flight OUT transfer (up to 40 ms).
  for (int i = 0; i < 20 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(2));
  if (sUsbOutBusy) {
    Serial.println("[pad] D3 write skipped: USB busy");
    return;
  }

  const uint8_t ctr = sMsgNum++;
  uint8_t buf[kReportSize] = {};
  buf[0] = 0x55; buf[1] = 0x08; buf[2] = 0xD3; buf[3] = ctr;
  buf[4] = figIdx; buf[5] = page;
  buf[6] = data[0]; buf[7] = data[1]; buf[8] = data[2]; buf[9] = data[3];
  buf[10] = toypadChecksum(buf, 10);
  Serial.printf("[pad] D3 write figIdx=%u page=%u data=%02x%02x%02x%02x ctr=%02x\n",
                figIdx, page, data[0], data[1], data[2], data[3], ctr);
  sD3LastCtr = ctr;   // track so usbInCallback can log the ACK
  if (!sendToToypad(buf)) {
    Serial.println("[pad] D3 send failed");
    sD3LastCtr = 0;
  }
}

// Fetch the LEGO character/vehicle ID from the physical pad.
//
// hintFigIdx: which physical reader slot (lpZone) the toy was detected on.
// Tried first with up to 3 attempts to handle the "fast zone move" case where
// the pad fires the "placed" event before its NFC page-read session is ready
// on the new reader.  TEA id0==id1 validates the correct slot; a mismatch stops
// retrying immediately (toy isn't on that reader).  Other slots are tried once
// as a fallback.
static uint32_t readPhysicalToyId(const uint8_t uid[7], uint8_t hintFigIdx) {
  if (!sUsbDevOpen) return 0;

  sLastD2IsVehicle = false;  // reset before each read

  uint8_t figKey[16];
  padGenerateFigureKey(uid, figKey);

  // order[]: hintFigIdx first, then the other two.
  uint8_t order[3];
  uint8_t cnt = 0;
  order[cnt++] = hintFigIdx;
  for (uint8_t i = 0; i < 3; i++) {
    if (i != hintFigIdx) order[cnt++] = i;
  }

  for (uint8_t oi = 0; oi < cnt; oi++) {
    const uint8_t figIdx = order[oi];
    // Retry D2 timeouts on the hint slot only; fallback slots get one shot.
    const uint8_t maxAttempts = (oi == 0) ? 3 : 1;

    for (uint8_t attempt = 0; attempt < maxAttempts; attempt++) {
      if (attempt > 0) vTaskDelay(pdMS_TO_TICKS(30));

      uint8_t pageData[16];
      if (!d2ReadPages(figIdx, 36, pageData)) continue;  // timeout → retry

      // Vehicle-first check: physical vehicle NFC tags store their ID
      // UNENCRYPTED at page 36 bytes 0-1 (LE uint16).  The vehicle marker is
      // at bytes 8-11 (= page 38): [0x00, 0x01, 0x00, 0x00].  Check this
      // BEFORE attempting TEA decryption (decryption gives garbage for vehicles).
      if (pageData[8]==0x00 && pageData[9]==0x01 &&
          pageData[10]==0x00 && pageData[11]==0x00) {
        const uint32_t vehicleId = (uint32_t)pageData[0] | ((uint32_t)pageData[1] << 8);
        if (vehicleId != 0) {
          if (oi > 0) {
            // Fallback slot: the hint figIdx (pad-assigned slot index) failed.
            // Don't return a vehicle ID from a different slot — it belongs to
            // a different physical toy (Bug-2 scenario).
            Serial.printf("[pad] vehicle fallback figIdx=%u skipped (wrong slot)\n", figIdx);
            break;
          }
          sLastD2IsVehicle = true;
          snprintf(sLastD2Status, sizeof(sLastD2Status),
                   "d2:veh:%04lx fi=%u", (unsigned long)vehicleId, figIdx);
          Serial.printf("[pad] vehicle id=%04lx figIdx=%u\n",
                        (unsigned long)vehicleId, figIdx);
          return vehicleId;
        }
        // vehicleId == 0: unusual, fall through to character decryption
      }

      uint8_t dec[8];
      padTeaDecryptWithKey(pageData, dec, figKey);
      const uint32_t id0 = (uint32_t)dec[0] | ((uint32_t)dec[1]<<8) |
                           ((uint32_t)dec[2]<<16) | ((uint32_t)dec[3]<<24);
      const uint32_t id1 = (uint32_t)dec[4] | ((uint32_t)dec[5]<<8) |
                           ((uint32_t)dec[6]<<16) | ((uint32_t)dec[7]<<24);
      Serial.printf("[pad] D2 figIdx=%u id0=%04lx id1=%04lx%s\n",
                    figIdx, (unsigned long)id0, (unsigned long)id1,
                    id0==id1 ? " OK" : " mm");

      // resolvedId: figure ID once any valid algorithm variant confirms id0==id1.
      uint32_t resolvedId = (id0 == id1) ? id0 : 0;

      // Diagnostic: try all 4 algorithm variants on first hint-slot attempt.
      // Also adopts the first alternative variant that validates when the primary
      // (aa-le) fails — handles vehicles that use a different scramble/endianness.
      // Format: "aa-le:ID= xr-le:ID= aa-be:ID! xr-be:ID="
      if (oi == 0 && attempt == 0) {
        struct { bool rpcs3; bool be; const char* tag; } variants[4] = {
          {false, false, "aa-le"},
          {true,  false, "xr-le"},
          {false, true,  "aa-be"},
          {true,  true,  "xr-be"},
        };
        char dbg[160]; int dpos = 0;
        for (int v = 0; v < 4; v++) {
          uint32_t vi0, vi1;
          padTryVariant(uid, pageData, variants[v].rpcs3, variants[v].be, vi0, vi1);
          dpos += snprintf(dbg+dpos, (int)sizeof(dbg)-dpos,
                           "%s:%lx%c ",
                           variants[v].tag, (unsigned long)vi0,
                           vi0==vi1 ? '=' : '!');
          // If primary (aa-le) failed but this variant validates, adopt it.
          if (resolvedId == 0 && vi0 == vi1) resolvedId = vi0;
          if (dpos >= (int)sizeof(dbg)-1) break;
        }
        strlcpy(sLastD2Debug, dbg, sizeof(sLastD2Debug));
        Serial.printf("[pad] D2 dbg: %s\n", sLastD2Debug);
      }

      if (resolvedId == 0) {
        // No algorithm validated — either wrong slot or wrong key for this toy.
        if (oi > 0) break;  // fallback slot: wrong slot, stop retrying
        continue;           // hint slot: might be transient, retry up to maxAttempts
      }

      // Vehicle marker check: bytes 8-11 of the page-36 D2 read (= raw page 38)
      // contain [0x00, 0x01, 0x00, 0x00] for vehicles, zeros for characters.
      // NOTE: this path is reached only for characters (resolvedId != 0 means
      // TEA decryption validated).  Physical vehicle tags store their ID
      // UNENCRYPTED, so they are caught by the vehicle-first check below.
      const bool isVehicle = (pageData[8]==0x00 && pageData[9]==0x01 &&
                              pageData[10]==0x00 && pageData[11]==0x00);
      if (isVehicle)
        snprintf(sLastD2Status, sizeof(sLastD2Status),
                 "d2:veh:%04lx fi=%u", (unsigned long)resolvedId, figIdx);
      else
        snprintf(sLastD2Status, sizeof(sLastD2Status),
                 "d2:ok:%04lx fi=%u", (unsigned long)resolvedId, figIdx);
      sLastD2IsVehicle = isVehicle;
      return resolvedId;
    }
  }

  snprintf(sLastD2Status, sizeof(sLastD2Status), "d2:mm");
  Serial.println("[pad] D2 all figIdx failed");
  return 0;
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
        evt.lpZone    = padZone - 1;  // 0=center, 1=left, 2=right
        evt.padFigIdx = d[4];           // pad-assigned slot index for D2 reads
        evt.placed    = (action == 0);
        memcpy(evt.uid, &d[6], 7);
        xQueueSend(sTagQueue, &evt, 0);
      }
    } else if (d[0] == 0x55) {
      if (d[1] == 0x12 && d[3] == 0x00) {
        // D2 success: [0x55, 0x12, counter, 0x00, <16 bytes NFC data>]
        const uint8_t ctr = d[2];
        if (sD2WaitCounter != 0 && ctr == sD2WaitCounter) {
          memcpy(sD2PageData, &d[4], 16);
          sD2WaitCounter = 0;   // clear before ready flag (single-core ordering)
          sD2ReplyReady  = true;
        }
      } else if (sD3LastCtr != 0 && d[2] == sD3LastCtr) {
        // D3 write ACK from physical toypad.
        // len=d[1], status=d[3] (0=success, non-zero=error).
        Serial.printf("[usb-in] D3 ack ctr=%02x len=%02x st=%02x\n", d[2], d[1], d[3]);
        sD3LastCtr = 0;
      } else if (sD2WaitCounter != 0 && d[2] == sD2WaitCounter) {
        // D2 error response (non-zero status or unexpected length)
        Serial.printf("[usb-in] D2 err d1=%02x st=%02x\n", d[1], d[3]);
        sD2WaitCounter = 0;  // break polling loop in readPhysicalToyId
      }
    }
  }
  // Keep polling as long as the device is open.
  if (sUsbDevOpen) {
    usb_host_transfer_submit(xfer);
  }
}

// ─── Device open / close ──────────────────────────────────────────────────────

// Auth sequence for the physical toypad.
// MUST run in its own task, NOT inside usbClientEventCb.
// Reason: openToypadDevice is called from usbClientEventCb, which itself runs
// inside usb_host_client_handle_events(). While blocked there, usbOutCallback
// can never fire (it also needs usb_host_client_handle_events to run), so
// sUsbOutBusy is never cleared, and every sendToToypad() call after the first
// is silently dropped. Running auth in a separate task lets vTaskDelay() yield
// to usbClientTask, which can then process completions between commands.
static void toypadAuthTask(void* /*arg*/) {
  Serial.println("[pad-auth] starting auth");

  // Let the USB stack settle after device open.
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!sUsbDevOpen) { vTaskDelete(NULL); return; }

  // B0 — wake the pad
  while (sUsbOutBusy) vTaskDelay(pdMS_TO_TICKS(2));
  sendToToypad(kToypadB0Cmd);
  for (int i = 0; i < 100 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(2));
  vTaskDelay(pdMS_TO_TICKS(50));
  if (!sUsbDevOpen) { vTaskDelete(NULL); return; }

  // B1 — seed. Generate conf here; B3 MUST use the same conf.
  const uint32_t seed = esp_random();
  const uint32_t conf = esp_random();
  sendToypadB1(seed, conf);
  for (int i = 0; i < 100 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(2));
  vTaskDelay(pdMS_TO_TICKS(50));
  if (!sUsbDevOpen) { vTaskDelete(NULL); return; }

  // B3 — challenge; conf must match the one sent in B1
  sendToypadB3(conf);
  for (int i = 0; i < 100 && sUsbOutBusy; i++) vTaskDelay(pdMS_TO_TICKS(2));
  vTaskDelay(pdMS_TO_TICKS(100));  // wait for pad's B3 response to arrive
  if (!sUsbDevOpen) { vTaskDelete(NULL); return; }

  Serial.println("[pad-usb] toypad auth complete");

  // Restore standby green if we were already paired (USB reconnect case).
  if (connState == CONN_PAIRED) {
    while (sUsbOutBusy) vTaskDelay(pdMS_TO_TICKS(2));
    toypadLedAll(kGR, kGG, kGB);
  }

  vTaskDelete(NULL);
}

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
  sUsbOutBusy  = false;  // safety reset before auth
  usb_host_transfer_submit(sUsbInXfer);  // begin IN polling

  // Auth runs in a dedicated task — see toypadAuthTask comment above.
  xTaskCreate(toypadAuthTask, "pad-auth", 3072, NULL, 5, NULL);
}

static void closeToypadDevice() {
  sUsbDevOpen    = false;
  sUsbOutBusy    = false;
  sD2WaitCounter = 0;      // abort any pending D2 read
  sD2ReplyReady  = false;
  // Give in-flight transfers a moment to complete with error status before freeing.
  vTaskDelay(pdMS_TO_TICKS(50));  // increased: 20ms was too short for in-flight xfers
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
  uint8_t p[LP_MAX_PAYLOAD];
  p[0] = 0xa1;
  // Always include 4 secret bytes; zero secret signals an enrollment request.
  if (sharedSecret != 0) {
    p[1] = (uint8_t)(sharedSecret);
    p[2] = (uint8_t)(sharedSecret >> 8);
    p[3] = (uint8_t)(sharedSecret >> 16);
    p[4] = (uint8_t)(sharedSecret >> 24);
  } else {
    p[1] = p[2] = p[3] = p[4] = 0;
  }
  const size_t rawVerLen = strlen(FIRMWARE_VERSION);
  const uint8_t verLen = (uint8_t)(rawVerLen < LP_MAX_PAYLOAD - 5 ? rawVerLen : LP_MAX_PAYLOAD - 5);
  memcpy(&p[5], FIRMWARE_VERSION, verLen);
  sendFrame(LP_MSG_HELLO, p, 5 + verLen, true);
}

// ─── Tag forwarding ───────────────────────────────────────────────────────────
#if PAD_USB_HOST
static void forwardTagEvent(const TagEvent& evt) {
  if (!paired) return;
  const uint8_t z = evt.lpZone;  // 0=center, 1=left, 2=right

  if (evt.placed) {
    // 1. Duplicate / put-back: UID already tracked in THIS zone.
    //    Also cancels any pending removal if the toy was briefly lifted and returned.
    for (uint8_t si = 0; si < 7; si++) {
      if (sSlotOccupied[si] && sSlotZone[si] == z &&
          memcmp(sSlotUid[si], evt.uid, 7) == 0) {
        sSlotPendingRemove[si] = false;  // toy is back — cancel deferred TAG_CLEAR
        return;
      }
    }
    // 2. Zone-move: UID tracked in a DIFFERENT zone.
    //    Handles both PLACE-before-REMOVE (physical pad quirk) and the normal
    //    REMOVE-then-PLACE case (REMOVE was deferred by kMoveGraceMs grace period).
    //    Free the old slot and assign a canonical slot in the new zone so the
    //    old zone can accept a full complement of toys again.  The RP2040 builds
    //    UIDs from toyId (slot-independent), so RPCS3 sees the same UID in the
    //    TAG_CLEAR + TAG_SET pair and recognises it as a zone-move.
    for (uint8_t si = 0; si < 7; si++) {
      if (sSlotOccupied[si] && sSlotZone[si] != z &&
          memcmp(sSlotUid[si], evt.uid, 7) == 0) {
        const uint8_t oldSlot   = si + 1;
        const uint8_t oldZ      = sSlotZone[si];
        const uint32_t toyId    = sSlotToyId[si];
        const bool isVehicle    = sSlotIsVehicle[si];
        uint8_t savedUid[7];
        memcpy(savedUid, sSlotUid[si], 7);
        sSlotPendingRemove[si] = false;
        sSlotOccupied[si]      = false;  // free old slot so new zone can use it

        // Pick a canonical slot in the new zone; fall back to any free slot.
        uint8_t newSlot = 0;
        for (uint8_t pi = 0; pi < kZoneSlotCnt[z] && newSlot == 0; pi++) {
          const uint8_t s = kZoneSlots[z][pi];
          if (s && !sSlotOccupied[s - 1] && !sSlotPendingRemove[s - 1]) newSlot = s;
        }
        for (uint8_t fsi = 0; fsi < 7 && newSlot == 0; fsi++) {
          if (!sSlotOccupied[fsi] && !sSlotPendingRemove[fsi]) newSlot = fsi + 1;
        }
        if (newSlot == 0) {
          // All slots full — re-occupy old slot and drop the move.
          sSlotOccupied[si] = true;
          Serial.printf("[pad] move z%u->z%u FULL, keeping slot=%u\n", oldZ, z, oldSlot);
          return;
        }

        // Send TAG_CLEAR for old slot, then TAG_SET for new slot.
        const uint8_t clrPayload[1] = {oldSlot};
        sendFrame(LP_MSG_TAG_CLEAR, clrPayload, 1, true);

        sSlotOccupied[newSlot - 1]    = true;
        memcpy(sSlotUid[newSlot - 1], savedUid, 7);
        sSlotToyId[newSlot - 1]       = toyId;
        sSlotZone[newSlot - 1]        = z;
        sSlotIsVehicle[newSlot - 1]   = isVehicle;
        uint8_t payload[7];
        payload[0] = newSlot; payload[1] = z;
        payload[2] = (uint8_t)toyId;         payload[3] = (uint8_t)(toyId >> 8);
        payload[4] = (uint8_t)(toyId >> 16); payload[5] = (uint8_t)(toyId >> 24);
        payload[6] = isVehicle ? 2 : 1;
        sendFrame(LP_MSG_TAG_SET, payload, 7, true);
        sLastLedCmdMs = millis();
        Serial.printf("[pad] move z%u->z%u oldslot=%u newslot=%u uid=%02x%02x%02x%02x\n",
                      oldZ, z, oldSlot, newSlot,
                      evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
        return;
      }
    }
    // 3. New toy: pick a slot from the zone-appropriate group first so the
    //    webui shows the toy in the correct column. Only consider slots that are
    //    truly free (not occupied, not pending-remove — pending-remove slots may
    //    still be reclaimed by the move-grace logic).
    //    webui shows the toy in the correct column (matches slotZone() in JS).
    //    Layout: center=slot2; left=slots1,4,5; right=slots3,6,7.
    //    Fall back to any free slot if the zone group is fully occupied.
    uint8_t slot = 0;
    // Try zone-preferred slots first.
    for (uint8_t pi = 0; pi < kZoneSlotCnt[z] && slot == 0; pi++) {
      const uint8_t s = kZoneSlots[z][pi];
      if (s && !sSlotOccupied[s - 1] && !sSlotPendingRemove[s - 1]) slot = s;
    }
    // Fall back to any truly free slot.
    for (uint8_t si = 0; si < 7 && slot == 0; si++) {
      if (!sSlotOccupied[si] && !sSlotPendingRemove[si]) slot = si + 1;
    }
    if (slot == 0) {
      Serial.printf("[pad] all slots full, dropping uid=%02x%02x%02x%02x\n",
                    evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
      return;
    }
    // Read real LEGO character ID from NFC page 36 of the physical figure.
    uint32_t toyId = readPhysicalToyId(evt.uid, evt.padFigIdx);
    if (toyId == 0) {
      // Fallback: UID bytes — game won't recognise the toy but at least shows it.
      toyId = (uint32_t)evt.uid[0] | ((uint32_t)evt.uid[1] << 8) |
              ((uint32_t)evt.uid[2] << 16) | ((uint32_t)evt.uid[3] << 24);
      Serial.printf("[pad] placed z=%u slot=%u uid=%02x%02x%02x%02x (UID fallback)\n",
                    z, slot, evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
    } else {
      Serial.printf("[pad] placed z=%u slot=%u uid=%02x%02x%02x%02x toyId=0x%04lx\n",
                    z, slot, evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3],
                    (unsigned long)toyId);
    }
    sSlotOccupied[slot - 1] = true;
    memcpy(sSlotUid[slot - 1], evt.uid, 7);
    sSlotToyId[slot - 1] = toyId;
    sSlotZone[slot - 1]  = z;
    sSlotIsVehicle[slot - 1] = sLastD2IsVehicle;
    uint8_t payload[7];
    payload[0] = slot; payload[1] = z;
    payload[2] = (uint8_t)toyId;         payload[3] = (uint8_t)(toyId >> 8);
    payload[4] = (uint8_t)(toyId >> 16); payload[5] = (uint8_t)(toyId >> 24);
    payload[6] = sLastD2IsVehicle ? 2 : 1;  // 1=character, 2=vehicle
    sendFrame(LP_MSG_TAG_SET, payload, 7, true);
    sLastLedCmdMs = millis();  // tag activity = game is active; suppress standby green
    // Report D2 outcome over LP debug so it appears in console output.
    sendFrame(LP_MSG_DEBUG, (const uint8_t*)sLastD2Status, strlen(sLastD2Status), false);
    // Send full 4-variant diagnostic (prefixed "aa-le:" so console can identify it).
    if (sLastD2Debug[0]) {
      const uint8_t dlen = (uint8_t)strnlen(sLastD2Debug, LP_MAX_PAYLOAD);
      sendFrame(LP_MSG_DEBUG, (const uint8_t*)sLastD2Debug, dlen, false);
    }
  } else {
    // REMOVE: find slot with matching UID AND matching zone.
    // Zone check prevents false removal when the toy already moved to a new zone
    // and a stale REMOVE arrives from the previous zone.
    uint8_t slot = 0;
    for (uint8_t si = 0; si < 7; si++) {
      if (sSlotOccupied[si] && sSlotZone[si] == z &&
          memcmp(sSlotUid[si], evt.uid, 7) == 0) {
        slot = si + 1; break;
      }
    }
    if (slot == 0) return;  // UID not in this zone -> stale REMOVE (toy moved), ignore
    // Defer the TAG_CLEAR instead of sending immediately.  If the toy is placed in
    // another zone within kMoveGraceMs we reuse this slot → console-rp2040 sees a
    // zone-move (same figIndex) → RPCS3 moves the character rather than spawning a copy.
    sSlotPendingRemove[slot - 1] = true;
    sSlotRemoveMs[slot - 1] = millis();
    // sSlotOccupied stays true so the slot is not reassigned during the grace period.
    sLastLedCmdMs = millis();  // tag activity = game is active; suppress standby green
    Serial.printf("[pad] pending-remove z=%u slot=%u uid=%02x%02x%02x%02x\n",
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
    :root{--muted:#93a0ba;--ink:#e5e7eb;}
    *{box-sizing:border-box;}
    body{margin:0;background:linear-gradient(170deg,#101827,#1b2a40);color:var(--ink);font-family:"Trebuchet MS","Segoe UI",sans-serif;}
    .app{max-width:640px;margin:0 auto;padding:14px;display:grid;gap:10px;}
    .card{background:rgba(25,34,52,.88);border:1px solid rgba(255,255,255,.14);border-radius:12px;padding:16px;}
    .badge{background:#273552;border:1px solid rgba(255,255,255,.2);border-radius:999px;padding:4px 10px;font-size:.84rem;display:inline-block;margin:3px 3px 3px 0;}
    .ok {background:rgba(22,163,74,.2);border-color:rgba(22,163,74,.4);color:#4ade80;}
    .warn{background:rgba(161,98,7,.2);border-color:rgba(161,98,7,.4);color:#fbbf24;}
    .err {background:rgba(185,28,28,.2);border-color:rgba(185,28,28,.4);color:#f87171;}
    h3{margin:0 0 10px;font-size:1rem;}
    .muted{color:var(--muted);font-size:.85rem;}
    .toy{padding:8px 0;border-bottom:1px solid rgba(255,255,255,.07);display:flex;gap:10px;align-items:flex-start;}
    .toy:last-child{border-bottom:none;}
    .toy-zone{font-size:.74rem;color:var(--muted);text-transform:uppercase;letter-spacing:.05em;min-width:46px;padding-top:2px;}
    .toy-name{font-weight:600;font-size:.95rem;}
    .toy-world{font-size:.8rem;color:var(--muted);}
    .empty{color:var(--muted);font-style:italic;font-size:.88rem;}
    code{background:#1a2540;border-radius:4px;padding:1px 5px;font-size:.85rem;}
  </style>
</head>
<body>
  <div class="app">
    <div class="card">
      <h3>Toy Pad Bridge</h3>
      <div class="muted" id="sub">Loading...</div>
    </div>
    <div class="card">
      <h3>Status</h3>
      <div id="status"></div>
    </div>
    <div class="card">
      <h3>Toys on Pad</h3>
      <div id="toys"><div class="empty">No toys detected</div></div>
    </div>
    <div class="card">
      <h3>WiFi</h3>
      <div id="wifi" class="muted">Loading...</div>
    </div>
    <div class="card muted" style="font-size:.82rem">
      Debug via USB serial at 115200 baud &mdash; press any key for menu.<br>
      <code>s</code>&nbsp;state &nbsp;<code>p N</code>&nbsp;place &nbsp;<code>r N</code>&nbsp;remove &nbsp;<code>f text</code>&nbsp;search
    </div>
    <div class="card" style="font-size:.8rem">
      <h3>NFC Decode Debug</h3>
      <div id="d2dbg" class="muted">Place a physical toy to see results&hellip;</div>
      <div class="muted" style="margin-top:.4rem">aa-le=Ellerbach/LE&nbsp; xr-le=RPCS3scr/LE&nbsp; aa-be=Ellerbach/BE&nbsp; xr-be=RPCS3scr/BE<br>(= means id0==id1 valid; ! means mismatch)</div>
    </div>
  </div>
  <script>
    var catalog=null;
    var ZONE=['Center','Left','Right'];
    function badge(txt,cls){return '<span class="badge'+(cls?' '+cls:'')+'">'+txt+'</span>';}
    async function loadCatalog(){
      if(catalog)return;
      try{
        var r=await fetch('/api/catalog',{cache:'force-cache'});
        var arr=await r.json();
        catalog={};
        arr.forEach(function(t){catalog[t.id]=t;});
      }catch(e){catalog={};}
    }
    function toyInfo(id){
      if(!catalog)return{name:'Toy '+id,world:'',type:'character'};
      return catalog[id]||{name:'Toy '+id,world:'',type:'character'};
    }
    async function refresh(){
      await loadCatalog();
      try{
        var r=await fetch('/api/state',{cache:'no-store'});
        var d=await r.json();
        document.getElementById('sub').textContent=
          d.consoleIp?'Paired with console at '+d.consoleIp:'Searching for console\u2026';
        var stCls=d.state==='paired'?'ok':d.state==='fail'?'err':'warn';
        document.getElementById('status').innerHTML=
          badge('Bridge: '+d.state,stCls)+
          badge('Game: '+(d.gameActive?'active':'idle'),d.gameActive?'ok':'warn')+
          badge('Console: '+(d.consoleIp||'\u2014'),d.consoleIp?'ok':'');
        var slots=d.slots||[];
        if(slots.length===0){
          document.getElementById('toys').innerHTML='<div class="empty">No toys on pad</div>';
        }else{
          document.getElementById('toys').innerHTML=slots.map(function(s){
            var t=toyInfo(s.toyId);
            return '<div class="toy"><span class="toy-zone">'+(ZONE[s.zone]||'?')+'</span>'
              +'<span><div class="toy-name">'+t.name+'</div>'
              +(t.world?'<div class="toy-world">'+t.world+'</div>':'')
              +'</span></div>';
          }).join('');
        }
        document.getElementById('wifi').textContent='SSID: '+d.ssid+' | IP: '+d.ip;
        if(d.d2dbg) document.getElementById('d2dbg').textContent=d.d2dbg;
      }catch(e){document.getElementById('sub').textContent='Error: '+e.message;}
    }
    refresh();
    setInterval(refresh,2000);
  </script>
</body>
</html>
)HTML";

static void handlePadCatalog() {
  web.send(200, "application/json", kLdCatalogJson);
}

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
  // Game active: LP LED_CMD received within the last 30 s.
  const uint32_t msNow = millis();
  json += ",\"gameActive\":";
  json += (sLastLedCmdMs != 0 && (msNow - sLastLedCmdMs) < kLedIdleMs) ? "true" : "false";
  // Current slot occupancy (USB-host builds only).
#if PAD_USB_HOST
  json += ",\"slots\":[";
  bool firstSlot = true;
  for (uint8_t si = 0; si < 7; si++) {
    if (!sSlotOccupied[si]) continue;
    if (!firstSlot) json += ",";
    json += "{\"slot\":";
    json += (uint32_t)(si + 1);
    json += ",\"zone\":";
    json += (uint32_t)sSlotZone[si];  // dynamic zone from global pool
    json += ",\"toyId\":";
    json += sSlotToyId[si];
    json += "}";
    firstSlot = false;
  }
  json += "]";
  // Diagnostic: last physical-toy D2 decryption results for all 4 variants.
  json += ",\"d2dbg\":\"";
  json += sLastD2Debug;
  json += "\"";
#else
  json += ",\"slots\":[]";
#endif
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
// For virtual toys the zone is derived from the slot number using the same
// mapping as slotToLpZone() in console-rp2040.
static uint8_t slotToLpZoneLocal(uint8_t slot) {
  if (slot == 2) return 0;                       // center
  if (slot == 1 || slot == 4 || slot == 5) return 1;  // left
  return 2;                                      // right (slots 3, 6, 7)
}

static void virtualPlace(uint8_t slot, uint32_t toyId) {
  if (slot < 1 || slot > 7) return;
  sVirtualOccupied[slot - 1] = true;
  sVirtualToyId[slot - 1]    = toyId;
  if (!paired) {
    Serial.println("  [not paired] cached — will sync once paired");
    return;
  }
  const uint8_t zone = slotToLpZoneLocal(slot);
  uint8_t p[7] = {slot, zone,
    (uint8_t)toyId, (uint8_t)(toyId >> 8),
    (uint8_t)(toyId >> 16), (uint8_t)(toyId >> 24),
    (uint8_t)(sVirtualIsVehicle[slot - 1] ? 2 : 1)};
  sendFrame(LP_MSG_TAG_SET, p, 7, true);
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
  web.on("/",            HTTP_GET, handlePadRoot);
  web.on("/api/state",   HTTP_GET, handlePadState);
  web.on("/api/catalog", HTTP_GET, handlePadCatalog);
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
      sLastLedCmdMs = millis();  // game is active
      // Buffer zone colour; flush as soon as all 3 zones are updated so that
      // ON and OFF batches queued back-to-back in the same TCP read are each
      // sent to the physical pad rather than the first being overwritten.
      // Coalesces zone-0/1/2 bursts into a single toypadLedAll() when all zones
      // share the same colour (eliminates zone-3 sync skew).
#if PAD_USB_HOST
      if (zone == 0xff) {
        sLedR[0]=r; sLedG[0]=g; sLedB[0]=b;
        sLedR[1]=r; sLedG[1]=g; sLedB[1]=b;
        sLedR[2]=r; sLedG[2]=g; sLedB[2]=b;
        sLedDirty = 0x07;
        flushPendingLed();   // zone=0xff sets all 3 at once → flush immediately
      } else if (zone < 3) {
        sLedR[zone]=r; sLedG[zone]=g; sLedB[zone]=b;
        sLedDirty |= (1u << zone);
        if (sLedDirty == 0x07) flushPendingLed();  // all 3 zones now updated → flush
      }
#else
      if (zone == 0xff) {
        toypadLedAll(r, g, b);
      } else {
        toypadLedZone(zone, r, g, b);
      }
#endif
      continue;
    }

#if PAD_USB_HOST
    // NFC_WRITE — game wrote to a page on a physical toy; forward as D3 to physical pad.
    if (frame.header.type == LP_MSG_NFC_WRITE && frame.header.length >= 6) {
      sendAck(frame.header.seq);
      const uint8_t slot = frame.payload[0];
      const uint8_t page = frame.payload[1];
      if (slot >= 1 && slot <= 7 && sSlotOccupied[slot - 1]) {
        const uint8_t figIdx = sSlotZone[slot - 1];  // 0=center,1=left,2=right (D3 figIdx)
        d3WritePages(figIdx, page, &frame.payload[2]);
      }
      continue;
    }
#endif

    // OTA_BEGIN — console-esp32 requests self-update via HTTP fetch
    if (frame.header.type == LP_MSG_OTA_BEGIN) {
      sendAck(frame.header.seq);
      Serial.println("[pad] OTA_BEGIN: starting HTTP update");
      WiFiClient httpCli;
      const t_httpUpdate_return ret =
          httpUpdate.update(httpCli, "http://192.168.44.1/ota/pad-esp32.bin");
      // HTTP_UPDATE_OK triggers automatic restart; only reached on failure.
      Serial.printf("[pad] OTA failed (%d): %s\n",
                    httpUpdate.getLastError(),
                    httpUpdate.getLastErrorString().c_str());
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
#if PAD_USB_HOST
  flushPendingLed();
#endif

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
      // While blinking red, check for the gesture-reset trigger:
      // placing Batman (toyId=1) in the center zone wipes credentials and
      // reboots into the provisioning portal.
#if PAD_USB_HOST
      {
        TagEvent evt;
        while (xQueueReceive(sTagQueue, &evt, 0) == pdTRUE) {
          if (evt.placed && evt.lpZone == 0) {
            const uint32_t toyId = readPhysicalToyId(evt.uid, evt.padFigIdx);
            if (toyId == kGestureResetToyId) {
              Serial.println("[pad] gesture reset: Batman in center → clearing config");
              clearConfig();
              delay(100);
              ESP.restart();
            }
          }
        }
      }
#endif  // PAD_USB_HOST
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
      // Move-grace flush: commit deferred TAG_CLEARs for toys removed >kMoveGraceMs
      // ago without being re-placed elsewhere.  If re-placed within the grace period,
      // forwardTagEvent() cancels the pending-remove flag and handles it as a move.
      {
        for (uint8_t si = 0; si < 7; si++) {
          if (sSlotPendingRemove[si] && (now - sSlotRemoveMs[si]) >= kMoveGraceMs) {
            sSlotPendingRemove[si] = false;
            sSlotOccupied[si]      = false;
            const uint8_t payload[1] = {(uint8_t)(si + 1)};
            sendFrame(LP_MSG_TAG_CLEAR, payload, 1, true);
            Serial.printf("[pad] removed (deferred) slot=%u\n", si + 1);
          }
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
            if (sSlotOccupied[si] && !sSlotPendingRemove[si]) {
              uint8_t p[7] = {s, sSlotZone[si],
                (uint8_t)sSlotToyId[si],        (uint8_t)(sSlotToyId[si] >> 8),
                (uint8_t)(sSlotToyId[si] >> 16), (uint8_t)(sSlotToyId[si] >> 24),
                (uint8_t)(sSlotIsVehicle[si] ? 2 : 1)};  // 1=char, 2=veh
              sendFrame(LP_MSG_TAG_SET, p, 7, false);
            } else {
              const uint8_t p[1] = {s};
              sendFrame(LP_MSG_TAG_CLEAR, p, 1, false);
            }
          }
        }
      }
      // LED keepalive: resend last known colour to the physical pad every
      // kLedKeepaliveMs to reset its built-in idle timer.  Without this the
      // pad hardware reverts to all-green after ~30 s of no C0 commands.
      {
        static uint32_t lastLedKeepaliveMs = 0;
        if (sUsbDevOpen && sLastLedCmdMs != 0 &&
            (now - lastLedKeepaliveMs) >= kLedKeepaliveMs) {
          lastLedKeepaliveMs = now;
          if (sLedLastSentValid) {
            toypadLedAll(sLedLastSentR, sLedLastSentG, sLedLastSentB);
          } else {
            for (uint8_t z = 0; z < 3; z++) {
              toypadLedZone(z, sLedR[z], sLedG[z], sLedB[z]);
            }
          }
        }
      }
#endif
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
              const uint8_t zone = slotToLpZoneLocal(s);
              uint8_t p[7] = {s, zone, (uint8_t)id, (uint8_t)(id >> 8),
                              (uint8_t)(id >> 16), (uint8_t)(id >> 24),
                              (uint8_t)(sVirtualIsVehicle[s - 1] ? 2 : 1)};
              sendFrame(LP_MSG_TAG_SET, p, 7, false);
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
