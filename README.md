# ⚡ Lumino Bridge
Wi-Fi programmer for UCS512C RS-485 RGB and RGBW fixtures — $3.50 in hardware, zero compromises

## The problem
AliExpress is full of beautiful RGB and RGBW fixtures built around the UCS512C chip, solid construction, great light quality. The kind of deal that makes you buy thirty of them before you've thought it through.

Then they arrive, and you discover the catch.

The only way to program their DMX addresses is with a dedicated Chinese programmer: a chunky brick that reads settings off an SD card, has no wireless connectivity, no app, no automation support, and costs $60 or more. You end up hunched over each fixture with this thing, manually configuring lights one by one, and when you're done they're still just dumb DMX fixtures — no smart home, no Home Assistant, no WLED effects. Just $3 lights held hostage by a $60 dinosaur.

It doesn't have to be this way.

## The solution
Lumino Bridge is an ESP32-based programmer that replaces the proprietary box entirely. Flash it onto a $3 ESP32, wire up a $0.50 MAX485 module, and you have a Wi-Fi-connected programmer that fits in your pocket and talks to your fixtures over RS-485.

Connect to its access point, open a browser, select your channel mode (3CH or 4CH), hit UNLOCK then START — and watch it assign DMX addresses to every fixture on the bus in minutes. No SD cards. No cables to a laptop. No ancient software. Once programmed, the fixtures respond to standard DMX512 and work immediately with WLED, QLC+, and Home Assistant.

The proprietary unlock sequence that makes this possible was reverse-engineered from a third-party programmer (the UCS512C does have official PI/PO address lines per the datasheet, but they're inaccessible in finished fixtures — so we go through RS-485 instead). The full protocol is documented in docs/protocol.md.

$3.50 in hardware. A five-minute setup. Thirty smart ceiling lights.

## Hardware

| Component | Notes |
|-----------|-------|
| ESP32 (classic) | Any classic ESP32 module works |
| MAX485 / RS-485 module | ~$0.50 on AliExpress |
| Power supply | 5V for ESP32, check fixture specs for fixture supply |

## Wiring
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

## Software setup
1. Install Arduino IDE with the ESP32 board package
2. Open `firmware/lumino_bridge/lumino_bridge.ino`
3. Select board: **ESP32 Dev Module**
4. Flash to your ESP32

No extra libraries needed — uses only `WiFi.h` and `WebServer.h` (bundled with the ESP32 Arduino core).

## Usage
1. Power up the ESP32
2. Connect to Wi-Fi network **LuminoBridge** (password: `lumino123`)
3. Open `http://192.168.4.1` in a browser
4. Select **3CH** (RGB) or **4CH** (RGBW) to match your fixtures
5. Make sure your fixtures are powered and in factory state
6. Press **UNLOCK** — wait ~5 seconds for the sequence to complete
7. Press **START** — the bridge assigns addresses 1–512 one by one
8. Done! Connect WLED or any DMX512 controller

The web UI shows live status and current address being programmed.

## Protocol documentation
See `docs/protocol.md` for the full reverse-engineered protocol spec — timing diagrams, frame structure, unlock sequence details, and everything needed to implement this in other environments.

## Hardware schematic
See `hardware/wiring.md` for a detailed wiring guide with notes on RS-485 termination and cable length.

## Tested fixtures

| Fixture | Chip | Channels | Result |
|---------|------|----------|--------|
| SH LED XQ0511 Wall Washer 24W (Shenzhen SH LED Technology) | UCS512C | 4CH (R G B W) | ✅ Working |

If you test with other fixtures, please open an issue or PR to add them to the list!

## WLED integration
After programming fixtures with Lumino Bridge:

1. Wire your WLED ESP32's GPIO 2 → MAX485 DI, GPIO 4 → DE/RE
2. In WLED: **Config → LED Preferences**
3. LED type: Serial → DMX
4. GPIO: 2
5. DMX type: RGB or RGBW depending on your fixture (3 or 4 channels)
6. Set LED count to match your fixture count

Full WLED DMX integration guide: `docs/wled-integration.md`

## Roadmap
Lumino Bridge is just the beginning.

The current firmware does one job — programming fixtures — and hands off to WLED for live control. That means two devices, two flash jobs, two points of failure. The next milestone changes that:

### Custom WLED build with native UCS512C addressing
A custom WLED firmware with a built-in UCS512C addressing mode. One ESP32 does everything: program the fixtures directly from the WLED UI, then switch into live DMX control without touching a second device. No separate programmer. No workflow interruption. Just flash, address, and go.

This will make deploying a full room of RS-485 fixtures as seamless as any off-the-shelf smart light system — and significantly more capable.

If you want to help build it or test early builds, watch this repo and open an issue.

## License
GPL v3 — free to use and modify for personal and non-commercial use. Commercial use requires written permission from the author. See LICENSE.

## Support the project
If this saved you hours of frustration, consider buying me a coffee ☕

[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-yellow?style=flat&logo=buy-me-a-coffee)](https://buymeacoffee.com/miotronic)

---

## Contributing
Issues and PRs welcome. If you have a fixture that doesn't work with the current timing values, open an issue with your oscilloscope captures and we'll figure it out together.
