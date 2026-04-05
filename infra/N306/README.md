# N306 Bring-Up

This directory contains the host-side bootstrap for a Kobo Nia (`N306`) connected over USB.

The goal is to get the device into a "ready for OTA deploy" state:

- Kobo Stuff installed with your SSH key
- NickelMenu installed with a `Hadisplay` launcher entry
- KOReader staged at `/mnt/onboard/.adds/koreader`
- `hadisplay` staged at `/mnt/onboard/.adds/hadisplay`
- `hadisplay-config.json` staged when a local JSON config exists
- `ForceWifi=true` enabled in `Kobo eReader.conf`

After the device reboots and applies `KoboRoot.tgz`, you should be able to:

```bash
ssh hackspace-kobo
./scripts/deploy.sh --target hackspace-kobo
```

## What The Script Does

`prepare.sh`:

- auto-detects a mounted Kobo volume under `/Volumes`
- verifies the mounted device is an `N306`
- builds `hadisplay` if no Kobo build artifact already exists
- downloads pinned copies of:
  - NickelMenu
  - Kobo Stuff
  - KOReader
- creates a merged `KoboRoot.tgz` containing NickelMenu + Kobo Stuff
- injects your SSH public key into Kobo Stuff's active and persistent `authorized_keys` paths
- stages KOReader and the current `hadisplay` payload onto the mounted volume
- stages `hadisplay-config.json` from the local repo when present
- updates NickelMenu config with the `Hadisplay` launcher entry
- ensures `[DeveloperSettings]` contains `ForceWifi=true`

## Usage

From the repo root:

```bash
./infra/N306/prepare.sh
```

Useful overrides:

```bash
./infra/N306/prepare.sh --mount /Volumes/KOBOeReader
./infra/N306/prepare.sh --ssh-pubkey ~/.ssh/id_ed25519.pub
./infra/N306/prepare.sh --binary build-kobo-docker/hadisplay
./infra/N306/prepare.sh --target hackspace-kobo
```

## Preconditions

- The Kobo is connected over USB and mounted on this Mac.
- Your SSH public key exists locally.
  Default path: `~/.ssh/id_ed25519.pub`
- A local JSON config is recommended at `.hadisplay-config.json`.
  The script falls back to `hadisplay-config.json` if the hidden file is absent.
- The repo has the target metadata you want to stage.
  Default target: `targets/hackspace-kobo.env`

## After Running

1. Safely eject the Kobo.
2. Let it reboot and apply `KoboRoot.tgz`.
3. Wait for it to rejoin Wi-Fi.
4. Test SSH:

```bash
ssh hackspace-kobo
```

5. Then deploy the latest app build over the air:

```bash
./scripts/deploy.sh --target hackspace-kobo
```

## Notes

- The script preserves unrelated entries already present in `.adds/nm/config`.
- The script writes a backup of `Kobo eReader.conf` before editing it.
- If the Kobo comes back on a different DHCP lease after reboot, the `hackspace-kobo` SSH alias may need its `HostName` updated in `~/.ssh/config`.
