#!/usr/bin/env python3
"""
Moon Companion phone emulator — pretends to be a paired Android phone
so we can exercise the Flipper-side BLE central + RPC stack without
shipping any real Android code yet.

On macOS this uses Core Bluetooth via `bless`. The Flipper scans for
our service UUID; we respond to MoonRequest writes on RPC_TX by
encoding MoonPhoneMessage{MoonResponse} or MoonPhoneMessage{MoonEvent}
notifications on RPC_RX.

Run:
    pip install -r requirements.txt
    python moon_phone.py

On first run macOS will prompt to allow Bluetooth for your terminal
(System Settings -> Privacy & Security -> Bluetooth).
"""

import asyncio
import logging
import os
import random
import signal
import struct
import sys
import time
from typing import Any, Optional

from bless import (
    BlessServer,
    BlessGATTCharacteristic,
    GATTCharacteristicProperties,
    GATTAttributePermissions,
)

# Import the generated protobuf module. It lives next to this script.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import moon_companion_pb2 as pb  # noqa: E402

# Must match moon_companion_ble.h on the Flipper side.
SERVICE_UUID = "df98ad38-b0b8-44c0-bd88-905db2d6b365"
RPC_TX_UUID  = "7a30f8d0-2a4e-43b7-a383-1f786173991b"  # Flipper writes here
RPC_RX_UUID  = "902dac74-55ba-46d4-8ff4-6d40893ae052"  # we notify here

LOG = logging.getLogger("moon-phone")

# ── Fake GPS track ────────────────────────────────────────────────────
# Emits positions that walk slowly — useful for visual testing. Starts
# at the Flatiron in NYC; feel free to pick a different coordinate for
# local testing.
_START_LAT_E7 = 407395680   # 40.739568  N
_START_LON_E7 = -739909780  # -73.990978 E


def _make_position(tick: int) -> pb.PositionData:
    """Return a PositionData that drifts slightly with each tick."""
    p = pb.PositionData()
    p.lat_e7       = _START_LAT_E7 + (tick * 50)       # ~5 mm / tick
    p.lon_e7       = _START_LON_E7 + (tick * 30)
    p.alt_mm       = 15_000
    p.accuracy_mm  = 2_500                              # 2.5 m CEP
    p.speed_mmps   = 1_200                              # 1.2 m/s walking
    p.heading_cdeg = (tick * 18) % 36000                # spinning slowly
    p.timestamp_ms = int(time.time() * 1000)
    p.fix_quality  = pb.FIX_3D
    p.satellites   = 9
    p.source_id    = b"moonemul"                        # 8 bytes
    return p


# ── Emulator state ────────────────────────────────────────────────────

class PhoneState:
    def __init__(self) -> None:
        self.paired: bool = False
        self.auth_token: bytes = b""
        self.subscribed: bool = False
        self.subscribe_interval_ms: int = 2000
        self.position_tick: int = 0
        self.rx_char: Optional[BlessGATTCharacteristic] = None
        self.server: Optional[BlessServer] = None
        # True once the Flipper subscribes to the RX characteristic's
        # CCCD. Without this guard our notifications are dropped by the
        # link layer.
        self.notify_ready: bool = False


STATE = PhoneState()


def _new_auth_token() -> bytes:
    return random.randbytes(16)


# ── GATT read / write handlers ───────────────────────────────────────

def read_request(characteristic: BlessGATTCharacteristic, **kwargs: Any) -> bytearray:
    # RPC_TX is write-only from our perspective. Return empty on any read.
    LOG.debug("read request on %s — ignoring", characteristic.uuid)
    return bytearray()


def _send_phone_message(msg: pb.MoonPhoneMessage) -> None:
    """Serialize a MoonPhoneMessage and push it out as an RX notification."""
    if STATE.server is None or STATE.rx_char is None:
        LOG.warning("server/rx_char not ready, dropping outbound message")
        return
    payload = msg.SerializeToString()
    LOG.debug("TX %d bytes", len(payload))
    STATE.rx_char.value = payload
    # Without the explicit update_value() call bless doesn't notify.
    STATE.server.update_value(SERVICE_UUID, RPC_RX_UUID)


def _handle_pair(req: pb.MoonRequest) -> pb.MoonResponse:
    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_OK

    STATE.auth_token = _new_auth_token()
    STATE.paired = True

    pr = resp.pair
    pr.auth_token = STATE.auth_token
    pr.phone_name = "Moon Phone Emulator"

    LOG.info("Paired! Issued auth_token=%s", STATE.auth_token.hex())
    return resp


def _check_auth(req: pb.MoonRequest) -> Optional[pb.MoonResponse]:
    """Return a MOON_UNAUTHORIZED response if the token doesn't match."""
    if not STATE.paired:
        resp = pb.MoonResponse()
        resp.request_id = req.request_id
        resp.status = pb.MOON_UNAUTHORIZED
        return resp
    if req.auth_token != STATE.auth_token:
        resp = pb.MoonResponse()
        resp.request_id = req.request_id
        resp.status = pb.MOON_UNAUTHORIZED
        return resp
    return None


def _handle_get_position(req: pb.MoonRequest) -> pb.MoonResponse:
    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_OK
    resp.position.CopyFrom(_make_position(STATE.position_tick))
    return resp


def _handle_subscribe_position(req: pb.MoonRequest) -> pb.MoonResponse:
    STATE.subscribed = True
    STATE.subscribe_interval_ms = max(500, req.subscribe_position.interval_ms or 2000)
    LOG.info("Position subscription on (interval=%d ms)", STATE.subscribe_interval_ms)

    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_OK
    return resp


def _handle_unsubscribe_position(req: pb.MoonRequest) -> pb.MoonResponse:
    STATE.subscribed = False
    LOG.info("Position subscription off")
    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_OK
    return resp


def _handle_get_time(req: pb.MoonRequest) -> pb.MoonResponse:
    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_OK
    resp.time.unix_ms = int(time.time() * 1000)
    resp.time.tz_offset_seconds = -time.timezone
    resp.time.tz_name = "UTC"  # good enough for testing
    return resp


def _handle_send_notification(req: pb.MoonRequest) -> pb.MoonResponse:
    n = req.send_notification
    LOG.info("Notification from Flipper: [%s] %s (priority=%d)",
             n.title, n.body, n.priority)
    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_OK
    resp.notification.delivered = True
    return resp


def _dispatch_request(req: pb.MoonRequest) -> Optional[pb.MoonResponse]:
    kind = req.WhichOneof("payload")
    LOG.info("RX MoonRequest rid=%d kind=%s", req.request_id, kind)

    # PairRequest is the only unauthenticated request we honour.
    if kind == "pair":
        return _handle_pair(req)

    # All other requests require a valid auth_token.
    unauth = _check_auth(req)
    if unauth is not None:
        LOG.warning("rejecting %s — unauthenticated", kind)
        return unauth

    if kind == "get_position":
        return _handle_get_position(req)
    if kind == "subscribe_position":
        return _handle_subscribe_position(req)
    if kind == "unsubscribe_position":
        return _handle_unsubscribe_position(req)
    if kind == "get_time":
        return _handle_get_time(req)
    if kind == "send_notification":
        return _handle_send_notification(req)

    # Tier 2 stubs — ack as unsupported.
    resp = pb.MoonResponse()
    resp.request_id = req.request_id
    resp.status = pb.MOON_UNSUPPORTED_REQUEST
    LOG.info("No handler for %s — returning UNSUPPORTED_REQUEST", kind)
    return resp


def write_request(
    characteristic: BlessGATTCharacteristic,
    value: bytearray,
    **kwargs: Any,
) -> None:
    """Inbound Flipper-to-phone frame. Parse as MoonRequest, dispatch, notify."""
    LOG.debug("RX %d bytes on %s", len(value), characteristic.uuid)

    req = pb.MoonRequest()
    try:
        req.ParseFromString(bytes(value))
    except Exception as exc:
        LOG.exception("Failed to parse inbound MoonRequest: %s", exc)
        return

    resp = _dispatch_request(req)
    if resp is None:
        return

    msg = pb.MoonPhoneMessage()
    msg.response.CopyFrom(resp)
    _send_phone_message(msg)


def subscribe_request(characteristic: BlessGATTCharacteristic) -> None:
    """Called by bless when a central subscribes to a notify characteristic."""
    LOG.info("Central subscribed to %s", characteristic.uuid)
    if str(characteristic.uuid).lower() == RPC_RX_UUID.lower():
        STATE.notify_ready = True


# ── Position streamer ────────────────────────────────────────────────

async def position_loop() -> None:
    """While subscribed, push a PositionData event every N ms."""
    while True:
        if STATE.subscribed and STATE.notify_ready and STATE.paired:
            STATE.position_tick += 1
            event = pb.MoonEvent()
            event.position_update.CopyFrom(_make_position(STATE.position_tick))
            msg = pb.MoonPhoneMessage()
            msg.event.CopyFrom(event)
            LOG.debug("emit position tick=%d", STATE.position_tick)
            _send_phone_message(msg)
        await asyncio.sleep(STATE.subscribe_interval_ms / 1000.0)


# ── Server setup ─────────────────────────────────────────────────────

async def main() -> None:
    logging.basicConfig(
        level=logging.DEBUG if os.environ.get("MOON_PHONE_DEBUG") else logging.INFO,
        format="%(asctime)s %(levelname)-5s %(name)s %(message)s",
    )

    LOG.info("Moon Phone Emulator starting")
    LOG.info("Service: %s", SERVICE_UUID)
    LOG.info("RPC_TX  (writes from Flipper): %s", RPC_TX_UUID)
    LOG.info("RPC_RX  (notifies to Flipper): %s", RPC_RX_UUID)

    server = BlessServer(name="MoonPhone")
    server.read_request_func = read_request
    server.write_request_func = write_request
    # Bless surfaces subscription events differently on each platform; we
    # optimistically mark notify_ready the first time the Flipper writes
    # a request, which guarantees it's connected and subscribed by then.

    await server.add_new_service(SERVICE_UUID)

    # RPC_TX (write, no response). Flipper-originated MoonRequest frames
    # land here.
    await server.add_new_characteristic(
        SERVICE_UUID,
        RPC_TX_UUID,
        GATTCharacteristicProperties.write
        | GATTCharacteristicProperties.write_without_response,
        None,
        GATTAttributePermissions.writeable,
    )

    # RPC_RX (notify). Phone-originated MoonPhoneMessage frames are
    # published here.
    await server.add_new_characteristic(
        SERVICE_UUID,
        RPC_RX_UUID,
        GATTCharacteristicProperties.notify | GATTCharacteristicProperties.read,
        None,
        GATTAttributePermissions.readable,
    )

    STATE.server = server
    STATE.rx_char = server.get_characteristic(RPC_RX_UUID)

    # Treat any successful write as the notify-ready signal so we start
    # the GPS stream even if bless never fires a subscribe callback on
    # this platform.
    original_write = server.write_request_func

    def write_and_mark(characteristic: BlessGATTCharacteristic,
                       value: bytearray, **kwargs: Any) -> None:
        STATE.notify_ready = True
        original_write(characteristic, value, **kwargs)

    server.write_request_func = write_and_mark

    LOG.info("Starting advertising as 'MoonPhone'")
    await server.start()
    LOG.info("Ready — pair from the Flipper's Moon Companion settings")

    # Run the position streamer in parallel.
    stream_task = asyncio.create_task(position_loop())

    # Hold forever until ^C.
    stop = asyncio.Event()
    loop = asyncio.get_event_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    await stop.wait()

    LOG.info("Shutting down")
    stream_task.cancel()
    await server.stop()


if __name__ == "__main__":
    asyncio.run(main())
