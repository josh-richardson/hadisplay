# hadisplay

`hadisplay` is a native Kobo framebuffer app for controlling Home Assistant from an e-ink touchscreen.

It runs as a standalone app on the Kobo, takes over the screen while active, and returns to Nickel when it exits. The current UI includes a top status bar plus a Home Assistant dashboard for lights, switches, and climate devices.

## Status

- Only tested on the **Kobo Clara Colour N367B**.
- It will likely **not** work on other Kobo models without additional development work.
- Current device assumptions include Clara Colour framebuffer behavior, touch mapping, and backlight/status paths.

## Current features

- Top status bar with:
  - time and date
  - weather from Home Assistant
  - Wi-Fi status
  - battery status
  - backlight toggle
- Setup flow for selecting which Home Assistant entities appear on the dashboard
- Dashboard cards for:
  - `light.*`
  - `switch.*`
  - `climate.*`
- Light detail view with:
  - on/off
  - brightness `+/-`
  - RGB preset buttons
  - white preset buttons
- Climate detail view with:
  - current and target temperature
  - heating state
  - heat on/off control
- Config persisted to JSON on the device

## Kobo requirements

The Kobo needs these components installed:

- **NickelMenu**
- **KOReader**
- **Kobo Stuff**

Notes:

- NickelMenu is used to launch `hadisplay`.
- KOReader is part of the tested Kobo setup and should already be installed on the device.
- Kobo Stuff provides the SSH, `rsync`, and shared library environment this project currently relies on.

## Project layout

```text
src/
  app/
    main.cpp
  common/
    app_config.{h,cpp}
    ha_client.{h,cpp}
    json.{h,cpp}
    scene.{h,cpp}
    scene_draw.{h,cpp}
    scene_icons.{h,cpp}
    scene_layout.{h,cpp}
    scene_style.h
    system_status.{h,cpp}
  fbink_smoke/
    main.cpp
  fbink_mirror/
    main.cpp
cmake/
  toolchains/
    kobo.cmake
  curl/
  kobo-libs/
scripts/
  deploy.sh
  run-hadisplay.sh
docs/
  kobo-setup.md
```

## Building

### Host build

```bash
cmake -S . -B build -DHADISPLAY_FETCH_FBINK=ON
cmake --build build -j
```

### Kobo cross-build

Requires the KOReader `koxtoolchain` with `arm-kobo-linux-gnueabihf-gcc` available in `PATH`.

```bash
cmake -S . -B build-kobo \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/kobo.cmake \
  -DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
cmake --build build-kobo -j
```

## Deploying

```bash
./scripts/deploy.sh
```

This will:

- build the Kobo target
- copy `hadisplay` and `run-hadisplay.sh` to `/mnt/onboard/.adds/hadisplay/`
- restart the app on the device
- tail the device log

It expects `ssh kobo` to be configured.

## Home Assistant configuration

Configuration lives in:

`/mnt/onboard/.adds/hadisplay/hadisplay-config.json`

Example:

```json
{
  "ha_url": "http://your-ha-instance:8123",
  "ha_token": "your_long_lived_access_token",
  "ha_weather_entity": "weather.forecast_home",
  "display_mode": "auto",
  "selected_entity_ids": [
    "light.kitchen",
    "switch.lamp_socket",
    "climate.hallway"
  ]
}
```

Behavior:

- `ha_url`, `ha_token`, and `ha_weather_entity` are loaded at startup
- `display_mode` accepts `auto`, `grayscale`, or `color`
- selected dashboard entities are persisted back into the same file
- if no config exists, the app can enumerate Home Assistant entities and let the user select what should appear on the dashboard

`.env` is still supported as a fallback:

```text
HA_URL=http://your-ha-instance:8123
HA_TOKEN=your_long_lived_access_token
```

If `ha_url` or `ha_token` are missing from the JSON config, the app falls back to `.env`.

## Running on the Kobo

### NickelMenu entry

Create a launcher entry in `/mnt/onboard/.adds/nm/hadisplay`:

```text
menu_item:main:Hadisplay:cmd_spawn:quiet:exec /bin/sh /mnt/onboard/.adds/hadisplay/run-hadisplay.sh
```

`run-hadisplay.sh`:

- captures the required Nickel environment
- stops Nickel and helper processes
- launches `hadisplay`
- restarts Nickel when `hadisplay` exits

### Manual SSH launch

```bash
ssh kobo
cd /mnt/onboard/.adds/hadisplay
LD_LIBRARY_PATH=/mnt/onboard/.niluje/usbnet/lib ./hadisplay
```

Manual launch is useful for debugging, but it does not provide the normal Nickel lifecycle handling. For normal use, prefer the NickelMenu launcher.

## Device notes

- Tested device: **Kobo Clara Colour N367B** / device code `393`
- Screen: `1072x1448`
- Wi-Fi and some runtime libraries depend on the Kobo environment inherited from Nickel and Kobo Stuff
- `ForceWifi=true` in `[DeveloperSettings]` is still useful for keeping Wi-Fi alive

## Related docs

For the fuller Kobo setup and SSH/deploy notes, see [docs/kobo-setup.md](docs/kobo-setup.md).
For the planned native color rendering work, see [docs/color-output-spec.md](docs/color-output-spec.md).

## Build options

| Option | Default | Description |
|---|---|---|
| `HADISPLAY_FETCH_FBINK` | `ON` | Fetch and build FBInk automatically |
| `HADISPLAY_BUILD_APP` | `ON` | Build the main `hadisplay` application |
| `HADISPLAY_BUILD_FBINK_SMOKE` | `ON` | Build the framebuffer smoke target |
| `HADISPLAY_BUILD_FBINK_MIRROR_X11` | `ON` | Build the X11 mirror target |

The X11 mirror requires `libX11` and `libcurl` on the host. Disable it when cross-compiling if you do not need it:

```bash
-DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
```
