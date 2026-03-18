# Kobo Sleep Mode Implementation

Date: 2026-03-16

## Summary

This document records the implementation of Kobo-style sleep/wake behavior in `hadisplay`.

The goal was to make `hadisplay` behave like KOReader or Nickel when the hardware power button is pressed:

- show a stable sleep screen
- stop normal app activity
- enter a much lower-power suspend state when possible
- wake back into the app instead of exiting to Nickel

The implementation lives primarily in [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp) and is coordinated with the launcher wrapper in [`scripts/run-hadisplay.sh`](/Users/joshua/code/hadisplay/scripts/run-hadisplay.sh).

## User-visible behavior

When `hadisplay` is launched via `run-hadisplay.sh`:

- pressing the Kobo power button while the app is running shows a sleep screen
- the app pauses normal redraws and periodic Home Assistant polling
- the Wi-Fi keepalive loop is paused
- Wi-Fi is brought down for sleep
- the app attempts a Kobo suspend-to-RAM cycle
- pressing the power button again wakes back into `hadisplay`

If full kernel suspend is not safe or available, `hadisplay` falls back to a software sleep mode:

- the sleep screen remains on display
- touch input is ignored
- periodic app work stays paused
- the power button wakes the app back to the normal dashboard/setup/detail UI

Current fallback cases include:

- charging over USB
- missing `/sys/power/state`
- missing `/sys/power/state-extended`
- missing `mem` support in `/sys/power/state`
- failed sysfs writes during suspend

## Why the launcher matters

`hadisplay` is not launched directly by Nickel. The normal flow is:

1. `run-hadisplay.sh` captures key Nickel environment variables.
2. It stops Nickel and related helper processes.
3. It starts a background Wi-Fi keepalive loop.
4. It runs the native `hadisplay` binary.
5. It restarts Nickel only when `hadisplay` exits.

Because of that:

- sleep cannot be implemented by exiting the app
- suspend/resume must happen while the process stays alive
- the keepalive loop must be paused during sleep or it defeats the power-saving goal

The wrapper now exports `HADISPLAY_KEEPALIVE_PID`, and the app uses that to pause and resume the keepalive loop with `SIGSTOP` and `SIGCONT`.

## KOReader behavior used as the reference

KOReader was inspected in `.deps/koreader`.

The key Kobo-specific references were:

- [`frontend/device/kobo/device.lua`](/Users/joshua/code/hadisplay/.deps/koreader/frontend/device/kobo/device.lua)
- [`frontend/device/kobo/powerd.lua`](/Users/joshua/code/hadisplay/.deps/koreader/frontend/device/kobo/powerd.lua)
- [`frontend/device/generic/device.lua`](/Users/joshua/code/hadisplay/.deps/koreader/frontend/device/generic/device.lua)

Important KOReader behaviors copied or adapted:

- detect the power button as an input event instead of assuming only touch input
- show a sleep screen before trying to suspend
- disable Wi-Fi before power-state transitions
- write `1` to `/sys/power/state-extended`
- then request suspend with `mem` via `/sys/power/state`
- write `0` to `/sys/power/state-extended` after resume

Some KOReader behaviors were not copied directly:

- its full UI suspend/resume event graph
- its wakeup guards and retry scheduling logic
- its frontlight ramp implementation
- its RTC wake alarm helpers

Those are tied to KOReader's own device and UI framework. `hadisplay` uses a simpler direct implementation.

## Code changes

## 1. Input handling was generalized

Before this change, `hadisplay` only opened one hard-coded touch device:

- `/dev/input/event1`

That was not sufficient for power-button sleep.

The app now:

- scans `/dev/input/event*`
- inspects evdev capability bits with `EVIOCGBIT`
- detects devices that expose touch coordinates
- detects devices that expose `KEY_POWER`
- keeps both touch and power-button FDs in the main poll loop

This logic is in:

- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L319)
- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1706)

The old hard-coded touch path remains as a fallback if auto-discovery finds no touch device.

## 2. A sleep screen was added

The app now renders a dedicated textual sleep screen via FBInk before suspend:

- `Sleeping`
- `Press power to wake`
- optional detail line such as charging state

This is implemented in:

- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1201)

This path intentionally uses a full refresh to leave the panel in a stable state before any sleep transition.

## 3. App runtime services are paused during sleep

The main loop originally kept doing all of the following continuously:

- clock refreshes
- Home Assistant entity polling
- weather polling
- local system-status polling
- touch interaction

That would waste power even if a fake sleep screen were shown.

The new `PowerStateContext` tracks whether the app is:

- `Awake`
- `Sleeping`

While sleeping:

- touch input is ignored
- redraws are suppressed
- periodic poll scheduling is skipped
- the poll timeout becomes infinite until an input or async wake event occurs

The sleep-aware loop is in:

- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1754)
- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1930)

## 4. Wi-Fi and keepalive are coordinated with sleep

Two runtime services are paused for sleep:

### Wrapper keepalive

The launcher's background Wi-Fi keepalive is paused and resumed using the exported PID:

- pause: `SIGSTOP`
- resume: `SIGCONT`

Relevant code:

- [`scripts/run-hadisplay.sh`](/Users/joshua/code/hadisplay/scripts/run-hadisplay.sh#L92)
- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L217)
- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1247)

### Wi-Fi interface state

For sleep, the app first tries to reuse KOReader's Kobo Wi-Fi helper scripts:

- `/mnt/onboard/.adds/koreader/disable-wifi.sh`
- `/mnt/onboard/.adds/koreader/enable-wifi.sh`

If those are not present or fail, it falls back to:

- `ifconfig <iface> down`
- `ifconfig <iface> up`

Relevant code:

- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L291)
- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L305)

## 5. The Kobo suspend sequence was added

The real suspend path currently used is:

1. Render the sleep screen.
2. Pause keepalive and disable Wi-Fi.
3. Check whether kernel suspend is supported and safe.
4. Write `1` to `/sys/power/state-extended`.
5. Wait briefly for the sleep screen and subsystems to settle.
6. Call `sync()`.
7. Write `mem` to `/sys/power/state`.
8. After wake, write `0` to `/sys/power/state-extended`.
9. Wait briefly for resume stabilization.
10. Re-enable Wi-Fi, resume keepalive, refresh input grabs, and redraw the main UI.

Relevant code:

- suspend support check:
  - [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1270)
- suspend execution:
  - [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1288)
- power-button entry and wake handling:
  - [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp#L1872)

## 6. Software sleep fallback was kept intentionally

The implementation does not blindly force kernel suspend in all cases.

This is deliberate because Kobo suspend behavior is device- and charging-state-sensitive.

Current behavior:

- if charging is detected, real suspend is skipped
- if kernel sleep sysfs nodes are missing, real suspend is skipped
- if any part of the suspend sysfs sequence fails, the app stays in software sleep mode

Software sleep still achieves:

- no touch interaction
- no background polling churn
- stable sleep screen
- wake by power button

This is a safer failure mode than exiting, clearing the screen, or leaving the app half-suspended.

## Current limitations

## 1. Charging forces software sleep

KOReader contains model-specific notes about suspend problems while charging on some Kobo hardware families. `hadisplay` currently takes the conservative path and avoids real suspend while `battery_charging` is true.

That should be revisited only after device testing confirms safe suspend on the target hardware while charging.

## 2. No long-press poweroff behavior

KOReader distinguishes a short power-button action from a longer hold that triggers poweroff. `hadisplay` currently uses only short press/release toggle behavior for sleep/wake.

That was intentionally deferred because:

- the main requirement was in-app sleep/wake
- accidental poweroff handling is higher risk than suspend
- there is no existing app-level shutdown menu logic tied to button hold duration

## 3. No dedicated frontlight ramping

The current implementation relies on:

- Wi-Fi suspension
- process quiescing
- device suspend

It does not yet implement KOReader-like frontlight ramp-down and ramp-up behavior around suspend.

Brightness state remains manageable through the existing `DeviceStatus` controls, but resume smoothness may not match KOReader exactly.

## 4. Input mapping still assumes the current Clara Colour touch transform

Input device discovery is now dynamic, but the touch coordinate mapping still uses the current hard-coded raw transform and max ranges:

- `kTouchMaxX`
- `kTouchMaxY`

That is unchanged from the previous app and remains a device-specific assumption.

## Validation performed

Implementation validation completed:

- source review against KOReader Kobo suspend paths
- Kobo-target build in Docker via `./scripts/build-kobo-in-docker.sh`
- rebuild after cleanup change to remove the new warning

Build result:

- `build-kobo-docker/hadisplay` built successfully

Not yet validated on hardware in this change record:

- real suspend/resume on the Kobo Clara Colour
- power-button wake timing
- Wi-Fi reconnection behavior after wake
- suspend behavior while an HA request is in flight
- suspend behavior across repeated rapid sleep/wake cycles

## Recommended device test matrix

Run these on the Kobo:

1. Battery power, Wi-Fi connected, dashboard idle:
   - power button sleeps
   - power button wakes
   - UI redraws correctly

2. Battery power, Wi-Fi connected, immediately after pressing `REFRESH`:
   - no crash
   - no wedged touch input after wake

3. USB/charging state:
   - app enters software sleep instead of attempting kernel suspend
   - wake returns cleanly

4. Repeated sleep/wake:
   - five to ten consecutive cycles
   - verify the power button still works every time
   - verify touch still works every time

5. Wi-Fi recovery:
   - after wake, confirm weather/entity polling resumes
   - if Wi-Fi is missing, confirm recovery is attempted

## Files changed

- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp)
- [`scripts/run-hadisplay.sh`](/Users/joshua/code/hadisplay/scripts/run-hadisplay.sh)
- [`README.md`](/Users/joshua/code/hadisplay/README.md)

## Notes for future work

- Add a real long-press poweroff path on the hardware power button.
- Decide whether frontlight should be explicitly forced off before suspend.
- Consider persisting a small sleep-state marker if debugging suspend failures on-device becomes difficult.
- If suspend is proven safe while charging on the Clara Colour, relax the conservative charging fallback.
- If more Kobo models are targeted, replace the hard-coded touch transform with per-device discovery or configuration.
