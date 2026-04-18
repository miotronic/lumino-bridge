/**
 * Lumino Bridge
 * =============
 * ESP32 firmware for programming UCS512C-based RGBW LED fixtures
 * over RS-485 using a reverse-engineered proprietary DMX-like protocol.
 *
 * Hardware:
 *   ESP32 (any classic variant)  +  MAX485 / RS-485 transceiver module
 *
 * Connections:
 *   ESP32 GPIO 17  →  MAX485 DI   (UART1 TX / data out)
 *   ESP32 GPIO  4  →  MAX485 DE + RE (tied together, direction control)
 *   ESP32 GPIO  0  →  Pushbutton → GND  (optional physical STOP)
 *   MAX485 A/B     →  RS-485 bus to fixtures
 *
 * Tested with: UCS512C (RGBW, 4 channels per fixture), up to 512 fixtures.
 * After programming, fixtures respond normally to standard DMX512 from WLED
 * or any DMX controller (GPIO 2 → MAX485 works fine for WLED).
 *
 * Workflow:
 *   1. Power up ESP32 — connect to Wi-Fi AP "LuminoBridge" / "12345678"
 *   2. Open http://192.168.4.1 in a browser
 *   3. Press UNLOCK — sends the proprietary unlock sequence to the fixtures
 *   4. Press START  — assigns DMX addresses 1-512 sequentially
 *   5. Connect WLED (or any DMX controller) and enjoy!
 *
 * Repository: https://github.com/YOUR_USERNAME/lumino-bridge
 * License:    MIT
 */

#include <WiFi.h>
#include <WebServer.h>

// =============================================================================
// HARDWARE PINS
// =============================================================================

#define PIN_TX    17   // UART1 TX → MAX485 DI
#define PIN_DE     4   // MAX485 DE/RE direction control
#define PIN_STOP   0   // Optional physical stop button (active LOW)

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================

#define DMX_BAUD        250000   // DMX512 baud rate
#define DMX_ADDRESSES      512   // Total fixture addresses to assign
#define DMX_CHANNELS         4   // Channels per fixture (R, G, B, W)
#define DMX_FRAME_SIZE    2048   // 512 fixtures × 4 channels

// Timing constants (µs) — reverse-engineered from UCS512C protocol captures
#define T_BREAK_PRE_00      68   // Line HIGH before BREAK leading to 0x00 blocks
#define T_MAB              149   // Mark After Break
#define T_INTER_FRAME      174   // Low gap between DMX frames
#define T_BREAK_PRE_FF     155   // Line HIGH before BREAK leading to 0xFF block
#define T_POST_FF_1         12   // First flush delay after 0xFF transmission
#define T_POST_FF_2         12   // Second flush delay after 0xFF transmission

// Unlock stub timings
#define T_STUB1_LOW       2000   // Line LOW after stub 1 (2 s)
#define T_STUB2_LOW       1500   // Line LOW after stub 2 (1.5 s)

// Address assignment
#define MARKER_BYTE       0x7F   // Value placed at target fixture position during addressing
#define PAUSE_BETWEEN       20   // ms pause between two sendAddress() frames

// =============================================================================
// WI-FI / WEB SERVER
// =============================================================================

const char* ssid     = "LuminoBridge";
const char* password = "12345678";

WebServer server(80);

// =============================================================================
// STATE
// =============================================================================

volatile bool stopRequested  = false;
bool          isUnlocking    = false;
bool          isAddressing   = false;
bool          unlockDone     = false;
int           currentAddress = 0;
String        statusMsg      = "READY";

// DMX output buffer  (static — allocated once)
static uint8_t dmxBuf[DMX_FRAME_SIZE];

// =============================================================================
// HTML WEB INTERFACE
// =============================================================================

const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Lumino Bridge</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: 'Segoe UI', sans-serif; background: #0d1117; color: #e6edf3;
           display: flex; flex-direction: column; align-items: center;
           min-height: 100vh; padding: 24px 16px; }
    h1   { font-size: 1.8rem; margin-bottom: 4px; color: #58a6ff; }
    .sub { color: #8b949e; font-size: .85rem; margin-bottom: 28px; }
    .card { background: #161b22; border: 1px solid #30363d; border-radius: 12px;
            padding: 20px 24px; width: 100%; max-width: 420px; margin-bottom: 16px; }
    .status-label { color: #8b949e; font-size: .75rem; text-transform: uppercase;
                    letter-spacing: .08em; margin-bottom: 6px; }
    #status { font-size: 1.1rem; font-weight: 600; min-height: 1.4em; color: #3fb950; }
    #address { color: #8b949e; font-size: .85rem; margin-top: 4px; }
    .btn-row { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 16px; }
    button { flex: 1; min-width: 80px; padding: 10px 14px; border: none;
             border-radius: 8px; font-size: .95rem; font-weight: 600;
             cursor: pointer; transition: opacity .15s; }
    button:hover { opacity: .85; }
    #btn-unlock { background: #1f6feb; color: #fff; }
    #btn-start  { background: #3fb950; color: #000; }
    #btn-stop   { background: #da3633; color: #fff; }
    #btn-reset  { background: #30363d; color: #e6edf3; }
    .info { color: #8b949e; font-size: .82rem; line-height: 1.55; }
    a { color: #58a6ff; }
  </style>
</head>
<body>
  <h1>&#9889; Lumino Bridge</h1>
  <p class="sub">UCS512C RS-485 fixture programmer</p>

  <div class="card">
    <div class="status-label">Status</div>
    <div id="status">READY</div>
    <div id="address"></div>
    <div class="btn-row">
      <button id="btn-unlock" onclick="send('/unlock')">UNLOCK</button>
      <button id="btn-start"  onclick="send('/start')">START</button>
      <button id="btn-stop"   onclick="send('/stop')">STOP</button>
      <button id="btn-reset"  onclick="send('/reset')">RESET</button>
    </div>
  </div>

  <div class="card info">
    <strong style="color:#e6edf3">How to use:</strong><br>
    1. Press <strong>UNLOCK</strong> — sends unlock sequence (~5 s)<br>
    2. Press <strong>START</strong>  — assigns addresses 1–512<br>
    3. Connect WLED / DMX controller &amp; enjoy 🎉<br><br>
    Fixtures must be <em>powered and in factory state</em> before unlock.
  </div>

  <script>
    function send(path) {
      fetch(path).catch(() => {});
    }
    function poll() {
      fetch('/status')
        .then(r => r.json())
        .then(d => {
          document.getElementById('status').textContent  = d.msg;
          document.getElementById('address').textContent =
            d.address > 0 ? 'Address: ' + d.address + ' / 512' : '';
        })
        .catch(() => {});
    }
    setInterval(poll, 600);
    poll();
  </script>
</body>
</html>
)rawliteral";

// =============================================================================
// LOW-LEVEL BUS CONTROL
// =============================================================================

/** Pull bus HIGH (idle / transmit-enable) */
static inline void lineHigh() {
  digitalWrite(PIN_DE, HIGH);
}

/** Pull bus LOW (break condition) */
static inline void lineLow() {
  digitalWrite(PIN_DE, LOW);
}

// =============================================================================
// DMX FRAME TRANSMISSION
// =============================================================================

/**
 * Send one raw DMX frame (BREAK + MAB + data).
 *
 * Timing for the "0x00-block" variant (used in transition frames):
 *   T_BREAK_PRE_00 µs HIGH → BREAK (line LOW via DE) →
 *   T_MAB µs HIGH → data bytes → flush +12 µs → T_INTER_FRAME µs LOW
 */
static void sendDMXFrame(const uint8_t* data, size_t len) {
  // Pre-BREAK HIGH
  lineHigh();
  delayMicroseconds(T_BREAK_PRE_00);

  // BREAK: pull line LOW long enough for receivers to detect it
  // We achieve this by transmitting a 0x00 at DMX baud while DE is LOW
  lineLow();
  Serial1.write((uint8_t)0x00);
  Serial1.flush();
  delayMicroseconds(12);   // flush settle

  // MAB (Mark After Break)
  lineHigh();
  delayMicroseconds(T_MAB);

  // Data bytes
  Serial1.write(data, len);
  Serial1.flush();
  delayMicroseconds(12);   // flush settle

  // Inter-frame gap
  lineLow();
  delayMicroseconds(T_INTER_FRAME);
}

// =============================================================================
// UNLOCK SEQUENCE
// =============================================================================

/**
 * Send the "all-0x00 transition block" — a burst of DMX frames where
 * all channels carry 0x00, sandwiched by the special HIGH/LOW pattern
 * that the UCS512C interprets as "prepare for address programming".
 */
static void sendTransitionBlock(bool isFinalStub) {
  // Four frames of all-zeros payload
  memset(dmxBuf, 0x00, DMX_FRAME_SIZE);

  for (int i = 0; i < 4; i++) {
    if (stopRequested) return;
    sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  }

  if (isFinalStub) return;   // caller handles the tail timing

  // Inter-stub pause
  lineHigh();
}

/**
 * Send the "0xFF stub" — a single DMX frame with all channels at 0xFF,
 * preceded by its own specific BREAK timing.
 */
static void sendFFStub() {
  // Pre-BREAK HIGH (different duration from normal frames)
  lineHigh();
  delayMicroseconds(T_BREAK_PRE_FF);

  // BREAK
  lineLow();
  Serial1.write((uint8_t)0x00);
  Serial1.flush();
  delayMicroseconds(T_POST_FF_1);

  // MAB
  lineHigh();
  delayMicroseconds(T_MAB);

  // Data: all 0xFF
  memset(dmxBuf, 0xFF, DMX_FRAME_SIZE);
  Serial1.write(dmxBuf, DMX_FRAME_SIZE);
  Serial1.flush();
  delayMicroseconds(T_POST_FF_2);
}

/**
 * Send the 6 closing init frames required before address assignment begins.
 * Pattern: 3 frames of all-0x00, then 3 frames where:
 *   - fixture at address 1 gets {127, 127, 127, 127}
 *   - all other positions get  {0, 0, 0, 127}
 */
static void sendClosingFrames() {
  // 3 × all-zero frames
  memset(dmxBuf, 0x00, DMX_FRAME_SIZE);
  for (int i = 0; i < 3; i++) {
    if (stopRequested) return;
    sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  }

  // 3 × "marker" frames
  memset(dmxBuf, 0x00, DMX_FRAME_SIZE);
  // Fixture 1 (position 0): all channels 127
  dmxBuf[0] = 127; dmxBuf[1] = 127; dmxBuf[2] = 127; dmxBuf[3] = 127;
  // Remaining 511 fixtures: W=127, RGB=0
  for (int f = 1; f < DMX_ADDRESSES; f++) {
    int pos = f * DMX_CHANNELS;
    dmxBuf[pos + 0] = 0;
    dmxBuf[pos + 1] = 0;
    dmxBuf[pos + 2] = 0;
    dmxBuf[pos + 3] = 127;
  }
  for (int i = 0; i < 3; i++) {
    if (stopRequested) return;
    sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  }
}

/**
 * Send 6 init frames before the address-assignment loop.
 * Same as sendClosingFrames — fixtures need this to enter receive mode.
 */
static void send6InitFrames() {
  sendClosingFrames();
}

/**
 * Full unlock sequence (two stubs + closing frames).
 * Must complete successfully before address assignment can begin.
 */
static void runUnlock() {
  statusMsg = "UNLOCK: STUB 1...";

  // ── STUB 1 ────────────────────────────────────────────────────────────────
  sendTransitionBlock(false);
  if (stopRequested) return;

  sendFFStub();
  if (stopRequested) return;

  // Long LOW pause after stub 1
  lineLow();
  delay(T_STUB1_LOW);         // 2 s
  if (stopRequested) return;

  // ── STUB 2 ────────────────────────────────────────────────────────────────
  statusMsg = "UNLOCK: STUB 2...";

  sendTransitionBlock(false);
  if (stopRequested) return;

  sendFFStub();
  if (stopRequested) return;

  // Shorter LOW pause after stub 2
  lineLow();
  delay(T_STUB2_LOW);         // 1.5 s
  if (stopRequested) return;

  // ── CLOSING FRAMES ────────────────────────────────────────────────────────
  lineHigh();
  statusMsg = "UNLOCK: CLOSING...";
  sendClosingFrames();
  if (stopRequested) return;

  lineHigh();
  statusMsg      = "UNLOCK DONE — press START";
  unlockDone     = true;
  isUnlocking    = false;
}

// =============================================================================
// ADDRESS ASSIGNMENT
// =============================================================================

/**
 * Send a single address-programming frame for `address`.
 * The target fixture recognises its position in the frame (marker byte at
 * offset (address-1)*DMX_CHANNELS) and latches that address into EEPROM.
 * Two identical frames are sent for reliability, separated by 2 ms.
 */
static void sendAddress(int address) {
  memset(dmxBuf, 0x00, DMX_FRAME_SIZE);
  int pos = (address - 1) * DMX_CHANNELS;
  if (pos + DMX_CHANNELS <= DMX_FRAME_SIZE) {
    for (int i = 0; i < DMX_CHANNELS; i++) {
      dmxBuf[pos + i] = MARKER_BYTE;
    }
  }
  sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  if (stopRequested) return;
  delayMicroseconds(2000);
  sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  if (stopRequested) return;
  delay(PAUSE_BETWEEN);
}

// =============================================================================
// WEB SERVER HANDLERS
// =============================================================================

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleUnlock() {
  if (!isUnlocking && !isAddressing) {
    isUnlocking    = true;
    unlockDone     = false;
    stopRequested  = false;
    currentAddress = 0;
    statusMsg      = "UNLOCKING...";
  }
  server.send(200, "text/plain", "OK");
}

void handleStart() {
  if (unlockDone && !isAddressing) {
    isAddressing   = true;
    stopRequested  = false;
    currentAddress = 1;
    statusMsg      = "ADDRESSING...";
  }
  server.send(200, "text/plain", "OK");
}

void handleStop() {
  stopRequested = true;
  isUnlocking   = false;
  isAddressing  = false;
  statusMsg     = "STOPPED";
  server.send(200, "text/plain", "OK");
}

void handleReset() {
  stopRequested  = true;
  isUnlocking    = false;
  isAddressing   = false;
  unlockDone     = false;
  currentAddress = 0;
  statusMsg      = "READY";
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  String json = "{\"running\":"  + String((isUnlocking || isAddressing) ? "true" : "false") +
                ",\"address\":"  + String(currentAddress) +
                ",\"msg\":\""    + statusMsg + "\"}";
  server.send(200, "application/json", json);
}

// =============================================================================
// SETUP & MAIN LOOP
// =============================================================================

void setup() {
  Serial.begin(115200);

  // DE pin: starts HIGH = transmitter enabled, bus idle (line HIGH)
  pinMode(PIN_DE, OUTPUT);
  lineHigh();

  // Optional physical stop button (active LOW with internal pull-up)
  pinMode(PIN_STOP, INPUT_PULLUP);

  // UART1 for DMX: 8N2, no RX
  Serial1.begin(DMX_BAUD, SERIAL_8N2, -1, PIN_TX);

  // Wi-Fi access point
  WiFi.softAP(ssid, password);
  Serial.println("AP: " + String(ssid));
  Serial.println("URL: http://" + WiFi.softAPIP().toString());

  // Web routes
  server.on("/",       handleRoot);
  server.on("/unlock", handleUnlock);
  server.on("/start",  handleStart);
  server.on("/stop",   handleStop);
  server.on("/reset",  handleReset);
  server.on("/status", handleStatus);
  server.begin();

  Serial.println("=== Lumino Bridge ready ===");
}

void loop() {
  server.handleClient();

  // Physical stop button (debounced)
  if (digitalRead(PIN_STOP) == LOW) {
    delay(50);
    if (digitalRead(PIN_STOP) == LOW) {
      stopRequested = true;
      isUnlocking   = false;
      isAddressing  = false;
      statusMsg     = "STOPPED";
    }
  }

  if (isUnlocking && !stopRequested) {
    runUnlock();
  }

  if (isAddressing && !stopRequested) {
    statusMsg = "ADDRESSING: INIT...";
    send6InitFrames();
    if (stopRequested) { isAddressing = false; return; }

    statusMsg = "ADDRESSING...";
    for (int addr = currentAddress; addr <= DMX_ADDRESSES; addr++) {
      server.handleClient();
      if (stopRequested) {
        currentAddress = addr;
        isAddressing   = false;
        return;
      }
      currentAddress = addr;
      sendAddress(addr);
    }

    isAddressing   = false;
    unlockDone     = false;
    currentAddress = DMX_ADDRESSES;
    statusMsg      = "DONE! All fixtures addressed.";
  }
}
