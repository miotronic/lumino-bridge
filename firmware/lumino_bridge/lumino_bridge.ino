// =============================================================================
// LUMINO BRIDGE — UCS512C DMX Addressing Firmware
// =============================================================================
//
// What this does:
//   Sends a special "unlock" sequence to UCS512C LED controllers (used in
//   cheap Chinese DMX wall washers), then assigns DMX addresses to each
//   fixture one by one. After addressing, fixtures work with any standard
//   DMX controller or WLED.
//
// Hardware required:
//   - ESP32 (any standard variant)
//   - MAX485 (RS-485 transceiver module, ~$0.50)
//
// Wiring:
//   ESP32 GPIO 17  →  MAX485 DI  (data in)
//   ESP32 GPIO 4   →  MAX485 DE + RE (tied together, direction control)
//   ESP32 GPIO 0   →  Pushbutton to GND (emergency stop, optional)
//   MAX485 A/B     →  RS-485 bus to fixtures
//
// How to use:
//   1. Flash this firmware to ESP32
//   2. Connect to WiFi "LuminoBridge" (password: lumino123)
//   3. Open browser → 192.168.4.1
//   4. Press UNLOCK — waits for fixtures to enter programming mode (~15 sec)
//   5. Press START  — assigns addresses 1, 2, 3... to each fixture in line
//   6. Done! Fixtures are now addressable via standard DMX512
//
// =============================================================================

#include <Arduino.h>
#include "driver/uart.h"
#include <WiFi.h>
#include <WebServer.h>

// ─── Pin definitions ──────────────────────────────────────────────────────────

#define PIN_TX    17    // UART TX → MAX485 DI (sends data to fixtures)
#define PIN_DE     4    // MAX485 DE+RE: HIGH = transmit, LOW = idle/listen
#define PIN_STOP   0    // Optional emergency stop button (active LOW)

// ─── WiFi credentials ─────────────────────────────────────────────────────────

const char* ssid     = "LuminoBridge";
const char* password = "lumino123";

// ─── DMX / Protocol constants ─────────────────────────────────────────────────

#define DMX_BAUD        250000  // Standard DMX baud rate — do not change
#define DMX_CHANNELS         4  // Channels per fixture: R, G, B, W
#define DMX_ADDRESSES      512  // Max fixtures on one DMX universe
#define MARKER_BYTE       0x7F  // Value (127) written to mark the addressed fixture
#define DMX_FRAME_SIZE    1542  // Total bytes per DMX frame (start byte + channel data)

// Timing for the DMX break signal
// A "break" is a long LOW pulse that tells all fixtures a new frame is starting
#define BREAK_US   120   // Break duration in microseconds (DMX spec minimum: 92µs)
#define MAB_US      12   // Mark After Break — short HIGH pause after break

// Timing between frames
#define INIT_PAUSE    250   // 250ms pause between init frames during unlock sequence
#define PAUSE_BETWEEN 290   // 290ms pause between individual address assignments

// ─── Unlock sequence lookup tables ───────────────────────────────────────────
//
// The UCS512C chip requires a proprietary unlock sequence before it accepts
// DMX addressing commands. This was reverse-engineered from the K1000C
// programmer using a logic analyzer — these values were captured directly
// from the RS-485 bus and confirmed to work.
//
// Each "transition block" sends 512 packets of 8 bytes.
// Bytes 1-6 are fixed magic values.
// Bytes 7 and 8 cycle through these tables (32 × 16 = 512 combinations).
//
const uint8_t byte7vals[32] = {
  1,9,17,25,33,41,49,57,65,73,81,89,97,105,113,121,
  129,137,145,153,161,169,177,185,193,201,209,217,225,233,241,249
};
const uint8_t byte8vals[16] = {
  2,34,18,50,10,42,26,58,6,38,22,54,14,46,30,62
};

// ─── Global state variables ───────────────────────────────────────────────────

WebServer server(80);
static uint8_t dmxBuf[DMX_FRAME_SIZE];  // Reusable buffer for building DMX frames

static bool stopRequested  = false;  // Goes true when user presses STOP
static bool isUnlocking    = false;  // True while unlock sequence is running
static bool isAddressing   = false;  // True while address assignment is running
static bool unlockDone     = false;  // True after unlock completes successfully
static int  currentAddress = 0;      // Which fixture we are currently addressing
static String statusMsg    = "IDLE"; // Displayed on the web interface

// ─── Web interface HTML ───────────────────────────────────────────────────────

const char* htmlPage = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1'>
  <title>Lumino Bridge</title>
  <style>
    body{font-family:Arial;background:#1a1a2e;color:white;text-align:center;padding:20px}
    h1{color:#e94560;margin-bottom:5px}
    .subtitle{color:#888;font-size:14px;margin-bottom:20px}

    /* Status banner */
    .banner{font-size:18px;margin:15px auto;padding:15px;border-radius:10px;max-width:400px}
    .banner-red  {background:#4a1a1a;border:2px solid #e94560;color:#e94560}
    .banner-yellow{background:#4a3a00;border:2px solid #f39c12;color:#f39c12}
    .banner-green{background:#1a4a1a;border:2px solid #2ecc71;color:#2ecc71}

    /* Progress bar */
    .progress-wrap{background:#16213e;border-radius:10px;height:20px;margin:10px auto;max-width:400px;overflow:hidden;display:none}
    .progress-bar{height:100%;width:0%;background:#e67e22;border-radius:10px;transition:width 0.3s}
    .progress-bar.indeterminate{width:40%;animation:slide 1.5s infinite ease-in-out}
    @keyframes slide{0%{margin-left:-40%}100%{margin-left:100%}}

    /* Address counter */
    .addr-box{font-size:18px;margin:10px;padding:10px;background:#16213e;border-radius:10px}
    .addr-num{font-size:48px;color:#e94560;font-weight:bold}

    /* Buttons */
    .btn{padding:18px 40px;font-size:20px;border:none;border-radius:10px;cursor:pointer;margin:8px;width:80%;display:block;margin-left:auto;margin-right:auto}
    .unlock{background:#e67e22;color:white}
    .start {background:#0f3460;color:white}
    .stop  {background:#e94560;color:white}
    .reset {background:#533483;color:white}
    .btn:disabled{opacity:0.3;cursor:not-allowed}
  </style>
  <script>
    function updateStatus(){
      fetch('/status').then(r=>r.json()).then(d=>{
        var msg        = d.msg;
        var running    = d.running;
        var unlocking  = running && (msg.indexOf('UNLOCK')>=0 || msg.indexOf('BLOCK')>=0 || msg.indexOf('CLOSING')>=0);
        var addressing = running && msg.indexOf('ADDRESS')>=0;
        var ready      = msg.indexOf('READY')>=0;
        var done       = msg.indexOf('DONE')>=0;
        var stopped    = msg.indexOf('STOP')>=0 || msg.indexOf('IDLE')>=0 || msg.indexOf('RESET')>=0;

        // Update status text
        document.getElementById('statusText').innerText = msg;
        document.getElementById('addr').innerText = d.address;

        // Banner logic
        var banner = document.getElementById('banner');
        if(ready){
          banner.className='banner banner-green';
          banner.innerHTML='&#128994; Chip unlocked! Press START to address fixtures.';
        } else if(unlocking){
          banner.className='banner banner-yellow';
          banner.innerHTML='&#9203; Unlocking chip, please wait...';
        } else if(addressing){
          banner.className='banner banner-yellow';
          banner.innerHTML='&#9203; Addressing fixtures...';
        } else if(done){
          banner.className='banner banner-green';
          banner.innerHTML='&#128994; All fixtures addressed successfully!';
        } else {
          banner.className='banner banner-red';
          banner.innerHTML='&#128308; Please press UNLOCK before addressing fixtures.';
        }

        // Progress bar
        var pw = document.getElementById('progressWrap');
        var pb = document.getElementById('progressBar');
        if(unlocking || addressing){
          pw.style.display='block';
          pb.className='progress-bar indeterminate';
        } else if(done){
          pw.style.display='block';
          pb.className='progress-bar';
          pb.style.width='100%';
          pb.style.background='#2ecc71';
        } else {
          pw.style.display='none';
          pb.style.width='0%';
          pb.style.background='#e67e22';
          pb.className='progress-bar';
        }

        // Button states
        // On load (idle/reset): only UNLOCK enabled
        // While unlocking: only STOP enabled
        // Ready: START + STOP + RESET enabled, UNLOCK disabled
        // While addressing: only STOP enabled
        // Done: RESET enabled
        document.getElementById('btnUnlock').disabled = unlocking || addressing || ready || done;
        document.getElementById('btnStart').disabled  = unlocking || addressing || !ready;
        document.getElementById('btnStop').disabled   = !running;
        document.getElementById('btnReset').disabled  = unlocking || addressing;
      });
    }
    function unlock(){ fetch('/unlock').then(()=>updateStatus()) }
    function start(){  fetch('/start').then(()=>updateStatus())  }
    function stop(){   fetch('/stop').then(()=>updateStatus())   }
    function reset(){  fetch('/reset').then(()=>updateStatus())  }
    setInterval(updateStatus, 500);
    updateStatus();
  </script>
</head>
<body>
  <h1>&#128275; Lumino Bridge</h1>
  <p class='subtitle'>UCS512C DMX Fixture Programmer</p>

  <div class='banner banner-red' id='banner'>
    &#128308; Please press UNLOCK before addressing fixtures.
  </div>

  <div class='progress-wrap' id='progressWrap'>
    <div class='progress-bar' id='progressBar'></div>
  </div>

  <div class='addr-box'>
    Address: <span class='addr-num' id='addr'>0</span> / 512
    <br><small id='statusText' style='color:#888'>IDLE</small>
  </div>

  <button class='btn unlock' id='btnUnlock' onclick='unlock()'>&#128275; UNLOCK</button>
  <button class='btn start'  id='btnStart'  onclick='start()'  disabled>&#9654; START</button>
  <button class='btn stop'   id='btnStop'   onclick='stop()'   disabled>&#9632; STOP</button>
  <button class='btn reset'  id='btnReset'  onclick='reset()'  disabled>&#8635; RESET</button>
</body>
</html>
)";

// =============================================================================
// LOW-LEVEL SIGNAL HELPERS
// =============================================================================

// Put the RS-485 bus in idle state (HIGH level, no transmission)
// DE LOW = MAX485 transmitter disabled, bus floats to idle HIGH
static inline void lineHigh() {
  digitalWrite(PIN_DE, LOW);
}

// Send a DMX break + mark-after-break (MAB)
// Every DMX frame must start with this — it resets all fixtures and
// tells them to read the data that follows
static void sendBreak() {
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);   // Force TX line LOW (break)
  delayMicroseconds(BREAK_US);                                // Hold LOW for break duration
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);// Release back to normal
  delayMicroseconds(MAB_US);                                  // Short HIGH pulse (MAB)
}

// Send one complete DMX frame over RS-485
//
// IMPORTANT — why the 12µs delay after flush():
//   Serial.flush() returns when the TX buffer is empty, but the UART
//   hardware is still physically sending the last byte's stop bits.
//   If we pull DE LOW too early, the stop bits get cut off and the
//   fixture sees a framing error on the last byte.
//   12µs = time for 2 stop bits at 250kbaud (2 × 4µs = 8µs + margin).
//
static void sendDMXFrame(uint8_t* buf, int len) {
  digitalWrite(PIN_DE, HIGH);    // Enable transmitter
  delayMicroseconds(5);          // Short settle time for MAX485
  sendBreak();                   // Break + MAB
  Serial1.write(buf, len);       // Send data
  Serial1.flush();               // Wait for TX buffer to empty
  delayMicroseconds(12);         // Wait for last stop bits to finish (see note above)
  digitalWrite(PIN_DE, LOW);     // Disable transmitter, bus goes idle
}

// =============================================================================
// INIT FRAME SEQUENCES
// =============================================================================

// Send 6 all-zero DMX frames with 250ms gaps
// This is the "hello, I am a DMX controller" signal that wakes up fixtures
// Used at the start of unlock AND before address assignment
static void send6InitFrames() {
  uint8_t buf[DMX_FRAME_SIZE];
  memset(buf, 0x00, DMX_FRAME_SIZE);
  for (int i = 0; i < 6; i++) {
    if (stopRequested) return;
    sendDMXFrame(buf, DMX_FRAME_SIZE);
    delay(INIT_PAUSE);
  }
}

// Send the 6 closing frames that complete the unlock sequence
//
// First 3 frames: all zeros (same as init frames)
// Last 3 frames:  special pattern reverse-engineered from K1000C:
//                 - Address 1: all 4 channels (R,G,B,W) = 127
//                 - Addresses 2-512: only W channel = 127, RGB = 0
//
// This specific pattern signals to fixtures that the unlock handshake
// is finished and they should enter address-receive mode.
//
static void sendClosingFrames() {

  // ── First 3 frames: all zeros ──────────────────────────────────────────────
  uint8_t zeroBuf[DMX_FRAME_SIZE];
  memset(zeroBuf, 0x00, DMX_FRAME_SIZE);
  for (int i = 0; i < 3; i++) {
    if (stopRequested) return;
    sendDMXFrame(zeroBuf, DMX_FRAME_SIZE);
    delay(INIT_PAUSE);
  }

  // ── Last 3 frames: closing pattern ────────────────────────────────────────
  uint8_t wBuf[DMX_FRAME_SIZE];
  memset(wBuf, 0x00, DMX_FRAME_SIZE);

  // Address 1 (byte positions 1-4): all channels = 127
  if (1 + DMX_CHANNELS <= DMX_FRAME_SIZE) {
    wBuf[1] = 127;  // R
    wBuf[2] = 127;  // G
    wBuf[3] = 127;  // B
    wBuf[4] = 127;  // W
  }

  // Addresses 2 through 512: only W = 127, RGB = 0
  for (int addr = 1; addr < DMX_ADDRESSES; addr++) {
    int pos = 1 + addr * DMX_CHANNELS;  // +1 = skip the DMX start code byte
    if (pos + DMX_CHANNELS <= DMX_FRAME_SIZE) {
      wBuf[pos]     = 0;    // R = off
      wBuf[pos + 1] = 0;    // G = off
      wBuf[pos + 2] = 0;    // B = off
      wBuf[pos + 3] = 127;  // W = 50%
    }
  }

  for (int i = 0; i < 3; i++) {
    if (stopRequested) return;
    sendDMXFrame(wBuf, DMX_FRAME_SIZE);
    delay(INIT_PAUSE);
  }
}

// =============================================================================
// TRANSITION BLOCK — core of the unlock handshake
// =============================================================================
//
// Sends a burst of 512 data packets that forms one half of the unlock
// handshake. Called twice: mode=1 for block 1, mode=3 for block 2.
//
// Wire-level structure:
//   bus HIGH ~119µs → 0x00 byte → bus HIGH ~162µs → 512 × [8-byte packet]
//
// Each 8-byte packet: { 85, 15, 44, 170, 53, mode, byte7, byte8 }
//   byte7 = byte7vals[i]  (i = 0..31)
//   byte8 = byte8vals[j]  (j = 0..15)
//   32 × 16 = 512 packets total
//
// ── NOTE ON TIMING COMPENSATION ────────────────────────────────────────────
//   All delayMicroseconds() values are intentionally lower than the target
//   wire timings. This is because ESP32 GPIO and UART operations add ~25-50µs
//   of overhead on top of the delay. Values were tuned with a logic analyzer
//   to produce the correct timings on the physical RS-485 bus.
//   DO NOT change these values without re-measuring with an analyzer.
//
// ── NOTE ON LAST PACKET ────────────────────────────────────────────────────
//   The last packet (i=31, j=15) skips the inter-packet gap delay.
//   The calling function (runUnlock) is responsible for the timing
//   that follows the last packet.
//
static void sendTransitionBlock(uint8_t mode) {

  // Lead-in: bus HIGH ~119µs → send 0x00 sync byte → bus HIGH ~162µs
  digitalWrite(PIN_DE, LOW);        // Bus HIGH (idle)
  delayMicroseconds(68);            // 68µs + ~51µs overhead = ~119µs on wire
  digitalWrite(PIN_DE, HIGH);       // Enable transmitter

  uint8_t zero = 0x00;
  Serial1.write(&zero, 1);          // Send 0x00 sync byte
  Serial1.flush();
  delayMicroseconds(12);            // Wait for stop bits
  digitalWrite(PIN_DE, LOW);        // Bus HIGH (MAB)
  delayMicroseconds(149);           // 149µs + ~37µs overhead = ~162µs on wire (tuned)

  // Send 512 data packets
  uint8_t seq[8] = {85, 15, 44, 170, 53, mode, 0, 0};

  for (int i = 0; i < 32; i++) {
    for (int j = 0; j < 16; j++) {
      if (stopRequested) return;

      seq[6] = byte7vals[i];
      seq[7] = byte8vals[j];

      digitalWrite(PIN_DE, HIGH);   // Enable transmitter
      Serial1.write(seq, 8);        // Send 8-byte packet
      Serial1.flush();
      delayMicroseconds(12);        // Wait for stop bits

      digitalWrite(PIN_DE, LOW);    // Bus HIGH (inter-packet gap)

      // All packets except the last get a 174µs gap
      // Last packet: no gap — caller controls what happens next
      if (!(i == 31 && j == 15)) {
        delayMicroseconds(174);
      }
    }
  }
  // Returns with DE LOW (bus idle), no trailing delay
}

// =============================================================================
// FULL UNLOCK SEQUENCE
// =============================================================================
//
// Complete proprietary handshake to put UCS512C fixtures into addressing mode.
// Reverse-engineered from the K1000C programmer.
//
// Total duration: approximately 15 seconds
//
// Sequence:
//   1. 6× all-zero init frames (250ms gaps between each)
//   2. Bus HIGH 250ms → LOW 2s  (long low = "wake up" pulse)
//   3. Transition block 1 (mode=1)
//      → bus HIGH ~189µs → send 0xFF → bus HIGH ~70µs → LOW 2s
//   4. Transition block 2 (mode=3)
//      → bus HIGH ~189µs → LOW 1.5s
//   5. 6× closing frames (3× all-zero + 3× W=127 pattern)
//   → Fixtures are now ready for address assignment
//
static void runUnlock() {
  lineHigh();
  statusMsg = "UNLOCK: INIT...";

  // Step 1: 6 all-zero init frames
  send6InitFrames();
  if (stopRequested) return;

  // Step 2: bus HIGH 250ms → LOW 2s
  // Note: delay(250) is commented out because send6InitFrames() already
  // ends with a 250ms INIT_PAUSE gap. Adding another 250ms would give
  // 500ms total instead of the required 250ms.
  // delay(250);
  digitalWrite(PIN_DE, HIGH);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);   // Force bus LOW
  delay(2000);                                                // Hold LOW for 2 seconds
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);

  // Step 3: Transition block 1
  statusMsg = "UNLOCK: BLOCK 1...";
  sendTransitionBlock(1);
  if (stopRequested) return;

  // End of block 1: bus HIGH ~189µs → 0xFF marker → bus HIGH ~70µs → LOW 2s
  // The 0xFF byte is a specific end-of-block marker required by the UCS512C.
  // Timing note: values tuned with logic analyzer (see sendTransitionBlock notes).
  digitalWrite(PIN_DE, LOW);
  delayMicroseconds(155);           // ~155µs + overhead = ~189µs on wire

  uint8_t ff = 255;                 // 0xFF = end-of-block marker
  digitalWrite(PIN_DE, HIGH);
  Serial1.write(&ff, 1);
  Serial1.flush();
  delayMicroseconds(12);            // Wait for stop bits
  digitalWrite(PIN_DE, LOW);
  delayMicroseconds(12);            // ~12µs + ~47µs overhead = ~70µs on wire

  // LOW 2s after block 1
  digitalWrite(PIN_DE, HIGH);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);
  delay(2000);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);

  // Step 4: Transition block 2
  statusMsg = "UNLOCK: BLOCK 2...";
  sendTransitionBlock(3);
  if (stopRequested) return;

  // End of block 2: bus HIGH ~189µs → LOW 1.5s (no 0xFF this time)
  // 161µs + ~28µs GPIO overhead = ~189µs on wire (tuned with analyzer)
  digitalWrite(PIN_DE, LOW);
  delayMicroseconds(161);
  digitalWrite(PIN_DE, HIGH);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_TXD_INV);
  delay(1500);
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_INV_DISABLE);

  // Step 5: Closing frames
  lineHigh();
  statusMsg = "UNLOCK: CLOSING...";
  sendClosingFrames();
  if (stopRequested) return;

  // Done — fixtures are now waiting for address assignment
  lineHigh();
  statusMsg = "READY — PRESS START";
  unlockDone  = true;
  isUnlocking = false;
}

// =============================================================================
// ADDRESS ASSIGNMENT
// =============================================================================
//
// After unlock, this assigns a unique DMX address to each fixture.
// Fixtures must be connected one at a time in the order you want them numbered.
//
// How it works:
//   We send a DMX frame where only one address slot has a non-zero value (0x7F).
//   The first unaddressed fixture on the bus "claims" that slot as its address.
//   We repeat for addresses 1, 2, 3... up to 512.
//
//   Each address is sent twice for reliability, with a 290ms pause between
//   addresses to give the fixture time to store the address in memory.
//
static void sendAddress(int address) {
  // Build DMX frame: all zeros except the target address slot = 0x7F
  memset(dmxBuf, 0x00, DMX_FRAME_SIZE);
  int pos = (address - 1) * DMX_CHANNELS;  // Byte offset in the frame
  if (pos + DMX_CHANNELS <= DMX_FRAME_SIZE) {
    for (int i = 0; i < DMX_CHANNELS; i++) {
      dmxBuf[pos + i] = MARKER_BYTE;  // 0x7F = "claim this address"
    }
  }

  // Send the frame twice (confirmed more reliable than once)
  sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  if (stopRequested) return;
  delayMicroseconds(2000);                // 2ms gap between the two sends
  sendDMXFrame(dmxBuf, DMX_FRAME_SIZE);
  if (stopRequested) return;
  delay(PAUSE_BETWEEN);                   // 290ms before moving to next address
}

// =============================================================================
// WEB SERVER HANDLERS
// =============================================================================

void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

// UNLOCK: start the unlock sequence (ignored if already running)
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

// START: begin address assignment (only works after a successful unlock)
void handleStart() {
  if (unlockDone && !isAddressing) {
    isAddressing   = true;
    stopRequested  = false;
    currentAddress = 1;
    statusMsg      = "ADDRESSING...";
  }
  server.send(200, "text/plain", "OK");
}

// STOP: halt any running operation safely
void handleStop() {
  stopRequested = true;
  isUnlocking   = false;
  isAddressing  = false;
  statusMsg     = "STOPPED";
  server.send(200, "text/plain", "OK");
}

// RESET: return to initial state, clear all flags
void handleReset() {
  stopRequested  = true;
  isUnlocking    = false;
  isAddressing   = false;
  unlockDone     = false;
  currentAddress = 0;
  statusMsg      = "IDLE";
  server.send(200, "text/plain", "OK");
}

// STATUS: called every 500ms by the web page to refresh the display
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

  // DE pin controls MAX485 direction (HIGH = transmit, LOW = idle)
  pinMode(PIN_DE, OUTPUT);
  lineHigh();  // Start in idle state

  // Physical emergency stop button (optional, active LOW)
  pinMode(PIN_STOP, INPUT_PULLUP);

  // UART1 for DMX output
  // SERIAL_8N2 = 8 data bits, no parity, 2 stop bits — required by DMX spec
  // -1 for RX = not used (we only transmit)
  Serial1.begin(DMX_BAUD, SERIAL_8N2, -1, PIN_TX);

  // Start WiFi in Access Point mode
  WiFi.softAP(ssid, password);
  Serial.println("WiFi AP ready. SSID: " + String(ssid));
  Serial.println("Open browser: http://" + WiFi.softAPIP().toString());

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
  // Handle web requests every iteration to keep UI responsive
  server.handleClient();

  // Physical stop button check (50ms debounce)
  if (digitalRead(PIN_STOP) == LOW) {
    delay(50);
    if (digitalRead(PIN_STOP) == LOW) {
      stopRequested = true;
      isUnlocking   = false;
      isAddressing  = false;
      statusMsg     = "STOPPED";
    }
  }

  // Run unlock if requested
  if (isUnlocking && !stopRequested) {
    runUnlock();
  }

  // Run address assignment if requested (requires prior successful unlock)
  if (isAddressing && !stopRequested) {
    // Fixtures need init frames to enter address-receive mode
    statusMsg = "ADDRESSING: INIT...";
    send6InitFrames();
    if (stopRequested) { isAddressing = false; return; }

    statusMsg = "ADDRESSING...";
    for (int addr = currentAddress; addr <= DMX_ADDRESSES; addr++) {
      server.handleClient();  // Keep web UI alive during the long addressing loop
      if (stopRequested) {
        currentAddress = addr;  // Save progress so we can resume later if needed
        isAddressing   = false;
        return;
      }
      currentAddress = addr;
      sendAddress(addr);
    }

    // All done
    isAddressing   = false;
    unlockDone     = false;  // Must re-unlock before next addressing run
    statusMsg      = "DONE! All fixtures addressed.";
    currentAddress = 512;
  }
}
