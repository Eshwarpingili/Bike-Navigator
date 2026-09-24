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
| 5 | via point | 39 | slight bend left |
| 6 | keep left | 40 | sharp bend left |
| 7 | keep right | 41 | steep bend left |
| 8 | left | 42 | slight bend right |
| 9 | off route | 43 | sharp bend right |
| 10 | right | 44 | steep bend right |
| 11 | sharp left | 45 | flyover |
| 12 | sharp right | 46 | underpass |
| 13 | straight | | |
| 14 | U-turn left | | |
| 15 | U-turn right | | |
| 16 | ferry | | |
| 17 | state boundary | | |
| 18 | follow road | | |
| 19 | motorway | | |

Roundabout exit directions are relative to the direction you enter: N is straight
across, E is a right turn, W a left turn, S going back the way you came.

### A turn and a bend are different instructions

A turn (8, 10, 11, …) happens at a junction: slow, look, pick a branch. A bend
(39–44) is the road itself curving, with nothing to decide — what it asks for is
lean, not brakes. They carry different arrows because on two wheels they are not
the same instruction, and one word for both says nothing about whether to slow.

The severity words mean the same thing wherever they appear, but the angles
behind them differ between the two on purpose. Sixty degrees at a junction is
ordinary, because the rider was slowing for the junction anyway; sixty degrees
mid-road at speed is not. The word describes what the road asks of the rider,
not what a protractor says.

| | slight | sharp | steep / U-turn |
|---|---|---|---|
| turn, at a junction | 20–45° | 100–160° | over 160° (U-turn) |
| bend, mid-road | 20–45° | 45–90° | over 90° |

Flyover and underpass (45, 46) are drawn the way a road map draws them: the road
passing underneath is the one with a gap in it, so an unbroken arrow means you
are the one on top.

## Board → phone

The board sends an indication on `DD3F0AD2` every 4 s while connected and not receiving
data, carrying one byte: `0x01` = waiting for data. Sygic uses this as a keep-alive;
centrals may ignore it.
