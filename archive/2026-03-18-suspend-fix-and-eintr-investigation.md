# Suspend Fix and EINTR Investigation

Date: 2026-03-18

## Summary

Two issues were worked on:

1. **Kernel suspend fix** (the intended task) — changes to `suspend_to_ram()` to address the `hwtcon_fb_suspend xon off timer pending` failure.
2. **EINTR on MXCFB_SEND_UPDATE ioctl** (encountered during testing) — every FBInk display refresh failed with EINTR after deploy, leaving the screen blank.

**Both bugs are now fixed.**

## EINTR Fix (Root Cause Found and Fixed)

### Root cause

The Kobo device reports device code **77** (from `/mnt/onboard/.kobo/version`), which FBInk does not recognize. When FBInk encounters an unknown device code, it falls through to defaults and does **not** set `deviceQuirks.isMTK = true`. This causes FBInk to use `refresh_kobo()` (the NXP EPDC i.MX code path) instead of `refresh_kobo_mtk()` (the correct MTK hwtcon path).

The two code paths use different ioctls with different struct sizes:

| Path | Ioctl | Struct | Encoded ioctl cmd |
|---|---|---|---|
| `refresh_kobo` (NXP) | `MXCFB_SEND_UPDATE_V1_NTX` | `mxcfb_update_data_v1_ntx` (68 bytes) | `0x4044462e` |
| `refresh_kobo_mtk` (correct) | `HWTCON_SEND_UPDATE` | `hwtcon_update_data` (36 bytes) | `0x4024462e` |

Both use magic `'F'` and command number `0x2E`, but the `_IOW` macro encodes the struct size into the ioctl number. The hwtcon driver receives `0x4044462e` (68-byte NXP struct), doesn't recognize it, and logs `[HWTCON ERR]err cmd:0x4044462e`. The wrong struct layout confuses the driver and corrupts its internal state, causing subsequent operations to fail with EINTR.

This also explains why:
- **Fresh boot worked** — Nickel (the stock reader) correctly identifies the device and uses the right ioctls, leaving the driver in a clean state. hadisplay inherits that clean state and the wrong ioctls happen to partially work.
- **Deploy after kill failed** — killing hadisplay and restarting it means the driver only sees the wrong ioctls, with no Nickel run to "prime" it.
- **Signal blocking didn't help** — the EINTR was from the driver rejecting the malformed ioctl, not from userspace signals.
- **dmesg showed `[HWTCON ERR]err cmd:0x4044462e` on every refresh** — this was the wrong ioctl being rejected.

### Fix

Patched `build-kobo-docker/_deps/FBInk-src/fbink_device_id.c` to add a `case 77U:` in `set_kobo_quirks()`, treating it as an MTK Clara Colour variant. The device is confirmed MTK via `/proc/device-tree/model` = "MediaTek MT8110 board", 1072×1448 at 32bpp with a color panel.

### Note on FBInk patch persistence

This patch modifies the FBInk source in the build directory. The ExternalProject has `UPDATE_DISCONNECTED TRUE` so cmake won't overwrite it, but a full clean rebuild would lose the patch. Consider upstreaming to FBInk or adding a cmake patch step.

## Suspend Fix (Working)

### Problem

`hwtcon_fb_suspend` refuses to suspend because its internal "xon off" power-down timer is still pending after a display refresh. This is the `HWTCON_SET_PWRDOWN_DELAY` timer (default 500ms) — after any display update completes, the driver waits this long before powering down the display controller. If suspend is attempted while this timer is running, `hwtcon_fb_suspend` returns `-1`.

### Key discovery: `HWTCON_SET_PWRDOWN_DELAY` ioctl

Found in `eink/mtk-kobo.h`:
```c
#define HWTCON_SET_PWRDOWN_DELAY _IOW('F', 0x30, int32_t)  // default 500ms, -1 = never
#define HWTCON_GET_PWRDOWN_DELAY _IOR('F', 0x31, int32_t)
```

This is the exact timer that causes the "xon off timer pending" suspend failure. Setting it to 0 forces immediate power-down after any refresh completes.

### Fix

The first reliable version of the fix was to **skip the pre-suspend sleep screen entirely** on the kernel suspend path. That kept hwtcon idle and made kernel suspend work again, but it meant the display did not visibly change when the user pressed the power button.

The current working implementation restores a visible sleep indication with a **dedicated kernel sleep hint** that is gentler than the old full sleep screen:

1. Pause keepalive and disable Wi-Fi.
2. Render a built-in `SLEEPING / Press power to wake` hint on screen.
3. `wait_for_fbink_update_completion` — wait for the hint update marker.
4. Wait a fixed 5 seconds so hwtcon has time to fully settle.
5. `write /sys/power/state-extended = 1`
6. Wait `kSuspendScreenSettleDelay` (2s).
7. `wait_for_display_suspend_path_idle` — poll `hwtcon_wakelock` and `cmdq_wakelock`.
8. `HWTCON_SET_PWRDOWN_DELAY(0)` — force immediate display-controller power-down before suspend.
9. Wait 900ms for that power-down change to take effect.
10. `sync()`
11. `write /sys/power/state = mem` (with retry loop, up to 3 attempts)
12. On resume: `HWTCON_SET_PWRDOWN_DELAY(saved_value)` to restore the original delay.

If a suspend failure is attributed to hwtcon while using the visible kernel sleep hint, the app disables that hint for the rest of the session rather than repeatedly risking the same regression.

### Test result

Current on-device result:

- The screen now visibly changes to a `SLEEPING` message when the power button is pressed.
- Kernel suspend still succeeds with that visible hint in place.
- A successful test cycle logged:
  - `Rendered kernel sleep hint; waiting 5000ms before suspend`
  - `Set hwtcon power-down delay to 0ms (was 500ms)`
  - `Kernel suspend cycle completed after 132ms (attempt 1)`
- Kernel `dmesg` for that same cycle showed a real suspend/resume:
  - suspend entry at `2026-03-18 20:27:44.374 UTC`
  - suspend exit at `2026-03-18 20:27:55.539 UTC`

So the current state is:

- visible sleep screen on kernel suspend: **working**
- kernel suspend with that visible screen: **working**
- attempt 1 reliability in the latest test: **working**

One metric is still suspect: the app-reported suspend duration (`132ms` above) does not match the wall-clock gap in kernel timestamps (~11 seconds). The suspend itself is successful; only that duration measurement appears inaccurate.

### Wake from suspend fix

After kernel suspend, the power button press that wakes the device was being misinterpreted as a new "go to sleep" command, causing an infinite sleep loop. Two fixes:

1. **`drain_input_events()`** — new function called after resume that discards all queued input events (the power button press/release that woke the device).
2. **Ignore window set after `resume_runtime_services`** — the previous code set `ignore_power_until` before Wi-Fi re-enable, which takes ~5 seconds. By the time the main loop read input events, the 2-second ignore window had expired.
3. **Removed duration-based "unexpected wake" check** — the old code treated any wake shorter than 5 seconds as spurious, but real kernel suspend/resume cycles are ~2 seconds. Any wake from a successful kernel suspend is now treated as legitimate.

## Other Changes

### In `src/app/main.cpp`

- `sigaction` with `SA_RESTART` instead of `signal()` for SIGINT/SIGTERM
- `retry_fbink_call` template for retrying FBInk calls on transient errors
- `kFbinkRetryCount = 4` with 50ms delay between retries
- `SuspendStatsSnapshot` — reads `/sys/kernel/debug/suspend_stats` before/after suspend to detect failures
- `wait_for_display_suspend_path_idle` — polls `hwtcon_wakelock` and `cmdq_wakelock` wakeup sources
- `kernel_suspend_blocked` flag — disables kernel suspend for the session after confirmed failure
- `kernel_sleep_hint_blocked` flag — disables the visible kernel sleep hint for the rest of the session after an hwtcon-related suspend failure
- dedicated kernel sleep hint render path with a fixed 5-second settle before suspend
- Async HTTP work deferred until after first render

### In `scripts/deploy.sh`

- Improved kill sequence: kills both `run-hadisplay.sh` and `hadisplay`, with force-kill of wrapper to prevent Nickel from launching between deploys

### In `build-kobo-docker/_deps/FBInk-src/fbink_device_id.c`

- Added `case 77U:` to recognize this device as MTK Clara Colour variant

## Device Info

- Model: MediaTek MT8110 board (mt8110/mt8512)
- Screen: 1072×1448 @ 32bpp, color panel
- Kobo device code: 77 (unrecognized by FBInk)
- FBInk device quirks needed: `isMTK=true`, `hasColorPanel=true`, `hasEclipseWfm=true`
- Unsupported ioctls: `FBIOBLANK`, `MXCFB_WAIT_FOR_ANY_UPDATE_COMPLETE_MTK`
- Supported ioctls: `HWTCON_SEND_UPDATE`, `HWTCON_SET_PWRDOWN_DELAY`, `HWTCON_GET_PWRDOWN_DELAY`

## Files Modified

- `src/app/main.cpp` — suspend, wake, EINTR hardening, pwrdown delay control, visible kernel sleep hint
- `scripts/deploy.sh` — improved kill sequence
- `build-kobo-docker/_deps/FBInk-src/fbink_device_id.c` — device ID patch
