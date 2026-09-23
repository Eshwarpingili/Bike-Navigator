# BikeNav BLE protocol (v1)

The board is a BLE **peripheral**. The phone (or the PC test tool) is the **central**
and pushes small packets; the board only draws what it is told.

The service and characteristic UUIDs are the ones Sygic's hidden "BLE HUD" feature
uses, so the same board also works with Sygic (packet type `0x01`) without any app of
our own.

| Item | UUID | Properties |
|---|---|---|
| Service | `DD3F0AD1-6239-4E1F-81F1-91F6C9F01D86` | primary, advertised |
| Indicate | `DD3F0AD2-6239-4E1F-81F1-91F6C9F01D86` | indicate (board → phone keep-alive) |
| Write | `DD3F0AD3-6239-4E1F-81F1-91F6C9F01D86` | write, write-without-response |

Advertised name: `BikeNav`.

All multi-byte integers are **little-endian**. Byte 0 of every write is the packet type.

## `0x01` — basic (Sygic-compatible)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0x01` |
| 1 | 1 | speed limit, km/h (`0` = none) |
| 2 | 1 | direction code (table below) |
| 3 | n | distance as ASCII text, e.g. `350m` (optional trailing NUL) |

## `0x02` — full navigation state (BikeNav app)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0x02` |
| 1 | 1 | direction code |
| 2 | 1 | flags: bit0 rerouting, bit1 arrived, bit2 GPS weak |
| 3 | 4 | distance to the maneuver, metres (u32) |
| 7 | 4 | distance remaining to destination, metres (u32, `0xFFFFFFFF` unknown) |
| 11 | 2 | time remaining, minutes (u16, `0xFFFF` unknown) |
| 13 | 1 | current speed, km/h (`0xFF` unknown) |
| 14 | 1 | speed limit, km/h (`0` none) |
| 15 | 1 | direction code of the maneuver *after* this one (`0` none) |
| 16 | ≤ 48 | street name, UTF-8, no terminator needed |

Maximum length 64 bytes. Centrals should send it with *write-with-response* so that a
link with a small MTU falls back to a long write; the board accepts up to 128 bytes.

## `0x03` — clock sync

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0x03` |
| 1 | 4 | Unix time, seconds (u32) |
| 5 | 2 | UTC offset, minutes (i16), e.g. `330` for IST |

## `0x04` — idle

One byte, `0x04`. Clears the route and shows the connected/idle screen (clock).

## Direction codes

| Code | Meaning | Code | Meaning |
|---|---|---|---|
| 0 | none | 20 | tunnel |
| 1 | start | 21 | exit left |
| 2 | slight left | 22 | exit right |
| 3 | slight right | 23–30 | roundabout, right-hand traffic (counter-clockwise), exit SE, E, NE, N, NW, W, SW, S |
| 4 | destination | 31–38 | roundabout, left-hand traffic (clockwise, e.g. India), exit SE, E, NE, N, NW, W, SW, S |
| 5 | via point | | |
| 6 | keep left | | |
| 7 | keep right | | |
| 8 | left | | |
| 9 | off route | | |
| 10 | right | | |
| 11 | sharp left | | |
| 12 | sharp right | | |
| 13 | straight | | |
| 14 | U-turn left | | |
| 15 | U-turn right | | |
| 16 | ferry | | |
| 17 | state boundary | | |
| 18 | follow road | | |
| 19 | motorway | | |

Roundabout exit directions are relative to the direction you enter: N is straight
across, E is a right turn, W a left turn, S going back the way you came.

## Board → phone

The board sends an indication on `DD3F0AD2` every 4 s while connected and not receiving
data, carrying one byte: `0x01` = waiting for data. Sygic uses this as a keep-alive;
centrals may ignore it.
