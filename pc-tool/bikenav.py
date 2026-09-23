"""PC test tool for the BikeNav board.

Plays the phone's role over BLE so the board can be tested before the iPhone app
exists, and reads the board's serial log.

    python bikenav.py scan
    python bikenav.py send --dir right --dist 350 --street "MG Road"
    python bikenav.py demo
    python bikenav.py sygic --dir right --text 350m --limit 50
    python bikenav.py idle
    python bikenav.py serial --port COM5
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import time

SERVICE_UUID = "dd3f0ad1-6239-4e1f-81f1-91f6c9f01d86"
INDICATE_UUID = "dd3f0ad2-6239-4e1f-81f1-91f6c9f01d86"
WRITE_UUID = "dd3f0ad3-6239-4e1f-81f1-91f6c9f01d86"
DEVICE_NAME = "BikeNav"

# Direction codes, see PROTOCOL.md.
DIRECTIONS = {
    "none": 0, "start": 1, "slight-left": 2, "slight-right": 3, "destination": 4,
    "via": 5, "keep-left": 6, "keep-right": 7, "left": 8, "off-route": 9,
    "right": 10, "sharp-left": 11, "sharp-right": 12, "straight": 13,
    "uturn-left": 14, "uturn-right": 15, "ferry": 16, "state-boundary": 17,
    "follow": 18, "motorway": 19, "tunnel": 20, "exit-left": 21, "exit-right": 22,
}
_EXITS = ["se", "e", "ne", "n", "nw", "w", "sw", "s"]
for _i, _e in enumerate(_EXITS):
    DIRECTIONS[f"rb-rht-{_e}"] = 23 + _i  # right-hand traffic, counter-clockwise
    DIRECTIONS[f"rb-lht-{_e}"] = 31 + _i  # left-hand traffic (India), clockwise

FLAG_REROUTING, FLAG_ARRIVED, FLAG_GPS_WEAK = 0x01, 0x02, 0x04
UNKNOWN_U32, UNKNOWN_U16, UNKNOWN_U8 = 0xFFFFFFFF, 0xFFFF, 0xFF
MAX_STREET_BYTES = 48


def direction_code(name: str) -> int:
    if name.isdigit():
        return int(name)
    try:
        return DIRECTIONS[name]
    except KeyError:
        raise SystemExit(f"unknown direction {name!r}; choose from: {', '.join(DIRECTIONS)}")


def truncate_utf8(text: str, max_bytes: int) -> bytes:
    data = text.encode("utf-8")
    if len(data) <= max_bytes:
        return data
    return data[:max_bytes].decode("utf-8", errors="ignore").encode("utf-8")


def packet_full(direction: int, dist_m: int, remaining_m: int = UNKNOWN_U32,
                minutes_left: int = UNKNOWN_U16, speed_kmh: int = UNKNOWN_U8,
                limit_kmh: int = 0, then_direction: int = 0, street: str = "",
                flags: int = 0) -> bytes:
    head = struct.pack("<BBBIIHBBB", 0x02, direction, flags, dist_m, remaining_m,
                       minutes_left, speed_kmh, limit_kmh, then_direction)
    return head + truncate_utf8(street, MAX_STREET_BYTES)


def packet_basic(direction: int, text: str, limit_kmh: int = 0) -> bytes:
    return bytes([0x01, limit_kmh, direction]) + text.encode("ascii")


def packet_clock() -> bytes:
    offset_min = -(time.altzone if time.localtime().tm_isdst else time.timezone) // 60
    return struct.pack("<BIh", 0x03, int(time.time()), offset_min)


PACKET_IDLE = bytes([0x04])


async def find_board(timeout: float = 8.0):
    from bleak import BleakScanner

    print(f"Scanning for {DEVICE_NAME} ({timeout:.0f} s)...")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for device, adv in devices.values():
        if SERVICE_UUID in [u.lower() for u in adv.service_uuids] or adv.local_name == DEVICE_NAME:
            print(f"Found {adv.local_name or device.name} at {device.address} (RSSI {adv.rssi})")
            return device
    raise SystemExit("Board not found. Is it powered, and not connected to the phone?")


class Link:
    """Connection to the board that logs the keep-alive indications."""

    def __init__(self, client):
        self.client = client

    @classmethod
    async def open(cls):
        from bleak import BleakClient

        device = await find_board()
        client = BleakClient(device)
        await client.connect()
        print(f"Connected, MTU {client.mtu_size}")
        link = cls(client)
        try:
            await client.start_notify(INDICATE_UUID, link._on_indicate)
        except Exception as exc:  # keep going: indications are only a keep-alive
            print(f"(could not subscribe to indications: {exc})")
        return link

    def _on_indicate(self, _char, data: bytearray):
        print(f"  <- board: {data.hex()}")

    async def send(self, packet: bytes):
        print(f"  -> {packet.hex()}")
        await self.client.write_gatt_char(WRITE_UUID, packet, response=True)

    async def close(self):
        await self.client.disconnect()


# A short scripted ride: (direction, metres to maneuver, street, the maneuver after).
DEMO_LEGS = [
    ("start", 40, "Gachibowli Road", "right"),
    ("right", 350, "Old Mumbai Highway", "rb-lht-n"),
    ("rb-lht-n", 220, "Biodiversity Junction", "keep-left"),
    ("keep-left", 600, "Flyover", "slight-right"),
    ("slight-right", 180, "Road No. 36", "uturn-right"),
    ("uturn-right", 90, "Road No. 36", "left"),
    ("left", 260, "Jubilee Hills Road No. 45", "destination"),
    ("destination", 120, "Destination", "none"),
]


async def run_demo(step_s: float, speed_kmh: int):
    link = await Link.open()
    try:
        await link.send(packet_clock())
        remaining = sum(leg[1] for leg in DEMO_LEGS)
        metres_per_step = max(10, round(speed_kmh / 3.6 * step_s))
        for direction, length, street, then in DEMO_LEGS:
            for dist in range(length, -1, -metres_per_step):
                minutes = max(1, round(remaining / (speed_kmh / 3.6) / 60))
                await link.send(packet_full(
                    DIRECTIONS[direction], dist, remaining, minutes, speed_kmh,
                    limit_kmh=50, then_direction=DIRECTIONS[then], street=street))
                remaining = max(0, remaining - metres_per_step)
                await asyncio.sleep(step_s)
        await link.send(packet_full(DIRECTIONS["destination"], 0, 0, 0, 0,
                                    street="You have arrived", flags=FLAG_ARRIVED))
        await asyncio.sleep(3)
        await link.send(PACKET_IDLE)
    finally:
        await link.close()


async def run_sygic_demo(step_s: float):
    """Imitate Sygic's BLE HUD: type 0x01 packets, distance as text, speed limit."""
    link = await Link.open()
    try:
        for direction, metres, limit in [("right", 400, 50), ("rb-lht-n", 250, 40),
                                         ("keep-left", 600, 60), ("left", 300, 50),
                                         ("destination", 120, 0)]:
            while metres > 0:
                text = f"{metres}m" if metres < 1000 else f"{metres / 1000:.1f}km"
                await link.send(packet_basic(DIRECTIONS[direction], text, limit))
                metres -= 20
                await asyncio.sleep(step_s)
        await link.send(PACKET_IDLE)
    finally:
        await link.close()


async def send_once(packet: bytes, hold_s: float):
    link = await Link.open()
    try:
        await link.send(packet_clock())
        await link.send(packet)
        if hold_s:
            print(f"Holding the connection for {hold_s:.0f} s...")
            await asyncio.sleep(hold_s)
    finally:
        await link.close()


def read_serial(port: str, baud: int):
    import serial

    with serial.Serial(port, baud, timeout=1) as ser:
        print(f"Reading {port} at {baud} baud, Ctrl+C to stop")
        while True:
            line = ser.readline()
            if line:
                sys.stdout.write(line.decode("utf-8", errors="replace"))
                sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("scan", help="find the board")

    p = sub.add_parser("send", help="send one full navigation packet")
    p.add_argument("--dir", default="right")
    p.add_argument("--dist", type=int, default=350, help="metres to the maneuver")
    p.add_argument("--street", default="MG Road")
    p.add_argument("--then", default="none", help="maneuver after this one")
    p.add_argument("--remaining", type=int, default=5400, help="metres to destination")
    p.add_argument("--minutes", type=int, default=12)
    p.add_argument("--speed", type=int, default=32)
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--hold", type=float, default=10, help="seconds to stay connected")

    p = sub.add_parser("sygic", help="send a Sygic-format (0x01) packet")
    p.add_argument("--dir", default="right")
    p.add_argument("--text", default="350m")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--hold", type=float, default=10)
    p.add_argument("--demo", action="store_true", help="play a ride in Sygic's format")

    p = sub.add_parser("demo", help="play a scripted ride")
    p.add_argument("--step", type=float, default=0.5, help="seconds between updates")
    p.add_argument("--speed", type=int, default=40, help="simulated km/h")

    p = sub.add_parser("idle", help="clear the route")
    p.add_argument("--hold", type=float, default=3)

    p = sub.add_parser("serial", help="print the board's serial log")
    p.add_argument("--port", required=True)
    p.add_argument("--baud", type=int, default=2000000)

    args = parser.parse_args()
    if args.cmd == "scan":
        asyncio.run(find_board())
    elif args.cmd == "send":
        pkt = packet_full(direction_code(args.dir), args.dist, args.remaining, args.minutes,
                          args.speed, args.limit, direction_code(args.then), args.street)
        asyncio.run(send_once(pkt, args.hold))
    elif args.cmd == "sygic":
        if args.demo:
            asyncio.run(run_sygic_demo(0.4))
        else:
            asyncio.run(send_once(packet_basic(direction_code(args.dir), args.text, args.limit), args.hold))
    elif args.cmd == "demo":
        asyncio.run(run_demo(args.step, args.speed))
    elif args.cmd == "idle":
        asyncio.run(send_once(PACKET_IDLE, args.hold))
    elif args.cmd == "serial":
        read_serial(args.port, args.baud)


if __name__ == "__main__":
    main()
