<h1 align="center">Moon Firmware</h1>

<p align="center">
  <img src="https://img.shields.io/badge/release-004-blue" alt="Release 004">
  <img src="https://img.shields.io/badge/status-experimental-red" alt="Experimental">
  <img src="https://img.shields.io/badge/target-STM32WB55-green" alt="STM32WB55">
</p>

<p align="center">
  A hard fork of <a href="https://github.com/Next-Flip/Momentum-Firmware">Momentum Firmware</a> for Flipper Zero.
</p>

> **This firmware is experimental.** It may contain bugs, cause unexpected behavior, or require manual recovery. Use at your own risk. Always keep a known-good firmware (Momentum or stock) available for recovery.

---

## What is Moon?

Moon is a hard fork of Momentum Firmware that makes two fundamental architectural changes to the Flipper Zero platform:

1. **Full BLE Stack** -- Replaces the BLE Light radio stack with BLE Full, enabling BLE central/scanning, GATT client, and up to 8 simultaneous connections. This unlocks capabilities like BLE device scanning, active GATT enumeration, and peripheral interaction that aren't possible on the Light stack.

2. **Execute-in-Place (XIP) Flash Loading** -- A custom loader that runs application code directly from internal flash instead of RAM. This allows every user-facing application to run as an external FAP from the SD card, including large apps like Sub-GHz (247 KB) and NFC (183 KB) that would never fit in the Flipper's ~128 KB of available heap.

All Momentum features, customization options, and community apps are included.

---

## Execute-in-Place (XIP)

The Flipper Zero has 1 MB of internal flash but only ~128 KB of free RAM for applications. Stock and Momentum firmware compile large protocol libraries (Sub-GHz, NFC, etc.) directly into the firmware binary, consuming flash permanently and limiting what can be changed without a full firmware update.

Moon takes a different approach: all protocol libraries are compiled into their respective FAP applications on the SD card, and a **300 KB XIP region** in free internal flash is used to execute them in place.

### How It Works

When a FAP launches, the loader checks its size. Small apps load entirely into RAM as usual. Larger apps have their read-only sections (`.text` and `.rodata`) written to the XIP flash region, where the CPU executes them directly -- no RAM copy needed. Only writable data (`.data`, `.bss`) goes to RAM, typically under 200 bytes.

| App | Total Size | In Flash (XIP) | In RAM |
|---|---|---|---|
| Sub-GHz | 247 KB | 241 KB | 194 bytes |
| NFC | 183 KB | 179 KB | 68 bytes |
| Infrared | 42 KB | -- | 42 KB (fits in RAM) |
| 125kHz RFID | 55 KB | -- | 55 KB (fits in RAM) |

This means you can now build and run applications that are **far larger than available RAM**. The only constraint is the 300 KB XIP region, and that limit is adjustable.

### Flash Cache

The XIP region includes a cache header with file size, CRC32, and API version. When re-launching the same app, Moon validates the cache and skips the flash erase/write cycle entirely:

- **Re-launch**: ~100ms (cache hit) vs ~10 seconds (cold write)
- **Flash wear**: erase cycles only occur on first launch after a firmware or app update
- STM32WB55 flash is rated for 10,000 erase cycles per page, giving years of normal use

### Firmware Size

Moving protocol libraries out of firmware and into apps reduces the firmware binary significantly:

| | Momentum (dev) | Moon | Saved |
|---|---|---|---|
| `firmware.bin` | ~877 KB | ~496 KB | **~381 KB** |
| Free internal flash | ~172 KB | ~528 KB | **+356 KB** |

The freed flash headroom accommodates the full BLE stack, the XIP region, and leaves room for future expansion.

---

## Full BLE Stack

Moon ships with the STM32WB55 BLE Full radio stack (`stm32wb5x_BLE_Stack_full_fw.bin`), which adds GAP Observer and Central roles on top of the Peripheral role used by stock firmware. This enables:

- **BLE scanning** -- Discover and enumerate nearby BLE devices
- **GATT client** -- Connect to peripherals and read/write characteristics
- **Multiple connections** -- Up to 8 simultaneous BLE connections
- **Extra beacon advertising** -- Concurrent advertisement sets

These capabilities are exposed to FAP applications through the existing Flipper BLE API.

---

## Automotive SubGHz Protocols

Moon expands SubGHz automotive protocol coverage using protocol implementations from [D4C1-Labs/Flipper-ARF](https://github.com/D4C1-Labs/Flipper-ARF).

| Manufacturer | Protocol | Notes |
|---|---|---|
| VAG Group | AUT64 | VW, Audi, Skoda -- AUT64 encrypted |
| KIA / Hyundai | V0 -- V6 | 6 generational variants |
| PSA Group | PSA | Peugeot, Citroen, DS, Opel |
| Ford | Ford V0 | |
| Fiat | SPA, Marelli | Two separate protocol families |
| Subaru | Subaru | |
| Suzuki | Suzuki | |
| Porsche | Porsche Cayenne | |
| Mazda | Mazda Siemens | |
| Mitsubishi | Mitsubishi V0 | |
| StarLine | StarLine | Multi-page custom button support |
| Scher-Khan | Scher-Khan | Multi-page custom button support |
| Sheriff | Sheriff CFM | |

KeeLoq shifted button position display for 12+ brands, full encode and decode for 44 gate and access control protocols.

---

## Build & Flash

```bash
./fbt flash_usb        # Build and flash over USB
./fbt updater_package  # Build SD card update package
```

For SD card updates, copy the output directory (`dist/f7-C/f7-update-*`) to the Flipper's SD card under `update/` and apply from **Settings > Update**.

---

## Recovery

If the device boot-loops or becomes unresponsive after a failed update:

1. Enter DFU mode -- hold **LEFT + BACK** while plugging in USB
2. Flash a clean Momentum firmware via [qFlipper](https://flipperzero.one/update)
3. Once Momentum boots, re-apply Moon via the SD card update package

---

## Base

Built on [Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware), which is built on [Official Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware).

---

### Disclaimer

*This firmware is provided as-is for educational and research purposes only. It is not affiliated with, endorsed by, or supported by Flipper Devices, the Momentum Firmware project, or any vehicle manufacturer. The authors assume no responsibility for any damage to hardware, loss of data, or legal consequences resulting from the use of this firmware. Users are solely responsible for ensuring their use complies with all applicable local, state, and federal laws. Unauthorized access to vehicle systems, interception of RF signals, or bypassing of security mechanisms may violate laws including but not limited to the Computer Fraud and Abuse Act (CFAA), the European Cybercrime Convention, and national telecommunications regulations. This software must not be used for unauthorized entry, theft, stalking, or any other illegal activity.*
