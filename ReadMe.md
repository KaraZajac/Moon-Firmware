<h1 align="center">Bloodmoon Firmware</h1>

<p align="center">
  <strong>BM-FRM-01</strong><br>
  A Flipper Zero firmware fork built on <a href="https://github.com/Next-Flip/Momentum-Firmware">Momentum</a>, focused on automotive RF research and extended BLE capabilities.
</p>

---

## What This Is

Bloodmoon is a research-oriented Flipper Zero firmware that expands SubGHz automotive protocol coverage far beyond stock. It ports protocol implementations from [D4C1-Labs/Flipper-ARF](https://github.com/D4C1-Labs/Flipper-ARF) and reorganizes internal flash to support the BLE Full Extended advertising stack.

> **Warning:** This firmware modifies STM32WB55 Option Bytes (SFSA) and the BLE radio stack partition. Flashing with `flash_usb_full` carries brick risk if interrupted. Use SWD (`./fbt flash`) for safer flashing when changing OB or radio stack configurations.

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

## BLE Extended Stack

Bloodmoon reconfigures the STM32WB55 flash layout to run the full BLE extended advertising stack (`stm32wb5x_BLE_Stack_full_extended_fw.bin`):

| Parameter | Stock | Bloodmoon |
|---|---|---|
| SFSA | 0xD7 | 0xC5 |
| CPU1 (App) | 860 KB | 788 KB |
| CPU2 (BLE) | 164 KB | 236 KB |

This enables extended advertising, multiple advertising sets, and larger payloads at the cost of 72 KB of application flash.

---

## Memory Optimization

Core apps moved to external storage (SD card FAPs) to offset the reduced internal flash:

- iButton
- Infrared
- LFRFID
- Sub-GHz
- JS Runner

---

## Build & Flash

### Standard (USB DFU)

```bash
./fbt flash_usb_full
```

### SWD (ST-Link) — recommended for OB changes

```bash
./fbt flash
```

Connect ST-Link to Flipper GPIO: SWCLK (pin 10), SWDIO (pin 12), GND (pin 18). Power Flipper via USB separately.

---

## Recovery

If the device is unresponsive after a failed flash (no LED, no USB enumeration), the STM32 ROM bootloader cannot be reached via the normal button combo. Recovery requires SWD:

1. Connect an ST-Link to the GPIO header
2. Power the Flipper via USB
3. Flash from a clean Momentum checkout: `./fbt flash`

---

## Base

Built on [Momentum Firmware](https://github.com/Next-Flip/Momentum-Firmware), which is built on [Official Flipper Zero Firmware](https://github.com/flipperdevices/flipperzero-firmware). All Momentum features, customization, and community apps are included.
