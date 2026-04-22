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
#define DMX_ADDRESSES      512  // Max fixtures on one DMX universe
#define MARKER_BYTE       0x7F  // Value (127) written to mark the addressed fixture
#define DMX_FRAME_SIZE    2048  // Max buffer: 512 fixtures × 4 channels

// Timing for the DMX break signal
// A "break" is a long LOW pulse that tells all fixtures a new frame is starting
// Target wire timings (measured from K1000C with logic analyzer):
//   BREAK = 232.25µs, MAB = 84µs
// Overhead on ESP32 is ~3.5µs for BREAK, ~19µs for MAB
#define BREAK_US   228   // Break duration → ~232µs on wire
#define MAB_US      65   // Mark After Break → ~84µs on wire

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
// 4CH: nested loop — 32 byte7 values × 16 byte8 values = 512 packets total
// 3CH: flat sequence — 510 packets (groups have varying byte8 counts: 20/21/22)
//

// ── 4CH tables ──
static const uint8_t byte7vals_4ch[32] = {
  1,129,65,193,33,161,97,225,17,145,81,209,49,177,113,241,
  9,137,73,201,41,169,105,233,25,153,89,217,57,185,121,249
};
static const uint8_t byte8vals_4ch[16] = {
  2,34,18,50,10,42,26,58,6,38,22,54,14,46,30,62
};

// ── 3CH flat sequences (510 entries, direct from K1000C logic analyzer capture) ──
static const uint8_t byte7seq_3ch[510] = {
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,129,129,129,129,129,129,129,129,129,129,
  129,129,129,129,129,129,129,129,129,129,129,129,65,65,65,65,
  65,65,65,65,65,65,65,65,65,65,65,65,65,65,65,65,
  65,193,193,193,193,193,193,193,193,193,193,193,193,193,193,193,
  193,193,193,193,193,193,33,33,33,33,33,33,33,33,33,33,
  33,33,33,33,33,33,33,33,33,33,33,161,161,161,161,161,
  161,161,161,161,161,161,161,161,161,161,161,161,161,161,161,161,
  97,97,97,97,97,97,97,97,97,97,97,97,97,97,97,97,
  97,97,97,97,97,97,225,225,225,225,225,225,225,225,225,225,
  225,225,225,225,225,225,225,225,225,225,17,17,17,17,17,17,
  17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,145,
  145,145,145,145,145,145,145,145,145,145,145,145,145,145,145,145,
  145,145,145,145,145,81,81,81,81,81,81,81,81,81,81,81,
  81,81,81,81,81,81,81,81,81,81,209,209,209,209,209,209,
  209,209,209,209,209,209,209,209,209,209,209,209,209,209,209,49,
  49,49,49,49,49,49,49,49,49,49,49,49,49,49,49,49,
  49,49,49,49,49,177,177,177,177,177,177,177,177,177,177,177,
  177,177,177,177,177,177,177,177,177,177,113,113,113,113,113,113,
  113,113,113,113,113,113,113,113,113,113,113,113,113,113,241,241,
  241,241,241,241,241,241,241,241,241,241,241,241,241,241,241,241,
  241,241,241,241,9,9,9,9,9,9,9,9,9,9,9,9,
  9,9,9,9,9,9,9,9,9,137,137,137,137,137,137,137,
  137,137,137,137,137,137,137,137,137,137,137,137,137,137,73,73,
  73,73,73,73,73,73,73,73,73,73,73,73,73,73,73,73,
  73,73,73,73,201,201,201,201,201,201,201,201,201,201,201,201,
  201,201,201,201,201,201,201,201,201,41,41,41,41,41,41,41,
  41,41,41,41,41,41,41,41,41,41,41,41,41,41,169,169,
  169,169,169,169,169,169,169,169,169,169,169,169,169,169,169,169,
  169,169,169,169,105,105,105,105,105,105,105,105,105,105,105,105,
  105,105,105,105,105,105,105,105,105,233,233,233,233,233,233,233,
  233,233,233,233,233,233,233,233,233,233,233,233,233,233,
};
static const uint8_t byte8seq_3ch[510] = {
  2,194,98,146,50,242,74,170,26,218,122,134,38,230,86,182,
  14,206,110,158,62,254,66,162,18,210,22,114,138,42,234,90,
  186,6,198,102,150,54,246,78,174,30,222,126,130,34,226,82,
  178,10,202,106,154,58,250,70,166,22,214,118,142,46,238,94,
  190,194,98,146,50,242,74,170,26,218,122,134,38,230,86,182,
  14,206,110,158,62,254,66,162,18,210,114,138,42,234,90,186,
  6,198,102,150,54,246,78,174,30,222,126,130,34,226,82,178,
  10,202,106,154,58,250,70,166,22,214,118,142,46,238,94,190,
  2,194,98,146,50,242,74,170,26,218,122,134,38,230,86,182,
  14,206,110,158,62,254,66,162,18,210,114,138,42,234,90,186,
  6,198,102,150,54,246,78,174,30,126,130,34,226,82,178,10,
  202,106,154,58,250,70,166,22,214,118,142,46,238,94,190,2,
  194,98,146,50,242,74,170,26,218,122,134,38,230,86,182,14,
  206,110,158,62,254,66,162,18,210,114,138,42,234,90,186,6,
  198,102,150,54,246,78,174,30,222,126,130,34,226,82,178,10,
  202,106,154,58,250,70,166,22,214,118,142,46,238,94,190,2,
  194,98,146,50,242,74,170,26,218,122,134,38,230,86,182,14,
  206,110,158,62,254,66,162,18,210,114,138,42,234,90,186,6,
  198,102,150,54,246,78,174,30,222,126,130,34,226,82,178,10,
  202,106,154,58,250,70,166,214,118,142,46,238,94,190,2,194,
  98,146,50,242,74,170,26,218,122,134,38,230,86,182,14,206,
  110,158,62,254,66,162,18,210,114,138,42,234,90,186,6,198,
  102,150,54,246,78,174,30,222,126,130,34,226,82,178,10,202,
  106,154,58,250,70,166,22,214,118,142,46,238,94,190,2,194,
  98,146,50,242,74,170,26,218,122,134,38,230,86,182,14,206,
  110,158,62,254,66,162,18,210,114,138,42,234,90,186,6,198,
  102,150,54,246,78,174,30,222,126,130,34,226,82,178,10,202,
  106,154,58,250,70,166,22,214,118,142,46,238,94,190,2,194,
  98,146,50,242,74,170,26,218,122,134,38,230,86,182,14,206,
  110,158,62,254,66,162,18,210,114,138,42,234,90,186,6,198,
  102,150,54,246,78,174,30,222,126,130,34,226,82,178,10,202,
  106,154,58,250,70,166,22,214,118,142,46,238,94,190,
};

// ─── Global state variables ───────────────────────────────────────────────────

WebServer server(80);
static uint8_t dmxBuf[DMX_FRAME_SIZE];  // Reusable buffer for building DMX frames

static bool stopRequested  = false;  // Goes true when user presses STOP
static bool isUnlocking    = false;  // True while unlock sequence is running
static bool isAddressing   = false;  // True while address assignment is running
static bool unlockDone     = false;  // True after unlock completes successfully
static int  currentAddress = 0;      // Which fixture we are currently addressing
static int  channelCount   = 4;      // Channels per fixture: 3 (RGB) or 4 (RGBW)
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
    h1{color:#e94560}
    .btn{padding:20px 40px;font-size:20px;border:none;border-radius:10px;cursor:pointer;margin:10px;width:80%}
    .unlock{background:#e67e22;color:white}
    .start{background:#0f3460;color:white}
    .stop{background:#e94560;color:white}
    .reset{background:#533483;color:white}
    .status{font-size:18px;margin:20px;padding:10px;background:#16213e;border-radius:10px}
    .progress{font-size:48px;color:#e94560;font-weight:bold}
    .ch-row{display:flex;justify-content:center;gap:10px;margin:10px auto;width:80%}
    .ch-btn{flex:1;padding:14px;font-size:18px;border:none;border-radius:10px;cursor:pointer;background:#16213e;color:#888}
    .ch-btn.active{background:#e67e22;color:white}
  </style>
  <script>
    function updateStatus(){
      fetch('/status').then(r=>r.json()).then(d=>{
        document.getElementById('addr').innerText=d.address;
        document.getElementById('state').innerText=d.msg;
        document.getElementById('btn3ch').className='ch-btn'+(d.channels===3?' active':'');
        document.getElementById('btn4ch').className='ch-btn'+(d.channels===4?' active':'');
      });
    }
    function unlock(){fetch('/unlock').then(()=>updateStatus())}
    function start(){fetch('/start').then(()=>updateStatus())}
    function stop(){fetch('/stop').then(()=>updateStatus())}
    function reset(){fetch('/reset').then(()=>updateStatus())}
    function setChannels(n){fetch('/setchannels?n='+n).then(()=>updateStatus())}
    setInterval(updateStatus,500);
    updateStatus();
  </script>
</head>
<body>
  <h1>Lumino Bridge</h1>
  <div class='status'>Channel mode:
    <div class='ch-row'>
      <button id='btn3ch' class='ch-btn' onclick='setChannels(3)'>3CH (RGB)</button>
      <button id='btn4ch' class='ch-btn active' onclick='setChannels(4)'>4CH (RGBW)</button>
    </div>
  </div>
  <div class='status'>Status: <span id='state'>IDLE</span></div>
  <div class='status'>Address: <span class='progress' id='addr'>0</span> / 512</div>
  <button class='btn unlock' onclick='unlock()'>&#128275; UNLOCK</button>
  <button class='btn start'  onclick='start()'>&#9654; START</button>
  <button class='btn stop'   onclick='stop()'>&#9632; STOP</button>
  <button class='btn reset'  onclick='reset()'>&#8635; RESET</button>
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
  int frameSize = DMX_ADDRESSES * channelCount;
  memset(dmxBuf, 0x00, frameSize);
  for (int i = 0; i < 6; i++) {
    if (stopRequested) return;
    sendDMXFrame(dmxBuf, frameSize);
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
  int frameSize = DMX_ADDRESSES * channelCount;

  // ── First 3 frames: all zeros ──────────────────────────────────────────────
  memset(dmxBuf, 0x00, frameSize);
  for (int i = 0; i < 3; i++) {
    if (stopRequested) return;
    sendDMXFrame(dmxBuf, frameSize);
    delay(INIT_PAUSE);
  }

  // ── Last 3 frames: closing pattern ────────────────────────────────────────
  //
  // Byte 0:                    0 (DMX start code, from memset)
  // Bytes 1..channelCount+1:   all 127  (channelCount+1 values)
  // From byte channelCount+2:  repeat every channelCount bytes:
  //                              (channelCount-1) × 0  then  127
  //
  // 3CH: 0, 127,127,127,127, 0,0,127, 0,0,127 ...
  // 4CH: 0, 127,127,127,127,127, 0,0,0,127, 0,0,0,127 ...
  //
  memset(dmxBuf, 0x00, frameSize);

  // Bytes 1 to channelCount+1 = 127
  for (int c = 1; c <= channelCount + 1 && c < frameSize; c++) {
    dmxBuf[c] = 127;
  }

  // From channelCount+2: last byte of each channelCount-block = 127
  int startPos = channelCount + 2;
  for (int pos = startPos; pos + channelCount - 1 < frameSize; pos += channelCount) {
    dmxBuf[pos + channelCount - 1] = 127;
  }

  for (int i = 0; i < 3; i++) {
    if (stopRequested) return;
    sendDMXFrame(dmxBuf, frameSize);
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

  // ── Timing values per channel mode and stub ──────────────────────────────────
  //
  // Target wire timings (measured from K1000C logic analyzer):
  // Both stubs identical: H144.25µs → L36µs → H186.25µs → gap 186.75µs
  //
  // 4CH stub 1 current → target:
  //   PRE: delay=82 → 106.75µs, need 144.25µs → delay=119
  //   MAB: delay=151 → 189.75µs, need 186.25µs → delay=147
  //   GAP: delay=167 → 201µs,    need 186.75µs → delay=154
  //
  // 4CH stub 2 current → target:
  //   PRE: delay=71 → 87µs,    need 144.25µs → delay=128
  //   MAB: delay=145 → 177.25µs, need 186.25µs → delay=154
  //   GAP: delay=168 → 201.5µs,  need 186.75µs → delay=154
  //
  // 3CH: estimates based on K1000C capture — verify with analyzer
  //
  int T_PRE_SYNC, T_MAB_SYNC, T_GAP;
  if (channelCount == 3) {
    T_PRE_SYNC = 133;   // → ~144.5µs
    T_MAB_SYNC = 150;   // → ~186µs
    T_GAP      = 147;   // → ~186.5µs
  } else if (mode == 1) {
    T_PRE_SYNC = 119;   // → ~144.25µs
    T_MAB_SYNC = 147;   // → ~186.25µs
    T_GAP      = 154;   // → ~186.75µs
  } else {
    T_PRE_SYNC = 128;   // → ~144.25µs
    T_MAB_SYNC = 154;   // → ~186.25µs
    T_GAP      = 154;   // → ~186.75µs
  }

  // Lead-in: bus HIGH → send 0x00 sync byte → bus HIGH (MAB)
  digitalWrite(PIN_DE, LOW);
  delayMicroseconds(T_PRE_SYNC);
  digitalWrite(PIN_DE, HIGH);

  uint8_t zero = 0x00;
  Serial1.write(&zero, 1);
  Serial1.flush();
  delayMicroseconds(12);            // Wait for stop bits
  digitalWrite(PIN_DE, LOW);
  delayMicroseconds(T_MAB_SYNC);

  // Send data packets
  // 4CH: 32 × 16 = 512 packets (nested loop, same byte8 for every byte7 group)
  // 3CH: 510 packets (flat sequence — groups have varying byte8 counts)
  uint8_t seq[8] = {85, 15, 44, 170, 53, mode, 0, 0};

  if (channelCount == 4) {
    for (int i = 0; i < 32; i++) {
      for (int j = 0; j < 16; j++) {
        if (stopRequested) return;

        seq[6] = byte7vals_4ch[i];
        seq[7] = byte8vals_4ch[j];

        digitalWrite(PIN_DE, HIGH);
        Serial1.write(seq, 8);
        Serial1.flush();
        delayMicroseconds(12);
        digitalWrite(PIN_DE, LOW);

        if (!(i == 31 && j == 15)) {
          delayMicroseconds(T_GAP);
        }
      }
    }
  } else {
    // 3CH: flat sequence of 510 entries
    for (int k = 0; k < 510; k++) {
      if (stopRequested) return;

      seq[6] = byte7seq_3ch[k];
      seq[7] = byte8seq_3ch[k];

      digitalWrite(PIN_DE, HIGH);
      Serial1.write(seq, 8);
      Serial1.flush();
      delayMicroseconds(12);
      digitalWrite(PIN_DE, LOW);

      if (k < 509) {
        delayMicroseconds(T_GAP);
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
  int frameSize = DMX_ADDRESSES * channelCount;
  memset(dmxBuf, 0x00, frameSize);
  int pos = (address - 1) * channelCount;
  if (pos + channelCount <= frameSize) {
    for (int i = 0; i < channelCount; i++) {
      dmxBuf[pos + i] = MARKER_BYTE;
    }
  }

  // Send the frame twice (confirmed more reliable than once)
  sendDMXFrame(dmxBuf, frameSize);
  if (stopRequested) return;
  delayMicroseconds(2000);
  sendDMXFrame(dmxBuf, frameSize);
  if (stopRequested) return;
  delay(PAUSE_BETWEEN);
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

// SETCHANNELS: set channel mode (3 or 4), only when not running
void handleSetChannels() {
  if (!isUnlocking && !isAddressing) {
    String n = server.arg("n");
    if (n == "3") channelCount = 3;
    else          channelCount = 4;
  }
  server.send(200, "text/plain", "OK");
}

// STATUS: called every 500ms by the web page to refresh the display
void handleStatus() {
  String json = "{\"running\":"   + String((isUnlocking || isAddressing) ? "true" : "false") +
                ",\"address\":"   + String(currentAddress) +
                ",\"channels\":"  + String(channelCount) +
                ",\"msg\":\""     + statusMsg + "\"}";
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

  server.on("/",            handleRoot);
  server.on("/unlock",      handleUnlock);
  server.on("/start",       handleStart);
  server.on("/stop",        handleStop);
  server.on("/reset",       handleReset);
  server.on("/setchannels", handleSetChannels);
  server.on("/status",      handleStatus);
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
