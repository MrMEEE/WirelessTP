#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ctype.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>

#include <lwip/sockets.h>  // for direct ::send() — bypasses WiFiClient::_connected stale-errno
#include "link_protocol.h"
#include "fw_header.h"
#include "ld_catalog_data.h"

static const uint16_t kPadListenPort = 25100;
static const uint32_t kRp2040Baud = 115200;
static const uint32_t kHelloMs = 3000;
static const uint32_t kStateSyncMs = 1000;  // full-state broadcast interval
// If no LP frame arrives from the pad within this window, declare it disconnected.
// The pad sends pad-dbg every 1 s and HELLO heartbeat every 5 s, so 8 s gives ample
// margin while still recovering well within any human-perceptible timeout.
static const uint32_t kPadRxTimeoutMs = 8000;
static const uint16_t kDnsPort = 53;
static const uint16_t kHttpPort = 80;
static const uint8_t kSlotCount = 7;
static const uint8_t kLightZoneCount = 3;
static const uint8_t kToyboxLimit = 24;
static const uint32_t kDebugHeartbeatMs = 5000;
static const uint8_t kUsbCmdMaxLen = 96;
// Enable USB debug console to forward RP2040 debug messages (LP_MSG_DEBUG) to
// the ESP32 USB serial port so they can be read without a separate UART adapter.
static const bool kEnableUsbDebugConsole = true;

enum RuntimeMode : uint8_t {
  MODE_EMULATOR = 0,
  MODE_PASSTHROUGH = 1,
};

typedef struct {
  uint8_t r;
  uint8_t g;
  uint8_t b;
} LedZoneState;

typedef struct {
  bool occupied;
  uint32_t toyId;
  char label[24];
  uint8_t toyType;
  uint8_t zone;           // LP zone: 0=center, 1=left, 2=right
  bool isPhysical;        // true = came from pad-esp32 (physical NFC tag)
  bool fromToybox;
  uint8_t toyboxOriginIndex;
} SlotState;

typedef struct {
  bool inUse;
  uint32_t toyId;
  char label[24];
  uint8_t toyType;
} ToyboxEntry;

enum ToyType : uint8_t {
  TOY_UNKNOWN = 0,
  TOY_CHARACTER = 1,
  TOY_VEHICLE = 2,
};

WiFiServer tcpServer(kPadListenPort);
WiFiClient padClient;
lp_stream_parser_t tcpParser;
DNSServer dns;
WebServer web(kHttpPort);
Preferences prefs;
uint8_t seqCounter = 0;
lp_stream_parser_t uartParser;
IPAddress padIp;
bool     padKnown    = false;
bool     padPaired   = false;
uint32_t padLastRxMs = 0;  // millis() of last successfully-parsed LP frame from pad
uint32_t lastHelloMs = 0;
uint32_t lastStateSyncMs = 0;
uint32_t lastDebugHeartbeatMs = 0;
static char sPadD2Debug[LP_MAX_PAYLOAD + 1] = "";  // last NFC-variant diagnostic from pad

// NFC tag programmer state — set by /api/tag/program/begin, cleared on success/cancel.
static bool     sProgramModeActive    = false;
static uint32_t sProgramTargetId      = 0;
static uint8_t  sProgramTargetType    = TOY_UNKNOWN;
static char     sProgramTargetLabel[24] = {};
static uint8_t  sProgramStatus        = 0;   // 0=idle,1=waiting,2=success,3=fail
static bool     sProgramIntercepted   = false;
static uint8_t  sProgramInterceptedSlot = 0;
String apSsid;
uint32_t sharedSecret = 0;
RuntimeMode runtimeMode = MODE_EMULATOR;
bool ledStateKnown = false;  // true once RP2040 has sent at least one LED_CMD
uint32_t sLastLedCmdFromRp2040Ms = 0;  // millis() of last LED_CMD from RP2040; 0=never
LedZoneState lightZones[kLightZoneCount] = {};
SlotState padSlots[kSlotCount] = {};
ToyboxEntry toybox[kToyboxLimit] = {};
bool piConsoleBridgeMode = false;
char usbCmdBuf[kUsbCmdMaxLen] = {};
uint8_t usbCmdLen = 0;

// 3-state ANSI/VT escape filter for Pi console bridge output.
enum AnsiFilterState : uint8_t {
  ANSI_NORMAL    = 0,  // pass printable text through
  ANSI_AFTER_ESC = 1,  // saw 0x1b, waiting for type byte
  ANSI_IN_CSI    = 2,  // inside ESC [ ... sequence
  ANSI_IN_OSC    = 3,  // inside ESC ] ... sequence (terminated by BEL or ESC \)
};
AnsiFilterState piAnsiState = ANSI_NORMAL;

// ─── OTA / DFU ──────────────────────────────────────────────────────────────
static const char*    kOtaRp2040Path   = "/ota-rp2040.bin";
static const char*    kOtaPadPath      = "/ota-pad.bin";
static const uint16_t kDfuChunkSize    = 1024;
static const uint8_t  kDfuAckByte      = 0x06;
static const uint8_t  kDfuNakByte      = 0x15;
static const uint32_t kDfuChunkToutMs  = 5000;
static const uint8_t  kDfuMaxRetries   = 3;

static File sOtaFile;
static bool sOtaError = false;
static bool sOtaConsoleValidated = false;
static char sRp2040Version[LP_MAX_PAYLOAD + 1] = "?";
static char sPadVersion[LP_MAX_PAYLOAD + 1]    = "?";
// Non-const so the linker places this string in the .data segment,
// making it scannable in the binary image for OTA target validation.
static char kFwIdent[] = FW_IDENT_STR;

static const char* kModePrefKey = "mode";

static uint32_t readU32Le(const uint8_t* p);
static void writeU32Le(uint8_t* p, uint32_t v);
static bool sendFrameUart(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                          uint8_t forceSeq = 0xff);
static bool sendFrameTcp(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                         uint8_t forceSeq = 0xff);

static const char* toyTypeToString(uint8_t toyType);
static uint8_t toyTypeFromString(const String& s);
static uint8_t slotToLpZone(uint8_t slotNum);
static void sendManifestJson();
static void sendServiceWorkerJs();
static void sendAppIconSvg();
static void processUsbConsole();
static void processPiConsoleBridge();

static const char kPortalPage[] = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="theme-color" content="#101827">
  <meta name="mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <meta name="apple-mobile-web-app-title" content="Toy Pad Console">
  <link rel="manifest" href="/manifest.webmanifest">
  <link rel="icon" href="/icon.svg" type="image/svg+xml">
  <link rel="apple-touch-icon" href="/icon.svg">
  <title>Toy Pad Console</title>
  <style>
    :root { --bg:#111827; --panel:#1f2937; --muted:#93a0ba; --ink:#e5e7eb; --accent:#4f46e5; }
    * { box-sizing: border-box; }
    body { margin:0; background:linear-gradient(170deg,#101827,#1b2a40); color:var(--ink); font-family:"Trebuchet MS","Segoe UI",sans-serif; }
    .app { max-width:1160px; margin:0 auto; padding:14px; display:grid; gap:10px; }
    .card { background:rgba(25,34,52,.88); border:1px solid rgba(255,255,255,.14); border-radius:12px; padding:12px; }
    .top { display:flex; justify-content:space-between; align-items:center; gap:8px; flex-wrap:wrap; }
    .mode-pill { display:inline-flex; background:#202a3d; padding:4px; border-radius:999px; gap:6px; }
    .mode-pill button { border:0; background:transparent; color:var(--ink); border-radius:999px; padding:6px 10px; font-weight:700; }
    .mode-pill button.active { background:#f4b740; color:#222; }
    .install-btn { border:1px solid rgba(255,255,255,.25); background:#2f855a; color:#fff; border-radius:999px; padding:6px 12px; font-weight:700; display:none; }
    .install-btn.visible { display:inline-block; }
    .layout { display:grid; grid-template-columns:1fr; gap:10px; }
    .stack { display:grid; gap:10px; }
    .toggle-row { display:flex; gap:8px; flex-wrap:wrap; }
    .toggle-row button { border:1px solid rgba(255,255,255,.2); background:#23314c; color:var(--ink); border-radius:8px; padding:7px 10px; font-weight:700; }
    .toggle-row button.active { background:#3e6acc; border-color:#76a4ff; }
    .filter-row { display:grid; grid-template-columns:1fr 1fr; gap:8px; margin-top:8px; }
    .catalog-search { width:100%; margin-top:8px; background:#192438; color:var(--ink); border:1px solid rgba(255,255,255,.22); border-radius:8px; padding:8px; }
    .catalog-list { max-height:260px; overflow-y:auto; display:grid; gap:6px; margin-top:8px; }
    .catalog-item { display:grid; grid-template-columns:1fr auto; gap:8px; align-items:center; background:#162032; border:1px solid rgba(255,255,255,.12); border-radius:8px; padding:8px; font-size:.84rem; }
    .catalog-item small { color:var(--muted); display:block; }
    .catalog-item .actions { display:flex; gap:4px; }
    .catalog-item button { border:0; border-radius:7px; background:#4f7ce7; color:#fff; font-weight:700; padding:6px 8px; font-size:.78rem; }
    .catalog-item button:hover { background:#5a8ff5; }
    .token { border:0; border-radius:10px; min-height:66px; font-size:.76rem; font-weight:700; padding:12px 10px 10px; margin:2px; cursor:grab; }
    .token.character { background:linear-gradient(160deg,#7ed5ff,#4a85c0); color:#0c2538; }
    .token.vehicle { background:linear-gradient(160deg,#ffca75,#e2852c); color:#2d1808; }
    .token.selected { outline:3px solid #fff; }
    .token { position:relative; }
    .token-remove { position:absolute; top:2px; right:2px; width:16px; height:16px; border-radius:999px; border:0; background:rgba(123,29,24,.85); color:#fff; font-size:10px; line-height:16px; padding:0; font-weight:700; cursor:pointer; }
    .token-remove:hover { background:rgba(220,38,38,.95); }
    .touch-note { color:var(--muted); font-size:.82rem; margin-bottom:6px; }
    .status-row { display:flex; gap:8px; flex-wrap:wrap; }
    .badge { background:#273552; border:1px solid rgba(255,255,255,.2); border-radius:999px; padding:5px 8px; font-size:.84rem; }
    .pad-grid { display:grid; grid-template-columns:2fr 1fr 2fr; gap:12px; align-items:start; }
    .zone-col { display:flex; flex-direction:column; gap:8px; align-items:center; width:100%; }
    .zone-label { font-size:.72rem; font-weight:700; color:var(--muted); text-transform:uppercase; letter-spacing:.07em; padding:2px 0; }
    .slot { width:100%; max-width:130px; min-height:76px; border-radius:11px; background:#3b465e; border:1px solid rgba(255,255,255,.25); text-align:center; font-size:.76rem; line-height:1.15; padding:8px; display:flex; align-items:center; justify-content:center; position:relative; color:var(--muted); }
    .slot.filled { color:#ffffff; text-shadow:0 1px 2px rgba(0,0,0,.7); font-weight:700; }
    .slot.spacer { visibility:hidden; }
    .clear-slot { position:absolute; top:4px; right:4px; width:18px; height:18px; border-radius:999px; border:0; background:rgba(123,29,24,.85); color:#fff; font-size:11px; line-height:18px; padding:0; font-weight:700; }
    .modal-overlay { display:none; position:fixed; top:0; left:0; right:0; bottom:0; background:rgba(0,0,0,.7); z-index:1000; }
    .modal-overlay.active { display:flex; }
    .modal-content { background:var(--panel); border:1px solid rgba(255,255,255,.2); border-radius:16px; padding:24px; box-shadow:0 8px 32px rgba(0,0,0,.3); margin:auto; min-width:320px; }
    .modal-title { font-size:1.2rem; font-weight:700; margin-bottom:16px; color:var(--ink); }
    .slot-buttons { display:grid; grid-template-columns:repeat(auto-fit,minmax(60px,1fr)); gap:12px; }
    .slot-button { background:#3e6acc; border:2px solid rgba(255,255,255,.2); border-radius:10px; color:var(--ink); font-weight:700; padding:12px; font-size:1rem; cursor:pointer; }
    .slot-button:hover { background:#4f7ce7; border-color:#76a4ff; }
    .install-note { color:var(--muted); font-size:.82rem; margin-top:8px; }
    .prog-status { margin-top:12px; padding:10px; border-radius:8px; font-weight:700; text-align:center; display:none; }
    .prog-status.waiting { background:#1e3a5f; color:#93c5fd; display:block; }
    .prog-status.success { background:#14532d; color:#86efac; display:block; }
    .prog-status.fail    { background:#7f1d1d; color:#fca5a5; display:block; }
    @media (max-width:880px) { .layout { grid-template-columns:1fr; } }
  </style>
</head>
<body>
  <div class="app">
    <div class="card top">
      <div><b>Toy Pad Console Interface</b><div id="subtitle" style="color:var(--muted)">Loading...</div></div>
      <div style="display:flex; gap:8px; align-items:center; flex-wrap:wrap;">
        <button id="installBtn" class="install-btn">Install App</button>
        <div class="mode-pill" id="modePill"><button data-mode="emulator">Emulator</button><button data-mode="passthrough">Passthrough</button></div>
      </div>
    </div>
    <div class="card" id="installInfo" style="display:none;"><div class="install-note" id="installNote"></div></div>
    <div class="card"><div class="status-row" id="statusRow"></div><div id="d2dbg" style="font-size:.78rem;color:#94a3b8;margin-top:.4rem;font-family:monospace"></div></div>
    <div class="card"><h3 style="margin-top:0">The Toy Pad (7 slots)</h3><div class="pad-grid" id="padGrid"></div></div>
    <div class="layout">
      <div class="stack">
        <div class="card">
          <h3 style="margin-top:0">Toy Box</h3>
          <div class="touch-note" id="touchSelectionState">No toy selected.</div>
          <div class="toybox" id="toybox"></div>
        </div>
        <div class="card">
          <h3 style="margin-top:0">Catalog</h3>
          <div class="toggle-row"><button id="toggleCharacters" class="active">Characters</button><button id="toggleVehicles" class="active">Vehicles</button></div>
          <input id="catalogSearch" class="catalog-search" placeholder="Search by name or ID">
          <div class="filter-row">
            <input id="worldFilter" class="catalog-search" style="margin-top:0" placeholder="Filter by world">
            <input id="abilityFilter" class="catalog-search" style="margin-top:0" placeholder="Filter by ability">
          </div>
          <div class="catalog-list" id="catalogList"></div>
        </div>
      </div>
    </div>
  </div>

  <div class="modal-overlay" id="slotModal">
    <div class="modal-content">
      <div class="modal-title">Select a Slot</div>
      <div class="slot-buttons" id="slotButtons"></div>
    </div>
  </div>

  <div class="modal-overlay" id="progModal">
    <div class="modal-content">
      <div class="modal-title">&#9889; Flash NFC Tag</div>
      <p style="margin:0 0 4px">Vehicle: <b id="progVehicleName"></b></p>
      <p style="color:var(--muted);font-size:.86rem;margin:0 0 12px">Place a blank NTAG213 tag anywhere on the physical pad, then tap Flash.</p>
      <div id="progStatus" class="prog-status"></div>
      <div style="display:flex;gap:8px;margin-top:14px;">
        <button id="progFlashBtn" class="slot-button" style="flex:1;">Flash</button>
        <button id="progCancelBtn" class="slot-button" style="flex:1;background:#374151;border-color:#4b5563;">Cancel</button>
      </div>
    </div>
  </div>

  <script>
    // padRows removed — pad grid now renders by actual LP zone from /api/state
    const FAST_POLL_MS = 250;
    const SLOW_POLL_MS = 1200;
    let selectedToyboxIndex = null;
    let catalog = [];
    let filterCharacters = true;
    let filterVehicles = true;
    let catalogSearch = "";
    let catalogWorld = "";
    let catalogAbility = "";
    let deferredInstallPrompt = null;
    let refreshTimer = null;
    let refreshInFlight = false;
    let progPollTimer = null;

    function slotLabel(slot) {
      if (!slot.occupied) return "Empty";
      return `${slot.label || "Toy"}<br>0x${Number(slot.toyId).toString(16).toUpperCase()}`;
    }
    function slotZone(slotNum) {
      // LP zone 0=center, 1=left, 2=right
      // S2 = center; S1,S4,S5 = left; S3,S6,S7 = right
      if (slotNum === 2) return 0;
      if (slotNum === 1 || slotNum === 4 || slotNum === 5) return 1;
      return 2;
    }
    function rgbCss(o, a) {
      return `rgba(${Number(o.r)||0},${Number(o.g)||0},${Number(o.b)||0},${a})`;
    }
    // Game sends LED-calibrated PWM values for physical hardware.
    // Blue LEDs are ~11.6x more efficient per unit than red, green ~2.43x.
    // Scale channels so the physical white point (244,105,22) maps to (255,255,255).
    function ledCorrect(o) {
      return {
        r: Math.min(255, Math.round((o.r||0) * 1.05)),
        g: Math.min(255, Math.round((o.g||0) * 2.43)),
        b: Math.min(255, Math.round((o.b||0) * 11.6))
      };
    }
    function colorHex(c) { return (Number(c)&0xFF).toString(16).padStart(2,"0"); }
    function rgbHex(o) { return `#${colorHex(o.r)}${colorHex(o.g)}${colorHex(o.b)}`; }
    async function postJson(url, payload) {
      const r = await fetch(url, { method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(payload||{}) });
      if (!r.ok) throw new Error(await r.text());
      return r.text();
    }
    function isIos() {
      return /iphone|ipad|ipod/i.test(navigator.userAgent);
    }
    function isInStandalone() {
      return window.matchMedia("(display-mode: standalone)").matches || window.navigator.standalone === true;
    }
    function showInstallNote(text) {
      const host = document.getElementById("installInfo");
      const note = document.getElementById("installNote");
      note.textContent = text;
      host.style.display = "block";
    }
    function setupInstallUx() {
      const installBtn = document.getElementById("installBtn");

      // Service worker requires HTTPS or localhost. SoftAP IP is typically HTTP only.
      if ((location.protocol === "https:" || location.hostname === "localhost") && "serviceWorker" in navigator) {
        navigator.serviceWorker.register("/sw.js").catch(() => {});
      }

      window.addEventListener("beforeinstallprompt", (e) => {
        e.preventDefault();
        deferredInstallPrompt = e;
        installBtn.classList.add("visible");
      });

      installBtn.onclick = async () => {
        if (deferredInstallPrompt) {
          deferredInstallPrompt.prompt();
          await deferredInstallPrompt.userChoice;
          deferredInstallPrompt = null;
          installBtn.classList.remove("visible");
          return;
        }
        if (isIos()) {
          showInstallNote("On iPhone/iPad: Share -> Add to Home Screen.");
          return;
        }
        showInstallNote("On Android: browser menu -> Add to Home screen. Full install prompt may require HTTPS.");
      };

      if (isIos() && !isInStandalone()) {
        installBtn.classList.add("visible");
      }
    }
    async function loadCatalog() {
      if (catalog.length) return;
      const r = await fetch("/api/catalog", { cache:"force-cache" });
      catalog = await r.json();
      renderCatalog();
    }
    function passesCatalogFilter(item) {
      if (item.type === "character" && !filterCharacters) return false;
      if (item.type === "vehicle" && !filterVehicles) return false;
      const q = catalogSearch.toLowerCase();
      const worldQ = catalogWorld.toLowerCase();
      const abilityQ = catalogAbility.toLowerCase();
      const nameMatch = !q || item.name.toLowerCase().includes(q) || String(item.id).includes(q);
      const worldMatch = !worldQ || (item.world || "").toLowerCase().includes(worldQ);
      const abilityMatch = !abilityQ || (item.abilities || "").toLowerCase().includes(abilityQ);
      return nameMatch && worldMatch && abilityMatch;
    }
    function showSlotModal() {
      return new Promise((resolve) => {
        const modal = document.getElementById("slotModal");
        const buttonsContainer = document.getElementById("slotButtons");
        buttonsContainer.innerHTML = Array.from({length: 7}, (_, i) => i + 1).map(slot => 
          `<button class="slot-button" data-slot-select="${slot}">Slot ${slot}</button>`
        ).join("");
        modal.classList.add("active");
        buttonsContainer.querySelectorAll("button").forEach(btn => {
          btn.onclick = () => {
            modal.classList.remove("active");
            resolve(parseInt(btn.dataset.slotSelect));
          };
        });
        // Close on background click
        modal.onclick = (e) => {
          if (e.target === modal) {
            modal.classList.remove("active");
            resolve(null);
          }
        };
      });
    }
    function renderCatalog() {
      const list = document.getElementById("catalogList");
      const entries = catalog.filter(passesCatalogFilter);
      list.innerHTML = entries.map((item) => {
        const flashBtn = item.type === "vehicle"
          ? `<button data-flash-id="${item.id}" data-flash-name="${item.name.replace(/"/g,"&quot;")}" data-flash-type="${item.type}" title="Flash to NFC tag">&#9889;</button>`
          : "";
        return `<div class="catalog-item"><div>${item.name}<small>${item.type} | 0x${Number(item.id).toString(16).toUpperCase()} | ${item.world || "Unknown"}</small></div><div class="actions"><button data-add-id="${item.id}" data-add-name="${item.name.replace(/"/g,"&quot;")}" data-add-type="${item.type}" title="Add to Toy Box">&#128230;</button><button data-play-id="${item.id}" data-play-name="${item.name.replace(/"/g,"&quot;")}" data-play-type="${item.type}" title="Play (place on pad)">&#9654;</button>${flashBtn}</div></div>`;
      }).join("");
      list.querySelectorAll("button[data-add-id]").forEach((btn) => {
        btn.onclick = async () => {
          try {
            await postJson("/api/toybox/add", { id:Number(btn.dataset.addId), label:btn.dataset.addName, type:btn.dataset.addType });
            await refresh();
          } catch (e) {
            alert("Add failed: " + e.message);
          }
        };
      });
      list.querySelectorAll("button[data-play-id]").forEach((btn) => {
        btn.onclick = async () => {
          const slot = await showSlotModal();
          if (slot === null) return;
          try {
            await postJson("/api/slot/place", { slot: slot, id:Number(btn.dataset.playId), label:btn.dataset.playName, type:btn.dataset.playType });
            await refresh();
          } catch (e) {
            alert("Place failed: " + e.message);
          }
        };
      });
      list.querySelectorAll("button[data-flash-id]").forEach((btn) => {
        btn.onclick = () => openProgramModal(Number(btn.dataset.flashId), btn.dataset.flashName, btn.dataset.flashType);
      });
    }
    function showProgStatus(text, cls) {
      const el = document.getElementById("progStatus");
      el.textContent = text;
      el.className = "prog-status " + cls;
    }
    function closeProgramModal() {
      clearInterval(progPollTimer);
      progPollTimer = null;
      document.getElementById("progModal").classList.remove("active");
    }
    async function openProgramModal(id, name, type) {
      const modal = document.getElementById("progModal");
      const flashBtn = document.getElementById("progFlashBtn");
      const cancelBtn = document.getElementById("progCancelBtn");
      document.getElementById("progVehicleName").textContent = name;
      document.getElementById("progStatus").className = "prog-status";
      document.getElementById("progStatus").textContent = "";
      flashBtn.disabled = false;
      flashBtn.textContent = "Flash";
      flashBtn.style.display = "";
      cancelBtn.textContent = "Cancel";
      modal.classList.add("active");
      let started = false;
      cancelBtn.onclick = async () => {
        if (started) {
          await fetch("/api/tag/program/cancel", { method:"POST" }).catch(() => {});
        }
        closeProgramModal();
      };
      modal.onclick = (e) => { if (e.target === modal) closeProgramModal(); };
      flashBtn.onclick = async () => {
        flashBtn.disabled = true;
        flashBtn.textContent = "...";
        try {
          await postJson("/api/tag/program/begin", { id, label:name, type });
          started = true;
          showProgStatus("Waiting \u2014 place a blank tag on the pad\u2026", "waiting");
          progPollTimer = setInterval(async () => {
            try {
              const r = await fetch("/api/tag/program/status", { cache:"no-store" });
              const d = await r.json();
              if (d.status === "success") {
                clearInterval(progPollTimer); progPollTimer = null;
                showProgStatus("\u2713 Tag programmed! Remove it and re-place to use.", "success");
                flashBtn.style.display = "none";
                cancelBtn.textContent = "Close";
                refresh().catch(() => {});
              } else if (d.status === "fail") {
                clearInterval(progPollTimer); progPollTimer = null;
                showProgStatus("\u2717 Failed. Check pad connection and try again.", "fail");
                flashBtn.disabled = false;
                flashBtn.textContent = "Retry";
              }
            } catch(e) {}
          }, 500);
        } catch(e) {
          showProgStatus("\u2717 " + e.message, "fail");
          flashBtn.disabled = false;
          flashBtn.textContent = "Flash";
        }
      };
    }
    function installModeButtons() {
      document.querySelectorAll("#modePill button").forEach((btn) => {
        btn.onclick = async () => {
          try { await postJson("/api/mode", { mode: btn.dataset.mode }); await refresh(); }
          catch (e) { alert("Failed to set mode: " + e.message); }
        };
      });
    }
    function installCatalogControls() {
      const c = document.getElementById("toggleCharacters");
      const v = document.getElementById("toggleVehicles");
      const s = document.getElementById("catalogSearch");
      const w = document.getElementById("worldFilter");
      const a = document.getElementById("abilityFilter");
      c.onclick = () => { filterCharacters = !filterCharacters; c.classList.toggle("active", filterCharacters); renderCatalog(); };
      v.onclick = () => { filterVehicles = !filterVehicles; v.classList.toggle("active", filterVehicles); renderCatalog(); };
      s.oninput = () => { catalogSearch = s.value.trim(); renderCatalog(); };
      w.oninput = () => { catalogWorld = w.value.trim(); renderCatalog(); };
      a.oninput = () => { catalogAbility = a.value.trim(); renderCatalog(); };
    }
    function updateSelectionState(data) {
      const host = document.getElementById("touchSelectionState");
      const toy = data.toybox.find((t) => t.index === selectedToyboxIndex);
      host.textContent = toy ? `Selected: ${toy.label} (tap slot to place)` : "No toy selected.";
    }
    function installDragAndDrop(data) {
      const touchMode = ("ontouchstart" in window) || navigator.maxTouchPoints > 0;
      const tokens = document.querySelectorAll(".token");
      tokens.forEach((el) => {
        el.draggable = !touchMode;
        el.ondragstart = (e) => e.dataTransfer.setData("text/plain", el.dataset.index);
        el.onclick = () => {
          selectedToyboxIndex = Number(el.dataset.index);
          tokens.forEach((t) => t.classList.toggle("selected", Number(t.dataset.index) === selectedToyboxIndex));
          updateSelectionState(data);
        };
      });
      document.querySelectorAll(".slot.drop").forEach((el) => {
        el.ondragover = (e) => e.preventDefault();
        el.ondrop = async (e) => {
          e.preventDefault();
          const idx = Number(e.dataTransfer.getData("text/plain"));
          const toy = data.toybox.find((t) => t.index === idx);
          if (!toy) return;
          try {
            await postJson("/api/slot/place", { slot:Number(el.dataset.slot), id:toy.toyId, label:toy.label, type:toy.type, toyboxIndex:toy.index });
            await refresh();
          } catch (err) { alert("Place failed: " + err.message); }
        };
        el.onclick = async () => {
          if (selectedToyboxIndex === null) return;
          const toy = data.toybox.find((t) => t.index === selectedToyboxIndex);
          if (!toy) return;
          try {
            await postJson("/api/slot/place", { slot:Number(el.dataset.slot), id:toy.toyId, label:toy.label, type:toy.type, toyboxIndex:toy.index });
            selectedToyboxIndex = null;
            await refresh();
          } catch (err) { alert("Place failed: " + err.message); }
        };
        el.ondblclick = async () => {
          try { await postJson("/api/slot/clear", { slot:Number(el.dataset.slot) }); await refresh(); }
          catch (err) { alert("Clear failed: " + err.message); }
        };
      });
      document.querySelectorAll(".clear-slot").forEach((btn) => {
        btn.onclick = async (e) => {
          e.stopPropagation();
          try { await postJson("/api/slot/clear", { slot:Number(btn.dataset.clear) }); await refresh(); }
          catch (err) { alert("Clear failed: " + err.message); }
        };
      });
    }
    function render(data) {
      document.getElementById("subtitle").textContent = `SSID ${data.ssid} | ${data.mode.toUpperCase()} mode`;
      document.querySelectorAll("#modePill button").forEach((btn) => btn.classList.toggle("active", btn.dataset.mode === data.mode));
      document.getElementById("statusRow").innerHTML = [
        `<span class="badge">Pad ${data.paired ? "Paired" : "Waiting"}</span>`,
        `<span class="badge">Endpoint: ${data.paired ? data.padEndpoint : "-"}</span>`,
        `<span class="badge">Secret: ${data.hasSecret ? "yes" : "no"}</span>`,
        `<span class="badge ${data.gameActive ? "ok" : ""}">Game: ${data.gameActive ? "Active" : "Idle"}</span>`,
        `<span class="badge">Touch: tap toy then slot</span>`
      ].join("");
      document.getElementById("toybox").innerHTML = data.toybox.map((t) => `<button class="token ${t.type}" data-index="${t.index}"><span class="token-remove" data-remove-idx="${t.index}" title="Remove from toybox">×</span>${t.label}<br>0x${Number(t.toyId).toString(16).toUpperCase()}</button>`).join("") || `<div style="color:var(--muted)">Toy box is empty.</div>`;
      document.querySelectorAll(".token-remove").forEach((btn) => {
        btn.onclick = async (e) => {
          e.stopPropagation();
          try { await postJson("/api/toybox/remove", { index:Number(btn.dataset.removeIdx) }); await refresh(); }
          catch (err) { alert("Remove failed: " + err.message); }
        };
      });
      const renderPadSlot = (slotNum, posLabel) => {
        const s = data.slots[slotNum - 1];
        const zone = s.zone !== undefined ? s.zone : slotZone(slotNum);
        const light = ledCorrect(data.lights[zone] || { r: 0, g: 0, b: 0 });
        const slotStyle = `background:${rgbCss(light, 0.3)}; border-color:${rgbCss(light, 0.9)};`;
        const clear = s.occupied ? `<button class="clear-slot" data-clear="${slotNum}">x</button>` : "";
        const lbl = (typeof posLabel === 'string') ? posLabel : `S${slotNum}`;
        return `<div class="slot ${s.occupied ? "filled" : ""} drop" data-slot="${slotNum}" style="${slotStyle}">${lbl}<br>${slotLabel(s)}${clear}</div>`;
      };
      const renderZoneCol = (zoneIdx, zoneName) => {
        // Each column always shows a fixed number of rows (L:3, C:1, R:3).
        // Rows are filled by toys currently in this zone (s.zone === zoneIdx),
        // then padded with empty placeholders.  A toy that has moved to another
        // zone does NOT appear here — it appears only in its current zone's column.
        const canonical = [1,2,3,4,5,6,7].filter(n => slotZone(n) === zoneIdx);
        const capacity  = canonical.length;
        const pfx = ['C', 'L', 'R'][zoneIdx];

        // Slots physically present in this zone right now.
        const inZone = [];
        for (let n = 1; n <= 7; n++) {
          if (data.slots[n - 1].occupied && data.slots[n - 1].zone === zoneIdx) inZone.push(n);
        }

        // Free canonical slots (not occupied anywhere) — recycled as drop targets for empty rows.
        const freeCanonical = canonical.filter(n => !data.slots[n - 1].occupied);

        const rows = [];
        let freeIdx = 0;
        for (let pos = 0; pos < capacity; pos++) {
          const lbl = capacity > 1 ? `${pfx}${pos + 1}` : pfx;
          if (pos < inZone.length) {
            rows.push(renderPadSlot(inZone[pos], lbl));
          } else {
            const light = ledCorrect(data.lights[zoneIdx] || { r: 0, g: 0, b: 0 });
            const style = `background:${rgbCss(light, 0.3)}; border-color:${rgbCss(light, 0.9)};`;
            if (freeIdx < freeCanonical.length) {
              const slot = freeCanonical[freeIdx++];
              rows.push(`<div class="slot drop" data-slot="${slot}" style="${style}">${lbl}<br>&nbsp;</div>`);
            } else {
              rows.push(`<div class="slot" style="${style}">${lbl}<br>&nbsp;</div>`);
            }
          }
        }
        return `<div class="zone-col" data-zone="${zoneIdx}"><div class="zone-label">${zoneName}</div>${rows.join('')}</div>`;
      };
      // lpZone: 0=center, 1=left, 2=right
      document.getElementById("padGrid").innerHTML =
        renderZoneCol(1, "Left") + renderZoneCol(0, "Center") + renderZoneCol(2, "Right");
      updateSelectionState(data);
      installDragAndDrop(data);
    }
    async function refresh() {
      if (refreshInFlight) return;
      refreshInFlight = true;
      try {
        const r = await fetch("/api/state", { cache:"no-store" });
        const data = await r.json();
        render(data);
        const dbgEl = document.getElementById('d2dbg');
        if (dbgEl && data.d2dbg) dbgEl.textContent = 'NFC dbg: ' + data.d2dbg;
      } finally {
        refreshInFlight = false;
      }
    }
    function scheduleRefresh() {
      if (refreshTimer) {
        clearInterval(refreshTimer);
      }
      const period = document.hidden ? SLOW_POLL_MS : FAST_POLL_MS;
      refreshTimer = setInterval(() => {
        refresh().catch(() => {});
      }, period);
    }
    installModeButtons();
    installCatalogControls();
    setupInstallUx();
    document.addEventListener("visibilitychange", scheduleRefresh);
    loadCatalog().then(() => refresh()).catch(() => {});
    scheduleRefresh();
    async function refreshVersions() {
      try {
        const r = await fetch("/api/versions", { cache:"no-store" });
        const v = await r.json();
        const set = (id, val) => { const e = document.getElementById(id); if (e) e.textContent = val || "?"; };
        set("fv-console", v.console_esp32);
        set("fv-rp2040",  v.console_rp2040);
        set("fv-pad",     v.pad_esp32);
      } catch(_) {}
    }
    refreshVersions();
    setInterval(() => refreshVersions(), 30000);
    async function flashFirmware(formEl, statusId) {
      const el = document.getElementById(statusId);
      const fi = formEl.querySelector('input[type=file]');
      if (!fi.files.length) { el.style.color='var(--muted)'; el.textContent='Select a .bin file first'; return; }
      el.style.color='var(--muted)'; el.textContent='Uploading…';
      try {
        const r = await fetch(formEl.action, { method:'POST', body:new FormData(formEl) });
        const t = await r.text();
        el.style.color = r.ok ? '#4ade80' : '#f87171';
        el.textContent = t;
      } catch(e) {
        el.style.color='#f87171';
        el.textContent='Upload failed: '+e.message;
      }
    }
  </script>
  <div style="max-width:680px;margin:8px auto;padding:0 14px 16px">
    <div style="background:rgba(25,34,52,.88);border:1px solid rgba(255,255,255,.14);border-radius:12px;padding:12px 14px">
      <div style="font-size:.76rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;margin-bottom:8px">Firmware Versions</div>
      <div style="display:grid;grid-template-columns:repeat(3,1fr);gap:10px;text-align:center">
        <div><div style="font-size:.72rem;color:var(--muted)">console-esp32</div><div id="fv-console" style="font-family:monospace;font-size:.88rem;margin-top:2px">…</div></div>
        <div><div style="font-size:.72rem;color:var(--muted)">console-rp2040</div><div id="fv-rp2040" style="font-family:monospace;font-size:.88rem;margin-top:2px">…</div></div>
        <div><div style="font-size:.72rem;color:var(--muted)">pad-esp32</div><div id="fv-pad" style="font-family:monospace;font-size:.88rem;margin-top:2px">…</div></div>
      </div>
      <div style="margin-top:14px;border-top:1px solid rgba(255,255,255,.1);padding-top:12px;display:flex;flex-direction:column;gap:10px">
        <div style="font-size:.76rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em">OTA Update</div>
        <div>
          <div style="font-size:.78rem;color:var(--muted);margin-bottom:4px">RP2040 firmware (.bin)</div>
          <form action="/ota/upload-rp2040" enctype="multipart/form-data" onsubmit="event.preventDefault();flashFirmware(this,'st-rp2040')" style="display:flex;gap:6px;align-items:center">
            <input type="file" name="firmware" accept=".bin" style="flex:1;font-size:.8rem;color:var(--ink);background:#192438;border:1px solid rgba(255,255,255,.2);border-radius:7px;padding:5px 8px">
            <button type="submit" style="border:0;border-radius:8px;background:#3e6acc;color:#fff;font-weight:700;padding:6px 12px;font-size:.8rem;white-space:nowrap">Flash</button>
          </form>
          <div id="st-rp2040" style="font-size:.78rem;margin-top:4px;min-height:1em"></div>
        </div>
        <div>
          <div style="font-size:.78rem;color:var(--muted);margin-bottom:4px">Pad firmware (.bin)</div>
          <form action="/ota/upload-pad" enctype="multipart/form-data" onsubmit="event.preventDefault();flashFirmware(this,'st-pad')" style="display:flex;gap:6px;align-items:center">
            <input type="file" name="firmware" accept=".bin" style="flex:1;font-size:.8rem;color:var(--ink);background:#192438;border:1px solid rgba(255,255,255,.2);border-radius:7px;padding:5px 8px">
            <button type="submit" style="border:0;border-radius:8px;background:#3e6acc;color:#fff;font-weight:700;padding:6px 12px;font-size:.8rem;white-space:nowrap">Flash</button>
          </form>
          <div id="st-pad" style="font-size:.78rem;margin-top:4px;min-height:1em"></div>
        </div>
        <div>
          <div style="font-size:.78rem;color:var(--muted);margin-bottom:4px">Console firmware (.bin) — device will reboot</div>
          <form action="/ota/upload-console" enctype="multipart/form-data" onsubmit="event.preventDefault();flashFirmware(this,'st-console')" style="display:flex;gap:6px;align-items:center">
            <input type="file" name="firmware" accept=".bin" style="flex:1;font-size:.8rem;color:var(--ink);background:#192438;border:1px solid rgba(255,255,255,.2);border-radius:7px;padding:5px 8px">
            <button type="submit" style="border:0;border-radius:8px;background:#7c3aed;color:#fff;font-weight:700;padding:6px 12px;font-size:.8rem;white-space:nowrap">Flash</button>
          </form>
          <div id="st-console" style="font-size:.78rem;margin-top:4px;min-height:1em"></div>
        </div>
      </div>
    </div>
  </div>
</body>
</html>
)HTML";

  static const char kManifestJson[] =
    "{"
    "\"name\":\"Toy Pad Console\"," 
    "\"short_name\":\"ToyPad\"," 
    "\"start_url\":\"/\"," 
    "\"scope\":\"/\"," 
    "\"display\":\"standalone\"," 
    "\"background_color\":\"#101827\"," 
    "\"theme_color\":\"#101827\"," 
    "\"icons\":[{\"src\":\"/icon.svg\",\"sizes\":\"any\",\"type\":\"image/svg+xml\",\"purpose\":\"any maskable\"}]"
    "}";

  static const char kServiceWorkerJs[] =
    "const CACHE='toypad-console-v1';\n"
    "self.addEventListener('install',e=>{e.waitUntil(caches.open(CACHE).then(c=>c.addAll(['/','/manifest.webmanifest','/icon.svg'])));self.skipWaiting();});\n"
    "self.addEventListener('activate',e=>{e.waitUntil(self.clients.claim());});\n"
    "self.addEventListener('fetch',e=>{if(e.request.method!=='GET')return;e.respondWith(caches.match(e.request).then(r=>r||fetch(e.request).then(resp=>{const copy=resp.clone();caches.open(CACHE).then(c=>c.put(e.request,copy));return resp;}).catch(()=>r)));});\n";

  static const char kAppIconSvg[] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 192 192'>"
    "<defs><linearGradient id='g' x1='0' x2='1' y1='0' y2='1'><stop offset='0' stop-color='#1f2937'/><stop offset='1' stop-color='#0f172a'/></linearGradient></defs>"
    "<rect width='192' height='192' rx='38' fill='url(#g)'/>"
    "<circle cx='52' cy='98' r='24' fill='#60a5fa'/><circle cx='96' cy='98' r='24' fill='#34d399'/><circle cx='140' cy='98' r='24' fill='#f59e0b'/>"
    "<rect x='36' y='38' width='120' height='18' rx='9' fill='#f4b740'/>"
    "<text x='96' y='166' font-size='20' text-anchor='middle' fill='#e5e7eb' font-family='Trebuchet MS, Segoe UI, sans-serif'>Toy Pad</text>"
    "</svg>";

static void safeCopyLabel(char* out, size_t outLen, const String& value) {
  if (outLen == 0) {
    return;
  }
  size_t n = value.length();
  if (n >= outLen) {
    n = outLen - 1;
  }
  memcpy(out, value.c_str(), n);
  out[n] = '\0';
}

static const char* modeToString(RuntimeMode mode) {
  return (mode == MODE_PASSTHROUGH) ? "passthrough" : "emulator";
}

static RuntimeMode modeFromString(const String& s) {
  if (s == "passthrough") {
    return MODE_PASSTHROUGH;
  }
  return MODE_EMULATOR;
}

static const char* toyTypeToString(uint8_t toyType) {
  if (toyType == TOY_CHARACTER) {
    return "character";
  }
  if (toyType == TOY_VEHICLE) {
    return "vehicle";
  }
  return "unknown";
}

static uint8_t toyTypeFromString(const String& s) {
  if (s == "character") {
    return TOY_CHARACTER;
  }
  if (s == "vehicle") {
    return TOY_VEHICLE;
  }
  return TOY_UNKNOWN;
}

static bool parseJsonString(const String& body, const char* key, String& out) {
  const String needle = String("\"") + key + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }
  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }
  int valueStart = colonPos + 1;
  while (valueStart < (int)body.length() && isspace((unsigned char)body[valueStart])) {
    valueStart++;
  }
  // Only parse JSON strings when the value itself starts with a quote.
  if (valueStart >= (int)body.length() || body[valueStart] != '"') {
    return false;
  }
  int firstQuote = valueStart;
  if (firstQuote < 0) {
    return false;
  }
  int secondQuote = body.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) {
    return false;
  }
  out = body.substring(firstQuote + 1, secondQuote);
  return true;
}

static bool parseJsonUint(const String& body, const char* key, uint32_t* outValue) {
  const String needle = String("\"") + key + "\"";
  int keyPos = body.indexOf(needle);
  if (keyPos < 0) {
    return false;
  }
  int colonPos = body.indexOf(':', keyPos + needle.length());
  if (colonPos < 0) {
    return false;
  }

  int start = colonPos + 1;
  while (start < (int)body.length() && isspace((unsigned char)body[start])) {
    start++;
  }
  if (start >= (int)body.length()) {
    return false;
  }

  int end = start;
  while (end < (int)body.length() && body[end] != ',' && body[end] != '}' && body[end] != '"') {
    end++;
  }
  String token = body.substring(start, end);
  token.trim();
  if (token.length() == 0) {
    return false;
  }

  if (token.startsWith("0x") || token.startsWith("0X")) {
    *outValue = strtoul(token.c_str() + 2, NULL, 16);
  } else {
    *outValue = strtoul(token.c_str(), NULL, 10);
  }
  return true;
}

static bool parseJsonFlexibleId(const String& body, const char* key, uint32_t* outValue) {
  String token;
  if (parseJsonString(body, key, token)) {
    token.trim();
    if (token.startsWith("0x") || token.startsWith("0X")) {
      *outValue = strtoul(token.c_str() + 2, NULL, 16);
    } else {
      *outValue = strtoul(token.c_str(), NULL, 10);
    }
    return true;
  }
  return parseJsonUint(body, key, outValue);
}

static void clearSlotState(uint8_t slotIndex) {
  if (slotIndex >= kSlotCount) {
    return;
  }
  padSlots[slotIndex].occupied = false;
  padSlots[slotIndex].toyId = 0;
  padSlots[slotIndex].toyType = TOY_UNKNOWN;
  padSlots[slotIndex].label[0] = '\0';
  padSlots[slotIndex].zone = slotToLpZone(slotIndex + 1);
  padSlots[slotIndex].isPhysical = false;
  padSlots[slotIndex].fromToybox = false;
  padSlots[slotIndex].toyboxOriginIndex = 0xff;
}

static void setSlotState(uint8_t slotIndex, uint32_t toyId, const char* label, uint8_t toyType,
                        bool fromToybox = false, uint8_t toyboxOriginIndex = 0xff) {
  if (slotIndex >= kSlotCount) {
    return;
  }
  padSlots[slotIndex].occupied = true;
  padSlots[slotIndex].toyId = toyId;
  padSlots[slotIndex].toyType = toyType;
  padSlots[slotIndex].isPhysical = false;  // caller sets true if needed (observeFrameState)
  // zone is set separately (by observeFrameState for physical toys, or by
  // handleApiSlotPlace for virtual toys). Default to slot-derived value only
  // if never set (zone field was zeroed at init).
  padSlots[slotIndex].fromToybox = fromToybox;
  padSlots[slotIndex].toyboxOriginIndex = toyboxOriginIndex;
  if (label != NULL) {
    strncpy(padSlots[slotIndex].label, label, sizeof(padSlots[slotIndex].label) - 1);
    padSlots[slotIndex].label[sizeof(padSlots[slotIndex].label) - 1] = '\0';
  } else {
    snprintf(padSlots[slotIndex].label, sizeof(padSlots[slotIndex].label), "Toy %lu",
             (unsigned long)toyId);
  }
}

static void setLightZone(uint8_t zone, uint8_t r, uint8_t g, uint8_t b) {
  if (zone >= kLightZoneCount) {
    return;
  }
  lightZones[zone].r = r;
  lightZones[zone].g = g;
  lightZones[zone].b = b;
}

static void setAllLightZones(uint8_t r, uint8_t g, uint8_t b) {
  for (uint8_t i = 0; i < kLightZoneCount; i++) {
    setLightZone(i, r, g, b);
  }
}

// Search kLdCatalogJson for a toy by numeric ID; fills nameBuf and *typeOut.
static bool catalogLookupById(uint32_t toyId, char* nameBuf, size_t nameBufLen, uint8_t* typeOut) {
  const char* p = kLdCatalogJson;
  while (true) {
    const char* idTag = strstr(p, "\"id\":");
    if (!idTag) break;
    p = idTag + 5;
    uint32_t id = 0;
    const char* q = p;
    while (*q >= '0' && *q <= '9') { id = id * 10 + (uint32_t)(*q++ - '0'); }
    if (id == toyId && (*q < '0' || *q > '9')) {
      const char* nt = strstr(q, "\"name\":\"");
      if (!nt) break;
      const char* ns = nt + 8;
      size_t nl = 0;
      while (ns[nl] && ns[nl] != '"' && nl < nameBufLen - 1) { nameBuf[nl] = ns[nl]; nl++; }
      nameBuf[nl] = '\0';
      if (typeOut) {
        const char* tt = strstr(q, "\"type\":\"");
        *typeOut = (tt && strncmp(tt + 8, "vehicle", 7) == 0) ? TOY_VEHICLE : TOY_CHARACTER;
      }
      return true;
    }
    p = q;
  }
  return false;
}

// fromPad: true when the frame arrived via TCP from the pad-esp32.
//          false when it arrived via UART from the RP2040.
static void observeFrameState(const lp_frame_t& frame, bool fromPad) {
  if (frame.header.type == LP_MSG_LED_CMD && frame.header.length >= 4) {
    const uint8_t zone = frame.payload[0];
    const uint8_t r = frame.payload[1];
    const uint8_t g = frame.payload[2];
    const uint8_t b = frame.payload[3];
    ledStateKnown = true;  // RP2040 has provided LED state; safe to sync to pad
    if (zone == 0xff) {
      setAllLightZones(r, g, b);
    } else {
      setLightZone(zone, r, g, b);
    }
    return;
  }

  // TAG_SET from pad uses 6-byte payload: [slot, zone, toyId[4 LE]].
  // TAG_SET from RP2040 uses 5-byte payload: [slot, toyId[4 LE]] (no zone).
  // In emulator mode the console owns slot state; ignore tag events from the pad
  // so its periodic TAG_CLEAR syncs don't wipe virtual toys.
  if (frame.header.type == LP_MSG_TAG_SET) {
    const uint8_t minLen = fromPad ? 6 : 5;
    if (frame.header.length < minLen) return;
    const uint8_t slotNum = frame.payload[0];
    if (slotNum >= 1 && slotNum <= kSlotCount) {
      // NFC tag programmer: intercept physical tag placement to flash vehicle data.
      // Must run BEFORE the emulator early-return so it works in both modes.
      if (fromPad && sProgramModeActive) {
        // Send NFC page writes to the pad-esp32 (fire-and-forget D3).
        if (padKnown && padPaired) {
          uint8_t p36[6] = {slotNum, 36,
            (uint8_t)(sProgramTargetId & 0xFF), (uint8_t)(sProgramTargetId >> 8), 0, 0};
          uint8_t p38[6] = {slotNum, 38, 0x00, 0x01, 0x00, 0x00};
          sendFrameTcp(LP_MSG_NFC_WRITE, p36, sizeof(p36));
          sendFrameTcp(LP_MSG_NFC_WRITE, p38, sizeof(p38));
          sProgramStatus = 2;  // success — NFC_WRITE commands sent
        } else {
          sProgramStatus = 3;  // fail — pad not connected
        }
        setSlotState(slotNum - 1, sProgramTargetId, sProgramTargetLabel, sProgramTargetType);
        padSlots[slotNum - 1].zone = frame.payload[1];
        padSlots[slotNum - 1].isPhysical = true;
        sProgramModeActive = false;
        sProgramInterceptedSlot = slotNum;
        sProgramIntercepted = true;
        Serial.printf("[prog] flash slot=%u id=0x%04lx st=%u\n",
                      slotNum, (unsigned long)sProgramTargetId, sProgramStatus);
        return;
      }
      // In emulator mode the console owns slot state; ignore tag events from the pad
      // so its periodic TAG_CLEAR syncs don't wipe virtual toys.
      if (fromPad && runtimeMode == MODE_EMULATOR) return;
      // Pad: payload[1]=zone, payload[2..5]=toyId. UART: payload[1..4]=toyId.
      const uint8_t toyOff = fromPad ? 2 : 1;
      const uint32_t toyId = readU32Le(&frame.payload[toyOff]);
      char name[24] = {};
      uint8_t toyType = TOY_UNKNOWN;
      if (!catalogLookupById(toyId, name, sizeof(name), &toyType)) {
        snprintf(name, sizeof(name), "Toy %lu", (unsigned long)toyId);
      }
      setSlotState(slotNum - 1, toyId, name, toyType);
      // Save the actual zone reported by the pad so kStateSyncMs resync uses
      // the correct zone rather than the slot-derived fallback.
      if (fromPad) {
        padSlots[slotNum - 1].zone = frame.payload[1];
        padSlots[slotNum - 1].isPhysical = true;
      } else {
        padSlots[slotNum - 1].zone = slotToLpZone(slotNum);
        padSlots[slotNum - 1].isPhysical = false;
      }
    }
    return;
  }

  if (frame.header.type == LP_MSG_TAG_CLEAR && frame.header.length >= 1) {
    // In emulator mode, pad TAG_CLEARs (periodic sync) must not wipe virtual toys.
    if (fromPad && runtimeMode == MODE_EMULATOR) return;
    const uint8_t slotNum = frame.payload[0];
    if (slotNum >= 1 && slotNum <= kSlotCount) {
      padSlots[slotNum - 1].isPhysical = false;
      padSlots[slotNum - 1].zone = slotToLpZone(slotNum);  // reset to static zone
      clearSlotState(slotNum - 1);
    }
    return;
  }

  // LP_MSG_NFC_WRITE: RP2040 asks us to forward a D3 page-write to the physical
  // toypad so that vehicle tag data is written to the real NFC tag.
  if (frame.header.type == LP_MSG_NFC_WRITE && !fromPad &&
      frame.header.length >= 6) {
    const uint8_t slot = frame.payload[0];
    if (slot >= 1 && slot <= kSlotCount && padSlots[slot - 1].isPhysical &&
        padKnown && padPaired) {
      sendFrameTcp(LP_MSG_NFC_WRITE, frame.payload, frame.header.length);
    }
    return;
  }
}

// Maps web UI slot number (1-7) to LP zone (0=center, 1=left, 2=right).
// Matches the slotZone() function in the web UI JavaScript.
// S2 = center (LP 0); S1,S4,S5 = left pad (LP 1); S3,S6,S7 = right pad (LP 2).
static uint8_t slotToLpZone(uint8_t slotNum) {
  if (slotNum == 2) return 0;
  if (slotNum == 1 || slotNum == 4 || slotNum == 5) return 1;
  return 2;
}

static bool sendTagSet(uint8_t slot, uint32_t toyId, uint8_t toyType = TOY_UNKNOWN) {
  // RP2040 handler accepts 6 bytes [slot, zone, toyId[4 LE]] or 7 bytes
  // [slot, zone, toyId[4 LE], type] where type: 1=character, 2=vehicle, 0=unknown.
  // Use the stored zone for this slot (set by observeFrameState for physical
  // toys, or by handleApiSlotPlace for virtual toys) so the RP2040 always
  // sees a consistent zone and never emits a spurious REMOVE+PLACE move event.
  uint8_t payload[8];
  payload[0] = slot;
  payload[1] = (slot >= 1 && slot <= kSlotCount)
                   ? padSlots[slot - 1].zone
                   : slotToLpZone(slot);
  writeU32Le(&payload[2], toyId);
  payload[6] = toyType;  // TOY_UNKNOWN=0, TOY_CHARACTER=1, TOY_VEHICLE=2
  // Byte 7: isPhysical flag so the RP2040 knows to forward D3 writes back.
  payload[7] = (slot >= 1 && slot <= kSlotCount && padSlots[slot - 1].isPhysical) ? 1 : 0;
  bool ok = sendFrameUart(LP_MSG_TAG_SET, payload, sizeof(payload));
  if (padKnown && padPaired) {
    sendFrameTcp(LP_MSG_TAG_SET, payload, sizeof(payload));
  }
  return ok;
}

static bool sendTagClear(uint8_t slot) {
  const uint8_t payload[1] = {slot};  // slot (1-7) matches the index used in TAG_SET
  bool ok = sendFrameUart(LP_MSG_TAG_CLEAR, payload, sizeof(payload));
  if (padKnown && padPaired) {
    sendFrameTcp(LP_MSG_TAG_CLEAR, payload, sizeof(payload));
  }
  return ok;
}

static int firstFreeToyboxIndex() {
  for (int i = 0; i < (int)kToyboxLimit; i++) {
    if (!toybox[i].inUse) {
      return i;
    }
  }
  return -1;
}

static bool toyboxContains(uint32_t toyId) {
  for (int i = 0; i < (int)kToyboxLimit; i++) {
    if (toybox[i].inUse && toybox[i].toyId == toyId) {
      return true;
    }
  }
  return false;
}

static bool toyIsOnPad(uint32_t toyId) {
  for (int i = 0; i < (int)kSlotCount; i++) {
    if (padSlots[i].occupied && padSlots[i].toyId == toyId) {
      return true;
    }
  }
  return false;
}

static void addToyboxEntry(uint32_t toyId, const String& label, uint8_t toyType) {
  // Prevent duplicates - each toy can only be added once
  if (toyboxContains(toyId)) {
    return;
  }
  int idx = firstFreeToyboxIndex();
  if (idx < 0) {
    return;
  }
  toybox[idx].inUse = true;
  toybox[idx].toyId = toyId;
  toybox[idx].toyType = toyType;
  safeCopyLabel(toybox[idx].label, sizeof(toybox[idx].label), label);
}

static void removeToyboxEntry(uint8_t idx) {
  if (idx >= kToyboxLimit) {
    return;
  }
  toybox[idx].inUse = false;
  toybox[idx].toyId = 0;
  toybox[idx].toyType = TOY_UNKNOWN;
  toybox[idx].label[0] = '\0';
}

static void sendCatalogJson() {
  web.send_P(200, "application/json", kLdCatalogJson, kLdCatalogJsonLen);
}

static void sendManifestJson() {
  web.send(200, "application/manifest+json", kManifestJson);
}

static void sendServiceWorkerJs() {
  web.send(200, "application/javascript", kServiceWorkerJs);
}

static void sendAppIconSvg() {
  web.send(200, "image/svg+xml", kAppIconSvg);
}

static void loadMode() {
  prefs.begin("console", true);
  const String modeStr = prefs.getString(kModePrefKey, "emulator");
  prefs.end();
  runtimeMode = modeFromString(modeStr);
}

static void saveMode() {
  prefs.begin("console", false);
  prefs.putString(kModePrefKey, modeToString(runtimeMode));
  prefs.end();
}

// Append a JSON-escaped string to buf[pos..bufSize-1].  Returns new pos.
static int jsonAppendEscaped(char* buf, int pos, int bufSize, const char* s) {
  for (; *s && pos < bufSize - 2; s++) {
    const char c = *s;
    if (c == '"' || c == '\\') { buf[pos++] = '\\'; }
    buf[pos++] = c;
  }
  return pos;
}

static void sendStateJson() {
  // Static buffer — zero heap allocation per call, no fragmentation.
  // Sized for worst case: 3 lights + 7 slots (24-char labels) +
  // 24 toybox entries (24-char labels) + fixed fields + d2dbg.
  static char buf[4096];
  int pos = 0;
  const int N = (int)sizeof(buf);
#define JS(fmt, ...) pos += snprintf(buf + pos, N - pos, fmt, ##__VA_ARGS__)

  // IP as plain C string to avoid String allocation.
  char ipStr[16] = "-";
  if (padPaired) {
    const IPAddress ip = padIp;
    snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  }

  JS("{\"mode\":\"%s\",", modeToString(runtimeMode));
  JS("\"ssid\":\"%s\",", apSsid.c_str());
  JS("\"paired\":%s,", padPaired ? "true" : "false");
  JS("\"padEndpoint\":\"%s", ipStr);
  if (padPaired) {
    JS(":%u", (unsigned)(padClient.connected() ? padClient.remotePort() : 0));
  }
  JS("\",");
  JS("\"hasSecret\":%s,", sharedSecret != 0 ? "true" : "false");

  JS("\"lights\":[");
  for (uint8_t i = 0; i < kLightZoneCount; i++) {
    if (i > 0) JS(",");
    JS("{\"r\":%u,\"g\":%u,\"b\":%u}", lightZones[i].r, lightZones[i].g, lightZones[i].b);
  }
  JS("],");

  JS("\"slots\":[");
  for (uint8_t i = 0; i < kSlotCount; i++) {
    if (i > 0) JS(",");
    JS("{\"occupied\":%s,\"toyId\":%lu,\"type\":\"%s\",\"zone\":%u,\"label\":\"",
       padSlots[i].occupied ? "true" : "false",
       (unsigned long)padSlots[i].toyId,
       toyTypeToString(padSlots[i].toyType),
       (unsigned)padSlots[i].zone);
    pos = jsonAppendEscaped(buf, pos, N, padSlots[i].label);
    JS("\"}");
  }
  JS("],");

  JS("\"toybox\":[");
  bool first = true;
  for (uint8_t i = 0; i < kToyboxLimit; i++) {
    if (!toybox[i].inUse) continue;
    if (!first) JS(",");
    first = false;
    JS("{\"index\":%u,\"toyId\":%lu,\"type\":\"%s\",\"label\":\"",
       i, (unsigned long)toybox[i].toyId, toyTypeToString(toybox[i].toyType));
    pos = jsonAppendEscaped(buf, pos, N, toybox[i].label);
    JS("\"}");
  }
  JS("],");

  const uint32_t msNow = millis();
  const bool gameActive = (sLastLedCmdFromRp2040Ms != 0) &&
                          (msNow - sLastLedCmdFromRp2040Ms) < 30000u;
  JS("\"gameActive\":%s,", gameActive ? "true" : "false");

  JS("\"d2dbg\":\"");
  pos = jsonAppendEscaped(buf, pos, N, sPadD2Debug);
  JS("\"}");

#undef JS
  if (pos >= N) pos = N - 1;
  buf[pos] = '\0';

  web.send(200, "application/json", buf);
}

static void handleApiMode() {
  const String body = web.arg("plain");
  String modeStr;
  if (!parseJsonString(body, "mode", modeStr)) {
    web.send(400, "text/plain", "missing mode");
    return;
  }
  const RuntimeMode prevMode = runtimeMode;
  runtimeMode = modeFromString(modeStr);
  saveMode();

  if (runtimeMode == MODE_EMULATOR && prevMode != MODE_EMULATOR) {
    // Clear all RP2040 slot state — physical toys are no longer forwarded
    // in emulator mode so the game should start with a clean slate.
    sendFrameUart(LP_MSG_TAG_CLEAR, nullptr, 0);
    // Turn off the physical toypad lights; emulator mode owns the light state.
    if (padKnown && padPaired) {
      const uint8_t offPayload[4] = {0xff, 0, 0, 0};  // zone=all, r=g=b=0
      sendFrameTcp(LP_MSG_LED_CMD, offPayload, sizeof(offPayload));
    }
  }

  web.send(200, "text/plain", "ok");
}

static void handleApiToyboxAdd() {
  const String body = web.arg("plain");
  uint32_t toyId = 0;
  String label;
  String type;
  if (!parseJsonFlexibleId(body, "id", &toyId) || !parseJsonString(body, "label", label)) {
    web.send(400, "text/plain", "missing id/label");
    return;
  }
  label.trim();
  if (label.length() == 0) {
    web.send(400, "text/plain", "empty label");
    return;
  }
  
  if (!parseJsonString(body, "type", type)) {
    type = "unknown";
  }
  addToyboxEntry(toyId, label, toyTypeFromString(type));
  web.send(200, "text/plain", "ok");
}

static void handleApiSlotPlace() {
  if (runtimeMode != MODE_EMULATOR) {
    web.send(409, "text/plain", "slot control disabled in passthrough mode");
    return;
  }

  const String body = web.arg("plain");
  uint32_t slot = 0;
  uint32_t toyId = 0;
  String label;
  String type;
  uint32_t toyboxIndex = 0;
  bool isToyboxDirect = false;

  if (!parseJsonUint(body, "slot", &slot) || !parseJsonFlexibleId(body, "id", &toyId) ||
      !parseJsonString(body, "label", label)) {
    web.send(400, "text/plain", "missing slot/id/label");
    return;
  }

  if (slot < 1 || slot > kSlotCount) {
    web.send(400, "text/plain", "invalid slot");
    return;
  }

  // For virtual toys the zone is derived from the slot number.
  padSlots[slot - 1].zone = slotToLpZone((uint8_t)slot);
  if (!parseJsonString(body, "type", type)) {
    type = "unknown";
  }
  sendTagSet((uint8_t)slot, toyId, toyTypeFromString(type));

  if (parseJsonUint(body, "toyboxIndex", &toyboxIndex)) {
    // Toy is being placed from the toybox; remove it from toybox
    removeToyboxEntry((uint8_t)toyboxIndex);
    isToyboxDirect = true;
    setSlotState((uint8_t)(slot - 1), toyId, label.c_str(), toyTypeFromString(type),
                 true, (uint8_t)toyboxIndex);
  } else {
    // Toy is being placed directly from catalog (without adding to toybox first)
    setSlotState((uint8_t)(slot - 1), toyId, label.c_str(), toyTypeFromString(type),
                 false, 0xff);
  }

  web.send(200, "text/plain", "ok");
}

static void handleApiSlotClear() {
  if (runtimeMode != MODE_EMULATOR) {
    web.send(409, "text/plain", "slot control disabled in passthrough mode");
    return;
  }

  const String body = web.arg("plain");
  uint32_t slot = 0;
  if (!parseJsonUint(body, "slot", &slot) || slot < 1 || slot > kSlotCount) {
    web.send(400, "text/plain", "invalid slot");
    return;
  }

  const uint8_t slotIndex = (uint8_t)(slot - 1);
  const SlotState& s = padSlots[slotIndex];

  // If this toy came from the toybox, restore it
  if (s.fromToybox && s.toyboxOriginIndex < kToyboxLimit && s.occupied) {
    // Try to restore to the original toybox index first
    if (!toybox[s.toyboxOriginIndex].inUse) {
      toybox[s.toyboxOriginIndex].inUse = true;
      toybox[s.toyboxOriginIndex].toyId = s.toyId;
      toybox[s.toyboxOriginIndex].toyType = s.toyType;
      strncpy(toybox[s.toyboxOriginIndex].label, s.label, sizeof(toybox[s.toyboxOriginIndex].label) - 1);
      toybox[s.toyboxOriginIndex].label[sizeof(toybox[s.toyboxOriginIndex].label) - 1] = '\0';
    } else {
      // If original index is taken, find a new free slot
      addToyboxEntry(s.toyId, String(s.label), s.toyType);
    }
  }

  sendTagClear((uint8_t)slot);
  clearSlotState(slotIndex);
  web.send(200, "text/plain", "ok");
}

static void handleApiToyboxRemove() {
  const String body = web.arg("plain");
  uint32_t index = 0;
  if (!parseJsonUint(body, "index", &index) || index >= kToyboxLimit) {
    web.send(400, "text/plain", "invalid index");
    return;
  }
  removeToyboxEntry((uint8_t)index);
  web.send(200, "text/plain", "ok");
}

static void handleApiTagProgramBegin() {
  const String body = web.arg("plain");
  uint32_t toyId = 0;
  String label;
  String type;
  if (!parseJsonFlexibleId(body, "id", &toyId) || !parseJsonString(body, "label", label)) {
    web.send(400, "text/plain", "missing id/label");
    return;
  }
  if (!parseJsonString(body, "type", type)) type = "vehicle";
  if (!padKnown || !padPaired) {
    web.send(409, "text/plain", "pad not connected");
    return;
  }
  sProgramTargetId   = toyId;
  sProgramTargetType = toyTypeFromString(type);
  safeCopyLabel(sProgramTargetLabel, sizeof(sProgramTargetLabel), label);
  sProgramStatus     = 1;  // waiting
  sProgramModeActive = true;
  sProgramIntercepted = false;
  Serial.printf("[prog] begin id=0x%04lx label=%s\n",
                (unsigned long)toyId, sProgramTargetLabel);
  web.send(200, "text/plain", "ok");
}

static void handleApiTagProgramCancel() {
  sProgramModeActive = false;
  sProgramStatus = 0;
  web.send(200, "text/plain", "ok");
}

static void handleApiTagProgramStatus() {
  const char* s;
  switch (sProgramStatus) {
    case 1:  s = "waiting"; break;
    case 2:  s = "success"; break;
    case 3:  s = "fail";    break;
    default: s = "idle";    break;
  }
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"status\":\"%s\",\"label\":\"%s\",\"id\":%lu}",
           s, sProgramTargetLabel, (unsigned long)sProgramTargetId);
  web.send(200, "application/json", buf);
}

static void handlePortalRoot() {
  web.send(200, "text/html", kPortalPage);
}

// ─── OTA / DFU helpers ──────────────────────────────────────────────────────

// Wait up to timeoutMs for an LP ACK on Serial2 (uses a local parser so the
// global uartParser state is not disturbed).
static bool waitUartAckLocal(uint32_t timeoutMs) {
  lp_stream_parser_t p;
  lp_stream_init(&p);
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    while (Serial2.available()) {
      lp_frame_t f;
      if (lp_stream_push(&p, (uint8_t)Serial2.read(), &f) == LP_PARSE_FRAME_OK &&
          f.header.type == LP_MSG_ACK) {
        return true;
      }
    }
    delay(1);
  }
  return false;
}

// Wait up to timeoutMs for an LP HELLO on Serial2 (used after RP2040 reboots).
static bool waitUartHelloLocal(uint32_t timeoutMs) {
  lp_stream_parser_t p;
  lp_stream_init(&p);
  const uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    while (Serial2.available()) {
      lp_frame_t f;
      if (lp_stream_push(&p, (uint8_t)Serial2.read(), &f) == LP_PARSE_FRAME_OK &&
          f.header.type == LP_MSG_HELLO) {
        return true;
      }
    }
    delay(5);
  }
  return false;
}

// Stream RP2040 firmware from LittleFS over the UART DFU protocol.
// Blocks until the transfer completes or fails.
static bool streamDfuToRp2040() {
  File f = LittleFS.open(kOtaRp2040Path, "r");
  if (!f) { Serial.println("[ota] rp2040 bin missing"); return false; }
  const uint32_t size = f.size();
  if (size == 0) { f.close(); Serial.println("[ota] rp2040 bin empty"); return false; }

  // LP_MSG_OTA_BEGIN: [size LE4][reserved LE4]
  uint8_t beginBuf[8];
  writeU32Le(&beginBuf[0], size);
  writeU32Le(&beginBuf[4], 0);
  sendFrameUart(LP_MSG_OTA_BEGIN, beginBuf, 8);
  Serial.printf("[ota] OTA_BEGIN -> rp2040, %u bytes\n", size);

  if (!waitUartAckLocal(5000)) {
    f.close();
    Serial.println("[ota] OTA_BEGIN: no ACK");
    return false;
  }

  // Stream DFU chunks: [0xDF][0xC0][offset LE4][len LE2][data][crc16 LE2]
  uint8_t  chunk[kDfuChunkSize];
  uint32_t offset = 0;
  while (offset < size) {
    const uint32_t rem     = size - offset;
    const uint16_t clen    = (rem > kDfuChunkSize) ? kDfuChunkSize : (uint16_t)rem;
    f.seek(offset);
    if (f.read(chunk, clen) != clen) {
      Serial.println("[ota] read error"); f.close(); return false;
    }
    const uint16_t crc = lp_crc16_ccitt(chunk, clen);

    bool ok = false;
    for (uint8_t attempt = 0; attempt < kDfuMaxRetries && !ok; attempt++) {
      const uint8_t hdr[8] = {
        0xDF, 0xC0,
        (uint8_t)(offset), (uint8_t)(offset >> 8),
        (uint8_t)(offset >> 16), (uint8_t)(offset >> 24),
        (uint8_t)(clen), (uint8_t)(clen >> 8),
      };
      const uint8_t crcB[2] = {(uint8_t)(crc), (uint8_t)(crc >> 8)};
      Serial2.write(hdr, 8);
      Serial2.write(chunk, clen);
      Serial2.write(crcB, 2);

      const uint32_t t = millis() + kDfuChunkToutMs;
      while (millis() < t) {
        if (Serial2.available()) {
          const int r = Serial2.read();
          if (r == kDfuAckByte) { ok = true; break; }
          if (r == kDfuNakByte) { break; }  // retry
        }
        delay(1);
      }
    }
    if (!ok) {
      Serial.printf("[ota] chunk @%u failed\n", offset);
      f.close(); return false;
    }
    offset += clen;
    if ((offset & 0x3FFF) == 0) {
      Serial.printf("[ota] dfu %u/%u bytes\n", offset, size);
    }
  }
  f.close();

  sendFrameUart(LP_MSG_OTA_COMMIT, nullptr, 0);
  Serial.println("[ota] OTA_COMMIT sent, waiting for reboot...");
  lp_stream_reset(&uartParser);
  if (waitUartHelloLocal(12000)) {
    Serial.println("[ota] rp2040 rebooted OK");
  } else {
    Serial.println("[ota] rp2040 reboot timeout");
  }
  return true;
}

// ─── OTA upload handlers ────────────────────────────────────────────────────

// Scan a LittleFS file for the magic prefix "WTPFW:<target>:".
// Uses overlapping reads so the needle cannot straddle a buffer boundary.
static bool fsMagicCheck(const char* path, const char* target) {
  char needle[48];
  const int nlen = snprintf(needle, sizeof(needle), "WTPFW:%s:", target);

  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint8_t buf[560];  // 512 data bytes + up to 47 bytes overlap
  size_t overlap = 0;
  bool found = false;

  while (!found && f.available()) {
    const size_t n = f.read(buf + overlap, 512);
    const size_t total = overlap + n;
    for (size_t i = 0; i + (size_t)nlen <= total; i++) {
      if (memcmp(buf + i, needle, (size_t)nlen) == 0) { found = true; break; }
    }
    if (!found && total >= (size_t)(nlen - 1)) {
      overlap = (size_t)(nlen - 1);
      memmove(buf, buf + total - overlap, overlap);
    } else if (!found) {
      overlap = total;
    }
  }
  f.close();
  return found;
}

// Return true if the LittleFS binary at path has the ESP image magic (0xE9)
// at byte 0 and the given chip_id at bytes [12..13] (little-endian).
static bool fsChipCheck(const char* path, uint16_t expectedChipId) {
  File f = LittleFS.open(path, "r");
  if (!f || f.size() < 14) { if (f) f.close(); return false; }
  uint8_t hdr[14];
  f.read(hdr, 14);
  f.close();
  if (hdr[0] != 0xE9) return false;
  const uint16_t chipId = (uint16_t)hdr[12] | ((uint16_t)hdr[13] << 8);
  return chipId == expectedChipId;
}

// POST /ota/upload-rp2040
static void handleOtaRp2040Upload() {
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    sOtaError = false;
    sOtaFile  = LittleFS.open(kOtaRp2040Path, "w");
    if (!sOtaFile) { sOtaError = true; }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!sOtaError && sOtaFile &&
        sOtaFile.write(up.buf, up.currentSize) != up.currentSize) {
      sOtaError = true;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (sOtaFile) sOtaFile.close();
  }
}
static void handleOtaRp2040Done() {
  if (sOtaError) { web.send(500, "text/plain", "upload failed"); return; }
  // Validate: RP2040 binary must not start with 0xE9 (that would be an ESP image).
  {
    File f = LittleFS.open(kOtaRp2040Path, "r");
    if (!f || f.size() < 1) { if (f) f.close(); web.send(400, "text/plain", "error: empty file"); return; }
    uint8_t firstByte = 0; f.read(&firstByte, 1); f.close();
    if (firstByte == 0xE9) {
      web.send(400, "text/plain", "error: this is an ESP32 binary, not an RP2040 binary");
      return;
    }
  }
  if (!fsMagicCheck(kOtaRp2040Path, "console-rp2040")) {
    web.send(400, "text/plain", "error: firmware target mismatch (expected console-rp2040)");
    return;
  }
  web.send(200, "text/plain", "OK: streaming to rp2040...\n");
  streamDfuToRp2040();
}

// POST /ota/upload-pad — save binary, trigger TCP OTA_BEGIN
static void handleOtaPadUpload() {
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    sOtaError = false;
    sOtaFile  = LittleFS.open(kOtaPadPath, "w");
    if (!sOtaFile) { sOtaError = true; }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!sOtaError && sOtaFile &&
        sOtaFile.write(up.buf, up.currentSize) != up.currentSize) {
      sOtaError = true;
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (sOtaFile) sOtaFile.close();
  }
}
static void handleOtaPadDone() {
  if (sOtaError) { web.send(500, "text/plain", "upload failed"); return; }
  if (!fsChipCheck(kOtaPadPath, 0x0002)) {  // 0x0002 = ESP32-S2
    web.send(400, "text/plain", "error: expected ESP32-S2 binary for pad-esp32");
    return;
  }
  if (!fsMagicCheck(kOtaPadPath, "pad-esp32")) {
    web.send(400, "text/plain", "error: firmware target mismatch (expected pad-esp32)");
    return;
  }
  File f = LittleFS.open(kOtaPadPath, "r");
  if (!f) { web.send(500, "text/plain", "bin not found"); return; }
  const uint32_t size = f.size();
  f.close();
  uint8_t beginBuf[8];
  writeU32Le(&beginBuf[0], size);
  writeU32Le(&beginBuf[4], 0);
  if (!sendFrameTcp(LP_MSG_OTA_BEGIN, beginBuf, 8)) {
    web.send(503, "text/plain", "pad not connected");
    return;
  }
  web.send(200, "text/plain", "OK: pad OTA triggered\n");
}

// GET /ota/pad-esp32.bin — serve pad binary for HTTPUpdate fetch
static void handleOtaPadServe() {
  File f = LittleFS.open(kOtaPadPath, "r");
  if (!f) { web.send(404, "text/plain", "not found"); return; }
  web.streamFile(f, "application/octet-stream");
  f.close();
}

// POST /ota/upload-console — self-OTA via Update library
static void handleOtaConsoleUpload() {
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    sOtaError            = false;
    sOtaConsoleValidated = false;
    // Defer Update.begin() until the first chunk so we can validate the
    // ESP32 image header (magic 0xE9 + chip_id 0x0000) before committing.
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (sOtaError) return;
    if (!sOtaConsoleValidated) {
      // Need at least 14 bytes to read magic + chip_id.
      if (up.currentSize < 14) {
        sOtaError = true;
        Serial.println("[ota] console first chunk too small to validate");
        return;
      }
      if (up.buf[0] != 0xE9) {
        sOtaError = true;
        Serial.printf("[ota] bad magic 0x%02x (expected 0xE9)\n", up.buf[0]);
        return;
      }
      const uint16_t chipId = (uint16_t)up.buf[12] | ((uint16_t)up.buf[13] << 8);
      if (chipId != 0x0000) {  // 0x0000 = ESP32
        sOtaError = true;
        Serial.printf("[ota] wrong chip id 0x%04x (expected 0x0000 for ESP32)\n", chipId);
        return;
      }
      sOtaConsoleValidated = true;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        sOtaError = true;
        Serial.printf("[ota] self begin fail: %s\n", Update.errorString());
        return;
      }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      sOtaError = true;
      Serial.printf("[ota] self write fail: %s\n", Update.errorString());
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!sOtaError && !Update.end(true)) {
      sOtaError = true;
      Serial.printf("[ota] self end fail: %s\n", Update.errorString());
    }
  }
}
static void handleOtaConsoleDone() {
  if (sOtaError) {
    const char* errMsg = sOtaConsoleValidated ? Update.errorString() : "binary validation failed";
    web.send(500, "text/plain", errMsg);
    return;
  }
  web.send(200, "text/plain", "OK: rebooting...\n");
  delay(500);
  ESP.restart();
}

static void handleApiVersions() {
  char buf[192];
  snprintf(buf, sizeof(buf),
    "{\"console_esp32\":\"%s\",\"console_rp2040\":\"%s\",\"pad_esp32\":\"%s\"}",
    FIRMWARE_VERSION, sRp2040Version, sPadVersion);
  web.send(200, "application/json", buf);
}

static void redirectToPortalRoot() {
  web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  web.send(302, "text/plain", "");
}

static void setupCaptivePortal() {
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(kDnsPort, "*", WiFi.softAPIP());

  web.on("/", HTTP_GET, handlePortalRoot);
  web.on("/api/catalog", HTTP_GET, sendCatalogJson);
  web.on("/manifest.webmanifest", HTTP_GET, sendManifestJson);
  web.on("/sw.js", HTTP_GET, sendServiceWorkerJs);
  web.on("/icon.svg", HTTP_GET, sendAppIconSvg);
  web.on("/api/state", HTTP_GET, sendStateJson);
  web.on("/api/mode", HTTP_POST, handleApiMode);
  web.on("/api/toybox/add", HTTP_POST, handleApiToyboxAdd);
  web.on("/api/toybox/remove", HTTP_POST, handleApiToyboxRemove);
  web.on("/api/slot/place", HTTP_POST, handleApiSlotPlace);
  web.on("/api/slot/clear", HTTP_POST, handleApiSlotClear);
  web.on("/api/tag/program/begin", HTTP_POST, handleApiTagProgramBegin);
  web.on("/api/tag/program/cancel", HTTP_POST, handleApiTagProgramCancel);
  web.on("/api/tag/program/status", HTTP_GET, handleApiTagProgramStatus);
  web.on("/api/versions", HTTP_GET, handleApiVersions);
  // OTA firmware update endpoints
  web.on("/ota/upload-rp2040",  HTTP_POST, handleOtaRp2040Done,   handleOtaRp2040Upload);
  web.on("/ota/upload-pad",     HTTP_POST, handleOtaPadDone,      handleOtaPadUpload);
  web.on("/ota/pad-esp32.bin",  HTTP_GET,  handleOtaPadServe);
  web.on("/ota/upload-console", HTTP_POST, handleOtaConsoleDone,  handleOtaConsoleUpload);
  // Probe URLs used by common client OSes for captive portal detection.
  web.on("/generate_204", HTTP_GET, redirectToPortalRoot);
  web.on("/hotspot-detect.html", HTTP_GET, redirectToPortalRoot);
  web.on("/connecttest.txt", HTTP_GET, redirectToPortalRoot);
  web.on("/ncsi.txt", HTTP_GET, redirectToPortalRoot);
  web.on("/fwlink", HTTP_GET, redirectToPortalRoot);
  web.onNotFound(redirectToPortalRoot);
  web.begin();
}

static uint32_t readU32Le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void writeU32Le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static bool isValidPadHello(const lp_frame_t& frame) {
  if (frame.header.type != LP_MSG_HELLO || frame.header.length < 1) {
    return false;
  }
  if (frame.payload[0] != 0xa1) {
    return false;
  }

  // Legacy 1-byte enrollment from old pad firmware (no secret bytes).
  if (frame.header.length == 1) {
    return true;
  }

  // New format: 1-byte marker + 4-byte secret + optional version string.
  // A zero secret means the pad is requesting fresh enrollment.
  if (frame.header.length < 5) {
    return false;  // malformed
  }
  const uint32_t incomingSecret = readU32Le(&frame.payload[1]);
  if (incomingSecret == 0) {
    return true;  // enrollment request
  }
  if (sharedSecret == 0) {
    return false;  // console has no secret — reject non-enrollment
  }
  return incomingSecret == sharedSecret;
}

static void loadSecret() {
  prefs.begin("console", true);
  sharedSecret = prefs.getUInt("secret", 0);
  prefs.end();
}

static void saveSecret(uint32_t secret) {
  prefs.begin("console", false);
  prefs.putUInt("secret", secret);
  prefs.end();
}

static void ensureSecret() {
  if (sharedSecret != 0) {
    return;
  }

  sharedSecret = esp_random();
  if (sharedSecret == 0) {
    sharedSecret = 1;
  }
  saveSecret(sharedSecret);
}

static bool sendFrameUart(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                          uint8_t forceSeq) {
  uint8_t wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen = 0;
  const uint8_t seq = (forceSeq == 0xff) ? seqCounter++ : forceSeq;

  if (!lp_encode_frame(type, seq, payload, payloadLen, wire, sizeof(wire), &wireLen)) {
    return false;
  }

  Serial2.write(wire, wireLen);
  return true;
}

// Send an LP frame to the pad over TCP.  On write failure, tears down the TCP
// connection immediately so processTcpIn() stops treating the pad as paired.
static bool sendFrameTcp(uint8_t type, const uint8_t* payload, uint8_t payloadLen,
                         uint8_t forceSeq) {
  if (!padKnown || !padPaired) return false;

  uint8_t wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
  uint16_t wireLen = 0;
  const uint8_t seq = (forceSeq == 0xff) ? seqCounter++ : forceSeq;

  if (!lp_encode_frame(type, seq, payload, payloadLen, wire, sizeof(wire), &wireLen)) {
    return false;
  }

  const int cfd = padClient.fd();
  if (cfd < 0) {
    Serial.println("[console-esp32] tcp send: no fd — disconnecting");
    padClient.stop();
    padKnown    = false;
    padPaired   = false;
    padLastRxMs = 0;
    return false;
  }
  // Use direct send() instead of padClient.write() to bypass the cached
  // _connected flag, which write() checks first.  _connected can be set to
  // false by padClient.connected() via a stale lwIP errno, causing write() to
  // return 0 and tear down a perfectly healthy connection.
  ssize_t n = ::send(cfd, wire, wireLen, MSG_DONTWAIT);
  if (n < 0 && errno == EAGAIN) {
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(cfd, &wset);
    struct timeval tv = {0, 200000};  // 200 ms
    if (::select(cfd + 1, NULL, &wset, NULL, &tv) > 0 && FD_ISSET(cfd, &wset)) {
      n = ::send(cfd, wire, wireLen, MSG_DONTWAIT);
    }
  }
  if (n != (ssize_t)wireLen) {
    Serial.printf("[console-esp32] tcp write failed %zd/%u errno=%d — disconnecting\n",
                  n, (unsigned)wireLen, errno);
    padClient.stop();
    padKnown    = false;
    padPaired   = false;
    padLastRxMs = 0;
    return false;
  }
  return true;
}

static void sendAckUart(uint8_t receivedSeq) {
  const uint8_t payload[1] = {receivedSeq};
  sendFrameUart(LP_MSG_ACK, payload, sizeof(payload));
}

static void sendAckTcp(uint8_t receivedSeq) {
  const uint8_t payload[1] = {receivedSeq};
  sendFrameTcp(LP_MSG_ACK, payload, sizeof(payload));
}

static void processTcpIn() {
  // Accept incoming pad connection — always replace an existing one so the pad
  // can reconnect after a crash without waiting for TCP keepalive timeout.
  if (tcpServer.hasClient()) {
    WiFiClient incoming = tcpServer.accept();
    if (padKnown) {
      Serial.println("[console-esp32] replacing existing pad connection");
      padClient.stop();
    }
    padClient   = incoming;
    padClient.setNoDelay(true);  // send LP frames immediately, no Nagle buffering
    lp_stream_reset(&tcpParser);
    padKnown    = true;
    padPaired   = false;  // must re-handshake
    padLastRxMs = millis();  // start timeout from connection time
    padIp       = padClient.remoteIP();
    Serial.print("[console-esp32] TCP pad connected from ");
    Serial.println(padIp);
  }

  // Detect disconnect: no LP frame received from pad within kPadRxTimeoutMs.
  // Avoids all lwIP recv()/select() quirks — only fires on genuine silence.
  if (padKnown && (millis() - padLastRxMs) >= kPadRxTimeoutMs) {
    Serial.printf("[console-esp32] TCP pad disconnected (rx timeout)\n");
    padClient.stop();
    padKnown    = false;
    padPaired   = false;
    padLastRxMs = 0;
    return;
  }

  if (!padKnown) return;

  // Read stream bytes
  while (padClient.available()) {
    const int c = padClient.read();
    if (c < 0) break;

    lp_frame_t frame;
    const lp_parse_result_t res = lp_stream_push(&tcpParser, (uint8_t)c, &frame);
    if (res == LP_PARSE_FRAME_BAD_CRC) {
      Serial.println("[console-esp32] dropped bad TCP frame");
      continue;
    }
    if (res != LP_PARSE_FRAME_OK) continue;

    if (!padPaired) {
      if (!isValidPadHello(frame)) {
        Serial.println("[console-esp32] unpaired client ignored");
        continue;
      }

      padPaired = true;

      // Enrollment: legacy 1-byte HELLO OR new-format zero secret.
      const bool isEnrollment =
          (frame.header.length == 1) ||
          (frame.header.length >= 5 && readU32Le(&frame.payload[1]) == 0);
      if (isEnrollment) {
        sharedSecret = 0;
      }
      ensureSecret();

      // Extract pad firmware version (payload[5..length-1] in new format).
      if (frame.header.length > 5) {
        const uint8_t vl = frame.header.length - 5;
        memcpy(sPadVersion, &frame.payload[5], vl);
        sPadVersion[vl] = '\0';
      } else {
        strcpy(sPadVersion, "?");
      }
      uint8_t pairPayload[4];
      writeU32Le(pairPayload, sharedSecret);
      sendFrameTcp(LP_MSG_PAIR_SET, pairPayload, sizeof(pairPayload));

      Serial.print("[console-esp32] paired with ");
      Serial.println(padIp);
    }

    padLastRxMs = millis();  // refresh liveness timestamp on every valid frame

    if (frame.header.type != LP_MSG_ACK) {
      sendAckTcp(frame.header.seq);
    }

    observeFrameState(frame, true /*fromPad*/);

    // NFC programmer intercepted this TAG_SET: forward programmed vehicle ID to
    // RP2040 instead of the original blank-tag frame, then skip normal forwarding.
    if (sProgramIntercepted) {
      const uint8_t sn = sProgramInterceptedSlot;
      sProgramIntercepted = false;
      sProgramInterceptedSlot = 0;
      if (sn >= 1 && sn <= kSlotCount) {
        sendTagSet(sn, padSlots[sn - 1].toyId, padSlots[sn - 1].toyType);
      }
      continue;
    }

    if (frame.header.type == LP_MSG_HELLO || frame.header.type == LP_MSG_PAIR_SET) {
      continue;
    }

    // Debug messages from pad: print locally, do NOT forward to RP2040
    // (forwarding would cause the RP2040 to echo them back, creating a loop).
    if (frame.header.type == LP_MSG_DEBUG) {
      if (kEnableUsbDebugConsole) {
        char msg[LP_MAX_PAYLOAD + 1];
        const uint8_t len = frame.header.length < LP_MAX_PAYLOAD
                                ? frame.header.length : LP_MAX_PAYLOAD;
        memcpy(msg, frame.payload, len);
        msg[len] = '\0';
        Serial.print("[pad-dbg] ");
        Serial.println(msg);
      }
      // Capture the 4-variant NFC diagnostic for the webui.
      {
        char msg[LP_MAX_PAYLOAD + 1];
        const uint8_t len = frame.header.length < LP_MAX_PAYLOAD
                                ? frame.header.length : LP_MAX_PAYLOAD;
        memcpy(msg, frame.payload, len);
        msg[len] = '\0';
        if (strncmp(msg, "aa-le:", 6) == 0) {
          strlcpy(sPadD2Debug, msg, sizeof(sPadD2Debug));
        }
      }
      continue;
    }

    // Re-encode and forward to RP2040 with original seq.
    // In emulator mode the console owns slot state; physical TAG_SET/TAG_CLEAR
    // must not reach the RP2040 or the game would see phantom physical toys.
    if (runtimeMode == MODE_EMULATOR &&
        (frame.header.type == LP_MSG_TAG_SET ||
         frame.header.type == LP_MSG_TAG_CLEAR)) {
      continue;
    }
    uint8_t wire[sizeof(lp_header_t) + LP_MAX_PAYLOAD + 2];
    uint16_t wireLen = 0;
    // For TAG_SET from the physical pad, append isPhysical=1 so the RP2040
    // knows to forward D3 page-writes back via LP_MSG_NFC_WRITE.
    if (frame.header.type == LP_MSG_TAG_SET &&
        frame.header.length >= 6 &&
        frame.header.length < LP_MAX_PAYLOAD) {
      uint8_t augPayload[LP_MAX_PAYLOAD];
      memcpy(augPayload, frame.payload, frame.header.length);
      augPayload[frame.header.length] = 1;  // isPhysical = true
      if (lp_encode_frame(frame.header.type, frame.header.seq,
                          augPayload, frame.header.length + 1,
                          wire, sizeof(wire), &wireLen)) {
        Serial2.write(wire, wireLen);
      }
    } else {
      if (lp_encode_frame(frame.header.type, frame.header.seq,
                          frame.payload, frame.header.length,
                          wire, sizeof(wire), &wireLen)) {
        Serial2.write(wire, wireLen);
      }
    }
  }
}

static void processUartIn() {
  while (Serial2.available()) {
    const int c = Serial2.read();
    if (c < 0) {
      break;
    }

    lp_frame_t frame;
    const lp_parse_result_t res =
        lp_stream_push(&uartParser, (uint8_t)c, &frame);

    if (res == LP_PARSE_FRAME_BAD_CRC) {
      Serial.println("[console-esp32] dropped bad UART frame");
      continue;
    }
    if (res != LP_PARSE_FRAME_OK) {
      continue;
    }

    if (frame.header.type != LP_MSG_ACK && frame.header.type != LP_MSG_DEBUG) {
      sendAckUart(frame.header.seq);
    }

    if (frame.header.type == LP_MSG_DEBUG) {
      if (kEnableUsbDebugConsole) {
        // Print RP2040 debug messages to USB serial.
        char msg[LP_MAX_PAYLOAD + 1];
        const uint8_t len = frame.header.length < LP_MAX_PAYLOAD ? frame.header.length : LP_MAX_PAYLOAD;
        memcpy(msg, frame.payload, len);
        msg[len] = '\0';
        Serial.print("[lp-debug] ");
        Serial.println(msg);
      }
      continue;
    }

    if (frame.header.type == LP_MSG_HELLO) {
      // Extract RP2040 firmware version (payload[1..length-1]).
      if (frame.header.length > 1) {
        const uint8_t vl = frame.header.length - 1;
        memcpy(sRp2040Version, &frame.payload[1], vl);
        sRp2040Version[vl] = '\0';
      }
      continue;  // do not forward HELLO to pad
    }

    observeFrameState(frame, false /*fromPad*/);

    // Only forward LED commands to the pad. Tag events flow console→RP2040,
    // not the reverse; forwarding HELLOs/ACKs/etc. confuses the pad's LP state.
    // In emulator mode the physical pad is not under game control, so don't
    // push game LED colours onto it.
    if (frame.header.type == LP_MSG_LED_CMD) {
      sLastLedCmdFromRp2040Ms = millis();
      if (runtimeMode != MODE_EMULATOR) {
        sendFrameTcp(frame.header.type, frame.payload, frame.header.length);
      }
    }
  }
}

static void sendHeartbeatToRp2040() {
  const uint8_t payload[1] = {0xb1};
  sendFrameUart(LP_MSG_HELLO, payload, sizeof(payload));
}

static void processPiConsoleBridge() {
  // ESP USB serial -> Pi UART
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) {
      break;
    }

    // Ctrl+] exits bridge mode (common serial escape key).
    if (c == 0x1d) {
      piConsoleBridgeMode = false;
      usbCmdLen = 0;
      Serial.println("\n[console-esp32] pi-console bridge OFF");
      return;
    }

    Serial2.write((uint8_t)c);
  }

  // Pi UART -> ESP USB serial
  while (Serial2.available()) {
    const int c = Serial2.read();
    if (c < 0) {
      break;
    }

    const uint8_t b = (uint8_t)c;

    // 3-state ANSI/VT filter — strips escape sequences, forwards plain text.
    switch (piAnsiState) {
      case ANSI_NORMAL:
        if (b == 0x1b) {
          piAnsiState = ANSI_AFTER_ESC;
        } else if (b == '\t' || b == '\n' || b == '\r' || b == 0x08 ||
                   (b >= 0x20 && b <= 0x7E)) {
          Serial.write(b);
        }
        // else: other C0 controls (e.g. 0x0f, 0x0e charset switches) — discard
        break;

      case ANSI_AFTER_ESC:
        if (b == '[') {
          piAnsiState = ANSI_IN_CSI;  // CSI sequence — consume until 0x40-0x7E
        } else if (b == ']') {
          piAnsiState = ANSI_IN_OSC;  // OSC sequence — consume until BEL or ESC\
        } else {
          piAnsiState = ANSI_NORMAL;  // 2-char Fe escape, fully consumed
        }
        break;

      case ANSI_IN_CSI:
        // Parameter/intermediate bytes: 0x20-0x3F; final byte: 0x40-0x7E
        if (b >= 0x40 && b <= 0x7E) {
          piAnsiState = ANSI_NORMAL;
        }
        break;

      case ANSI_IN_OSC:
        if (b == 0x07) {
          piAnsiState = ANSI_NORMAL;  // BEL terminates OSC
        } else if (b == 0x1b) {
          piAnsiState = ANSI_AFTER_ESC;  // ESC \ (String Terminator) follows
        }
        break;
    }
  }
}

static void processUsbConsole() {
  while (Serial.available()) {
    const int c = Serial.read();
    if (c < 0) {
      break;
    }

    // Normalize line endings; only parse on '\n'.
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      // Echo newline when not in bridge mode (bridge gets echo from Pi PTY).
      if (!piConsoleBridgeMode) { Serial.write('\r'); Serial.write('\n'); }
      usbCmdBuf[usbCmdLen] = '\0';
      const String cmd(usbCmdBuf);
      usbCmdLen = 0;

      if (cmd.length() == 0) {
        continue;
      }

      if (cmd == "help" || cmd == "?") {
        Serial.println("[console-esp32] commands: help | pi-console on | pi-console off");
      } else if (cmd == "pi-console on") {
        piConsoleBridgeMode = true;
        Serial.println("[console-esp32] pi-console bridge ON");
        Serial.println("[console-esp32] Type Ctrl+] to exit bridge mode");
      } else if (cmd == "pi-console off") {
        piConsoleBridgeMode = false;
        Serial.println("[console-esp32] pi-console bridge OFF");
      } else {
        Serial.print("[console-esp32] unknown command: ");
        Serial.println(cmd);
      }

      continue;
    }

    if (usbCmdLen < (kUsbCmdMaxLen - 1)) {
      usbCmdBuf[usbCmdLen++] = (char)c;
      // Local echo: show typed characters when not in bridge mode (bridge
      // mode gets echo from the remote PTY instead).
      if (!piConsoleBridgeMode) {
        Serial.write((uint8_t)c);
      }
    }
  }
}

static void setupAccessPoint() {
  const uint32_t chip = (uint32_t)(ESP.getEfuseMac() & 0x00ffffff);
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "ToyPadConsole-%06lX", (unsigned long)chip);
  apSsid = ssid;

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);  // keep radio active; reduces AP-side beacon gaps
  // Use 192.168.44.x to avoid colliding with the pad's ToyPad-Setup AP
  // (also on 192.168.4.x). Clients that previously connected to ToyPad-Setup
  // would reuse their cached 192.168.4.x lease and fail to get an address here.
  WiFi.softAPConfig(IPAddress(192, 168, 44, 1),
                    IPAddress(192, 168, 44, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid.c_str(), nullptr, 1, 0, 8);  // open, ch1, visible, max 8 stations

  Serial.print("[console-esp32] AP SSID (open): ");
  Serial.println(apSsid);
  Serial.print("[console-esp32] AP IP: ");
  Serial.println(WiFi.softAPIP());

  setupCaptivePortal();
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);  // mount, format on first use
  Serial2.begin(kRp2040Baud, SERIAL_8N1, 16, 17);
  lp_stream_init(&uartParser);
  loadSecret();
  loadMode();

  // Initialise zone field so empty slots render in the correct column
  // from the moment the web UI first loads (before any TAG_SET arrives).
  for (uint8_t i = 0; i < kSlotCount; i++) {
    padSlots[i].zone = slotToLpZone(i + 1);
  }

  Serial.println("[console-esp32] boot");
  if (kEnableUsbDebugConsole) {
    Serial.println("[console-esp32] type 'help' for USB console commands");
  }

  setupAccessPoint();

  // Log WiFi-layer station events so we can correlate them with TCP drops.
  // (Arduino ESP32 2.x AP disconnect event has no reason code field.)
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    const uint8_t* m = info.wifi_ap_stadisconnected.mac;
    Serial.printf("[console-esp32] WiFi STA left AP %02x:%02x:%02x:%02x:%02x:%02x\n",
                  m[0], m[1], m[2], m[3], m[4], m[5]);
  }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println("[console-esp32] WiFi STA joined AP");
  }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);

  tcpServer.begin();
  lp_stream_init(&tcpParser);
  sendHeartbeatToRp2040();
}

void loop() {
  const uint32_t now = millis();

  if (kEnableUsbDebugConsole) {
    if (piConsoleBridgeMode) {
      processPiConsoleBridge();
      dns.processNextRequest();
      web.handleClient();
      delay(2);
      return;
    }

    processUsbConsole();
  }

  processTcpIn();
  processUartIn();
  dns.processNextRequest();
  web.handleClient();

  if ((now - lastHelloMs) >= kHelloMs) {
    lastHelloMs = now;
    sendHeartbeatToRp2040();
  }

  // Periodically re-broadcast full state on both links so peers recover after
  // a restart without waiting for the next change event.
  if ((now - lastStateSyncMs) >= kStateSyncMs) {
    lastStateSyncMs = now;
    // Active slots → RP2040 via UART. Only occupied slots need syncing;
    // empty slots are the RP2040's default state so TAG_CLEAR floods are
    // unnecessary and overflow its event queue.
    for (uint8_t s = 1; s <= kSlotCount; s++) {
      if (padSlots[s - 1].occupied) {
        sendTagSet(s, padSlots[s - 1].toyId, padSlots[s - 1].toyType);
      }
    }
    // All 3 LED zone states → pad-esp32 so it keeps the toypad lit.
    // Only send while the game is active (RP2040 LED_CMD seen within last 30 s).
    // When the game goes quiet the pad auto-reverts to standby green via its
    // own 30 s timeout without us pushing stale game colours at it.
    const bool gameNowActive = (sLastLedCmdFromRp2040Ms != 0) &&
                               (now - sLastLedCmdFromRp2040Ms) < 30000u;
    if (padKnown && padPaired && ledStateKnown && gameNowActive && runtimeMode != MODE_EMULATOR) {
      for (uint8_t z = 0; z < kLightZoneCount; z++) {
        const uint8_t ledPayload[4] = {z, lightZones[z].r, lightZones[z].g, lightZones[z].b};
        sendFrameTcp(LP_MSG_LED_CMD, ledPayload, sizeof(ledPayload));
      }
    }
  }

  if (kEnableUsbDebugConsole && (now - lastDebugHeartbeatMs) >= kDebugHeartbeatMs) {
    lastDebugHeartbeatMs = now;
    Serial.print("[console-esp32] alive mode=");
    Serial.print(runtimeMode == MODE_PASSTHROUGH ? "passthrough" : "emulator");
    Serial.print(" paired=");
    Serial.print(padPaired ? "yes" : "no");
    Serial.print(" ap=");
    Serial.println(apSsid);
  }

  delay(2);
}
