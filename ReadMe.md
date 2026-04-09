<h1 align="center">Moon Firmware</h1>

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

Moon expands SubGHz automotive protocol coverage far beyond stock, porting protocol implementations from [D4C1-Labs/Flipper-ARF](https://github.com/D4C1-Labs/Flipper-ARF).

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

## Execute-in-Place (XIP) Flash Loading

Moon moves all protocol libraries (Sub-GHz, NFC, LFRFID, Infrared, iButton, mJS) out of the firmware and into their respective FAP applications. This reduces firmware size by **~36%** while keeping applications fast through a custom XIP (Execute-in-Place) loader that runs app code directly from internal flash instead of RAM.

| | Momentum (dev) | Moon | Saved |
|---|---|---|---|
| `firmware.bin` | ~877 KB | ~496 KB | **~381 KB** |
| Flash pages used | ~214 | ~125 | 89 pages |
| Free internal flash | ~172 KB | ~528 KB | **+356 KB** |

### How XIP Works

When a FAP application launches, Moon checks if it fits in RAM. Small apps load entirely into RAM as usual. Larger apps (like Sub-GHz at 247 KB and NFC at 183 KB) have their read-only sections (`.text` and `.rodata`) written to a **300 KB XIP region** in free internal flash, where the CPU executes them in place. Only writable data (`.data`, `.bss`) goes to RAM — typically under 200 bytes.

| App | Total Size | XIP (flash) | RAM (heap) |
|---|---|---|---|
| Sub-GHz | 247 KB | 241 KB (99.9%) | 194 bytes (0.1%) |
| NFC | 183 KB | 179 KB (100%) | 68 bytes (0.0%) |
| Infrared | 42 KB | Fits in RAM | 42 KB |
| RFID | 55 KB | Fits in RAM | 55 KB |
| iButton | 24 KB | Fits in RAM | 24 KB |

### Flash Cache

The XIP region includes a cache header. When re-launching the same app, Moon validates the cached data (file size + CRC32 + API version) and skips the erase/write cycle entirely. This provides:

- **Instant re-launch**: ~100ms instead of ~10 seconds
- **Near-zero flash wear**: erase cycles only occur on the first launch after a firmware or app update

| Usage Pattern | Erases/Year | Flash Lifetime |
|---|---|---|
| Re-launching same app | 0 | Infinite (cache hit) |
| Monthly firmware updates | ~4,400 | **139 years** |
| 2 different large apps/day | ~44,500 | **13+ years** |

STM32WB55 flash is rated for 10,000 erase cycles per page.

### Protocol Libraries

All protocol libraries are compiled as `fap_private_libs` in their respective apps, not in firmware:

| Library | Size | Used By |
|---|---|---|
| Sub-GHz protocols | 204 KB | Sub-GHz app |
| NFC protocols | 112 KB | NFC app |
| LFRFID protocols | 34 KB | RFID app |
| Infrared protocols | 12 KB | Infrared app |
| iButton protocols | 10 KB | iButton app |
| mJS engine | 48 KB | JavaScript app |

The freed flash headroom (~356 KB) is available for the full BLE stack, additional firmware features, and future expansion without changing the radio stack partition.

The updater has also been hardened with a CRC peripheral reset before FUS calls (ST errata workaround) and FUS error state recovery for more reliable over-the-air updates.

---

All user-facing applications run as FAPs from the SD card:

| App | Momentum | Moon |
|---|---|---|
| Sub-GHz | Internal | **External (XIP)** |
| NFC | External | **External (XIP)** |
| Infrared | External | External (RAM) |
| 125kHz RFID | External | External (RAM) |
| iButton | External | External (RAM) |
| GPIO | External | External (RAM) |
| Bad KB | External | External (RAM) |
| U2F | External | External (RAM) |
| JavaScript | External | External (RAM) |

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
3. Once Momentum boots, re-apply Moon via the SD card update package

---

## Base

Built on [Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware), which is built on [Official Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware). All Momentum features, customization, and community apps are included.

---

### Disclaimer

*This firmware is provided as-is for educational and research purposes only. It is not affiliated with, endorsed by, or supported by Flipper Devices, the Momentum Firmware project, or any vehicle manufacturer. The authors assume no responsibility for any damage to hardware, loss of data, or legal consequences resulting from the use of this firmware. Users are solely responsible for ensuring their use complies with all applicable local, state, and federal laws. Unauthorized access to vehicle systems, interception of RF signals, or bypassing of security mechanisms may violate laws including but not limited to the Computer Fraud and Abuse Act (CFAA), the European Cybercrime Convention, and national telecommunications regulations. This software must not be used for unauthorized entry, theft, stalking, or any other illegal activity.*
