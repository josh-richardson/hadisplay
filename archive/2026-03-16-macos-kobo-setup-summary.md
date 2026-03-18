# macOS Kobo Setup Summary

This session focused on getting the Kobo build and deploy flow working from a macOS host while preserving the existing Linux-native workflow.

## What changed

- Added a Docker-based Kobo cross-build path:
  - [`scripts/build-kobo-in-docker.sh`](/Users/joshua/code/hadisplay/scripts/build-kobo-in-docker.sh)
  - [`docker/kobo-build.Dockerfile`](/Users/joshua/code/hadisplay/docker/kobo-build.Dockerfile)
- Updated [`scripts/deploy.sh`](/Users/joshua/code/hadisplay/scripts/deploy.sh) so it:
  - uses the native KOReader toolchain if `arm-kobo-linux-gnueabihf-gcc/g++` are available
  - otherwise falls back to the Docker build path
  - deploys with Kobo-safe `rsync` flags and explicit `/usr/bin/rsync`
- Added a local [`.env`](/Users/joshua/code/hadisplay/.env) with the current Home Assistant URL and token, and updated deploy to copy it to the Kobo when present.

## Build-system fixes made along the way

These were needed to make the Kobo build reliable in the containerized environment:

- Fixed FBInk external build metadata in [`CMakeLists.txt`](/Users/joshua/code/hadisplay/CMakeLists.txt) so Kobo-only outputs such as `libi2c.a` are declared as byproducts.
- Changed the FBInk external build invocation to use a real `make` binary instead of reusing CMake's top-level generator backend.
- Aligned the Docker build helper to use `Unix Makefiles` so it behaves more like the previously working Arch flow.

## Runtime and deploy results

- Verified `ssh kobo` connectivity from the Mac.
- Built `hadisplay` successfully in Docker using KOReader's published `2025.05` Kobo toolchain archive.
- Deployed the binary and launcher script to `/mnt/onboard/.adds/hadisplay/`.
- Verified the app starts on-device and writes logs normally.
- Fixed Home Assistant connectivity on the Kobo by deploying the `.env` file with:
  - `HA_URL=http://homeassistant/`
  - the provided long-lived token

## Debugging work after the macOS setup

After the build/deploy path was working, there were two UI/runtime issues investigated and fixed:

- Input dispatch bug:
  - touch release handling was storing a raw button index
  - async refreshes could rebuild the button list before dispatch
  - result: a tap could be applied to the wrong entity/control
  - fixed in [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp) by storing the pending button by identity instead of stale index
- Post-toggle stale-state bug:
  - Home Assistant can briefly return the old state immediately after `light.toggle`
  - result: the UI could keep showing `OFF` or `ON` incorrectly until the next refresh
  - fixed in [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp) by retrying `fetch_entity_state` for a short period after toggle-style actions until the state changes or a short timeout expires

## Current state

At the end of the session:

- macOS can build and deploy to the Kobo without a locally installed Kobo toolchain
- the Kobo app is configured against the current local Home Assistant instance
- the main known toggle-state sync issue was patched and redeployed
