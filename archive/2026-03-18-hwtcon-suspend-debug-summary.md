# Kobo Suspend Debug Summary

Date: 2026-03-18

## Problem

`hadisplay` can enter software sleep and wake cleanly, but real kernel suspend-to-RAM is still failing on the Kobo.

The failure is consistent:

- app log reports:
  - `Kernel suspend rejected: kernel suspend failed at 14000000.hwtcon during suspend (errno -1)`
- kernel log reports:
  - `hwtcon_fb_suspend xon off timer pending ! @hwtcon_fb_suspend,2248`
  - `PM: Device 14000000.hwtcon failed to suspend: error -1`
  - `PM: Some devices failed to suspend, or early wake event detected`

This means the device is not actually waking from a successful suspend. The suspend attempt is being aborted by the `hwtcon` display controller path.

## Current User-Visible Behavior

- Pressing the power button shows the sleep screen.
- The app pauses keepalive and disables Wi-Fi.
- Kernel suspend is attempted.
- Suspend fails almost immediately due to `hwtcon`.
- The app falls back to software sleep.
- Pressing power again wakes the app and resumes Wi-Fi/keepalive correctly.

So:

- software sleep/wake works
- real suspend does not

## Concrete Evidence

### Latest app log example

Recent cycle on 2026-03-18:

- `18:07:50` keepalive paused, Wi-Fi disable started
- `18:07:53` suspend rejected
- `18:07:53` entered software sleep
- `18:08:14` to `18:08:21` wake/resume restored Wi-Fi and resumed app activity

### Latest kernel log example

Recent cycle:

- `PM: suspend entry 2026-03-18 18:07:53.348916182 UTC`
- `PM: Suspending system (mem)`
- `hwtcon_fb_suspend xon off timer pending ! @hwtcon_fb_suspend,2248`
- `PM: Device 14000000.hwtcon failed to suspend: error -1`
- `PM: suspend exit 2026-03-18 18:07:53.461930105 UTC`

### Debugfs state

`/sys/kernel/debug/suspend_stats` records:

- `last_failed_dev: 14000000.hwtcon`
- `last_failed_step: suspend`
- `last_failed_errno: -1`

`/sys/kernel/debug/wakeup_sources` showed:

- `hwtcon_wakelock`
- `cmdq_wakelock`

These appeared relevant, but polling them did not resolve the failure.

## What Has Already Been Implemented

### 1. Better suspend failure detection

`src/app/main.cpp` was changed so the app:

- snapshots `/sys/kernel/debug/suspend_stats` before and after suspend
- detects failed/aborted kernel suspend instead of treating it as a real wake
- falls back to software sleep on failure
- blocks further kernel suspend attempts for that app session after a confirmed kernel suspend failure

This fixed the previous bad behavior where the app effectively bounced back on after a failed suspend.

### 2. FBInk EINTR handling

Startup/shutdown instability was traced to FBInk refresh calls returning interrupted-system-call errors.

The app now retries FBInk calls on `EINTR` instead of immediately exiting on the first interrupted refresh.

This addressed a separate issue where the app was exiting cleanly due to transient FBInk failures, not crashing.

### 3. Attempted display-idle wait

The app was changed to poll debugfs wakeup-source state before suspend:

- `/sys/kernel/debug/wakeup_sources`
- `hwtcon_wakelock`
- `cmdq_wakelock`

This did not fix the suspend failure.

### 4. Attempted FBInk completion fence

The app now calls:

- `fbink_wait_for_complete(fbfd, LAST_MARKER)`

before suspend to try to ensure the sleep-screen refresh actually completed.

This also did not fix the suspend failure. The failure mode remained identical.

## Relevant Upstream Findings

KOReader uses the same broad suspend pattern on Kobo:

1. write `1` to `/sys/power/state-extended`
2. wait
3. write `mem` to `/sys/power/state`
4. write `0` to `/sys/power/state-extended` on failure/resume

KOReader comments explicitly note that suspend failures are often due to EPDC/touch related `EBUSY`-style conditions.

FBInk also exposes completion waits:

- `fbink_wait_for_complete`
- `fbink_wait_for_any_complete`

The former was tried in the app. The latter has not yet been tried in the suspend path.

## Most Likely Current Theory

The sleep-screen full refresh is still leaving the MTK Kobo display pipeline in a state where `hwtcon_fb_suspend` sees a pending `xon off` timer and refuses suspend.

In other words:

- the display controller is still internally busy, even after the app-side refresh fence attempts
- the failure is not caused by Wi-Fi
- the failure is not caused by the app mistaking an early wake for a valid suspend

## Suggested Next Steps

### 1. Try `fbink_wait_for_any_complete`

This may better match the MTK driver’s notion of all pending updates being drained than `fbink_wait_for_complete(LAST_MARKER)`.

### 2. Inspect whether the sleep-screen refresh itself is the trigger

Useful experiment:

- skip the sleep-screen redraw entirely
- attempt suspend from an otherwise idle display state

If suspend starts working, the root cause is almost certainly the sleep-screen refresh path itself.

### 3. Try alternative pre-suspend display sequencing

Candidate variants:

- render sleep screen with a different waveform mode
- avoid a flashing/full refresh before suspend
- insert a longer delay after sleep-screen render
- perform explicit clear + wait + text render + wait
- try grayscale-only sleep-screen path even on color panel devices

### 4. Look for MTK/Kobo-specific display driver controls

Nothing obvious was found yet in sysfs/debugfs besides the `hwtcon` device itself, but it may still be worth checking for:

- a panel idle fence
- a cmdq flush control
- an hwtcon-specific debug or state node not yet inspected

### 5. Compare against Nickel behavior more directly

If possible:

- trace Nickel around a successful suspend on this exact hardware/firmware
- see whether Nickel avoids a display update immediately before suspend
- see whether Nickel uses a different refresh mode or extra fence

## Files Most Relevant To This Work

- `src/app/main.cpp`
- `scripts/run-hadisplay.sh`
- `archive/2026-03-16-kobo-sleep-mode.md`
- `archive/2026-03-18-sleep-mode-change-summary.md`

## Current Status

As of this note:

- the app is stable enough for testing
- software sleep/wake works
- real suspend remains blocked by `14000000.hwtcon`
- latest attempted fix (`fbink_wait_for_complete`) did not change the failure signature
