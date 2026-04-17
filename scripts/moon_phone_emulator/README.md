# Moon Phone Emulator

Python BLE peripheral that pretends to be a paired Android phone for
testing the Flipper's Moon Companion service end-to-end before the real
Android app exists.

Runs on macOS (Core Bluetooth via `bless`) or Linux (BlueZ via `bless`).

## Install

```bash
# Recommended — isolated venv so the deps don't pollute the toolchain
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Run

```bash
source .venv/bin/activate
python moon_phone.py
```

On first launch macOS will prompt to allow Bluetooth for your terminal
application (Terminal, iTerm, etc). Approve it in **System Settings ->
Privacy & Security -> Bluetooth**, then re-run.

## What it does

- Advertises the Moon Companion service UUID.
- Accepts `MoonRequest` writes on `RPC_TX` and replies with a
  `MoonPhoneMessage{MoonResponse}` notification on `RPC_RX`.
- First `PairRequest` wins: returns a freshly-generated 16-byte
  `auth_token`; subsequent requests must include it.
- Once subscribed via `SubscribePositionRequest`, streams synthetic
  `PositionData` events every N ms (walks slowly from the Flatiron
  building — hack the coords in `_START_LAT_E7` / `_START_LON_E7`).

## Regenerating protobuf bindings

If you change `moon_companion.proto`, regenerate the Python bindings:

```bash
protoc --python_out=. \
  -I../../applications/services/moon_companion/proto \
  ../../applications/services/moon_companion/proto/moon_companion.proto
```

(Use the `protoc` from the Moon-Firmware toolchain:
`toolchain/x86_64-darwin/bin/protoc`.)

## Debug logging

```bash
MOON_PHONE_DEBUG=1 python moon_phone.py
```

Shows every decoded `MoonRequest`, every outbound notification size,
and every position tick.
