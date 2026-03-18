# Kobo Sleep/Wake Change Summary

Date: 2026-03-18

This note summarizes the sleep/wake changes that were added to `hadisplay` and the follow-up fixes made during on-device testing.

## Main behavior added

- `hadisplay` now responds to the Kobo hardware power button while it is running.
- Pressing the power button shows a sleep screen, pauses normal UI work, and attempts a Kobo suspend-to-RAM cycle.
- Pressing the power button again wakes back into `hadisplay` instead of exiting to Nickel.
- If full suspend is unavailable or unsafe, the app falls back to a software sleep state and still wakes from the power button.

## Application changes

- Input handling was refactored to discover usable Linux input devices instead of assuming a fixed touch event node.
- Power-button events are handled alongside touch input.
- The main loop now has an explicit sleep state that suppresses redraws, Home Assistant polling, and background refresh work while asleep.
- A dedicated sleep screen is rendered before suspend so the display is stable before the device enters low power mode.

## Kobo integration changes

- The launcher now exports the background Wi-Fi keepalive PID so the app can pause it during sleep and resume it after wake.
- The app reuses KOReader's Kobo Wi-Fi helper scripts to disable and re-enable Wi-Fi during suspend/resume.
- The suspend path writes `/sys/power/state-extended` before `/sys/power/state`, matching the Kobo-specific pattern used by KOReader.

## Follow-up fixes from device testing

- Wi-Fi restore was changed to be synchronous on wake: enable Wi-Fi, wait for `wpa_state=COMPLETED`, then reacquire DHCP.
- Resume handling now ignores very short suspend cycles and treats them as unexpected wakes instead of fully resuming the app.
- A short post-resume power-button ignore window was added so stale button events do not immediately toggle sleep again.

## Related files

- [`src/app/main.cpp`](/Users/joshua/code/hadisplay/src/app/main.cpp)
- [`scripts/run-hadisplay.sh`](/Users/joshua/code/hadisplay/scripts/run-hadisplay.sh)
- [`archive/2026-03-16-kobo-sleep-mode.md`](/Users/joshua/code/hadisplay/archive/2026-03-16-kobo-sleep-mode.md)
