---
name: New SubGHz Automotive Protocols
description: 17+ new automotive RF protocols added to lib/subghz/protocols/ covering VAG, KIA/Hyundai, PSA, Ford, Fiat, Subaru, Suzuki, Mitsubishi, Porsche
type: project
---

New protocols added to `lib/subghz/protocols/` and registered in `protocol_items.h/.c`:

- **VAG GROUP** (VW/Audi/Skoda) — AUT64 + TEA encryption, 433 MHz
- **KIA/Hyundai V0-V6** — 6 protocol versions covering different generations, CRC8-based
- **PSA GROUP** (Peugeot/Citroen/DS/Opel) — TEA encryption with brute-force support, FM
- **Ford V0** — 315/433 MHz
- **Fiat SPA + Fiat Marelli** — Two supplier variants
- **Subaru, Suzuki, Porsche AG, Mazda Siemens** — Various automotive
- **Mitsubishi V0** — 868 MHz FM, scrambling + bit inversion
- **AUT64** — Supporting encryption library for VAG (12-round block cipher)

All protocols implement the standard SubGhzProtocolDecoder/Encoder interface with Decodable + Load + Save + Send capabilities.

Resource files added: expanded `keeloq_mfcodes` keystore and new `vag` encrypted keystore.
