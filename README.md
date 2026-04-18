# ⚡ Lumino Bridge

**ESP32 programmer for cheap RS-485 RGBW ceiling fixtures (UCS512C chip)**

These fixtures are sold on AliExpress for a fraction of the price of name-brand smart lights, but they ship with *no documentation* on how to program their DMX addresses. This project reverse-engineers the proprietary unlock protocol and gives you a dead-simple web UI to get them working.

Once programmed, the fixtures respond to standard **DMX512** — use them with WLED, QLC+, or any DMX controller.

---

## What it solves

UCS512C-based fixtures require a **proprietary unlock sequence** before they will accept DMX address programming. Without it, pressing the fixture's own address button does nothing and no standard DMX programmer works.

Lumino Bridge:
1. Sends the reverse-engineered unlock sequence over RS-485
2. Assigns DMX addresses 1–512 sequentially
3. Fixtures permanently store their address and respond to standard DMX from that point on

### Why RS-485 programming?

The UCS512C datasheet documents official **PI/PO coding lines** for daisy-chaining address programming. However, those lines are only accessible on the bare chip — finished fixtures do not expose them. Lumino Bridge instead programs addresses entirely over the RS-485 bus using a method reverse-engineered from a third-party programmer, making it practical for real-world fixtures where PI/PO lines are inaccessible.

---

## Hardware

| Component | Notes |
|-----------|-------|
| ESP32 (classic) | Any classic ESP32 module works |
| MAX485 / RS-485 module | ~$0.50 on AliExpress |
| Power supply | 5 V for ESP32, check fixture specs for fixture supply |

### Wiring

```
ESP32 GPIO 17  ──►  MAX485 DI     (UART1 data out)
ESP32 GPIO  4  ──►  MAX485 DE+RE  (direction, tied together)
ESP32 GND      ──►  MAX485 GND
ESP32 3.3V/5V  ──►  MAX485 VCC

MAX485 A ─┐
           ├──  RS-485 bus  ──►  Fixtures
MAX485 B ─┘

Optional:
ESP32 GPIO 0  ──  Pushbutton  ──  GND   (physical STOP)
```

> **WLED note:** After fixtures are programmed, connect your DMX controller's MAX485 to GPIO 2 on WLED and it works out of the box.

---

## Software setup

1. Install **Arduino IDE** with the [ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
2. Open `firmware/lumino_bridge/lumino_bridge.ino`
3. Select board: **ESP32 Dev Module**
4. Flash to your ESP32

No extra libraries needed — uses only `WiFi.h` and `WebServer.h` (bundled with the ESP32 Arduino core).

---

## Usage

1. Power up the ESP32
2. Connect to Wi-Fi network **`LuminoBridge`** (password: `12345678`)
3. Open **http://192.168.4.1** in a browser
4. Make sure your fixtures are powered and in factory state
5. Press **UNLOCK** — wait ~5 seconds for the sequence to complete
6. Press **START** — the bridge assigns addresses 1–512 one by one
7. Done! Connect WLED or any DMX512 controller

The web UI shows live status and current address being programmed.

---

## Protocol documentation

See [`docs/protocol.md`](docs/protocol.md) for the full reverse-engineered protocol spec — timing diagrams, frame structure, unlock sequence details, and everything needed to implement this in other environments.

---

## Hardware schematic

See [`hardware/wiring.md`](hardware/wiring.md) for a detailed wiring guide with notes on RS-485 termination and cable length.

---

## Tested fixtures

| Fixture | Chip | Channels | Result |
|---------|------|----------|--------|
| Generic 8W RGBW RS-485 ceiling light (AliExpress) | UCS512C | 4 (R G B W) | ✅ Working |

If you test with other fixtures, please open an issue or PR to add them to the list!

---

## WLED integration

After programming fixtures with Lumino Bridge:

1. Wire your WLED ESP32's GPIO 2 → MAX485 DI, GPIO 4 → DE/RE
2. In WLED: **Config → LED Preferences**  
   - LED type: `Serial` → `DMX`
   - GPIO: `2`
   - DMX type: `RGBW` (4 channels)
3. Set LED count to match your fixture count

Full WLED DMX integration guide: [`docs/wled-integration.md`](docs/wled-integration.md)

---

## License

MIT — free to use, modify, and distribute. See [LICENSE](LICENSE).

---

## Support the project

If this saved you hours of frustration, consider buying me a coffee ☕

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-yellow?style=flat&logo=buy-me-a-coffee)](https://buymeacoffee.com/YOUR_USERNAME)

---

## Contributing

Issues and PRs welcome. If you have a fixture that doesn't work with the current timing values, open an issue with your oscilloscope captures and we'll figure it out together.
