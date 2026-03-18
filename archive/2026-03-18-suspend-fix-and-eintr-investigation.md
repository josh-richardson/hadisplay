# Suspend Fix and EINTR Investigation

Date: 2026-03-18

## Summary

Two issues were worked on:

1. **Kernel suspend fix** (the intended task) — changes to `suspend_to_ram()` to address the `hwtcon_fb_suspend xon off timer pending` failure.
2. **EINTR on MXCFB_SEND_UPDATE ioctl** (encountered during testing) — every FBInk display refresh failed with EINTR after deploy, leaving the screen blank.

**The EINTR bug is now fixed.** The root cause was FBInk misidentifying the device.

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

Patched `build-kobo-docker/_deps/FBInk-src/fbink_device_id.c` to add a `case 77U:` in `set_kobo_quirks()`, treating it as an MTK Clara Colour variant:

```c
case 77U:    // Unrecognized device code 77 — MTK Clara Colour variant (hadisplay patch)
    deviceQuirks.hasColorPanel              = true;
    deviceQuirks.isMTK                      = true;
    deviceQuirks.hasEclipseWfm              = true;
    deviceQuirks.canHWInvert                = false;
    deviceQuirks.ntxRotaQuirk               = NTX_ROTA_CCW_TOUCH;
    // rotation map matching Clara Colour
    deviceQuirks.screenDPI                  = 300U;
    ...
```

The device is confirmed to be a MediaTek MT8110/MT8512 (`/proc/device-tree/model` = "MediaTek MT8110 board", `/proc/device-tree/compatible` = `mediatek,mt8110` / `mediatek,mt8512`), 1072×1448 at 32bpp, with a color panel — consistent with a Clara Colour.

### Result

After patching and deploying:
- No more `[FBInk] Unidentified Kobo device code (77)!` warning
- No more EINTR on display refreshes
- No more `[HWTCON ERR]err cmd:0x4044462e` in dmesg from the new process
- **Deploy-after-deploy works reliably** — no reboot needed
- Screen displays correctly on every deploy

### Note on FBInk patch persistence

This patch modifies the FBInk source in the build directory (`build-kobo-docker/_deps/FBInk-src/`). The FBInk ExternalProject has `UPDATE_DISCONNECTED TRUE`, so cmake won't overwrite it. However, a full clean rebuild or `rm -rf build-kobo-docker` would lose the patch. Consider either:
- Upstreaming the device ID to FBInk (file an issue/PR at https://github.com/NiLuJe/FBInk)
- Adding a cmake patch step to apply this automatically
- Fixing the version file on the device to use a recognized device code

## Suspend Fix

### Problem

`hwtcon_fb_suspend` refuses to suspend because its internal "xon off" power-down timer is still pending after a display refresh. The existing `fbink_wait_for_complete(LAST_MARKER)` only waits for the app's own update marker; it does not wait for the driver's internal power-down timer to fire.

### What was tested on-device (before EINTR fix)

After a fresh reboot (to work around the EINTR bug), the suspend path was tested:

- **Attempt 1**: `hwtcon_fb_suspend xon off timer pending` — same as before.
- **Attempt 2**: `bd71827-power.4.auto` (battery PMIC) failed with `wait resume_jiffies` — a different device failed on the retry.
- **Attempt 3**: `hwtcon_fb_suspend xon off timer pending` — same as attempt 1.
- All three retries failed; the app fell back to software sleep.

### Device capabilities discovered

| Feature | Status |
|---|---|
| `FBIOBLANK(FB_BLANK_POWERDOWN)` | **Not supported** — returns `EINVAL` |
| `FBIOBLANK(FB_BLANK_UNBLANK)` | **Not supported** — returns `EINVAL` |
| `fbink_wait_for_any_complete` (MTK ioctl) | **Not supported** on this device |
| `HWTCON_SET_PWRDOWN_DELAY` | Available — controls the "xon off" timer directly |
| `HWTCON_GET_PWRDOWN_DELAY` | Available — reads current power-down delay |

### Changes made

#### In `src/app/main.cpp`

1. **`HWTCON_SET_PWRDOWN_DELAY` / `HWTCON_GET_PWRDOWN_DELAY` ioctls defined** — these control the exact timer that causes the "xon off timer pending" suspend failure. Default is 500ms. Before suspend, we set it to 0 to force immediate power-down, then restore after resume.

2. **`fbink_wait_for_any_complete` removed from `wait_for_fbink_update_completion`** — it was unsupported on this device and generated `[HWTCON ERR]` in dmesg on every call.

3. **Sleep screen render skipped before kernel suspend** — the full refresh triggers the "xon off" power-down countdown. By not refreshing, the display pipeline stays idle.

4. **Suspend retry loop** — retries up to 3 times with 500ms delays.

5. **`suspend_to_ram` now**: saves current pwrdown delay → sets delay to 0 → waits 600ms for power-down → attempts suspend (with retries) → restores delay on resume.

6. **Other retained changes**: `sigaction` with `SA_RESTART`, `kFbinkRetryCount` increased to 16, async HTTP work deferred until after first render.

#### In `scripts/deploy.sh`

- Improved kill sequence: kills both `run-hadisplay.sh` and `hadisplay`, with force-kill of the wrapper script to prevent Nickel from launching between deploys.

#### In `build-kobo-docker/_deps/FBInk-src/fbink_device_id.c`

- Added `case 77U:` to recognize this device as MTK Clara Colour variant (the EINTR fix).

### Suspend status

**Not yet tested with the EINTR fix in place.** Now that deploys work reliably, the suspend path can be properly tested. The combination of:
- Skipping the sleep screen render before kernel suspend
- Setting `HWTCON_SET_PWRDOWN_DELAY` to 0 before suspend
- Waiting 600ms for the power-down to complete

...should address the "xon off timer pending" failure. Press the power button to test.

### Suggested next steps for suspend

1. **Test the suspend path now** — press the power button and check logs.
2. If suspend works, consider re-enabling the sleep screen render with a non-flashing waveform mode (`WFM_GL16` instead of `WFM_GCC16`) so the user sees something during sleep, and test whether the `HWTCON_SET_PWRDOWN_DELAY(0)` approach clears the timer even after a refresh.
3. If it still fails, try increasing the post-pwrdown-delay wait beyond 600ms, or skipping the sleep screen and relying solely on the pwrdown delay trick.

## Files Modified

- `src/app/main.cpp` — suspend improvements, EINTR hardening, removed unsupported ioctl calls
- `scripts/deploy.sh` — improved kill sequence
- `build-kobo-docker/_deps/FBInk-src/fbink_device_id.c` — patched to recognize device code 77 as MTK (EINTR root cause fix)
