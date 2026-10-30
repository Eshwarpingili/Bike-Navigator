# Code review, 2026-10-30

Files read in full: `ble_nav.c`, `mapdata.c`, `logbuf.c`, the `nav_state.c`
parser and text path, the `apple_link.c` AMS chain, and the routing path in
`Navigator.swift`. Severity is about what a rider experiences, not how ugly the
code is.

## Confirmed defects

### 1. HIGH — song titles are cut at 40 bytes
`firmware/main/nav_state.h:27` — `NAV_MUSIC_MAX 40`.

`copy_text` fills to `cap - 1` and stops, with no indication that anything was
dropped. Most song titles exceed 40 bytes once the artist suffix and the
`(From "...")` that Indian film music carries are counted, so the board shows a
chopped title and `music_state_word()` happily reports `ok`.

This is the reported "song is not being updated completely". Nothing upstream is
at fault: `CONFIG_BT_L2CAP_TX_MTU` is 247, so AMS can deliver ~241 bytes in one
notification. The board simply refuses to store them.

**Failure scenario:** play anything with a title over 40 characters. The board
shows the first 40 and never the rest, for the whole song.

### 2. HIGH — media commands race the AMS step machine
`firmware/main/apple_link.c`, `apple_link_media_command()`.

It is called from `ui_tap()` on the **LVGL task**, while `apple_link_tick()`
drives every other GATT request from the **BLE task** (`ble_nav.c:213`). It
`memset`s a `static struct bt_gatt_write_params` that the stack may still be
holding from the previous tap, and issues `bt_gatt_write()` with no regard for
`g_busy`.

This breaks the rule the rest of this file is built around - one request at a
time, always from the tick - and it is the best candidate for the "multiple bugs
with audio and navigation running simultaneously" report, which has never had a
reproduction.

**Failure scenario:** tap volume or next several times quickly while a route is
running. A live ATT request is overwritten; the reply lands on a reused params
block.

### 3. MEDIUM — a cancelled or superseded route can come back
`ios/BikeNav/Navigator.swift:121`.

The `MKDirections.calculate` completion handler has no generation guard. End the
route, or pick a new destination, while a request is in flight and the late
reply still calls `rebuild(with:)`, which sets `route`, `path` and `maneuvers`.

**Failure scenario:** rider taps End route during a reroute. Seconds later the
route reappears and the board starts giving directions again.

### 4. MEDIUM — the approach bar runs backwards between turns
`firmware/main/ui.c`, `glide_width()`.

The bar is animated to its new width over 900 ms whatever the change. When one
manoeuvre is completed and the next is 400 m away, it animates from the previous
turn's full width *down*, so the warning appears to retreat.

**Failure scenario:** any two turns in succession. Visible on every route.

### 5. MEDIUM — the subscription retry can starve the re-probe
`firmware/main/apple_link.c`, the `STEP_DONE` branch of `apple_link_tick()`.
Introduced in `d3a242d`, this session.

`(g_sub_lost || !g_subscribed)` jumps to `STEP_SUBSCRIBE` every 10 s and returns
before the `incomplete` check. If the handle is stale the subscribe fails
forever and the full re-probe that would fix it is never reached.

**Failure scenario:** phone drops the subscription and the handle goes stale.
The board retries the same dead handle every 10 s until it is power-cycled.

### 6. LOW — wrong colours for up to a minute after the day/night flip
`firmware/main/ui.c`, `ui_update()`.

`apply_theme()` runs before the redraw gate and resets label colours that
`draw_home`/`draw_nav` later override - the violet track title, the route
strip's severity. If no other state changed, the gate returns and those stay
default until the next change. The minute counter corrects it within 60 s, so it
is cosmetic, but it is a wrong colour on the one channel that is supposed to be
rigid.

## Checked and found sound

Worth recording, so the next review does not repeat the work.

- `ble_nav.c on_write` — `offset + len > RX_MAX` is checked before the `memcpy`,
  and long writes are handled by offset. Sound.
- `nav_state.c` packet parsers — `0x01` (len<3), `0x02` (len<16), `0x03` (len<7),
  `0x05` (count vs `NAV_ROUTE_MAX` *and* `len < 3 + 2*count`), `0x06` (len<11).
  Every copy goes through `copy_text` with `sizeof`. Sound.
- `packet_is_new` — memcmp is bounded by the stored length. Sound.
- `mapdata.c` — erased flash and a bad magic both exit cleanly; the header is
  checked for consistency (`tile_count == rows * cols`); a tile longer than the
  24 KB buffer is clamped; `road_class` and `count` are validated and the walk
  stops on anything malformed. `count` is a `uint8_t` and `MAP_MAX_POINTS` is
  255, so the point loop cannot overflow by construction. Sound.
- `logbuf.c` — both the write and the snapshot sit inside
  `taskENTER_CRITICAL()`. Sound.
- Route shape sizing — the app caps at 60 points (`Protocol.swift:208`), the
  board at `NAV_ROUTE_MAX 60`, giving a 123-byte packet against `RX_MAX 128`.
  Deliberately aligned, and commented as such on both sides. Sound.
- iOS location setup — `pausesLocationUpdatesAutomatically = false`,
  `allowsBackgroundLocationUpdates` toggled with the route, and a negative
  `horizontalAccuracy` is treated as a weak fix rather than a small one. Sound.

## Not fixable here

Inter and Montserrat carry ASCII and Latin-1 only. A title in Devanagari or any
other non-Latin script renders as nothing, and LVGL 8.3 cannot shape Indic text
at all. The diagnostic should say so rather than reporting `ok`, but the fix is
a different text engine, not a wider font.
