#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>

#include "link_protocol.h"

static const char* kSetupApSsid = "ToyPad-Setup";
static const char* kSetupApPass = "toypadsetup";
static const uint16_t kSetupPort = 80;
static const uint16_t kDnsPort = 53;
static const int kResetPin = 0;
static const uint32_t kResetHoldMs = 3000;
static const uint16_t kConsolePort = 25100;
static const uint32_t kRetryMs = 50;
static const uint8_t kMaxRetries = 3;
static const uint32_t kHelloMs = 3000;
static const uint32_t kVirtualTagMs = 5000;

WiFiUDP udp;
WebServer web(kSetupPort);
DNSServer dns;
Preferences prefs;
uint8_t seqCounter = 0;
IPAddress consoleIp(192, 168, 4, 1);

struct PadConfig {
  String ssid;
  String pass;
  uint32_t sharedSecret;
  bool valid;
};

PadConfig cfg;

struct PendingTx {
  bool active;
  uint8_t wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen;
  uint8_t seq;
  uint8_t retries;
  uint32_t lastSentMs;
};

PendingTx pending = {0};
uint32_t lastHelloMs = 0;
uint32_t lastVirtualTagMs = 0;

static void clearConfig() {
  prefs.begin("toypad", false);
  prefs.clear();
  prefs.end();
}

static PadConfig loadConfig() {
  PadConfig out;
  out.valid = false;

  prefs.begin("toypad", true);
  out.ssid = prefs.getString("ssid", "");
  out.pass = prefs.getString("pass", "");
  out.sharedSecret = prefs.getUInt("secret", 0);
  prefs.end();

  out.valid = out.ssid.length() > 0;
  return out;
}

static void saveWifiConfig(const String& ssid, const String& pass) {
  prefs.begin("toypad", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putUInt("secret", 0);
  prefs.end();
  cfg.sharedSecret = 0;
}

static void saveSharedSecret(uint32_t secret) {
  prefs.begin("toypad", false);
  prefs.putUInt("secret", secret);
  prefs.end();
  cfg.sharedSecret = secret;
}

static String htmlEscape(const String& input) {
  String out = input;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

static void handleRoot() {
  const int found = WiFi.scanNetworks();
  String options;
  for (int i = 0; i < found; i++) {
    options += "<option value=\"";
    options += htmlEscape(WiFi.SSID(i));
    options += "\">";
    options += htmlEscape(WiFi.SSID(i));
    options += " (";
    options += String(WiFi.RSSI(i));
    options += " dBm)</option>";
  }

  String page;
  page += "<!doctype html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>ToyPad Setup</title></head><body>";
  page += "<h2>ToyPad Setup</h2>";
  page += "<p>Select the console AP. First connection will auto-negotiate a shared secret.</p>";
  page += "<form method='POST' action='/save'>";
  page += "<label>Console SSID</label><br><select name='ssid'>";
  page += options;
  page += "</select><br><br>";
  page += "<label>Console Password (leave empty for open AP)</label><br><input name='pass' type='password'><br><br>";
  page += "<button type='submit'>Save and Reboot</button></form>";
  page += "</body></html>";

  web.send(200, "text/html", page);
}

static void handleSave() {
  const String ssid = web.arg("ssid");
  const String pass = web.arg("pass");

  if (ssid.length() == 0) {
    web.send(400, "text/plain", "invalid input");
    return;
  }

  saveWifiConfig(ssid, pass);
  web.send(200, "text/plain", "saved, rebooting");
  delay(500);
  ESP.restart();
}

static void redirectToPortalRoot() {
  web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  web.send(302, "text/plain", "");
}

static void runProvisioningPortal() {
  Serial.println("[pad-esp32] entering setup AP mode");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(kSetupApSsid, kSetupApPass);

  Serial.print("[pad-esp32] setup AP: ");
  Serial.println(kSetupApSsid);
  Serial.print("[pad-esp32] setup AP pass: ");
  Serial.println(kSetupApPass);
  Serial.print("[pad-esp32] setup URL: http://");
  Serial.println(WiFi.softAPIP());

  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(kDnsPort, "*", WiFi.softAPIP());

  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  // Common captive portal probe endpoints used by Android/iOS/Windows/macOS.
  web.on("/generate_204", HTTP_GET, redirectToPortalRoot);
  web.on("/hotspot-detect.html", HTTP_GET, redirectToPortalRoot);
  web.on("/connecttest.txt", HTTP_GET, redirectToPortalRoot);
  web.on("/ncsi.txt", HTTP_GET, redirectToPortalRoot);
  web.on("/fwlink", HTTP_GET, redirectToPortalRoot);
  web.onNotFound(redirectToPortalRoot);
  web.begin();

  while (true) {
    dns.processNextRequest();
    web.handleClient();
    delay(5);
  }
}

static bool connectToConsoleAp() {
  WiFi.mode(WIFI_STA);
  if (cfg.pass.length() == 0) {
    WiFi.begin(cfg.ssid.c_str());
  } else {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  }

  Serial.print("[pad-esp32] connecting to console AP ");
  Serial.println(cfg.ssid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
    if ((millis() - start) > 20000) {
      Serial.println();
      Serial.println("[pad-esp32] connect timeout");
      return false;
    }
  }

  Serial.println();
  Serial.print("[pad-esp32] station ip: ");
  Serial.println(WiFi.localIP());

  consoleIp = WiFi.gatewayIP();
  if (consoleIp == IPAddress((uint32_t)0)) {
    consoleIp = IPAddress(192, 168, 4, 1);
  }

  Serial.print("[pad-esp32] console ip: ");
  Serial.println(consoleIp);
  return true;
}

static void maybeFactoryResetOnBoot() {
  pinMode(kResetPin, INPUT_PULLUP);
  if (digitalRead(kResetPin) != LOW) {
    return;
  }

  const uint32_t start = millis();
  while (digitalRead(kResetPin) == LOW) {
    delay(20);
    if ((millis() - start) >= kResetHoldMs) {
      Serial.println("[pad-esp32] reset requested, clearing saved config");
      clearConfig();
      break;
    }
  }
}

static bool udpSendWire(const uint8_t* data, uint16_t len) {
  udp.beginPacket(consoleIp, kConsolePort);
  udp.write(data, len);
  return udp.endPacket() == 1;
}

static bool sendFrame(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                      bool trackAck) {
  uint8_t wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen = 0;
  const uint8_t seq = seqCounter++;

  if (!lp_encode_frame(type, seq, payload, payloadLen, wire, sizeof(wire), &wireLen)) {
    Serial.println("[pad-esp32] encode failed");
    return false;
  }

  if (!udpSendWire(wire, wireLen)) {
    Serial.println("[pad-esp32] udp send failed");
    return false;
  }

  if (trackAck) {
    pending.active = true;
    pending.wireLen = wireLen;
    pending.seq = seq;
    pending.retries = 0;
    pending.lastSentMs = millis();
    memcpy(pending.wire, wire, wireLen);
  }

  return true;
}

static void sendAck(uint8_t seq) {
  sendFrame(LP_MSG_ACK, &seq, 1, false);
}

static uint32_t readU32Le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void sendHeartbeat() {
  if (cfg.sharedSecret == 0) {
    const uint8_t payload[1] = {0xa1};
    sendFrame(LP_MSG_HELLO, payload, sizeof(payload), true);
    return;
  }

  uint8_t payload[5];
  payload[0] = 0xa1;
  payload[1] = (uint8_t)(cfg.sharedSecret & 0xff);
  payload[2] = (uint8_t)((cfg.sharedSecret >> 8) & 0xff);
  payload[3] = (uint8_t)((cfg.sharedSecret >> 16) & 0xff);
  payload[4] = (uint8_t)((cfg.sharedSecret >> 24) & 0xff);
  sendFrame(LP_MSG_HELLO, payload, sizeof(payload), true);
}

static void sendVirtualTagSet(uint8_t zone, uint32_t toyId) {
  uint8_t payload[5];
  payload[0] = zone;
  payload[1] = static_cast<uint8_t>(toyId & 0xff);
  payload[2] = static_cast<uint8_t>((toyId >> 8) & 0xff);
  payload[3] = static_cast<uint8_t>((toyId >> 16) & 0xff);
  payload[4] = static_cast<uint8_t>((toyId >> 24) & 0xff);

  sendFrame(LP_MSG_TAG_SET, payload, sizeof(payload), true);

  Serial.print("[pad-esp32] sent virtual TAG_SET zone=");
  Serial.print(zone);
  Serial.print(" toyId=");
  Serial.println(toyId);
}

void setup() {
  Serial.begin(115200);

  maybeFactoryResetOnBoot();
  cfg = loadConfig();
  if (!cfg.valid) {
    runProvisioningPortal();
  }

  if (!connectToConsoleAp()) {
    runProvisioningPortal();
  }

  udp.begin(25101);

  sendHeartbeat();
}

void loop() {
  const uint32_t now = millis();

  int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    uint8_t inbuf[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
    const int bytes = udp.read(inbuf, sizeof(inbuf));
    if (bytes > 0) {
      lp_frame_t frame;
      if (lp_decode_frame(inbuf, (uint16_t)bytes, &frame)) {
        if (frame.header.type == LP_MSG_ACK && frame.header.length == 1) {
          if (pending.active && frame.payload[0] == pending.seq) {
            pending.active = false;
            Serial.println("[pad-esp32] ack received");
          }
        } else if (frame.header.type == LP_MSG_PAIR_SET && frame.header.length == 4) {
          const uint32_t secret = readU32Le(frame.payload);
          saveSharedSecret(secret);
          sendAck(frame.header.seq);
          Serial.println("[pad-esp32] learned shared secret");
        } else {
          sendAck(frame.header.seq);
        }
      }
    }
  }

  if (pending.active && (now - pending.lastSentMs) >= kRetryMs) {
    if (pending.retries >= kMaxRetries) {
      pending.active = false;
      Serial.println("[pad-esp32] ack timeout");
    } else {
      pending.retries++;
      pending.lastSentMs = now;
      udpSendWire(pending.wire, pending.wireLen);
      Serial.println("[pad-esp32] retry pending frame");
    }
  }

  if (!pending.active && (now - lastHelloMs) >= kHelloMs) {
    lastHelloMs = now;
    sendHeartbeat();
  }

  if (!pending.active && (now - lastVirtualTagMs) >= kVirtualTagMs) {
    lastVirtualTagMs = now;
    sendVirtualTagSet(0, 0x12345678);
  }

  delay(5);
}
