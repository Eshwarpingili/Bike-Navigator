# BikeNav — Product Requirements

A handlebar display for a bike: the next turn, the time, and what's playing — from an
iPhone, over Bluetooth Low Energy, with **no app to install and nothing to launch**.

Status as of 2026-09-23. This document is the single source of truth for what the
product is, what it deliberately is not, and what is left to build.

---

## 1. Why this exists

The rider wants three things while moving: **where do I turn**, **what time is it**,
and **what is playing / let me change it**. Anything that requires unlocking a phone,
opening an app, or re-pairing at the start of every ride has already failed.

The hard constraint that shapes everything below: **the link must survive the phone
being in a pocket with the screen off and the app switched away.** That single
requirement eliminated most of the obvious designs (see §7).

---

## 2. Hardware

| | |
|---|---|
| Board | Ai-Thinker **Ai-M61-32S** on an AiPi-DSL-Display carrier |
| SoC | Bouffalo **BL618** — 320 MHz RISC-V, 532 KB SRAM + 4 MB PSRAM, 8 MB flash |
| Radio | Wi-Fi 6 (2.4 GHz), **BLE 5.3** — no Bluetooth Classic in the shipped controller |
| Display | ST7789V over SPI, **240×320 native**, driven rotated to **320×240 landscape** |
| Touch | **CST816D** capacitive, I²C on GPIO 10/11 |
| Flashing | Hold **BURN** while plugging in USB; the download window is ~5 s |

Two hardware facts cost real debugging time and are written down so they are never
rediscovered:

- **The UART pins do not reach the USB connector.** There is no serial console. The
  screen is the only console, which is why the firmware carries an on-screen log
  (`logbuf.c`).
- **The touch panel's axes run opposite to its pixel origin.** The vendor LVGL port
  inverts both axes; LVGL then applies the display rotation separately. Both
  transforms are required. Removing either one mirrors every tap through the centre.

---

## 3. What the phone gives us, and what it does not

Everything on screen comes from standard BLE services the iPhone exposes to any
bonded accessory. No app, no MFi chip, no entitlements.

| Service | Gives us | Notes |
|---|---|---|
| **AMS** (Apple Media Service) | Track title, artist, album, duration, play/pause state; and accepts play/pause/next/previous commands | Works with **any** app that publishes to Now Playing — YT Music, Spotify, Apple Music, podcasts |
| **CTS** (Current Time Service) | Wall-clock time | Sets the clock with no user action |
| **Custom GATT service** | Turn-by-turn packets | Matches Sygic's BLE HUD protocol, see `PROTOCOL.md` |

### Not available, and why

- **Album artwork.** AMS Track exposes exactly four attributes — Artist, Album,
  Title, Duration. There is no artwork field. Art can only come from a companion app
  or a network lookup. *Decision: skipped.*
- **Apple CarPlay.** Requires an MFi authentication coprocessor, a 5 GHz Wi-Fi access
  point, and H.264 decode. The BL618 has none of the three. *Decision: impossible,
  closed.*
- **Live map tiles.** Would need the phone's hotspot on continuously. *Deferred.*

---

## 4. Functional requirements

### 4.1 Connection — the thing that must not break

| ID | Requirement |
|---|---|
| C1 | The board advertises its service UUID so iOS can find it; the name is in the scan response |
| C2 | On connect, the board requests encryption (`BT_SECURITY_L2`) — AMS and CTS require a bonded, encrypted link |
| C3 | The link survives the phone being locked, pocketed, and switched between apps |
| C4 | After a dropped connection the board re-advertises and iOS reconnects on its own |
| C5 | Discovery retries indefinitely — 5 s for the first few rounds, then every 20 s |

### 4.2 Music

| ID | Requirement |
|---|---|
| M1 | Show the current title and artist; scroll titles too long for the screen |
| M2 | Show play/pause state, and dim the title when paused |
| M3 | Tapping ⏮ / ⏯ / ⏭ controls the phone |
| M4 | **The song currently playing appears on connect** — not only after the next track change |
| M5 | Works with whatever app is playing, with no per-app configuration |

> **M4 is subtle and was a real bug.** iOS keeps an accessory's Entity Update
> registration across reconnects and only notifies on *change*. Re-registering
> identically therefore sends nothing, and the screen stays blank until the next
> song. The fix is to cancel the registration (write the entity ID with no
> attributes) and then re-add it, which makes it new again.

### 4.3 Navigation

| ID | Requirement |
|---|---|
| N1 | Show the next manoeuvre: arrow, distance, street name |
| N2 | Show the following manoeuvre ("then …") when space allows |
| N3 | Speed limit badge when the route supplies one |
| N4 | Remaining distance, time left, and arrival time |
| N5 | Return to the home screen when a route ends — Sygic never says "finished", so this is inferred from the instruction going unchanged for 3 minutes |
| N6 | Mark the display stale if no packet arrives for 8 s |

### 4.4 Display

| ID | Requirement |
|---|---|
| D1 | A widget layout: a route card above a music card, on a plain background |
| D2 | The route card holds the turn when there is one, and the clock when there is not — never dead space |
| D3 | The clock is always visible in the top bar, so time never disappears behind navigation |
| D4 | **Day/night palette chosen from the phone's clock** — light 07:00–19:00, dark otherwise, dark until the phone shares the time |
| D5 | High contrast in both palettes; readable at a glance, in motion, in sunlight |
| D6 | Controls sit clear of the bottom edge, not against the rim |
| D7 | Screen rotation flips with a 2-second press and persists across reboots |
| D8 | Diagnostics appear **only** when the link is broken, never during normal use |

### 4.5 Touch

| ID | Requirement |
|---|---|
| T1 | Tap targets are generous — aimed at with a thumb, without looking |
| T2 | A tap registers regardless of how long the finger rests on the glass |
| T3 | No touch interaction can drop the Bluetooth connection |

> **T2 and T3 were both bugs.** T2: listening for `LV_EVENT_SHORT_CLICKED` silently
> discards any press longer than 400 ms, which is most deliberate presses. T3: the
> media command passed the GATT stack a write with a null callback; the stack calls
> it unconditionally on the reply, so the board faulted and rebooted — which the
> rider sees as "touching the screen disconnects it".

---

## 5. Architecture

```
  iPhone ──BLE──┬── AMS  ─────────┐
                ├── CTS  ─────────┤
                └── nav service ───┤
                                   ▼
 apple_link.c ── ble_nav.c ──▶ nav_state.c ──▶ ui.c ──▶ LVGL ──▶ ST7789V
  (GATT client)  (GATT server)  (mutexed,      (widgets)
                                 versioned)
                                     ▲
                        CST816D ─────┘ (main.c: taps → media commands)
```

| File | Responsibility |
|---|---|
| `apple_link.c` | AMS + CTS client. A tick-driven step machine — one GATT request at a time, never started from inside a callback, because the stack rejects overlapping discovery |
| `ble_nav.c` | GATT server for navigation packets; advertising; connection callbacks |
| `nav_state.c` | The single shared state, mutex-protected, with a version counter so the UI can skip redraws |
| `ui.c` | All widgets and both palettes |
| `logbuf.c` | 7-line RAM ring buffer shown on screen — the board's only console |
| `main.c` | Board bring-up, LVGL task, touch handling, rotation persistence |

### Design rules learned the hard way

1. **One GATT request at a time, always from the tick.** Starting a request inside a
   callback is rejected by this stack (`BFLB_BLE_DISCOVER_ONGOING`).
2. **Never clear a `bt_gatt_subscribe_params` while it is live.** The stack keeps
   them in an intrusive linked list; zeroing one unlinks everything behind it, and
   arriving notifications then have nowhere to land.
3. **Do not trust descriptor discovery on this stack.** It returns nothing at all
   inside Apple's services. Derive the notification descriptor from the
   characteristic properties instead, and confirm it by writing to it.
4. **Anchor idle timers explicitly.** `next_step()` zeroes the timestamp, so an
   unanchored "have we waited long enough" check sees an enormous elapsed time and
   always answers yes.

### The descriptor problem, written down once

AMS characteristics report properties `0x98` = write + notify + **extended
properties**. That last bit means a read-only `0x2900` descriptor sits immediately
after the value handle, pushing the notification descriptor to **value + 2**, not the
usual value + 1. Writing to value + 1 hits the read-only descriptor and returns ATT
error `0x03`, which in turn makes every Entity Update write fail with AMS error
`0xA0` ("not subscribed"). The handle map confirms it: service 66–76, Remote Command
value 67 (descriptors 68, 69), Entity Update value 71 (descriptors 72, **73**).

---

## 6. Current state

**Working**

- Pairing, encryption, and automatic reconnection with no app
- Clock from the phone
- Title, artist, and play/pause state from any player
- Turn-by-turn display, verified against Sygic and the PC test tool
- Widget layout with day/night palettes
- Rotation flip, persisted

**Open**

- Touch controls — mapping corrected from measurement; awaiting confirmation on hardware
- M4 (song on connect) — fix written; awaiting confirmation
- Navigation source: Sygic today; Apple Maps and Google Routes are unbuilt options
- The iOS companion app exists in `ios/` but is unbuilt — no Mac, so it needs GitHub
  Actions plus sideloading. Not on the critical path, since nothing requires it

---

## 7. Decisions, and what they cost

| Decision | Why |
|---|---|
| **BLE, not Bluetooth Classic** | The shipped controller has no BR/EDR. This also rules out AVRCP and its cover-art extension |
| **No companion app** | iOS suspends app-owned BLE links in the background. An accessory using only standard services is reconnected by iOS itself — this is the whole reason the link survives a pocketed phone |
| **AMS over a custom protocol** | Works with every music app, needs no configuration, and cannot be broken by an app update |
| **No album art** | AMS has no artwork field. The alternatives were a network lookup over the phone's hotspot, or the companion app. Not worth the battery or the complexity |
| **Sygic for navigation** | It already speaks a BLE HUD protocol, so no app had to be written to get turn-by-turn working |

---

## 8. Getting it onto a bike

Desk testing hides the failures that matter at 25 km/h. These are ordered by what
actually hurts during a ride.

### 8.1 It must not hang — done

A frozen screen still showing the last turn is worse than a blank one, because it
looks correct. There is no way to restart the board on a handlebar without stopping.

A hardware watchdog now reboots the board if either task stops running. Both the
drawing task and the Bluetooth task check in; the watchdog is only fed while **both**
are alive, so a stall in either one restarts the board rather than leaving a
convincing, stale screen. Recovery is automatic: iOS reconnects to a bonded accessory
without being asked.

### 8.2 It must not change state by accident — done

The two-second press that flips the screen could previously be triggered anywhere on
the glass. A bag strap or a glove resting against the display would turn the picture
upside down mid-ride, with no easy way to undo it while moving. The gesture is now
confined to the **top-left corner**, well away from the transport controls.

### 8.3 Power — bike supply, answered from the schematic

The board runs from the bike's USB-C charger, so it powers up with the ignition and
has no power button. That works because the BLE bond is stored in flash
(`CONFIG_BT_SETTINGS`), so every cold start reconnects on its own. Without persisted
bonds this design would mean re-pairing from iOS Settings at the start of every ride.

The schematic (`sch_aipi-dsl_2023-07-04.pdf`, archived copy — the link on Ai-Thinker's
own docs site is broken and returns the page shell) settles the hardware questions:

- **Backlight: GPIO14**, through a 1 K gate resistor into Q2, a CJ2301 N-channel
  MOSFET, switching the panel's `BL_A`/`BL_K` rails (5.1 Ω per connector). The gate is
  pulled up, which is why the backlight is on before any firmware touches the pin.
  Driving it high holds it on; PWM on it dims. **Implemented**: 1 kHz PWM, full
  brightness by day, 40 % after dark, floored at 5 % so no code path can black out
  the screen mid-ride.
- **No battery circuit at all.** Page 2 is the whole power section: USB `VBUS` → an
  RT8059 buck → 3.38 V. No charger IC, no battery connector, no ADC divider. A state
  of charge would need added hardware.

A pass-through USB power bank in line with the bike charger is the cheap way to ride
through cranking sags and keep the screen up briefly after switch-off, with no
modification to the board.

### 8.4 Mounting and weather — rider's side

The display connects to the carrier board by a **flat flex ribbon**, which is the most
fragile part of the whole build and the thing most likely to fail from vibration. Any
enclosure has to hold both boards rigidly and give that ribbon strain relief. The USB
port and the exposed carrier also need covering against rain and road spray.

### 8.5 Then

1. Progress bar for the current track, from AMS Duration and the elapsed time already
   present in PlaybackInfo — costs nothing extra over the wire
2. A second screen reachable by swipe, following the watch convention of one idea per
   screen (see §9)
3. Navigation without Sygic — Apple Maps has no third-party turn feed, so this means
   either the companion app or a routing API over the phone's hotspot

---

## 9. Prior art worth borrowing from

Open-source watch firmware solves the same problem — a small round-the-clock display,
LVGL, a tight RAM budget — and its conventions are worth following rather than
reinventing:

- **[InfiniTime](https://osrtos.com/projects/infinitime/)** (PineTime, 240×240, LVGL):
  one idea per screen, swipe between them, and a deliberately restrained UI so the
  MCU is never the bottleneck. Its app/watch-face separation is a good model for the
  "second screen" item above.
- **[ZSWatch](https://electronics.alibaba.com/question/zswatch-explained-open-source-smartwatch-for-developers)**
  (Zephyr + LVGL): built to run its whole UI inside ≤ 96 KB of heap — a useful
  discipline, since our LVGL draw buffers already live in PSRAM.
- **[SquareLine Studio workflow](https://www.instructables.com/Design-Watch-Face-With-LVGL/)**:
  design in a vector tool, export PNG assets, lay out the LVGL screen. Worth adopting
  if the layout starts changing often, instead of hand-tuning pixel offsets.
- **[PineTime custom watch faces](https://pine64.org/documentation/PineTime/Watchfaces/Custom_watchface/)**:
  notes that real watches often use light faces for power reasons, while LCD UIs tend
  dark because black hides the bezel. Ours switches by time of day, which gets both.

The most useful idea from all of them for this project is **a simulator**. InfiniTime
builds watch faces to WebAssembly and renders LVGL to an HTML canvas, so layouts are
previewed in a browser instead of on hardware. Every layout change here currently
costs a BURN-and-replug cycle and a photograph — the single biggest drag on iteration
speed, and the clearest thing to fix next.

---

## 10. Glossary

| Term | Meaning |
|---|---|
| **AMS** | Apple Media Service — the BLE service an iPhone exposes for media info and control |
| **CTS** | Current Time Service — standard BLE clock service |
| **CCC / CCCD** | Client Characteristic Configuration Descriptor — the attribute a client writes to switch notifications on |
| **Entity Update** | The AMS characteristic a client writes to register interest in attributes |
| **ATT `0x03`** | Write Not Permitted — returned when writing to a read-only attribute |
| **AMS `0xA0`** | Invalid State — AMS's way of saying "you have not subscribed yet" |
