# Kobo Setup

This is the working setup for deploying native `hadisplay` binaries to a Kobo over Wi-Fi.

## Device State

- Device: Kobo Clara Colour (`Spa Colour`, device code `393`) running firmware `4.45.23646`
- SSH server: Kobo Stuff `dropbear`
- Native app payloads: `/mnt/onboard/.adds/`
- Current app path: `/mnt/onboard/.adds/hadisplay/`

## Installed Components

These were installed on the Kobo user storage and applied via reboot:

- KOReader
- NickelMenu
- Kobo Stuff

Useful resulting paths:

- KOReader: `/mnt/onboard/.adds/koreader/`
- NickelMenu config: `/mnt/onboard/.adds/nm/`
- Kobo Stuff persistent tree on user storage: `/mnt/onboard/.niluje/`
- Kobo Stuff active SSH key path: `/usr/local/niluje/usbnet/etc/authorized_keys`

## SSH Auth

Kobo Stuff's active `dropbear` instance does **not** read `/.ssh/authorized_keys`.
It checks:

```text
/usr/local/niluje/usbnet/etc/authorized_keys
```

Working fix:

```sh
install -m 600 /.ssh/authorized_keys /usr/local/niluje/usbnet/etc/authorized_keys
```

Verification:

```sh
ps w | grep dropbear
netstat -ltnp | grep ':22'
```

Expected server:

```text
/usr/bin/dropbear -P /usr/local/niluje/usbnet/run/sshd.pid -K 15 -n
```

## OTA Deploy

`scp` was unreliable against this device. `rsync` works after Kobo Stuff is installed.

Deploy the framebuffer smoke binary with:

```bash
rsync -rtvP --inplace --no-perms --no-owner --no-group \
  --rsync-path=/usr/bin/rsync -e ssh \
  build-kobo-smoke-crossfbink2/hello_fbink_linuxfb \
  kobo:/mnt/onboard/.adds/hadisplay/
```

If the target binary is currently running and the Kobo reports `Text file busy`, deploy to a temporary name and move it into place:

```bash
rsync -rtvcP --inplace --no-perms --no-owner --no-group \
  --rsync-path=/usr/bin/rsync -e ssh \
  build-kobo-smoke-crossfbink2/hello_fbink_linuxfb \
  kobo:/mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb.new

ssh kobo 'mv /mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb.new /mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb'
```

Why these flags:

- `/mnt/onboard` is VFAT, so preserving owner/group/perms is noisy and unnecessary.
- `--rsync-path=/usr/bin/rsync` forces the Kobo Stuff `rsync` binary explicitly.

Verification on-device:

```sh
ls -l /mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb
file /mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb
```

## Running The Smoke Binary

Manual launch over SSH:

```sh
/mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb
```

NickelMenu launcher entry:

```text
menu_item:main:Hadisplay Smoke:cmd_spawn:quiet:exec /bin/sh /mnt/onboard/.adds/hadisplay/run.sh
```

Wrapper script path:

```text
/mnt/onboard/.adds/hadisplay/run.sh
```

Current wrapper contents:

```sh
#!/bin/sh
exec /mnt/onboard/.adds/hadisplay/hello_fbink_linuxfb >/mnt/onboard/.adds/hadisplay/log.txt 2>&1
```

Observed result on the Clara Colour:

- direct SSH launch works
- NickelMenu launch works
- the smoke text shows up properly on-screen
- current smoke target behavior is a clear + centered FBInk text render with a short hold before exit

## Build Notes

The working Kobo cross-build output is:

```text
build-kobo-smoke-crossfbink2/hello_fbink_linuxfb
```

The working FBInk revision for this device is pinned in CMake:

```text
ede0a8d0a107768609fde9a68aab5f5c583be398
```

Reason:

- FBInk `v1.25.0` does not recognize Kobo device code `393`
- newer upstream FBInk adds explicit `Clara Colour` support

That binary was verified as:

- `ELF 32-bit LSB executable, ARM, EABI5`
- dynamically linked with interpreter `/lib/ld-linux-armhf.so.3`
- linked with `libstdc++` and `libgcc` statically to avoid Kobo runtime version mismatches

The Kobo-mode FBInk build also requires its vendored `libi2c.a`, which is linked in via CMake for Kobo builds.

## Known Constraints

- Parts of stock Kobo rootfs config are ephemeral across reboot.
- Do not rely on edits to `/etc/ssh/sshd_config`.
- Kobo Stuff's SSH behavior and key path are the durable setup to target.
- The OpenSSH client will warn that this server is not using PQ key exchange. That is expected for this Kobo Stuff `dropbear` build.
- A generic `LINUX=true` FBInk build is not sufficient for this device. Kobo builds must use Kobo-mode FBInk support.
