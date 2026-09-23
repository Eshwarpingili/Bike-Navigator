# BikeNav

Turn-by-turn directions for a bike, shown on an Ai-Thinker **Ai-M61-32S** 2.4" display
(ST7796, 320×240 landscape). The iPhone plans the route and follows your GPS; the board
shows the next turn as a big arrow with the distance, street, time left and arrival time.

```
iPhone (BikeNav app: Apple Maps route + GPS) ──BLE──▶ Ai-M61-32S board ──▶ 2.4" screen
```

No CarPlay: that needs Apple-licensed hardware, 5 GHz Wi-Fi and a video decoder this chip
doesn't have. No Wi-Fi hotspot either: directions are ~30-byte BLE messages.

| Folder | What |
|---|---|
| `firmware/` | Board firmware (C, FreeRTOS, LVGL 8.3, Bouffalo BLE stack) |
| `ios/` | iPhone app (SwiftUI, MapKit, CoreBluetooth) |
| `pc-tool/` | Laptop stand-in for the phone, to test the board over BLE |
| `PROTOCOL.md` | The BLE message format, shared by all three |
| `firmware/main/apple_link.c` | Clock + now-playing read straight from the iPhone (no app) |
| `.github/workflows/ios.yml` | Builds the iPhone app on a GitHub Mac (no Mac needed) |

## 1. Flash the board (Windows)

One-time setup is already done on this laptop (`third_party/` holds the SDK, the RISC-V
compiler and Ai-Thinker's board kit; the SDK has been patched with `firmware/tools/patch_sdk.sh`).

The board (Ai-Thinker AiPi-DSL) has no USB-serial chip: its USB-C goes straight to the
BL618, which only shows up as a serial port in **download mode**, and only for ~5 seconds
unless the flasher connects. So start the flasher first:

```powershell
cd C:\Users\Pingi\dev\bike-nav\firmware
.\build.ps1
.\flash_when_ready.ps1
```

Then: unplug the board, hold **BURN**, plug it in, keep holding ~3 s, release. (BURN + RST
does not work on this board.) When it prints `[All Success]`, press **RST** to start.

- The factory music demo is saved in `backups\factory_music_demo_8MB.bin`. To put it back:
  `.\flash.ps1 -Port COM6 -Restore backups\factory_music_demo_8MB.bin` (after BURN + plug-in;
  the COM number depends on which USB socket you use).
- Every flash-tool run needs a fresh BURN + plug-in.
- On boot the screen shows "Waiting for phone… Bluetooth name: BikeNav".
  Upside down? Hold a finger on the screen for 2 seconds; it remembers.
- The board's log is on UART pins (GPIO21, 2 Mbaud), not USB.

## Home screen: clock and what's playing

When no route is running, the board shows the time, the current track and artist, and
⏮ ⏯ ⏭ along the bottom — tap the lower third of the screen to skip back, play/pause or
skip forward.

None of that needs an app. The iPhone offers two standard services to any **paired**
accessory, and the firmware reads them as a GATT client:

- **Apple Media Service** — title, artist, play state, and remote commands. It reflects
  whatever is playing: Spotify, Apple Music, YouTube.
- **Current Time Service** — the phone's clock, so the time is right even when riding
  with Sygic.

Both need an encrypted link, so on first connection the board asks the phone to pair and
iOS shows a **Bluetooth Pairing Request**; tap Pair. The keys are stored on both sides
(`CONFIG_BT_SETTINGS`), so it is a one-time step. If you ever "Forget This Device" on the
phone, the board's saved key goes stale — pair again from Settings → Bluetooth.

## 2. Test the board from the laptop

```powershell
cd C:\Users\Pingi\dev\bike-nav
.venv\Scripts\python pc-tool\bikenav.py demo        # plays a short fake ride
.venv\Scripts\python pc-tool\bikenav.py send --dir rb-lht-n --dist 250 --street "MG Road"
.venv\Scripts\python pc-tool\bikenav.py idle                 # back to the clock screen
```

## 3. Ride with Sygic, no app of our own

Sygic GPS Navigation has a hidden, unofficial "BLE HUD" that speaks this protocol
(packet type `0x01`), so the board works with it as flashed.

1. Install **Sygic GPS Navigation & Maps** on the iPhone, download the India map.
2. Menu → Settings → Info → **About** → tap any item **3 times** → a new "About" appears
   at the top → **About → BLE HUD → Start**.
3. Keep Sygic in the foreground until it connects to the board, then start the route.
   Once it is connected and navigating, the phone screen can be locked.

It finds the board by service UUID, not by name (the two reference firmwares advertise
"ESP32 HUD" and "nRF51 HUD" and both work), so `BikeNav` is fine. The feature turns off
whenever Sygic restarts, so step 2 is needed each time. Sygic sends only the arrow, the
distance text and the speed limit: no street name, time left or arrival time.

Test what Sygic's packets look like without Sygic:

```powershell
.venv\Scripts\python pc-tool\bikenav.py sygic --demo
```

## 4. Install the iPhone app without a Mac

The app is written but has never been compiled — that needs a Mac, which these steps rent
for free from GitHub. Expect a round of compile fixes on the first run. It uses Apple Maps
routing (no key, no account); swapping in Google Routes or OpenRouteService is a small
change if Apple's directions disappoint.

1. Put this folder in a GitHub repository (private is fine) and push it. The
   **iOS app** workflow builds on a GitHub-hosted Mac (about 5 minutes; free on public
   repos, uses the monthly Actions minutes on private ones, where Mac minutes count 10×).
2. Open the workflow run → **Artifacts** → download `BikeNav-ipa` and unzip `BikeNav.ipa`.
3. On Windows, install iTunes and iCloud (the website versions, not the Microsoft Store ones), then
   [Sideloadly](https://sideloadly.io). Connect the iPhone by cable, drop `BikeNav.ipa`
   into Sideloadly, sign in with your Apple ID and press Start.
4. On the iPhone: Settings → Privacy & Security → **Developer Mode** → on (restarts), then
   Settings → General → VPN & Device Management → trust your Apple ID.
5. A free Apple ID signature lasts **7 days**; re-run Sideloadly weekly (a paid developer
   account makes it a year).

With a Mac instead: `brew install xcodegen && cd ios && xcodegen generate`, open
`BikeNav.xcodeproj`, pick your team, run on the phone.

## Using it

1. Power the board from the bike (12 V → 5 V USB converter).
2. Open BikeNav on the iPhone; the badge turns green ("Display connected").
3. Where to? → pick a place → Start. Lock the phone and put it away; guidance continues
   in the background (blue location pill), and the board updates about once a second.
4. Settings → **Test the display** plays a fake ride. Settings → "Traffic drives on the
   left" controls how roundabouts are drawn (on by default in India).

How the arrows are chosen: Apple Maps gives each step as text, not as a maneuver type.
The app reads the text when it's explicit ("Keep left", "At the roundabout…") and
otherwise measures the road's angle at the turn. Leaving the route by more than ~35 m for
3 GPS fixes triggers a reroute.

## Rebuilding the arrow images

`firmware/tools/gen_arrows.py` draws every arrow with Pillow and writes
`firmware/main/arrows.c` plus `firmware/tools/arrows_preview.png`.
