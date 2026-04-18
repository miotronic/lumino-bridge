# WLED Integration

After programming your fixtures with Lumino Bridge, use this guide to
control them with [WLED](https://kno.wled.ge/).

---

## Requirements

- WLED 0.14 or newer (any build with DMX output support)
- ESP32 with a MAX485 module (can reuse the Lumino Bridge hardware)

---

## Wiring for WLED

Connect MAX485 to your WLED ESP32:

| ESP32 GPIO | MAX485 pin |
|-----------|-----------|
| 2 (or any free GPIO) | DI |
| 4 (or any free GPIO) | DE + RE (tied) |
| GND | GND |
| 3.3V or 5V | VCC |

---

## WLED configuration

1. Open WLED web UI → **Config → LED Preferences**
2. Add a new LED strip:
   - **Type:** `Serial`  → sub-type: `DMX`
   - **GPIO:** `2` (or whichever pin you used for DI)
   - **Length:** number of fixtures (e.g. `50`)
3. Under **DMX settings**:
   - **DMX type:** `RGBW` — 4 channels per fixture
   - **Start address:** `1`
4. Save and reboot

Your fixtures should now respond to WLED effects in real time.

---

## Channel mapping

Each fixture uses 4 consecutive DMX channels:

| Offset | WLED color |
|--------|-----------|
| +0 | Red |
| +1 | Green |
| +2 | Blue |
| +3 | White |

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| No response from fixtures | Wrong GPIO or DE not wired | Check wiring |
| Wrong colors | Channel order mismatch | Try `RGBW` vs `WRGB` in WLED |
| Only first N fixtures work | Address programming stopped early | Re-run Lumino Bridge |
| Flickering | Long cable without termination | Add 120 Ω across A/B at far end |
