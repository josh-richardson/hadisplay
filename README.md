# hadisplay

Kobo e-ink display for Home Assistant, built with C++ and FBInk.

Renders a touch UI on the Kobo Clara Colour's e-ink screen and calls the Home Assistant REST API when buttons are tapped. Runs as a standalone app that replaces Nickel (Kobo stock software) while active, then returns to Nickel on exit.

## Targets

| Target | Description |
|---|---|
| **`hadisplay`** | Main application. Persistent event loop with FBInk rendering, touch input, and HA integration. Deployed to the Kobo. |
| **`hello_fbink_linuxfb`** | FBInk smoke test. Prints text to the framebuffer and exits after 5 seconds. Validates display access. |
| **`hello_fbink_mirror_x11`** | Local dev preview. Renders the scene through FBInk on a host Linux framebuffer and mirrors it into an X11 window. Requires a real `/dev/fb0`. |

## Project layout

```
src/
  app/               main hadisplay application (touch + HA)
    main.cpp
  common/            shared code (UI renderer, HA client)
    ha_client.{h,cpp}
    scene.{h,cpp}
  fbink_smoke/       FBInk smoke test entry point
    main.cpp
  fbink_mirror/      FBInk-to-X11 mirror entry point
    main.cpp
cmake/
  toolchains/
    kobo.cmake       cross-compile toolchain for Kobo ARM
  curl/              vendored curl headers (ABI-stable, for cross-compile)
  kobo-libs/         libcurl.so copied from Kobo (for linking)
scripts/
  deploy.sh          build, rsync to Kobo, restart on device
  run-hadisplay.sh   on-device launcher (kills Nickel, runs app, restarts Nickel)
docs/
  kobo-setup.md      device setup, SSH, deployment reference
archive/             development log and old artifacts (gitignored)
```

## Quick start

### Host build (all targets)

```bash
cmake -S . -B build -DHADISPLAY_FETCH_FBINK=ON
cmake --build build -j
```

### Cross-compile for Kobo

Requires the KOReader koxtoolchain (`arm-kobo-linux-gnueabihf-gcc` in PATH).

```bash
cmake -S . -B build-kobo \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/kobo.cmake \
  -DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
cmake --build build-kobo -j
```

### Deploy to Kobo

```bash
./scripts/deploy.sh
```

This builds, rsyncs the binary to `/mnt/onboard/.adds/hadisplay/`, kills any running instance, and restarts it. Requires `ssh kobo` to be configured.
It also syncs `run-hadisplay.sh` and starts the app through that launcher so exiting the app returns to Nickel correctly.

## Home Assistant configuration

The easiest option is to edit `hadisplay-config.json` in the hadisplay directory on the Kobo (`/mnt/onboard/.adds/hadisplay/hadisplay-config.json`):

```json
{
  "ha_url": "http://your-ha-instance:8123",
  "ha_token": "your_long_lived_access_token",
  "ha_weather_entity": "weather.forecast_home",
  "selected_light_ids": []
}
```

The app loads that JSON on startup and preserves those keys when it saves dashboard selections.

`.env` is still supported as a fallback:

```
HA_URL=http://your-ha-instance:8123
HA_TOKEN=your_long_lived_access_token
```

If `ha_url` or `ha_token` are missing from the JSON config, the app searches for `.env` relative to its working directory and executable path.

Current Clara Colour behavior:
- the dashboard is built from selected `light.*` entities from Home Assistant
- the detail view exposes brightness, RGB presets, and white presets where supported
- `EXIT` leaves hadisplay and returns to Nickel

## Running on the Kobo

### Via NickelMenu

Add a launcher entry at `/mnt/onboard/.adds/nm/hadisplay`:

```
menu_item:main:Hadisplay:cmd_spawn:quiet:exec /bin/sh /mnt/onboard/.adds/hadisplay/run-hadisplay.sh
```

The launcher script (`run-hadisplay.sh`) handles the full lifecycle:
1. Captures Nickel's environment (WiFi, dbus).
2. Kills Nickel and its helper processes.
3. Runs hadisplay with exclusive touch access.
4. Restarts Nickel on exit with Kobo's library path restored.

### Via SSH (manual)

```bash
ssh kobo
cd /mnt/onboard/.adds/hadisplay
killall nickel hindenburg sickel fickel strickel fontickel 2>/dev/null
LD_LIBRARY_PATH=/mnt/onboard/.niluje/usbnet/lib ./hadisplay
```

Directly launching `./hadisplay` this way does not restart Nickel when the app exits. For normal device use, prefer `run-hadisplay.sh` or the NickelMenu entry above.

## Device details

- **Device**: Kobo Clara Colour (device code 393), firmware 4.45.23646
- **Screen**: 1072x1448 portrait, framebuffer rotation=3
- **Touch**: Cypress cyttsp5_mt at `/dev/input/event1`, range X:0-1447, Y:0-1071
- **Touch mapping**: axes swapped + X inverted due to rotation=3
- **Current UI**: full-screen Clara-specific layout with live light state, sync, full refresh, and exit actions
- **Dependencies on device**: Kobo Stuff (SSH, rsync, libcurl), NickelMenu (launcher)
- **WiFi**: `ForceWifi=true` in `[DeveloperSettings]` keeps WiFi alive

## Kobo deployment reference

See [docs/kobo-setup.md](docs/kobo-setup.md) for the full device setup, SSH auth, and deployment details.

## Build options

| Option | Default | Description |
|---|---|---|
| `HADISPLAY_FETCH_FBINK` | `ON` | Fetch and build FBInk via ExternalProject |
| `HADISPLAY_BUILD_APP` | `ON` | Build the main `hadisplay` application |
| `HADISPLAY_BUILD_FBINK_SMOKE` | `ON` | Build the `hello_fbink_linuxfb` smoke test |
| `HADISPLAY_BUILD_FBINK_MIRROR_X11` | `ON` | Build the `hello_fbink_mirror_x11` dev preview |

The X11 mirror requires `libX11` and `libcurl` on the host. Disable it for cross-compilation:

```bash
-DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
```
