---
name: BLE App Ideas for Bloodmoon
description: Future BLE app ideas to build using BLE Full Extended stack - sniffer, notify listener, fingerprinter, relay
type: project
---

Built apps: BT Scanner, BT Explorer, BLE Cloner, BLE Beacon Toolkit, BLE RSSI Tracker

Future app ideas:
- **BLE Sniffer/Logger** — Log all advertising packets to SD card with timestamps, export as CSV
- **BLE Notify Listener** — Connect to device, subscribe to all notification-capable characteristics, display incoming data live
- **BLE Fingerprinter** — Identify devices by advertising patterns, service UUIDs, manufacturer data. Database of known device signatures (Apple, Samsung, Tile, AirTag)
- **BLE Relay/MITM** — Act as proxy between two devices, forward reads/writes. Advanced research tool

**Why:** Leverage the BLE 5.3 Full Extended stack with central mode, GATT client, and extended advertising.
**How to apply:** Each app uses the scanning/GATT client/extra beacon APIs added to Bloodmoon firmware.
