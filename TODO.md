# TODO

## UI / Scene

- [ ] Support dynamic button lists from HA (currently hardcoded to a fixed 4-button Clara-specific grid in `buttons_for()`)
- [ ] Improve long-text handling in the UI — the bitmap font and simple truncation work, but multi-line wrapping/overflow handling is still crude
- [ ] Revisit the full-refresh cadence on-device — current threshold/timer is heuristic and may need tuning for ghosting vs. flash frequency
- [ ] Add clearer visual distinction between the selected button and the pressed button state

## Home Assistant Integration

- [ ] Generalize beyond `light.josh_light` — make entities configurable via `.env` or a config file
- [ ] Support multiple HA service calls (not just `light/toggle`) — scenes, scripts, switches, etc.
- [ ] Expand GET/state support beyond a single light entity (temperature, switches, sensors, scenes)
- [ ] Replace the minimal string-based JSON field extraction with a proper lightweight parser once the HA model grows
- [ ] Improve HA failure handling further — requests fail faster now, but calls are still synchronous and block the UI while in flight

## Touch Input

- [ ] Touch mapping is hardcoded for Clara Colour rotation=3 — read `/sys/class/graphics/fb0/rotate` and `EVIOCGABS` at runtime to compute mapping dynamically
- [ ] Detect touch device automatically instead of hardcoding `/dev/input/event1`
- [ ] Support swipe gestures (e.g. swipe down to exit, swipe left/right to switch pages)

## Build / Deploy

- [ ] `deploy.sh` assumes `ssh kobo` is configured — document or detect this
- [ ] Deploy the `.env` file as part of `deploy.sh` (or warn if it's missing on device)
- [ ] Add a `deploy.sh --smoke` flag to deploy and run `hello_fbink_linuxfb` instead
- [ ] CI cross-compilation (GitHub Actions with the koxtoolchain)

## Code Quality

- [ ] The `render()` function allocates a full-screen pixel buffer on every call — consider keeping a persistent buffer and only redrawing dirty regions
- [ ] `ha_client.cpp` uses `std::filesystem` which pulls in a large runtime — could use plain POSIX if binary size matters
- [ ] The app still uses a hand-rolled JSON string extractor — replace it with a lightweight parser before the HA schema grows
- [ ] Log rotation — `log.txt` grows unbounded on device

## Launcher / Lifecycle

- [ ] `run-hadisplay.sh` restarts Nickel unconditionally on exit — add an option to stay in hadisplay (e.g. relaunch on crash)
- [ ] Watchdog: restart hadisplay if it crashes instead of falling back to Nickel
- [ ] Reduce FBInk noise in `log.txt` around shutdown/forced refresh interruptions
