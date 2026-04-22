# WLED Integration

After programming your fixtures with Lumino Bridge, use this guide to control them with [WLED](https://kno.wled.ge).

---

## Requirements

- WLED 0.14 or newer (any build with DMX output support)
- ESP32 with a MAX485 module (can reuse the Lumino Bridge hardware)

---

## Wiring for WLED

Connect MAX485 to your WLED ESP32:

| ESP32 GPIO | MAX485 pin |
|------------|------------|
| 2 (or any free GPIO) | DI |
| 4 (or any free GPIO) | DE + RE (tied) |
| GND | GND |
| 3.3V or 5V | VCC |

---

## WLED configuration

1. Open WLED web UI → **Config → LED Preferences**
2. Add a new LED strip:
   - Type: `Serial` → sub-type: `DMX`
   - GPIO: `2` (or whichever pin you used for DI)
   - Length: number of fixtures (e.g. `50`)
3. Under DMX settings:
   - DMX type: `RGBW` for 4CH fixtures, `RGB` for 3CH fixtures
   - Start address: `1`
4. Save and reboot

Your fixtures should now respond to WLED effects in real time.

---

## Channel mapping

**4CH fixtures (RGBW):**

| Offset | WLED color |
|--------|------------|
| +0 | Red |
| +1 | Green |
| +2 | Blue |
| +3 | White |

**3CH fixtures (RGB):**

| Offset | WLED color |
|--------|------------|
| +0 | Red |
| +1 | Green |
| +2 | Blue |

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| No response from fixtures | Wrong GPIO or DE not wired | Check wiring |
| Wrong colors | Channel order mismatch | Try `RGBW` vs `WRGB` in WLED |
| Only first N fixtures work | Address programming stopped early | Re-run Lumino Bridge |
| Flickering | Long cable without termination | Add 120 Ω across A/B at far end |
| White channel not working | Wrong DMX type selected | Make sure `RGBW` is selected for 4CH fixtures |
| 3CH fixtures show wrong colors | DMX type set to RGBW | Change DMX type to `RGB` in WLED |
