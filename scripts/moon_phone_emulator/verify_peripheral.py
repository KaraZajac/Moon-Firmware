#!/usr/bin/env python3
"""
Sanity check: use bleak (BLE central, pure Python) to connect to the
running moon_phone.py (BLE peripheral, bless) and verify it publishes
our service and characteristics to another central.

If this succeeds, bless is fine and the problem is Flipper-side.
If this fails the same way, bless itself isn't publishing the service.

Run *while* moon_phone.py is running, from a second terminal:
    .venv/bin/python verify_peripheral.py
"""
import asyncio
import logging
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "df98ad38-b0b8-44c0-bd88-905db2d6b365"
RPC_TX_UUID  = "7a30f8d0-2a4e-43b7-a383-1f786173991b"
RPC_RX_UUID  = "902dac74-55ba-46d4-8ff4-6d40893ae052"

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-5s %(message)s")
LOG = logging.getLogger("verify")


async def main() -> int:
    LOG.info("Scanning for MoonPhone...")
    device = None
    async with BleakScanner() as scanner:
        async for dev, adv in scanner.advertisement_data():
            if adv.local_name == "MoonPhone" or SERVICE_UUID in (adv.service_uuids or []):
                LOG.info("Found %s (%s) rssi=%d", dev.name, dev.address, adv.rssi)
                device = dev
                break

    if device is None:
        LOG.error("MoonPhone not found — is moon_phone.py running?")
        return 1

    LOG.info("Connecting...")
    async with BleakClient(device, timeout=10.0) as client:
        LOG.info("Connected; listing services")
        for service in client.services:
            LOG.info("SERVICE %s", service.uuid)
            for char in service.characteristics:
                LOG.info("  CHAR    %s props=%s", char.uuid, char.properties)

        found_service = any(s.uuid.lower() == SERVICE_UUID for s in client.services)
        found_tx = any(c.uuid.lower() == RPC_TX_UUID
                       for s in client.services for c in s.characteristics)
        found_rx = any(c.uuid.lower() == RPC_RX_UUID
                       for s in client.services for c in s.characteristics)

        LOG.info("Service present: %s", found_service)
        LOG.info("RPC_TX present:  %s", found_tx)
        LOG.info("RPC_RX present:  %s", found_rx)

        if not (found_service and found_tx and found_rx):
            LOG.error("FAIL: peripheral did not publish all expected attributes")
            return 2

        LOG.info("Subscribing to RPC_RX...")
        got = asyncio.Event()

        def on_notify(sender, data: bytearray) -> None:
            LOG.info("NOTIFY %d bytes: %s", len(data), data.hex())
            got.set()

        await client.start_notify(RPC_RX_UUID, on_notify)

        LOG.info("Writing a dummy byte to RPC_TX...")
        # A single 0x00 won't parse as a real MoonRequest but it will
        # exercise the write callback end-to-end.
        await client.write_gatt_char(RPC_TX_UUID, b"\x00", response=False)

        LOG.info("Waiting 2s for peripheral response (if any)...")
        try:
            await asyncio.wait_for(got.wait(), timeout=2.0)
            LOG.info("Peripheral notified back — round-trip works")
        except asyncio.TimeoutError:
            LOG.warning("No notification within 2s (may be expected if payload was junk)")

        await client.stop_notify(RPC_RX_UUID)

    LOG.info("OK — bless is exposing the service correctly to a bleak central")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
