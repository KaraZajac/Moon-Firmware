---
name: Momentum Firmware Project Context
description: Fork of Momentum Firmware for Flipper Zero with two major change sets - external app migration and new SubGHz protocols
type: project
---

Fork of Momentum Firmware (Flipper Zero custom firmware) on the `dev` branch.

Two major change sets in progress:
1. **External app migration** — Moving iButton, Infrared, LFRFID, SubGHz, and JS Runner from internal flash to SD card (FAP format) to save memory
2. **New SubGHz protocols** — Added 17+ automotive RF protocols for vehicle key fob analysis

**Why:** Internal flash on STM32WB55 is limited. Moving apps to external storage frees flash for firmware core while keeping full functionality via SD card FAPs.

**How to apply:** When modifying apps, check whether they're internal or external (MENUEXTERNAL). Library dependencies for external apps use symlink + fap_private_libs pattern.
