#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <Updater.h>
#include <cstdarg>

#include "link_protocol.h"
#include "fw_header.h"

// Non-const so the linker places this string in the .data segment,
// making it scannable in the binary image for OTA target validation.
static char kFwIdent[] = FW_IDENT_STR;

// Forward declarations
static void debugPrintf(const char* fmt, ...);
static bool sendFrameUart(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                          uint8_t forceSeq = 0xff);

// RP2040 role:
// 1) Receive framed events from console ESP32 over UART.
// 2) Feed Toy Pad USB emulation state machine.
//
// This starter prints incoming bytes and sets up structure for USB integration.

static const uint32_t kUartBaud = 115200;
static const uint32_t kHelloMs = 4000;
static const uint32_t kStateSyncMs = 1000;  // full-state broadcast interval

// ─── DFU / OTA state ───────────────────────────────────────────────────────────
static const uint16_t kDfuChunkMax  = 1024;
static const uint8_t  kDfuAckByte   = 0x06;
static const uint8_t  kDfuNakByte   = 0x15;
static const uint32_t kDfuTimeoutMs = 10000;  // abort DFU if no chunk for 10 s

static bool     sDfuActive     = false;
static uint32_t sDfuSize       = 0;
static uint32_t sDfuReceived   = 0;
static uint32_t sDfuLastRxMs   = 0;
static uint8_t  sDfuRxBuf[8 + kDfuChunkMax + 2];  // header(8) + data + crc(2)
static uint16_t sDfuRxIdx      = 0;
static uint16_t sDfuRxExpected = 0;
static const uint8_t kToyPacketSize = 32;
static const uint8_t kToyMagicHostToPortal = 0x55;
static const uint8_t kToyMagicPortalEvent = 0x56;
static const uint8_t kToyTagEventPayloadSize = 0x0b;
static const uint8_t kToyTagActionPlaced = 0x00;
static const uint8_t kToyTagActionRemoved = 0x01;
static const uint8_t kToyUidSize = 7;
static const uint8_t kToyInQueueSize = 8;
static const uint8_t kToyUsbReportSize = 32;

// RPCS3 Dimensions.cpp COMMAND_KEY — used as TEA key for B1/B3 auth.
static const uint8_t kCommandKey[16] = {
  0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
  0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B,
};

// TEA decrypt: 8 bytes in, 8 bytes out, 32 rounds, key = kCommandKey.
// Matches RPCS3 Dimensions.cpp decrypt() with key == std::nullopt.
static void teaDecrypt(const uint8_t* in, uint8_t* out) {
  static const uint32_t kDelta = 0x9E3779B9u;
  uint32_t v0 = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
  uint32_t v1 = (uint32_t)in[4] | ((uint32_t)in[5] << 8) |
                ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
  uint32_t k0 = (uint32_t)kCommandKey[0]  | ((uint32_t)kCommandKey[1]  << 8) |
                ((uint32_t)kCommandKey[2]  << 16) | ((uint32_t)kCommandKey[3]  << 24);
  uint32_t k1 = (uint32_t)kCommandKey[4]  | ((uint32_t)kCommandKey[5]  << 8) |
                ((uint32_t)kCommandKey[6]  << 16) | ((uint32_t)kCommandKey[7]  << 24);
  uint32_t k2 = (uint32_t)kCommandKey[8]  | ((uint32_t)kCommandKey[9]  << 8) |
                ((uint32_t)kCommandKey[10] << 16) | ((uint32_t)kCommandKey[11] << 24);
  uint32_t k3 = (uint32_t)kCommandKey[12] | ((uint32_t)kCommandKey[13] << 8) |
                ((uint32_t)kCommandKey[14] << 16) | ((uint32_t)kCommandKey[15] << 24);
  uint32_t sum = kDelta * 32u;  // = 0xC6EF3720
  for (int i = 0; i < 32; i++) {
    v1 -= (((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3));
    v0 -= (((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1));
    sum -= kDelta;
  }
  out[0] = (uint8_t)(v0);        out[1] = (uint8_t)(v0 >> 8);
  out[2] = (uint8_t)(v0 >> 16);  out[3] = (uint8_t)(v0 >> 24);
  out[4] = (uint8_t)(v1);        out[5] = (uint8_t)(v1 >> 8);
  out[6] = (uint8_t)(v1 >> 16);  out[7] = (uint8_t)(v1 >> 24);
}

// TEA encrypt: 8 bytes in, 8 bytes out, 32 rounds, key = kCommandKey.
static void teaEncrypt(const uint8_t* in, uint8_t* out) {
  static const uint32_t kDelta = 0x9E3779B9u;
  uint32_t v0 = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
  uint32_t v1 = (uint32_t)in[4] | ((uint32_t)in[5] << 8) |
                ((uint32_t)in[6] << 16) | ((uint32_t)in[7] << 24);
  uint32_t k0 = (uint32_t)kCommandKey[0]  | ((uint32_t)kCommandKey[1]  << 8) |
                ((uint32_t)kCommandKey[2]  << 16) | ((uint32_t)kCommandKey[3]  << 24);
  uint32_t k1 = (uint32_t)kCommandKey[4]  | ((uint32_t)kCommandKey[5]  << 8) |
                ((uint32_t)kCommandKey[6]  << 16) | ((uint32_t)kCommandKey[7]  << 24);
  uint32_t k2 = (uint32_t)kCommandKey[8]  | ((uint32_t)kCommandKey[9]  << 8) |
                ((uint32_t)kCommandKey[10] << 16) | ((uint32_t)kCommandKey[11] << 24);
  uint32_t k3 = (uint32_t)kCommandKey[12] | ((uint32_t)kCommandKey[13] << 8) |
                ((uint32_t)kCommandKey[14] << 16) | ((uint32_t)kCommandKey[15] << 24);
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += kDelta;
    v0 += (((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1));
    v1 += (((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3));
  }
  out[0] = (uint8_t)(v0);        out[1] = (uint8_t)(v0 >> 8);
  out[2] = (uint8_t)(v0 >> 16);  out[3] = (uint8_t)(v0 >> 24);
  out[4] = (uint8_t)(v1);        out[5] = (uint8_t)(v1 >> 8);
  out[6] = (uint8_t)(v1 >> 16);  out[7] = (uint8_t)(v1 >> 24);
}

// Bob Jenkins' small PRNG — matches RPCS3 Dimensions.cpp initialize_rng/get_next.
static uint32_t rngA, rngB, rngC, rngD;

static uint32_t rngRotl32(uint32_t x, int r) {
  return (x << r) | (x >> (32 - r));
}

static uint32_t rngGetNext() {
  uint32_t e = rngA - rngRotl32(rngB, 21);
  rngA = rngB ^ rngRotl32(rngC, 19);
  rngB = rngC + rngRotl32(rngD, 6);
  rngC = rngD + e;
  rngD = e + rngA;
  return rngD;
}

static void rngInit(uint32_t seed) {
  rngA = 0xF1EA5EEDu;
  rngB = seed;
  rngC = seed;
  rngD = seed;
  for (int i = 0; i < 42; i++) {
    rngGetNext();
  }
}

// Observed startup payload returned by a real Toy Pad after B0.
static const uint8_t kToyB0ReplyPayload[] = {
  0x00, 0x2f, 0x02, 0x01, 0x02, 0x02, 0x04, 0x02,
  0xf5, 0x00, 0x19, 0x8d, 0x54, 0x8e, 0x2d, 0x5b,
  0xae, 0x4e, 0x00, 0x42, 0x17, 0x01, 0x00, 0x15,
};

static const uint16_t kUsbVid = 0x0e6f;
static const uint16_t kUsbPid = 0x0241;
static const char* kUsbManufacturer = "PDP LIMITED. ";
static const char* kUsbProduct = "LEGO READER V2.10";
static const char* kUsbSerial = "P.D.P.000000";

// Raw HID report descriptor used by the original reader.
static const uint8_t kHidReportDesc[] = {
  0x06, 0x00, 0xff,  // Usage Page (Vendor)
  0x09, 0x01,        // Usage (0x01)
  0xa1, 0x01,        // Collection (Application)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x20,        //   Usage Maximum (32)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xff, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x20,        //   Report Count (32)
  0x81, 0x00,        //   Input (Data,Arr,Abs)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x20,        //   Usage Maximum (32)
  0x91, 0x00,        //   Output (Data,Arr,Abs)
  0xc0               // End Collection
};

// HID interface: protocol=none, 1ms poll, interrupt OUT endpoint enabled.
static Adafruit_USBD_HID usb_hid(kHidReportDesc, sizeof(kHidReportDesc),
                                  HID_ITF_PROTOCOL_NONE, 1, true);

static uint16_t onHidGetReport(uint8_t report_id, hid_report_type_t report_type,
                               uint8_t* buffer, uint16_t reqlen);
static void onHidSetReport(uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const* buffer, uint16_t bufsize);
static bool toyUsbSendInPacket(const uint8_t* packet);

// Set USB identity before core starts TinyUSB enumeration.
struct UsbDescriptorPreset {
  UsbDescriptorPreset() {
    TinyUSBDevice.setID(kUsbVid, kUsbPid);
    TinyUSBDevice.setManufacturerDescriptor(kUsbManufacturer);
    TinyUSBDevice.setProductDescriptor(kUsbProduct);
    TinyUSBDevice.setSerialDescriptor(kUsbSerial);
  }
};

static UsbDescriptorPreset gUsbDescriptorPreset;

extern "C" uint8_t const* __real_tud_descriptor_configuration_cb(uint8_t index);

extern "C" uint8_t const* __wrap_tud_descriptor_configuration_cb(uint8_t index) {
  const uint8_t* base = __real_tud_descriptor_configuration_cb(index);
  if (base == nullptr) {
    return base;
  }

  static uint8_t patched[64];
  memset(patched, 0, sizeof(patched));

  // Current config is one HID interface descriptor block (41 bytes).
  const uint16_t copyLen = 41;
  memcpy(patched, base, copyLen);

  // Match original Toy Pad configuration descriptor fields:
  // bmAttributes = 0x80 (bus powered, no remote wake), bMaxPower = 250 (500mA)
  patched[7] = 0x80;
  patched[8] = 250;

  // HID descriptor bcdHID = 1.00 (0x0100) instead of TinyUSB default 1.11.
  patched[20] = 0x00;
  patched[21] = 0x01;

  // Endpoint max packet sizes: 32 bytes for IN/OUT.
  patched[31] = 0x20;
  patched[32] = 0x00;
  patched[38] = 0x20;
  patched[39] = 0x00;

  return patched;
}

// Called after SET_CONFIGURATION is processed. No delay needed — tested up to
// 2000ms with no effect on the RPCS3 passthrough issue.
extern "C" void tud_set_configuration_cb(uint8_t cfg_num) {
  (void)cfg_num;
}

extern "C" __attribute__((used)) void __wrap_TinyUSB_Device_Init(uint8_t rhport) {
  // Start with reader identity.
  TinyUSBDevice.setID(kUsbVid, kUsbPid);
  TinyUSBDevice.setManufacturerDescriptor(kUsbManufacturer);
  TinyUSBDevice.setProductDescriptor(kUsbProduct);
  TinyUSBDevice.setSerialDescriptor(kUsbSerial);
  TinyUSBDevice.begin(rhport);

  // Convert default startup config to HID-only before normal setup() runs.
  TinyUSBDevice.detach();
  Serial.end();
  TinyUSBDevice.clearConfiguration();
  usb_hid.setReportCallback(onHidGetReport, onHidSetReport);
  usb_hid.begin();

  // clearConfiguration() resets descriptors to compile-time defaults.
  TinyUSBDevice.setID(kUsbVid, kUsbPid);
  TinyUSBDevice.setManufacturerDescriptor(kUsbManufacturer);
  TinyUSBDevice.setProductDescriptor(kUsbProduct);
  TinyUSBDevice.setSerialDescriptor(kUsbSerial);
  TinyUSBDevice.attach();
}

enum ToyPadProfile : uint8_t {
  PROFILE_PLAYSTATION = 0,
  PROFILE_XBOX360 = 1,
};

#ifdef TOY_PROFILE_XBOX360
static const ToyPadProfile kActiveProfile = PROFILE_XBOX360;
#else
static const ToyPadProfile kActiveProfile = PROFILE_PLAYSTATION;
#endif

enum ToyPadCommand : uint8_t {
  TOY_CMD_B0 = 0xb0,
  TOY_CMD_B1 = 0xb1,
  TOY_CMD_B3 = 0xb3,
  TOY_CMD_C0 = 0xc0,
  TOY_CMD_C2 = 0xc2,
  TOY_CMD_C3 = 0xc3,
  TOY_CMD_C6 = 0xc6,
  TOY_CMD_C8 = 0xc8,
  TOY_CMD_D2 = 0xd2,
  TOY_CMD_D3 = 0xd3,
  TOY_CMD_D4 = 0xd4,
};

struct ToyPadOutPacket {
  uint8_t raw[kToyPacketSize];
  uint8_t length;
  uint8_t command;
  uint8_t counter;
  const uint8_t* args;
  uint8_t argsLen;
  uint8_t checksum;
  bool validMagic;
  bool validChecksum;
};

struct PadRgb {
  bool enable;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct ToyPadState {
  PadRgb center;
  PadRgb left;
  PadRgb right;
  uint8_t lastCounter;
  bool initialized;
  bool sawDynamicB1;
  bool sawDynamicB3;
  uint8_t slotUid[7][kToyUidSize];  // per web-UI slot (index = slot-1)
  bool slotUidValid[7];
  uint32_t slotToyId[7];  // toy ID per slot (index = slot-1, slots 1-7)
  uint8_t slotZone[7];    // LP zone (0=center,1=left,2=right) of each slot
  bool slotIsVehicle[7];  // true if the toy in this slot is a vehicle (vs character)
  bool slotIsPhysical[7]; // true if the toy came from the physical pad (needs D3 forwarding)
};

struct ToyInPacketQueue {
  uint8_t packets[kToyInQueueSize][kToyPacketSize];
  uint8_t head;
  uint8_t tail;
  uint8_t count;
};

// Per-slot NFC write cache.  The game writes vehicle upgrade/initialization
// data via D3 to pages across the full NTAG213 user-data range (pages 4-44).
// Vehicles write to early pages (4-19) as well as the upper range (20-44),
// so the cache must cover all user-writable pages.  Pages 0-3 are UID/lock
// and are never written by the game.
// We cache all D3 writes and serve them back on D2 reads so that vehicle
// upgrades persist for the session.
static const uint8_t kPageCacheMin   = 4;
static const uint8_t kPageCacheMax   = 44;  // inclusive
static const uint8_t kPageCacheCount = kPageCacheMax - kPageCacheMin + 1;  // 41
struct PageCacheEntry { bool valid; uint8_t data[4]; };
static PageCacheEntry sPageCache[7][kPageCacheCount];  // [slot-1][page - kPageCacheMin]

lp_stream_parser_t uartParser;
uint8_t seqCounter = 0;
uint32_t lastHelloMs = 0;
ToyPadState toyState = {};
ToyInPacketQueue toyInQueue = {};
uint32_t b0ReceivedCount = 0;
uint32_t b0QueuedCount = 0;
uint32_t b0SentCount = 0;


static void handleToyCommand(const ToyPadOutPacket& p);
static bool toyUsbSendInPacket(const uint8_t* packet);

static bool isXboxProfile() {
  return kActiveProfile == PROFILE_XBOX360;
}

static void onXboxDynamicB1(const ToyPadOutPacket& packet) {
  (void)packet;
  // Placeholder for donor-auth passthrough/session tracking in Xbox profile.
  debugPrintf("xbox b1 hook");
}

static void onXboxDynamicB3(const ToyPadOutPacket& packet) {
  (void)packet;
  // Placeholder for donor-auth passthrough/session tracking in Xbox profile.
  debugPrintf("xbox b3 hook");
}

static void onXboxReadWriteCommand(const ToyPadOutPacket& packet) {
  (void)packet;
  // Placeholder for D2/D3 behavior that may require donor-auth collaboration.
  debugPrintf("xbox d2/d3 hook");
}

static uint8_t toyChecksum(const uint8_t* bytes, uint8_t len) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < len; ++i) {
    sum += bytes[i];
  }
  return (uint8_t)(sum & 0xff);
}

static void sendLedZoneUart(uint8_t zone, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t payload[4] = {zone, r, g, b};
  sendFrameUart(LP_MSG_LED_CMD, payload, sizeof(payload));
  debugPrintf("led z=%u r=%u g=%u b=%u", zone, r, g, b);
}

static void applyPadColor(uint8_t pad, uint8_t r, uint8_t g, uint8_t b) {
  auto setPad = [r, g, b](PadRgb& p) {
    p.enable = true;
    p.r = r;
    p.g = g;
    p.b = b;
  };

  switch (pad) {
    case 0:
      setPad(toyState.center);
      setPad(toyState.left);
      setPad(toyState.right);
      sendLedZoneUart(0xff, r, g, b);
      break;
    case 1:  // pad 1 = center (C0 convention)
      setPad(toyState.center);
      sendLedZoneUart(0, r, g, b);
      break;
    case 2:  // pad 2 = left (C0 convention)
      setPad(toyState.left);
      sendLedZoneUart(1, r, g, b);
      break;
    case 3:  // pad 3 = right (C0 convention)
      setPad(toyState.right);
      sendLedZoneUart(2, r, g, b);
      break;
    default:
      break;
  }
}

static bool parseToyPacket(const uint8_t* packet, uint8_t len, ToyPadOutPacket* out) {
  if (out == nullptr || packet == nullptr || len != kToyPacketSize) {
    return false;
  }

  for (uint8_t i = 0; i < kToyPacketSize; ++i) {
    out->raw[i] = packet[i];
  }

  out->validMagic = (packet[0] == kToyMagicHostToPortal);
  out->length = packet[1];

  // Host->portal packet format is 55, len, cmd, counter, args..., checksum, zero padding.
  if (!out->validMagic || out->length < 2) {
    out->validChecksum = false;
    return false;
  }

  const uint8_t checksumIndex = (uint8_t)(out->length + 2);
  if (checksumIndex >= kToyPacketSize) {
    out->validChecksum = false;
    return false;
  }

  out->command = packet[2];
  out->counter = packet[3];
  out->args = &packet[4];
  out->argsLen = (uint8_t)(out->length - 2);
  out->checksum = packet[checksumIndex];
  out->validChecksum = (toyChecksum(packet, checksumIndex) == out->checksum);
  return out->validChecksum;
}

static bool normalizeToyOutReport(const uint8_t* buffer, uint16_t bufsize,
                                  uint8_t outPacket[kToyPacketSize]) {
  if (buffer == nullptr || outPacket == nullptr) {
    return false;
  }

  // Normal case: raw 32-byte Toy Pad payload.
  if (bufsize >= kToyPacketSize && buffer[0] == kToyMagicHostToPortal) {
    memcpy(outPacket, buffer, kToyPacketSize);
    return true;
  }

  // Some HID stacks may include a leading report-id byte in callbacks.
  if (bufsize >= (uint16_t)(kToyPacketSize + 1) && buffer[1] == kToyMagicHostToPortal) {
    memcpy(outPacket, buffer + 1, kToyPacketSize);
    return true;
  }

  return false;
}

static uint8_t lpZoneToToyZone(uint8_t lpZone) {
  // Bridge convention: 0 center, 1 left, 2 right -> Toy Pad convention: 1,2,3
  switch (lpZone) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 3;
    default: return 0;
  }
}

// Maps web UI slot (1-7) to LP zone (0=center, 1=left, 2=right).
// Mirrors slotToLpZone() in ESP32 firmware.
static uint8_t slotToLpZone(uint8_t slot) {
  if (slot == 2) return 0;
  if (slot == 1 || slot == 4 || slot == 5) return 1;
  return 2;  // slots 3, 6, 7 = right
}

static bool queueToyInPacket(const uint8_t* packet) {
  if (packet == nullptr || toyInQueue.count >= kToyInQueueSize) {
    return false;
  }

  memcpy(toyInQueue.packets[toyInQueue.tail], packet, kToyUsbReportSize);
  toyInQueue.tail = (uint8_t)((toyInQueue.tail + 1) % kToyInQueueSize);
  toyInQueue.count++;
  return true;
}

static void buildToyReplyPacket(uint8_t counter, const uint8_t* payload, uint8_t payloadLen,
                                uint8_t out[kToyPacketSize]) {
  memset(out, 0, kToyPacketSize);
  out[0] = kToyMagicHostToPortal;
  out[1] = (uint8_t)(1 + payloadLen);  // counter + payload
  out[2] = counter;
  if (payloadLen > 0 && payload != nullptr) {
    memcpy(&out[3], payload, payloadLen);
  }
  const uint8_t checksumIndex = (uint8_t)(out[1] + 2);
  if (checksumIndex < kToyPacketSize) {
    out[checksumIndex] = toyChecksum(out, checksumIndex);
  }
}

static bool popToyInPacket(uint8_t* outPacket) {
  if (outPacket == nullptr || toyInQueue.count == 0) {
    return false;
  }

  memcpy(outPacket, toyInQueue.packets[toyInQueue.head], kToyUsbReportSize);
  toyInQueue.head = (uint8_t)((toyInQueue.head + 1) % kToyInQueueSize);
  toyInQueue.count--;
  return true;
}

// From RPCS3 Dimensions.cpp CHAR_CONSTANT — used in generate_figure_key scramble.
static const uint8_t kCharConstant[17] = {
  0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8,
  0xD8, 0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA
};

static uint32_t rotr32(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

// dimensions_randomize from RPCS3 Dimensions.cpp
static uint32_t dimensionsRandomize(const uint8_t* key, uint8_t count) {
  uint32_t scrambled = 0;
  for (uint8_t i = 0; i < count; i++) {
    const uint32_t v4 = rotr32(scrambled, 25);
    const uint32_t v5 = rotr32(scrambled, 10);
    const uint32_t b = (uint32_t)key[i * 4] | ((uint32_t)key[i * 4 + 1] << 8) |
                       ((uint32_t)key[i * 4 + 2] << 16) | ((uint32_t)key[i * 4 + 3] << 24);
    scrambled = b + v4 + v5 - scrambled;
  }
  return scrambled;
}

// scramble from RPCS3 Dimensions.cpp — uid is 7-byte NTAG213 UID (BCC0 excluded)
static uint32_t figureScramble(const uint8_t uid[kToyUidSize], uint8_t count) {
  uint8_t buf[kToyUidSize + 17];  // 7 + 17 = 24 bytes
  memcpy(buf, uid, kToyUidSize);
  memcpy(buf + kToyUidSize, kCharConstant, 17);
  buf[count * 4 - 1] ^= count;
  return dimensionsRandomize(buf, count);
}

// generate_figure_key from RPCS3 Dimensions.cpp — 16-byte figure-specific key
static void generateFigureKey(const uint8_t uid[kToyUidSize], uint8_t key[16]) {
  const uint32_t s3 = figureScramble(uid, 3);
  const uint32_t s4 = figureScramble(uid, 4);
  const uint32_t s5 = figureScramble(uid, 5);
  const uint32_t s6 = figureScramble(uid, 6);
  // Store as big-endian u32 words.
  key[0]=(s3>>24); key[1]=(s3>>16); key[2]=(s3>>8);  key[3]=s3;
  key[4]=(s4>>24); key[5]=(s4>>16); key[6]=(s4>>8);  key[7]=s4;
  key[8]=(s5>>24); key[9]=(s5>>16); key[10]=(s5>>8); key[11]=s5;
  key[12]=(s6>>24); key[13]=(s6>>16); key[14]=(s6>>8); key[15]=s6;
}

// TEA encrypt with arbitrary 16-byte key (vs teaEncrypt which uses kCommandKey)
static void teaEncryptWithKey(const uint8_t* in, uint8_t* out, const uint8_t key[16]) {
  static const uint32_t kDelta = 0x9E3779B9u;
  uint32_t v0 = (uint32_t)in[0] | ((uint32_t)in[1]<<8) | ((uint32_t)in[2]<<16) | ((uint32_t)in[3]<<24);
  uint32_t v1 = (uint32_t)in[4] | ((uint32_t)in[5]<<8) | ((uint32_t)in[6]<<16) | ((uint32_t)in[7]<<24);
  const uint32_t k0 = (uint32_t)key[0]  | ((uint32_t)key[1]  <<8) | ((uint32_t)key[2]  <<16) | ((uint32_t)key[3]  <<24);
  const uint32_t k1 = (uint32_t)key[4]  | ((uint32_t)key[5]  <<8) | ((uint32_t)key[6]  <<16) | ((uint32_t)key[7]  <<24);
  const uint32_t k2 = (uint32_t)key[8]  | ((uint32_t)key[9]  <<8) | ((uint32_t)key[10] <<16) | ((uint32_t)key[11] <<24);
  const uint32_t k3 = (uint32_t)key[12] | ((uint32_t)key[13] <<8) | ((uint32_t)key[14] <<16) | ((uint32_t)key[15] <<24);
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += kDelta;
    v0 += (((v1<<4)+k0)^(v1+sum)^((v1>>5)+k1));
    v1 += (((v0<<4)+k2)^(v0+sum)^((v0>>5)+k3));
  }
  out[0]=(uint8_t)v0;     out[1]=(uint8_t)(v0>>8); out[2]=(uint8_t)(v0>>16); out[3]=(uint8_t)(v0>>24);
  out[4]=(uint8_t)v1;     out[5]=(uint8_t)(v1>>8); out[6]=(uint8_t)(v1>>16); out[7]=(uint8_t)(v1>>24);
}

static void buildToyUidFromToyId(uint32_t toyId, uint8_t /*slot*/, uint8_t uidOut[kToyUidSize]) {
  uidOut[0] = (uint8_t)(toyId & 0xff);
  uidOut[1] = (uint8_t)((toyId >> 8) & 0xff);
  uidOut[2] = (uint8_t)((toyId >> 16) & 0xff);
  uidOut[3] = (uint8_t)((toyId >> 24) & 0xff);
  uidOut[4] = 0xa5;
  // Stable byte derived from toyId so the UID does not change when the toy
  // moves between zones (which reassigns its slot number).  RPCS3 matches
  // zone-moves by UID identity, not by figIndex.
  uidOut[5] = (uint8_t)(toyId ^ (toyId >> 8) ^ (toyId >> 16) ^ (toyId >> 24));
  uidOut[6] = (uint8_t)(toyId ^ 0x5a);
}

static bool enqueueToyTagEvent(uint8_t pad, uint8_t figIndex, uint8_t action, const uint8_t uid[kToyUidSize]) {
  uint8_t packet[kToyPacketSize] = {0};
  packet[0] = kToyMagicPortalEvent;
  packet[1] = kToyTagEventPayloadSize;
  packet[2] = pad;       // pad: 1=center, 2=left, 3=right
  packet[3] = 0x00;
  packet[4] = figIndex;  // figure index: slot number (1-7), unique per toy
  packet[5] = action;
  memcpy(&packet[6], uid, kToyUidSize);

  const uint8_t checksumIndex = (uint8_t)(packet[1] + 2);
  packet[checksumIndex] = toyChecksum(packet, checksumIndex);

  const bool queued = queueToyInPacket(packet);
  if (!queued) {
    debugPrintf("queue full: event dropped");
  }
  return queued;
}

static bool enqueueToyReply(uint8_t counter, const uint8_t* payload, uint8_t payloadLen) {
  uint8_t packet[kToyPacketSize] = {0};
  buildToyReplyPacket(counter, payload, payloadLen, packet);

  const uint8_t checksumIndex = (uint8_t)(packet[1] + 2);
  if (checksumIndex >= kToyPacketSize) {
    return false;
  }

  // Queue for delivery from the main loop (never call TinyUSB re-entrantly
  // from within a TinyUSB callback such as tud_hid_set_report_cb).
  const bool queued = queueToyInPacket(packet);
  if (!queued) {
    debugPrintf("queue full: reply dropped");
  }
  return queued;
}

static bool sendOrQueueToyReply(uint8_t counter, const uint8_t* payload, uint8_t payloadLen) {
  uint8_t packet[kToyPacketSize] = {0};
  buildToyReplyPacket(counter, payload, payloadLen, packet);

  // Try immediate send first to satisfy startup timing-sensitive commands.
  if (toyUsbSendInPacket(packet)) {
    return true;
  }
  return queueToyInPacket(packet);
}

static bool toyUsbSendInPacket(const uint8_t* packet) {
  if (packet == nullptr) return false;
  if (!TinyUSBDevice.mounted()) return false;
  bool ok = usb_hid.sendReport(0, packet, kToyUsbReportSize);
  // NOTE: do NOT debugPrintf here — called from a tight retry loop; UART
  // flood would block Serial1.write(), stall loop(), and prevent
  // TinyUSBDevice.task() from running, which deadlocks sendReport forever.
  return ok;
}

static uint16_t onHidGetReport(uint8_t report_id, hid_report_type_t report_type,
                               uint8_t* buffer, uint16_t reqlen) {
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  // Toy Pad traffic uses interrupt OUT/IN reports, not control GET_REPORT.
  return 0;
}

static void onHidSetReport(uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const* buffer, uint16_t bufsize) {
  (void)report_id;
  (void)report_type;
  uint8_t normalized[kToyPacketSize];
  if (!normalizeToyOutReport(buffer, bufsize, normalized)) {
    debugPrintf("rejected OUT: bad shape");
    return;
  }

  ToyPadOutPacket pkt = {};
  if (parseToyPacket(normalized, kToyPacketSize, &pkt)) {
    handleToyCommand(pkt);
  } else {
    debugPrintf("rejected OUT: bad magic/cksum");
  }
}

static bool peekToyInPacket(uint8_t* outPacket) {
  if (outPacket == nullptr || toyInQueue.count == 0) return false;
  memcpy(outPacket, toyInQueue.packets[toyInQueue.head], kToyUsbReportSize);
  return true;
}

static void serviceToyInQueue() {
  // Peek-before-pop: only remove a packet after a successful send.
  // This ensures the packet is retried on future loop() iterations if
  // the endpoint is busy or not yet ready.
  static bool loggedFirstAttempt = false;
  while (toyInQueue.count > 0) {
    if (!TinyUSBDevice.mounted()) {
      // No USB host connected — discard stale events to prevent overflow.
      // The host will receive fresh events via state-sync when it connects.
      memset(&toyInQueue, 0, sizeof(toyInQueue));
      break;
    }
    uint8_t packet[kToyUsbReportSize];
    if (!peekToyInPacket(packet)) break;
    // One-shot diagnostic: capture pre-send state, then log with the result.
    bool rdyBefore = !loggedFirstAttempt ? usb_hid.ready() : false;
    bool ok = toyUsbSendInPacket(packet);
    if (!loggedFirstAttempt) {
      loggedFirstAttempt = true;
      debugPrintf("svc1: rdy=%d ok=%d q=%u", rdyBefore, ok, toyInQueue.count);
    }
    if (!ok) break;  // not ready yet; retry later
    if (packet[0] == kToyMagicHostToPortal && packet[1] >= 1 && packet[2] == toyState.lastCounter) {
      // Count startup replies that were actually sent: len=0x19 indicates B0 payload.
      if (packet[1] == (uint8_t)(1 + sizeof(kToyB0ReplyPayload))) {
        b0SentCount++;
      }
    }
    popToyInPacket(packet);  // send succeeded, consume the slot
  }
}

// Generates 16 bytes (4 NTAG213 pages) of synthetic figure data for a given slot.
// Used to respond to D2 (block read) commands from the game.
static void buildSyntheticFigurePage16(uint8_t slot, uint8_t startPage, uint8_t out[16]) {
  memset(out, 0, 16);
  if (slot < 1 || slot > 7 || !toyState.slotUidValid[slot - 1]) return;

  const uint8_t* uid = toyState.slotUid[slot - 1];
  const uint32_t toyId = toyState.slotToyId[slot - 1];
  const bool isVehicle = toyState.slotIsVehicle[slot - 1];

  // Pre-compute page-36/37 encrypted data only for characters (not vehicles).
  // Vehicles store their ID unencrypted at page 36 bytes 0-1 (LE uint16).
  uint8_t enc36[8] = {0};
  if (!isVehicle && startPage <= 37 && startPage + 3 >= 36) {
    uint8_t figKey[16];
    generateFigureKey(uid, figKey);
    uint8_t toEnc[8];
    toEnc[0]=(uint8_t)toyId;     toEnc[1]=(uint8_t)(toyId>>8);
    toEnc[2]=(uint8_t)(toyId>>16); toEnc[3]=(uint8_t)(toyId>>24);
    toEnc[4]=(uint8_t)toyId;     toEnc[5]=(uint8_t)(toyId>>8);
    toEnc[6]=(uint8_t)(toyId>>16); toEnc[7]=(uint8_t)(toyId>>24);
    teaEncryptWithKey(toEnc, enc36, figKey);
  }

  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t page = startPage + i;
    uint8_t* pp = &out[i * 4];
    // Check write cache first (D3 writes take precedence over synthetic data).
    if (page >= kPageCacheMin && page <= kPageCacheMax) {
      const PageCacheEntry& e = sPageCache[slot - 1][page - kPageCacheMin];
      if (e.valid) { memcpy(pp, e.data, 4); continue; }
    }
    switch (page) {
      case 0:
        pp[0]=uid[0]; pp[1]=uid[1]; pp[2]=uid[2];
        pp[3]=uid[0]^uid[1]^uid[2]^0x88;  // BCC0
        break;
      case 1:
        pp[0]=uid[3]; pp[1]=uid[4]; pp[2]=uid[5]; pp[3]=uid[6];
        break;
      case 2:
        pp[0]=uid[3]^uid[4]^uid[5]^uid[6];  // BCC1
        pp[1]=0x48;  // NTAG213 capability container
        break;
      case 36:
        if (isVehicle) {
          // Vehicle: ID stored unencrypted as LE uint16 at bytes 0-1.
          pp[0]=(uint8_t)toyId; pp[1]=(uint8_t)(toyId>>8);
          // bytes 2-3 stay zero (memset'd above)
        } else {
          memcpy(pp, enc36, 4);
        }
        break;
      case 37:
        if (!isVehicle) memcpy(pp, enc36 + 4, 4);
        // vehicles: zeros (memset'd above)
        break;
      case 38:
        if (isVehicle) {
          // Vehicle marker: game distinguishes vehicle from character by
          // checking bytes 0-1 of page 38 == 0x0001 (big-endian).
          pp[0]=0x00; pp[1]=0x01; pp[2]=0x00; pp[3]=0x00;
        }
        break;
      default:
        break;  // zeros
    }
  }
}

static void handleToyCommand(const ToyPadOutPacket& p) {
  toyState.lastCounter = p.counter;

  switch (p.command) {
    case TOY_CMD_B0:
      // Reset b0SentCount so the keepalive gate (b0SentCount > 0) suppresses
      // keepalives until this new B0 reply is actually sent. This handles the
      // case where tud_mount_cb didn't fire (RPCS3 reconnect without USB reset).
      b0SentCount = 0;
      b0ReceivedCount++;
      toyState.initialized = true;
      debugPrintf("b0 ctr=%u", p.counter);
      // Queue exactly one reply — never replay. The physical device sends B0
      // reply once then goes silent. Replaying confuses RPCS3's state machine.
      if (enqueueToyReply(p.counter, kToyB0ReplyPayload, sizeof(kToyB0ReplyPayload))) {
        b0QueuedCount++;
      }
      break;
    case TOY_CMD_B1: {
      // B1 (Seed): decrypt challenge, seed RNG, re-encrypt confirmation.
      // Algorithm from RPCS3 Dimensions.cpp generate_random_number().
      toyState.sawDynamicB1 = true;
      if (isXboxProfile()) {
        onXboxDynamicB1(p);
      }
      if (p.argsLen >= 8) {
        uint8_t dec[8];
        teaDecrypt(p.args, dec);
        // Seed = first 4 bytes of decrypted payload (little-endian u32).
        uint32_t seed = (uint32_t)dec[0] | ((uint32_t)dec[1] << 8) |
                        ((uint32_t)dec[2] << 16) | ((uint32_t)dec[3] << 24);
        // Confirmation = last 4 bytes (big-endian u32).
        uint32_t conf = ((uint32_t)dec[4] << 24) | ((uint32_t)dec[5] << 16) |
                        ((uint32_t)dec[6] << 8)  | (uint32_t)dec[7];
        rngInit(seed);
        // Encrypt [conf in BE | 0x00000000].
        uint8_t toEnc[8] = {};
        toEnc[0] = (uint8_t)(conf >> 24); toEnc[1] = (uint8_t)(conf >> 16);
        toEnc[2] = (uint8_t)(conf >> 8);  toEnc[3] = (uint8_t)(conf);
        uint8_t enc[8];
        teaEncrypt(toEnc, enc);
        enqueueToyReply(p.counter, enc, 8);
        debugPrintf("b1 ok seed=%08lx", (unsigned long)seed);
      } else {
        enqueueToyReply(p.counter, nullptr, 0);
        debugPrintf("b1 short args=%u", p.argsLen);
      }
      break;
    }
    case TOY_CMD_B3: {
      // B3 (Challenge): decrypt challenge, get next RNG value, re-encrypt response.
      // Algorithm from RPCS3 Dimensions.cpp get_challenge_response().
      toyState.sawDynamicB3 = true;
      if (isXboxProfile()) {
        onXboxDynamicB3(p);
      }
      if (p.argsLen >= 8) {
        uint8_t dec[8];
        teaDecrypt(p.args, dec);
        // Confirmation = first 4 bytes of decrypted payload (big-endian u32).
        uint32_t conf = ((uint32_t)dec[0] << 24) | ((uint32_t)dec[1] << 16) |
                        ((uint32_t)dec[2] << 8)  | (uint32_t)dec[3];
        uint32_t nextRand = rngGetNext();
        // Encrypt [nextRand in LE | conf in BE].
        uint8_t toEnc[8] = {};
        toEnc[0] = (uint8_t)(nextRand);       toEnc[1] = (uint8_t)(nextRand >> 8);
        toEnc[2] = (uint8_t)(nextRand >> 16); toEnc[3] = (uint8_t)(nextRand >> 24);
        toEnc[4] = (uint8_t)(conf >> 24); toEnc[5] = (uint8_t)(conf >> 16);
        toEnc[6] = (uint8_t)(conf >> 8);  toEnc[7] = (uint8_t)(conf);
        uint8_t enc[8];
        teaEncrypt(toEnc, enc);
        enqueueToyReply(p.counter, enc, 8);
        debugPrintf("b3 ok rng=%08lx", (unsigned long)nextRand);
      } else {
        enqueueToyReply(p.counter, nullptr, 0);
        debugPrintf("b3 short args=%u", p.argsLen);
      }
      break;
    }
    case TOY_CMD_C0:
      if (p.argsLen >= 4) {
        const uint8_t pad = p.args[0];
        const uint8_t r = p.args[1];
        const uint8_t g = p.args[2];
        const uint8_t b = p.args[3];
        applyPadColor(pad, r, g, b);
        debugPrintf("c0 pad=%u rgb=%u,%u,%u", pad, r, g, b);
      }
      enqueueToyReply(p.counter, nullptr, 0);
      break;
    case TOY_CMD_C6:
      // Fade animation for all 3 zones. Order: center, left, right.
      // Each zone block is 6 bytes: [effect, duration, count, r, g, b]
      if (p.argsLen >= 18) {
        sendLedZoneUart(0, p.args[3],  p.args[4],  p.args[5]);   // center
        sendLedZoneUart(1, p.args[9],  p.args[10], p.args[11]);  // left
        sendLedZoneUart(2, p.args[15], p.args[16], p.args[17]);  // right
        toyState.center.r = p.args[3];  toyState.center.g = p.args[4];  toyState.center.b = p.args[5];
        toyState.left.r   = p.args[9];  toyState.left.g   = p.args[10]; toyState.left.b   = p.args[11];
        toyState.right.r  = p.args[15]; toyState.right.g  = p.args[16]; toyState.right.b  = p.args[17];
      }
      enqueueToyReply(p.counter, nullptr, 0);
      break;
    case TOY_CMD_C8:
      // Set all 3 pad colors at once. Order: left, center, right.
      // Each zone block is 4 bytes: [enable, r, g, b]
      if (p.argsLen >= 12) {
        toyState.center.enable = (p.args[0] != 0);
        toyState.center.r      = p.args[1];
        toyState.center.g      = p.args[2];
        toyState.center.b      = p.args[3];

        toyState.left.enable   = (p.args[4] != 0);
        toyState.left.r        = p.args[5];
        toyState.left.g        = p.args[6];
        toyState.left.b        = p.args[7];

        toyState.right.enable  = (p.args[8] != 0);
        toyState.right.r       = p.args[9];
        toyState.right.g       = p.args[10];
        toyState.right.b       = p.args[11];

        sendLedZoneUart(0, p.args[1], p.args[2],  p.args[3]);   // center
        sendLedZoneUart(1, p.args[5], p.args[6],  p.args[7]);   // left
        sendLedZoneUart(2, p.args[9], p.args[10], p.args[11]);  // right
        debugPrintf("c8 C=%02x%02x%02x L=%02x%02x%02x R=%02x%02x%02x",
          p.args[1],p.args[2],p.args[3], p.args[5],p.args[6],p.args[7],
          p.args[9],p.args[10],p.args[11]);
      }
      enqueueToyReply(p.counter, nullptr, 0);
      break;
    case TOY_CMD_C2:
      if (p.argsLen >= 6) {
        const uint8_t pad = p.args[0];
        const uint8_t pulseTime = p.args[1];
        const uint8_t pulseCount = p.args[2];
        const uint8_t r = p.args[3];
        const uint8_t g = p.args[4];
        const uint8_t b = p.args[5];
        applyPadColor(pad, r, g, b);
        debugPrintf("c2 pad=%u pulse=%u cnt=%u", pad, pulseTime, pulseCount);
      }
      enqueueToyReply(p.counter, nullptr, 0);
      break;
    case TOY_CMD_C3:
      if (p.argsLen >= 7) {
        const uint8_t pad = p.args[0];
        const uint8_t onLen = p.args[1];
        const uint8_t offLen = p.args[2];
        const uint8_t pulseCount = p.args[3];
        const uint8_t r = p.args[4];
        const uint8_t g = p.args[5];
        const uint8_t b = p.args[6];
        applyPadColor(pad, r, g, b);
        debugPrintf("c3 pad=%u on=%u off=%u cnt=%u", pad, onLen, offLen, pulseCount);
      }
      enqueueToyReply(p.counter, nullptr, 0);
      break;
    case TOY_CMD_D2: {
      // Block read: game requests 4 pages (16 bytes) of NFC figure data.
      // Reply format: [0x00, 16 bytes data] = 17-byte payload → packet[1]=0x12
      if (p.argsLen >= 2) {
        const uint8_t figIndex = p.args[0];  // 1-based, matches toyZone
        const uint8_t page    = p.args[1];
        uint8_t pageData[16] = {0};
        if (figIndex >= 1 && figIndex <= 7) {
          buildSyntheticFigurePage16(figIndex, page, pageData);
        }
        uint8_t reply[17];
        reply[0] = 0x00;
        memcpy(&reply[1], pageData, 16);
        enqueueToyReply(p.counter, reply, 17);
        debugPrintf("d2 idx=%u pg=%u", figIndex, page);
      } else {
        uint8_t reply[17] = {0};
        enqueueToyReply(p.counter, reply, 17);
      }
      break;
    }
    case TOY_CMD_D3: {
      // Block write: game writes 4 bytes to a page. Acknowledge with [0x00].
      // Reply format: [0x00] = 1-byte payload → packet[1]=0x02
      // Cache the write so D2 reads return consistent data for the session.
      const uint8_t reply[1] = {0x00};
      enqueueToyReply(p.counter, reply, 1);
      if (p.argsLen >= 6) {
        const uint8_t figIndex = p.args[0];
        const uint8_t page     = p.args[1];
        if (figIndex >= 1 && figIndex <= 7 &&
            page >= kPageCacheMin && page <= kPageCacheMax) {
          PageCacheEntry& e = sPageCache[figIndex - 1][page - kPageCacheMin];
          e.valid = true;
          memcpy(e.data, &p.args[2], 4);
          debugPrintf("d3 idx=%u pg=%u", figIndex, page);
          // Forward to physical NFC tag if this slot has a physical toy.
          if (toyState.slotIsPhysical[figIndex - 1]) {
            uint8_t fwd[6] = {
              figIndex, page,
              p.args[2], p.args[3], p.args[4], p.args[5]
            };
            sendFrameUart(LP_MSG_NFC_WRITE, fwd, sizeof(fwd));
          }
        } else {
          debugPrintf("d3 idx=%u pg=%u (skip)", p.args[0], p.args[1]);
        }
      } else {
        debugPrintf("d3 write short");
      }
      break;
    }
    case TOY_CMD_D4: {
      // Model query: game sends encrypted [figIndex, 0,0,0, conf_BE(4)],
      // we reply with encrypted [toyId_LE(4), conf_BE(4)] using COMMAND_KEY.
      if (p.argsLen >= 8) {
        uint8_t dec[8];
        teaDecrypt(p.args, dec);
        const uint8_t figIndex = dec[0];  // 1-based, matches toyZone
        const uint32_t conf = ((uint32_t)dec[4]<<24)|((uint32_t)dec[5]<<16)|
                              ((uint32_t)dec[6]<<8)|(uint32_t)dec[7];
        uint32_t toyId = 0;
        if (figIndex >= 1 && figIndex <= 7 && toyState.slotUidValid[figIndex - 1]) {
          toyId = toyState.slotToyId[figIndex - 1];
        }
        uint8_t toEnc[8];
        toEnc[0]=(uint8_t)toyId;       toEnc[1]=(uint8_t)(toyId>>8);
        toEnc[2]=(uint8_t)(toyId>>16); toEnc[3]=(uint8_t)(toyId>>24);
        toEnc[4]=(uint8_t)(conf>>24);  toEnc[5]=(uint8_t)(conf>>16);
        toEnc[6]=(uint8_t)(conf>>8);   toEnc[7]=(uint8_t)conf;
        uint8_t enc[8];
        teaEncrypt(toEnc, enc);
        uint8_t reply[9];
        reply[0] = 0x00;
        memcpy(&reply[1], enc, 8);
        enqueueToyReply(p.counter, reply, 9);
        debugPrintf("d4 idx=%u id=%lx", figIndex, (unsigned long)toyId);
      } else {
        enqueueToyReply(p.counter, nullptr, 0);
      }
      break;
    }
    default:
      debugPrintf("unhandled cmd=0x%02x", p.command);
      enqueueToyReply(p.counter, nullptr, 0);
      break;
  }
}

static bool sendFrameUart(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                          uint8_t forceSeq) {
  uint8_t wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen = 0;
  const uint8_t seq = (forceSeq == 0xff) ? seqCounter++ : forceSeq;

  if (!lp_encode_frame(type, seq, payload, payloadLen, wire, sizeof(wire), &wireLen)) {
    return false;
  }

  Serial1.write(wire, wireLen);
  return true;
}

static void sendAck(uint8_t receivedSeq) {
  const uint8_t payload[1] = {receivedSeq};
  sendFrameUart(LP_MSG_ACK, payload, sizeof(payload));
}

static void debugPrintf(const char* fmt, ...) {
  char buf[LP_MAX_PAYLOAD];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  const uint8_t len = (uint8_t)strnlen(buf, sizeof(buf));
  sendFrameUart(LP_MSG_DEBUG, (const uint8_t*)buf, len, 0);
}

static void handleFrame(const lp_frame_t& frame) {
  // LP_MSG_OTA_BEGIN requires conditional ACK (only on Updater.begin() success);
  // handle it before the generic ACK that covers all other frame types.
  if (frame.header.type == LP_MSG_OTA_BEGIN) {
    if (frame.header.length >= 4) {
      const uint32_t size = (uint32_t)frame.payload[0]
                          | ((uint32_t)frame.payload[1] << 8)
                          | ((uint32_t)frame.payload[2] << 16)
                          | ((uint32_t)frame.payload[3] << 24);
      if (Update.begin(size)) {
        sDfuActive     = true;
        sDfuSize       = size;
        sDfuReceived   = 0;
        sDfuRxIdx      = 0;
        sDfuRxExpected = 0;
        sDfuLastRxMs   = millis();
        debugPrintf("dfu begin sz=%u", size);
        sendAck(frame.header.seq);
      } else {
        debugPrintf("dfu begin fail: err %d", (int)Update.getError());
        // No ACK — console-esp32 will time out and report failure.
      }
    }
    return;
  }

  if (frame.header.type != LP_MSG_ACK) {
    sendAck(frame.header.seq);
  }

  switch (frame.header.type) {
    case LP_MSG_HELLO:
      debugPrintf("link hello");
      break;
    case LP_MSG_TAG_SET:
      if (frame.header.length >= 6) {
        const uint8_t slot   = frame.payload[0];  // global slot (1-7)
        const uint8_t zone   = frame.payload[1];  // LP zone (0=center,1=left,2=right)
        const uint32_t toyId = (uint32_t)frame.payload[2] |
                               ((uint32_t)frame.payload[3] << 8) |
                               ((uint32_t)frame.payload[4] << 16) |
                               ((uint32_t)frame.payload[5] << 24);
        // Byte 6 (optional, 7-byte payload): toy type: 0=unknown, 1=character, 2=vehicle.
        // Only update slotIsVehicle when type is explicitly specified (non-zero).
        const uint8_t typeTag = (frame.header.length >= 7) ? frame.payload[6] : 0;
        const uint8_t pad = lpZoneToToyZone(zone);  // 1=center,2=left,3=right
        uint8_t uid[kToyUidSize];
        buildToyUidFromToyId(toyId, slot, uid);

        if (slot >= 1 && slot <= 7) {
          const bool occupied  = toyState.slotUidValid[slot - 1];
          const bool sameId    = occupied && toyState.slotToyId[slot - 1] == toyId;
          const uint8_t oldZone = toyState.slotZone[slot - 1];  // capture before update
          const bool zoneMoved  = sameId && (oldZone != zone);

          toyState.slotToyId[slot - 1]  = toyId;
          toyState.slotZone[slot - 1]   = zone;
          memcpy(toyState.slotUid[slot - 1], uid, kToyUidSize);
          toyState.slotUidValid[slot - 1] = true;
          // Update vehicle flag only when explicitly specified (avoids stale
          // 6-byte re-syncs from old firmware overwriting the flag).
          if (typeTag == 2) toyState.slotIsVehicle[slot - 1] = true;
          else if (typeTag == 1) toyState.slotIsVehicle[slot - 1] = false;
          // Byte 7 (optional, 8-byte payload): isPhysical flag from console-esp32.
          if (frame.header.length >= 8) {
            toyState.slotIsPhysical[slot - 1] = (frame.payload[7] != 0);
          }

          if (zoneMoved) {
            // Toy moved between zones: emit REMOVE(oldZone) + PLACE(newZone) with
            // the SAME figIndex (slot) so the game sees a move, not a new toy.
            const uint8_t oldPad = lpZoneToToyZone(oldZone);
            enqueueToyTagEvent(oldPad, slot, kToyTagActionRemoved, uid);
            enqueueToyTagEvent(pad, slot, kToyTagActionPlaced, uid);
            debugPrintf("TAG_SET MOVE slot=%u z%u->z%u", slot, oldZone, zone);
          } else if (!sameId) {
            enqueueToyTagEvent(pad, slot, kToyTagActionPlaced, uid);
            debugPrintf("TAG_SET slot=%u zone=%u id=%lx veh=%u", slot, zone, (unsigned long)toyId, toyState.slotIsVehicle[slot-1]);
          }
          // else: same id + same zone (periodic re-sync) -> no-op
        }
      }
      break;
    case LP_MSG_TAG_CLEAR:
      if (frame.header.length >= 1) {
        const uint8_t slot  = frame.payload[0];  // global slot (1-7)
        if (slot >= 1 && slot <= 7) {
          if (toyState.slotUidValid[slot - 1]) {
            const uint8_t zone = toyState.slotZone[slot - 1];  // use tracked zone
            const uint8_t pad  = lpZoneToToyZone(zone);
            uint8_t uid[kToyUidSize] = {0};
            memcpy(uid, toyState.slotUid[slot - 1], kToyUidSize);
            toyState.slotUidValid[slot - 1] = false;
            toyState.slotIsVehicle[slot - 1] = false;
            toyState.slotIsPhysical[slot - 1] = false;
            memset(sPageCache[slot - 1], 0, sizeof(sPageCache[slot - 1]));
            enqueueToyTagEvent(pad, slot, kToyTagActionRemoved, uid);
            debugPrintf("TAG_CLEAR slot=%u", slot);
          }
          // else: already clear — no USB event needed (periodic re-sync dedup)
        }
      } else {
        // Broadcast clear: remove all occupied slots
        for (uint8_t s = 1; s <= 7; ++s) {
          if (!toyState.slotUidValid[s - 1]) continue;
          uint8_t uid[kToyUidSize] = {0};
          memcpy(uid, toyState.slotUid[s - 1], kToyUidSize);
          toyState.slotUidValid[s - 1] = false;
          toyState.slotIsVehicle[s - 1] = false;
          toyState.slotIsPhysical[s - 1] = false;
          memset(sPageCache[s - 1], 0, sizeof(sPageCache[s - 1]));
          const uint8_t z = toyState.slotZone[s - 1];  // use tracked zone
          enqueueToyTagEvent(lpZoneToToyZone(z), s, kToyTagActionRemoved, uid);
        }
        debugPrintf("TAG_CLEAR all");
      }
      break;
    case LP_MSG_LED_CMD:
      if (frame.header.length == kToyPacketSize) {
        ToyPadOutPacket packet = {};
        if (parseToyPacket(frame.payload, frame.header.length, &packet)) {
          handleToyCommand(packet);
        } else {
          debugPrintf("rejected LED_CMD: bad pkt");
        }
      } else {
        debugPrintf("LED_CMD: bad payload len");
      }
      break;
    case LP_MSG_ACK:
      break;
    case LP_MSG_OTA_COMMIT:
      // ACK already sent above; give it time to transmit before reboot.
      debugPrintf("dfu commit rcv=%u sz=%u", sDfuReceived, sDfuSize);
      delay(100);
      if (Update.end()) {
        debugPrintf("dfu ok, rebooting");
        delay(200);
        rp2040.restart();
      } else {
        debugPrintf("dfu end fail: err %d", (int)Update.getError());
      }
      break;
    default:
      debugPrintf("unhandled type=0x%02x", frame.header.type);
      break;
  }
}

// Called by TinyUSB when the host sets the device configuration (mount).
extern "C" void tud_mount_cb(void) {
  b0SentCount = 0;
  b0ReceivedCount = 0;
  b0QueuedCount = 0;
  debugPrintf("usb mounted");
}

void setup() {
  Serial1.begin(kUartBaud);  // UART from console ESP32
  lp_stream_init(&uartParser);

  debugPrintf("boot profile=%s", isXboxProfile() ? "xbox360" : "ps");
  debugPrintf("usb vid=0x%04x pid=0x%04x", kUsbVid, kUsbPid);

}

// Called by TinyUSB from USB IRQ context (usb_task_irq → tud_task) immediately
// after any HID IN transfer completes.  ep_in is guaranteed free (busy=0,
// claimed=0) when we arrive here.  We use this to chain the next queued TX
// packet immediately, so that RPCS3's pending IN URB picks up the B0 reply on
// the very next 1ms host poll — even if ep_in was busy with a keepalive when
// the B0 reply was first queued and serviceToyInQueue() was unable to send it.
extern "C" void tud_hid_report_complete_cb(uint8_t instance,
                                           uint8_t const* report,
                                           uint16_t len) {
  (void)instance;
  (void)report;
  (void)len;
  if (toyInQueue.count == 0) return;
  if (!TinyUSBDevice.mounted()) return;
  uint8_t packet[kToyUsbReportSize];
  if (!peekToyInPacket(packet)) return;
  const bool ok = usb_hid.sendReport(0, packet, kToyUsbReportSize);
  if (ok) {
    if (packet[0] == kToyMagicHostToPortal &&
        packet[1] == (uint8_t)(1 + sizeof(kToyB0ReplyPayload))) {
      b0SentCount++;
    }
    popToyInPacket(packet);
  }
}

void loop() {
  const uint32_t now = millis();

  // ── DFU raw-chunk receiver ────────────────────────────────────────────────────
  // Active only during a firmware update; skips LP processing for this loop() tick.
  if (sDfuActive) {
    if ((now - sDfuLastRxMs) > kDfuTimeoutMs) {
      debugPrintf("dfu timeout");
      Update.end();  // discard staged update
      sDfuActive = false;
      sDfuRxIdx = 0;
      sDfuRxExpected = 0;
    } else {
      while (Serial1.available()) {
        sDfuLastRxMs = now;
        sDfuRxBuf[sDfuRxIdx++] = (uint8_t)Serial1.read();
        // Stage 1: accumulate 8-byte chunk header
        if (sDfuRxExpected == 0 && sDfuRxIdx >= 8) {
          if (sDfuRxBuf[0] != 0xDF || sDfuRxBuf[1] != 0xC0) {
            sDfuRxIdx = 0;
            Serial1.write(kDfuNakByte);
          } else {
            const uint16_t clen = (uint16_t)sDfuRxBuf[6] | ((uint16_t)sDfuRxBuf[7] << 8);
            if (clen == 0 || clen > kDfuChunkMax) {
              sDfuRxIdx = 0;
              Serial1.write(kDfuNakByte);
            } else {
              sDfuRxExpected = 8 + clen + 2;
            }
          }
        }
        // Stage 2: process complete chunk (header + data + crc)
        if (sDfuRxExpected > 0 && sDfuRxIdx >= sDfuRxExpected) {
          const uint16_t clen    = (uint16_t)sDfuRxBuf[6] | ((uint16_t)sDfuRxBuf[7] << 8);
          const uint8_t* data    = &sDfuRxBuf[8];
          const uint16_t recvCrc = (uint16_t)sDfuRxBuf[8 + clen]
                                 | ((uint16_t)sDfuRxBuf[9 + clen] << 8);
          if (lp_crc16_ccitt(data, clen) == recvCrc) {
            const size_t written = Update.write(const_cast<uint8_t*>(data), clen);
            if (written == clen) {
              sDfuReceived += clen;
              Serial1.write(kDfuAckByte);
              // When all expected bytes are received, exit DFU mode so that
              // the subsequent LP_MSG_OTA_COMMIT frame is parsed normally.
              if (sDfuReceived >= sDfuSize) {
                sDfuActive = false;
                sDfuRxIdx = 0;
                sDfuRxExpected = 0;
                debugPrintf("dfu: all bytes rcv");
              }
            } else {
              debugPrintf("dfu write fail");
              Update.end();  // discard staged update
              sDfuActive = false;
              sDfuRxIdx = 0;
              sDfuRxExpected = 0;
              Serial1.write(kDfuNakByte);
            }
          } else {
            debugPrintf("dfu crc fail @%u", sDfuReceived);
            Serial1.write(kDfuNakByte);
          }
          sDfuRxIdx = 0;
          sDfuRxExpected = 0;
        }
      }
    }
    TinyUSBDevice.task();
    serviceToyInQueue();
    return;  // skip LP heartbeats while DFU is in progress
  }

  while (Serial1.available()) {
    const int c = Serial1.read();
    if (c < 0) {
      break;
    }

    lp_frame_t frame;
    const lp_parse_result_t res =
        lp_stream_push(&uartParser, (uint8_t)c, &frame);

    if (res == LP_PARSE_FRAME_BAD_HEADER) {
      debugPrintf("dropped bad header");
      continue;
    }
    if (res == LP_PARSE_FRAME_BAD_CRC) {
      debugPrintf("dropped bad crc");
      continue;
    }
    if (res == LP_PARSE_FRAME_OK) {
      handleFrame(frame);
    }
  }

  if ((now - lastHelloMs) >= kHelloMs) {
    lastHelloMs = now;
    uint8_t helloPayload[LP_MAX_PAYLOAD];
    helloPayload[0] = 0x02;
    const size_t rawVerLen = strlen(FIRMWARE_VERSION);
    const uint8_t verLen = (uint8_t)(rawVerLen < LP_MAX_PAYLOAD - 1 ? rawVerLen : LP_MAX_PAYLOAD - 1);
    memcpy(&helloPayload[1], FIRMWARE_VERSION, verLen);
    sendFrameUart(LP_MSG_HELLO, helloPayload, 1 + verLen);
  }

  // Drive USB task explicitly so pending events (callbacks, completions)
  // are processed before we attempt to drain the TX queue.
  TinyUSBDevice.task();
  serviceToyInQueue();
  // Second task() call processes any completions (e.g., clears ep_in busy).
  // Then retry the queue immediately in the same loop iteration so a packet
  // blocked by a momentarily-busy endpoint is sent without waiting a full
  // loop() cycle.
  TinyUSBDevice.task();
  serviceToyInQueue();

  static uint32_t lastB0StatsLogMs = 0;
  if ((now - lastB0StatsLogMs) >= 3000) {
    lastB0StatsLogMs = now;
    debugPrintf("b0 stats rx=%lu q=%lu tx=%lu", (unsigned long)b0ReceivedCount,
                (unsigned long)b0QueuedCount, (unsigned long)b0SentCount);
  }

  // Periodically re-broadcast full LED state to console-esp32 so it recovers
  // the correct zone colours after a restart without waiting for the next game
  // LED command.
  static uint32_t lastLedSyncMs = 0;
  if (toyState.initialized && (now - lastLedSyncMs) >= kStateSyncMs) {
    lastLedSyncMs = now;
    const uint8_t ledFrames[3][4] = {
      {0, toyState.center.r, toyState.center.g, toyState.center.b},
      {1, toyState.left.r,   toyState.left.g,   toyState.left.b},
      {2, toyState.right.r,  toyState.right.g,  toyState.right.b},
    };
    for (uint8_t z = 0; z < 3; z++) {
      sendFrameUart(LP_MSG_LED_CMD, ledFrames[z], 4);
    }
  }
}
