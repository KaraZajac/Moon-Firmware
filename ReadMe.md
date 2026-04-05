<h1 align="center">Bloodmoon Firmware</h1>

<p align="center">
  <strong>BM-FRM-01</strong><br>
  A Flipper Zero firmware fork built on <a href="https://github.com/Next-Flip/Momentum-Firmware">Momentum</a>, focused on automotive RF research and flash optimization.
</p>

---

## What This Is

Bloodmoon is a research-oriented Flipper Zero firmware that expands SubGHz automotive protocol coverage far beyond stock. It ports protocol implementations from [D4C1-Labs/Flipper-ARF](https://github.com/D4C1-Labs/Flipper-ARF) and restructures internal flash usage to maximize available storage for apps and assets.

---

## Automotive SubGHz Protocols

| Manufacturer | Protocol | Notes |
|---|---|---|
| VAG Group | AUT64 | VW, Audi, Skoda — AUT64 encrypted |
| KIA / Hyundai | V0 through V6 | 6 generational variants |
| PSA Group | PSA | Peugeot, Citroen, DS, Opel — TEA brute-force |
| Ford | Ford V0 | |
| Fiat | SPA, Marelli | Two separate protocol families |
| Subaru | Subaru | |
| Suzuki | Suzuki | |
| Porsche | Porsche Cayenne | |
| Mazda | Mazda Siemens | |
| Mitsubishi | Mitsubishi V0 | |

### Alarm Systems

| System | Notes |
|---|---|
| StarLine | Multi-page custom button support |
| Scher-Khan | Multi-page custom button support |
| Sheriff CFM | |

### KeeLoq

Shifted button position display for 12+ brands. Full encode+decode for 44 gate/access control protocols covering BFT, DEA, Nice, FAAC, Sommer, DoorHan, and 22 others.

### Custom Radio Presets

7 CC1101 presets: `OOK_LR`, `OOK_U`, `AM_1`, `FSK_1`, `F3`, `FM95`, `FM15k`

---

## Flash Optimization

Core apps moved to external storage (SD card FAPs) to free internal flash for firmware features and future expansion:

- iButton
- Infrared
- LFRFID
- Sub-GHz
- JS Runner

The updater has also been hardened with CRC peripheral reset before FUS calls (ST errata workaround) and FUS error state recovery for more reliable radio stack operations.

---

## Build & Flash

### Update Package (recommended)

```bash
./fbt updater_package
```

Copy the output directory (`dist/f7-C/f7-update-*`) to the Flipper's SD card under `update/` and apply from Settings > Update.

### SWD (ST-Link)

```bash
./fbt flash
```

Connect ST-Link to Flipper GPIO: SWCLK (pin 10), SWDIO (pin 12), GND (pin 18). Power Flipper via USB separately.

---

## Recovery

If the device boot-loops or shows "secure enclave damaged" after a failed update:

1. Enter DFU mode: hold **LEFT + BACK** while plugging in USB
2. Flash a clean Momentum firmware via [qFlipper](https://flipperzero.one/update)
3. Once Momentum boots, re-apply Bloodmoon via the SD card update package

For hard bricks (no USB enumeration), recovery requires SWD with an ST-Link.

---

## Base

Built on [Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware), which is built on [Official Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware). All Momentum features, customization, and community apps are included.
