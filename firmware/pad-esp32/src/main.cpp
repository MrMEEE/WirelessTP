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
#include <WiFiUdp.h>
#include <WebServer.h>
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

// ─── Constants ───────────────────────────────────────────────────────────────
static const char*    kSetupApSsid      = "ToyPad-Setup";
static const char*    kSetupApPass      = "toypadsetup";
static const uint16_t kSetupPort        = 80;
static const uint16_t kDnsPort          = 53;
static const int      kResetPin         = 0;
static const uint32_t kResetHoldMs     = 3000;
static const uint16_t kConsolePort     = 25100;
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
  uint8_t lpZone;   // LP convention: 0=center, 1=left, 2=right
  bool    placed;   // true=placed, false=removed
  uint8_t uid[7];   // 7-byte NFC UID
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
static volatile bool            sUsbDevOpen = false;
static volatile bool            sUsbOutBusy = false;
static QueueHandle_t            sTagQueue   = NULL;
static uint8_t                  sMsgNum     = 1;  // toypad OUT message counter
// Zone occupancy tracked for periodic re-sync to console.
static uint32_t sZoneToyId[3]    = {0, 0, 0};
static uint8_t  sZoneUid[3][7]   = {};
static bool     sZoneOccupied[3] = {false, false, false};
#endif

// ─── WiFi / LP globals ────────────────────────────────────────────────────────
static WiFiUDP   udp;
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

#if PAD_USB_HOST
// LP zone 0-2 → canonical slot 1-7 (one slot per zone for passthrough)
// zone 0=center→slot 2, zone 1=left→slot 1, zone 2=right→slot 3
static const uint8_t kZoneToSlot[3] = {2, 1, 3};

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
      // Tag event format:
      //  [0]=0x56 magic  [1]=0x0b length
      //  [2]=pad zone (1=center, 2=left, 3=right)
      //  [3]=action (0=placed, 1=removed)
      //  [4..5]=padding  [6..12]=UID (7 bytes)  [13]=checksum
      const uint8_t padZone = d[2];
      const uint8_t action  = d[3];
      if (padZone >= 1 && padZone <= 3) {
        TagEvent evt;
        evt.lpZone = padZone - 1;
        evt.placed = (action == 0);
        memcpy(evt.uid, &d[6], 7);
        xQueueSend(sTagQueue, &evt, 0);
      }
    }
    // B0 reply (0x55...) and other IN data are silently consumed.
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
  usb_host_transfer_submit(sUsbInXfer);  // begin polling

  // Initialise toypad with B0 startup command.
  vTaskDelay(pdMS_TO_TICKS(50));
  sendToToypad(kToypadB0Cmd);
  Serial.println("[pad-usb] toypad ready");
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
static bool udpSendWire(const uint8_t* data, uint16_t len) {
  udp.beginPacket(consoleIp, kConsolePort);
  udp.write(data, len);
  return udp.endPacket() == 1;
}

static bool sendFrame(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                      bool trackAck) {
  uint8_t  wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen = 0;
  const uint8_t seq = seqCounter++;

  if (!lp_encode_frame(type, seq, payload, payloadLen, wire, sizeof(wire), &wireLen)) {
    return false;
  }
  if (!udpSendWire(wire, wireLen)) {
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
static void forwardTagEvent(const TagEvent& evt) {
  if (!paired) return;
  const uint8_t slot = kZoneToSlot[evt.lpZone];

  // Update zone occupancy for periodic re-sync.
  sZoneOccupied[evt.lpZone] = evt.placed;
  if (evt.placed) {
    memcpy(sZoneUid[evt.lpZone], evt.uid, sizeof(sZoneUid[0]));
    sZoneToyId[evt.lpZone] = (uint32_t)evt.uid[0] | ((uint32_t)evt.uid[1] << 8) |
                              ((uint32_t)evt.uid[2] << 16) | ((uint32_t)evt.uid[3] << 24);
  }

  if (evt.placed) {
    // Use the first 4 bytes of the NFC UID as toyId (LE).
    // The game will query NFC pages (D2) separately for full figure data.
    uint8_t payload[5];
    payload[0] = slot;
    memcpy(&payload[1], evt.uid, 4);
    sendFrame(LP_MSG_TAG_SET, payload, 5, true);
    Serial.printf("[pad] placed z=%u slot=%u uid=%02x%02x%02x%02x...\n",
                  evt.lpZone, slot,
                  evt.uid[0], evt.uid[1], evt.uid[2], evt.uid[3]);
  } else {
    const uint8_t payload[1] = {slot};
    sendFrame(LP_MSG_TAG_CLEAR, payload, 1, true);
    Serial.printf("[pad] removed z=%u slot=%u\n", evt.lpZone, slot);
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

// ─── Web UI (matches console-esp32 style) ────────────────────────────────────
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
    label { display:block; margin-bottom:4px; font-size:.9rem; }
    select,input[type=text],input[type=password] { width:100%; background:#192438; color:var(--ink); border:1px solid rgba(255,255,255,.22); border-radius:8px; padding:8px; margin-bottom:10px; font-size:.95rem; }
    button.primary { background:#4f46e5; color:#fff; border:0; border-radius:8px; padding:10px 18px; font-weight:700; width:100%; }
    button.primary:hover { background:#4338ca; }
    .toy-row { display:flex; gap:8px; align-items:flex-end; }
    .toy-row > div { flex:1; }
    .toy-row button { flex-shrink:0; background:#2f855a; color:#fff; border:0; border-radius:8px; padding:10px 12px; font-weight:700; white-space:nowrap; }
    .toy-row button:hover { background:#276749; }
    .toy-row button.remove { background:#7b1d18; }
    .toy-row button.remove:hover { background:#dc2626; }
    .zone-row { display:grid; grid-template-columns:repeat(3,1fr); gap:8px; margin-top:8px; }
    .zone-btn { background:#2d3a52; border:1px solid rgba(255,255,255,.2); border-radius:8px; padding:8px 4px; font-weight:700; color:var(--ink); }
    .zone-btn:hover { background:#3e4f70; }
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
    <div class="card">
      <h3 style="margin-top:0">Debug — Place Virtual Toy</h3>
      <div class="toy-row">
        <div>
          <label>Toy ID (hex or decimal)</label>
          <input type="text" id="toyId" placeholder="e.g. 0x0001 or 1">
        </div>
        <div>
          <label>Slot (1-7)</label>
          <input type="text" id="toySlot" value="1" style="width:70px">
        </div>
        <button onclick="placeToy()">Place</button>
      </div>
      <div class="zone-row">
        <button class="zone-btn" onclick="removeToy(1)">Remove Slot 1</button>
        <button class="zone-btn" onclick="removeToy(2)">Remove Slot 2</button>
        <button class="zone-btn" onclick="removeToy(3)">Remove Slot 3</button>
        <button class="zone-btn" onclick="removeToy(4)">Remove Slot 4</button>
        <button class="zone-btn" onclick="removeToy(5)">Remove Slot 5</button>
        <button class="zone-btn" onclick="removeToy(6)">Remove Slot 6</button>
        <button class="zone-btn" onclick="removeToy(7)">Remove Slot 7</button>
      </div>
    </div>
    <div class="card">
      <h3 style="margin-top:0">WiFi</h3>
      <div id="wifiStatus" class="muted">Loading...</div>
    </div>
  </div>
  <script>
    async function postJson(url, body) {
      const r = await fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body) });
      if (!r.ok) throw new Error(await r.text());
      return r.text();
    }
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
    function parseToyId(v) {
      v = v.trim();
      if (v.startsWith('0x') || v.startsWith('0X')) return parseInt(v.slice(2), 16);
      return parseInt(v, 10);
    }
    async function placeToy() {
      const id = parseToyId(document.getElementById('toyId').value);
      const slot = parseInt(document.getElementById('toySlot').value);
      if (isNaN(id) || isNaN(slot) || slot < 1 || slot > 7) { alert('Invalid toy ID or slot'); return; }
      try { await postJson('/api/toy/place', { id, slot }); await refresh(); }
      catch (e) { alert('Failed: ' + e.message); }
    }
    async function removeToy(slot) {
      try { await postJson('/api/toy/remove', { slot }); await refresh(); }
      catch (e) { alert('Failed: ' + e.message); }
    }
    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>
)HTML";

static bool parseJsonUint(const String& body, const char* key, uint32_t* out) {
  const String needle = String('"') + key + '"';
  int kp = body.indexOf(needle);
  if (kp < 0) return false;
  int cp = body.indexOf(':', kp + needle.length());
  if (cp < 0) return false;
  int s = cp + 1;
  while (s < (int)body.length() && isspace((unsigned char)body[s])) s++;
  int e = s;
  while (e < (int)body.length() && body[e] != ',' && body[e] != '}') e++;
  String t = body.substring(s, e);
  t.trim();
  if (t.startsWith("0x") || t.startsWith("0X")) *out = strtoul(t.c_str() + 2, NULL, 16);
  else *out = strtoul(t.c_str(), NULL, 10);
  return true;
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
  json += "}";
  web.send(200, "application/json", json);
}

static void handleToyPlace() {
  const String body = web.arg("plain");
  uint32_t toyId = 0, slot = 0;
  if (!parseJsonUint(body, "id", &toyId) || !parseJsonUint(body, "slot", &slot)
      || slot < 1 || slot > 7) {
    web.send(400, "text/plain", "missing id/slot");
    return;
  }
  if (!paired) { web.send(409, "text/plain", "not paired"); return; }
  uint8_t payload[5];
  payload[0] = (uint8_t)slot;
  payload[1] = (uint8_t)(toyId);
  payload[2] = (uint8_t)(toyId >> 8);
  payload[3] = (uint8_t)(toyId >> 16);
  payload[4] = (uint8_t)(toyId >> 24);
  sendFrame(LP_MSG_TAG_SET, payload, 5, true);
  Serial.printf("[pad-dbg] virtual place slot=%u id=0x%08lx\n", (unsigned)slot, (unsigned long)toyId);
  web.send(200, "text/plain", "ok");
}

static void handleToyRemove() {
  const String body = web.arg("plain");
  uint32_t slot = 0;
  if (!parseJsonUint(body, "slot", &slot) || slot < 1 || slot > 7) {
    web.send(400, "text/plain", "missing slot");
    return;
  }
  if (!paired) { web.send(409, "text/plain", "not paired"); return; }
  const uint8_t payload[1] = {(uint8_t)slot};
  sendFrame(LP_MSG_TAG_CLEAR, payload, 1, true);
  Serial.printf("[pad-dbg] virtual remove slot=%u\n", (unsigned)slot);
  web.send(200, "text/plain", "ok");
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
  while (true) { dns.processNextRequest(); web.handleClient(); delay(5); }
}

static bool connectToConsoleAp() {
  WiFi.mode(WIFI_STA);
  if (cfg.pass.length() == 0) {
    WiFi.begin(cfg.ssid.c_str());
  } else {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  }
  Serial.print("[pad] connecting to ");
  Serial.println(cfg.ssid);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - start > 20000) {
      Serial.println("[pad] wifi timeout");
      return false;
    }
  }
  consoleIp = WiFi.gatewayIP();
  if (consoleIp == IPAddress((uint32_t)0)) consoleIp = IPAddress(192, 168, 4, 1);
  Serial.print("[pad] console ip: ");
  Serial.println(consoleIp);
  return true;
}

static void startPadWeb() {
  web.on("/",              HTTP_GET,  handlePadRoot);
  web.on("/api/state",     HTTP_GET,  handlePadState);
  web.on("/api/toy/place", HTTP_POST, handleToyPlace);
  web.on("/api/toy/remove",HTTP_POST, handleToyRemove);
  web.begin();
}

static void maybeFactoryReset() {
  pinMode(kResetPin, INPUT_PULLUP);
  if (digitalRead(kResetPin) != LOW) return;
  const uint32_t start = millis();
  while (digitalRead(kResetPin) == LOW) {
    if (millis() - start >= kResetHoldMs) {
      Serial.println("[pad] factory reset");
      clearConfig();
      break;
    }
    delay(20);
  }
}

// ─── UDP receive ──────────────────────────────────────────────────────────────
static void processUdpIn() {
  if (udp.parsePacket() <= 0) return;

  uint8_t buf[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  const int bytes = udp.read(buf, sizeof(buf));
  if (bytes <= 0) return;

  lp_frame_t frame;
  if (!lp_decode_frame(buf, (uint16_t)bytes, &frame)) return;

  lastPeerMs = millis();

  // ACK
  if (frame.header.type == LP_MSG_ACK && frame.header.length == 1) {
    if (pending.active && frame.payload[0] == pending.seq) pending.active = false;
    return;
  }

  // PAIR_SET — console accepted us
  if (frame.header.type == LP_MSG_PAIR_SET && frame.header.length == 4) {
    const uint32_t secret =
        (uint32_t)frame.payload[0]        |
        ((uint32_t)frame.payload[1] << 8)  |
        ((uint32_t)frame.payload[2] << 16) |
        ((uint32_t)frame.payload[3] << 24);
    saveSharedSecret(secret);
    sendAck(frame.header.seq);
    pending.active = false;
    paired     = true;
    connState  = CONN_BLINK_OK;
    startBlink(kBlinkCount);
    Serial.println("[pad] paired");
    return;
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
    return;
  }

  // Everything else: ack and ignore
  sendAck(frame.header.seq);
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  // On S2 Mini with USB CDC disabled, Serial routes to UART0 (GPIO 43/44).
  Serial.begin(115200);
  delay(300);
  Serial.println("[pad] boot");

  maybeFactoryReset();
  cfg = loadConfig();
  if (!cfg.valid)            runProvisioningPortal();  // blocks until saved+reboot
  if (!connectToConsoleAp()) runProvisioningPortal();  // blocks until saved+reboot

  // Connected as STA — serve the debug/status web UI.
  startPadWeb();

  udp.begin(25101);

#if PAD_USB_HOST
  sTagQueue = xQueueCreate(8, sizeof(TagEvent));

  // USB lib task must start first; notify this task when ready.
  TaskHandle_t self = xTaskGetCurrentTaskHandle();
  xTaskCreate(usbLibTask, "usb_lib", 4096, self, configMAX_PRIORITIES - 1, NULL);
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));  // wait for USB host install

  xTaskCreate(usbClientTask, "usb_cli", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
#endif

  helloStartMs = millis();
  blinkLastMs  = 0;
  Serial.println("[pad] searching for console");
}

// ─── Main loop ────────────────────────────────────────────────────────────────
void loop() {
  const uint32_t now = millis();

  processUdpIn();

  // Retry pending LP frame
  if (pending.active && (now - pending.lastSentMs) >= kRetryMs) {
    if (pending.retries >= kMaxRetries) {
      pending.active = false;
    } else {
      pending.retries++;
      pending.lastSentMs = now;
      udpSendWire(pending.wire, pending.wireLen);
    }
  }

  // Connection state machine
  switch (connState) {

    case CONN_SEARCHING:
      updateSearchBlink();
      if (!pending.active && (now - lastHelloMs) >= kHelloMs) {
        lastHelloMs = now;
        sendHello();
      }
      // Give up after kPairTimeoutMs, blink red, then retry
      if ((now - helloStartMs) >= kPairTimeoutMs) {
        helloStartMs = now;
        blinkLastMs  = 0;
        connState    = CONN_BLINK_FAIL;
        startBlink(kBlinkCount);
      }
      break;

    case CONN_BLINK_OK:
      if (updateBlink(kGR, kGG, kGB)) {
        connState = CONN_PAIRED;
        lastHeartbeatMs = now;
        lastPeerMs      = now;
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
        connState    = CONN_SEARCHING;
      }
      break;

    case CONN_PAIRED:
      // Periodic heartbeat
      if ((now - lastHeartbeatMs) >= kHeartbeatMs) {
        lastHeartbeatMs = now;
        if (!pending.active) sendHello();
      }
      // Detect console lost
      if (lastPeerMs > 0 && (now - lastPeerMs) >= kLostMs) {
        Serial.println("[pad] console lost");
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
      // Periodic re-sync: re-send current physical pad state so the console
      // recovers the correct figure list after a restart or re-pair.
      {
        static uint32_t lastPadSyncMs = 0;
        if ((now - lastPadSyncMs) >= kStateSyncMs) {
          lastPadSyncMs = now;
          for (uint8_t z = 0; z < 3; z++) {
            const uint8_t s = kZoneToSlot[z];
            if (sZoneOccupied[z]) {
              uint8_t p[5] = {s,
                (uint8_t)sZoneToyId[z],        (uint8_t)(sZoneToyId[z] >> 8),
                (uint8_t)(sZoneToyId[z] >> 16), (uint8_t)(sZoneToyId[z] >> 24)};
              sendFrame(LP_MSG_TAG_SET, p, 5, false);
            } else {
              const uint8_t p[1] = {s};
              sendFrame(LP_MSG_TAG_CLEAR, p, 1, false);
            }
          }
        }
      }
#endif
      break;
  }

  dns.processNextRequest();
  web.handleClient();
  delay(5);
}
