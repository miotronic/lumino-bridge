# Hardware Wiring Guide

## Components

- **ESP32** — any classic ESP32 development board (38-pin or 30-pin)
- **MAX485 module** — the common blue breakout board (~$0.50 on AliExpress)
- **RS-485 cable** — twisted pair; even cheap 2-core cable works for short runs

---

## Pin connections

```
┌─────────────────┐        ┌──────────────┐
│     ESP32       │        │   MAX485     │
│                 │        │              │
│  GPIO 17 (TX1) ─┼──────► DI            │
│  GPIO  4       ─┼──────► DE ──┐        │
│                 │        │    └──── RE  │
│  GND           ─┼──────► GND           │
│  3.3V / 5V     ─┼──────► VCC           │
│                 │        │      A ──────┼──► RS-485 bus (+)
│  GPIO 0        ─┼──┐    │      B ──────┼──► RS-485 bus (-)
│  (optional)     │  │    └──────────────┘
└─────────────────┘  │
                     └── Pushbutton ── GND   (physical STOP)
```

> **DE and RE must be tied together.** Both pins on the MAX485 module control
> direction. Connect them both to GPIO 4 so the code can switch between
> transmit and receive with a single `digitalWrite`.

---

## RS-485 bus notes

- **Cable length:** up to ~100 m with basic twisted-pair. Longer runs need
  proper shielded cable.
- **Termination:** for runs > ~10 m, add a 120 Ω resistor between A and B
  at the far end of the bus.
- **Multiple fixtures:** daisy-chain A→A and B→B along the bus. Star
  topology is not recommended for RS-485.
- **Ground:** RS-485 is differential and doesn't require a common ground
  between devices, but it is good practice to connect a third wire (GND)
  along the bus for noise immunity.

---

## After programming — WLED wiring

Once fixtures are programmed you no longer need Lumino Bridge. Connect your
WLED ESP32 directly:

```
WLED ESP32 GPIO 2  ──►  MAX485 DI
WLED ESP32 GPIO 4  ──►  MAX485 DE+RE   (or whatever pin you configure in WLED)
```

WLED DMX settings:
- LED type: **Serial → DMX**
- DMX type: **RGBW** (4 channels per fixture)
- Start universe: 1
- Start address: 1
