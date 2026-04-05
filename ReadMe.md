<h1 align="center">Bloodmoon Firmware</h1>

<p align="center">
  <img src="https://img.shields.io/badge/status-experimental-red" alt="Experimental">
  <img src="https://img.shields.io/badge/base-Momentum-blue" alt="Based on Momentum">
  <img src="https://img.shields.io/badge/target-STM32WB55-green" alt="STM32WB55">
</p>

<p align="center">
  A Flipper Zero firmware fork built on <a href="https://github.com/Next-Flip/Momentum-Firmware">Momentum</a>, focused on automotive RF research and internal flash optimization.
</p>

> **This firmware is experimental.** It may contain bugs, cause unexpected behavior, or require manual recovery. Use at your own risk. Always keep a known-good firmware (Momentum or stock) available for recovery.

---

## Automotive SubGHz Protocols

Bloodmoon expands SubGHz automotive protocol coverage far beyond stock, porting protocol implementations from [D4C1-Labs/Flipper-ARF](https://github.com/D4C1-Labs/Flipper-ARF).

| Manufacturer | Protocol | Notes |
|---|---|---|
| VAG Group | AUT64 | VW, Audi, Skoda — AUT64 encrypted |
| KIA / Hyundai | V0 – V6 | 6 generational variants |
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

Shifted button position display for 12+ brands. Full encode and decode for 44 gate and access control protocols covering BFT, DEA, Nice, FAAC, Sommer, DoorHan, and others.

### Custom Radio Presets

7 CC1101 presets optimized for different modulation and bandwidth scenarios:

`OOK_LR` · `OOK_U` · `AM_1` · `FSK_1` · `F3` · `FM95` · `FM15k`

---

## Flash Optimization

Bloodmoon reduces internal firmware size by **~9%** compared to Momentum by relocating the Sub-GHz application to an external FAP on the SD card. Momentum keeps Sub-GHz compiled into `firmware.bin` due to linking constraints — Bloodmoon resolves these to run it externally like the other major apps.

| | Momentum (dev) | Bloodmoon | Saved |
|---|---|---|---|
| `firmware.bin` | ~877 KB | ~797 KB | **~80 KB** |
| Flash pages used | ~214 | 195 | 19 pages |
| Free internal flash | ~172 KB | ~246 KB | **+74 KB** |

All user-facing applications run as FAPs from the SD card:

| App | Momentum | Bloodmoon |
|---|---|---|
| Sub-GHz | Internal | **External** |
| NFC | External | External |
| Infrared | External | External |
| 125kHz RFID | External | External |
| iButton | External | External |
| GPIO | External | External |
| Bad KB | External | External |
| U2F | External | External |

The freed flash headroom is available for additional firmware features, protocol decoders, and future expansion without needing to change the radio stack partition.

The updater has also been hardened with a CRC peripheral reset before FUS calls (ST errata workaround) and FUS error state recovery for more reliable over-the-air updates.

---

## Build & Flash

```bash
./fbt updater_package
```

Copy the output directory (`dist/f7-C/f7-update-*`) to the Flipper's SD card under `update/` and apply from **Settings > Update**.

---

## Recovery

If the device boot-loops or shows "secure enclave damaged" after a failed update:

1. Enter DFU mode — hold **LEFT + BACK** while plugging in USB
2. Flash a clean Momentum firmware via [qFlipper](https://flipperzero.one/update)
3. Once Momentum boots, re-apply Bloodmoon via the SD card update package

---

## Base

Built on [Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware), which is built on [Official Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware). All Momentum features, customization, and community apps are included.

---

<sub>

**Disclaimer:** This firmware is provided as-is for educational and research purposes only. It is not affiliated with, endorsed by, or supported by Flipper Devices, the Momentum Firmware project, or any vehicle manufacturer. The authors assume no responsibility for any damage to hardware, loss of data, or legal consequences resulting from the use of this firmware. Users are solely responsible for ensuring their use complies with all applicable local, state, and federal laws. Unauthorized access to vehicle systems, interception of RF signals, or bypassing of security mechanisms may violate laws including but not limited to the Computer Fraud and Abuse Act (CFAA), the European Cybercrime Convention, and national telecommunications regulations. This software must not be used for unauthorized entry, theft, stalking, or any other illegal activity.

</sub>
