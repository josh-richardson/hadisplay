#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

TARGET_NAME="hackspace-kobo"
MOUNTPOINT=""
SSH_PUBKEY="${HOME}/.ssh/id_ed25519.pub"
HADISPLAY_BINARY=""

NICKELMENU_URL="https://raw.githubusercontent.com/usetrmnl/trmnl-kobo/main/doc/distrib/nickelmenu/KoboRoot.tgz"
KOBO_STUFF_URL="https://raw.githubusercontent.com/usetrmnl/trmnl-kobo/main/doc/distrib/kobostuff/kobo-stuff-1.6.N-r18901.tar.xz"
KOREADER_URL="https://github.com/koreader/koreader/releases/download/v2026.03/koreader-kobo-v2026.03.zip"
HADISPLAY_MENU_ENTRY="menu_item:main:Hadisplay:cmd_spawn:quiet:exec /bin/sh /mnt/onboard/.adds/hadisplay/run-hadisplay.sh"

usage() {
    cat <<'EOF'
Usage: ./infra/N306/prepare.sh [options]

Options:
  --mount PATH         Mounted Kobo volume. Auto-detected if omitted.
  --ssh-pubkey PATH    SSH public key to embed into Kobo Stuff.
  --binary PATH        Hadisplay binary to stage. Auto-built/detected if omitted.
  --target NAME        Target metadata file from targets/NAME.env. Default: hackspace-kobo
  -h, --help           Show this help.
EOF
}

log() {
    printf '[N306] %s\n' "$*"
}

die() {
    printf '[N306] ERROR: %s\n' "$*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"
}

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        echo 4
    fi
}

detect_mountpoint() {
    local candidates=()
    local volume
    for volume in /Volumes/*; do
        [ -d "${volume}" ] || continue
        [ -f "${volume}/.kobo/version" ] || continue
        if grep -q 'N306' "${volume}/.kobo/version"; then
            candidates+=("${volume}")
        fi
    done

    if [ "${#candidates[@]}" -eq 0 ]; then
        die "Could not find a mounted Kobo N306 under /Volumes"
    fi
    if [ "${#candidates[@]}" -gt 1 ]; then
        printf '[N306] Found multiple N306 mounts:\n' >&2
        printf '  %s\n' "${candidates[@]}" >&2
        die "Pass --mount explicitly"
    fi

    printf '%s\n' "${candidates[0]}"
}

resolve_config_file() {
    if [ -n "${HADISPLAY_CONFIG_FILE:-}" ] && [ -f "${HADISPLAY_CONFIG_FILE}" ]; then
        printf '%s\n' "${HADISPLAY_CONFIG_FILE}"
        return 0
    fi

    for candidate in \
        "${PROJECT_ROOT}/.hadisplay-config.json" \
        "${PROJECT_ROOT}/hadisplay-config.json"
    do
        if [ -f "${candidate}" ]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

ensure_hadisplay_binary() {
    local jobs build_dir

    if [ -n "${HADISPLAY_BINARY}" ]; then
        [ -f "${HADISPLAY_BINARY}" ] || die "Hadisplay binary not found: ${HADISPLAY_BINARY}"
        printf '%s\n' "${HADISPLAY_BINARY}"
        return 0
    fi

    if [ -f "${PROJECT_ROOT}/build-kobo-docker/hadisplay" ]; then
        printf '%s\n' "${PROJECT_ROOT}/build-kobo-docker/hadisplay"
        return 0
    fi

    if [ -f "${PROJECT_ROOT}/build-kobo/hadisplay" ]; then
        printf '%s\n' "${PROJECT_ROOT}/build-kobo/hadisplay"
        return 0
    fi

    jobs="$(detect_jobs)"
    if command -v arm-kobo-linux-gnueabihf-gcc >/dev/null 2>&1 && command -v arm-kobo-linux-gnueabihf-g++ >/dev/null 2>&1; then
        build_dir="${PROJECT_ROOT}/build-kobo"
        if [ ! -d "${build_dir}" ]; then
            log "Configuring native Kobo build"
            cmake -S "${PROJECT_ROOT}" -B "${build_dir}" \
                -DCMAKE_TOOLCHAIN_FILE="${PROJECT_ROOT}/cmake/toolchains/kobo.cmake" \
                -DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
        fi
        log "Building hadisplay with native Kobo toolchain"
        cmake --build "${build_dir}" --target hadisplay --parallel "${jobs}"
        printf '%s\n' "${build_dir}/hadisplay"
        return 0
    fi

    log "Building hadisplay in Docker"
    BUILD_DIR="${PROJECT_ROOT}/build-kobo-docker" HADISPLAY_JOBS="${jobs}" \
        "${PROJECT_ROOT}/scripts/build-kobo-in-docker.sh"
    printf '%s\n' "${PROJECT_ROOT}/build-kobo-docker/hadisplay"
}

download_file() {
    local url="$1"
    local output="$2"
    curl -fsSL "${url}" -o "${output}"
}

backup_if_missing() {
    local source="$1"
    local backup="$2"
    if [ ! -e "${backup}" ]; then
        cp "${source}" "${backup}"
    fi
}

ensure_force_wifi() {
    local conf_path="$1"
    local tmp_path
    tmp_path="$(mktemp "${TMPDIR:-/tmp}/n306-conf.XXXXXX")"

    awk '
        BEGIN {
            in_dev = 0
            saw_dev = 0
            wrote_force = 0
        }
        /^\[.*\]$/ {
            if (in_dev && !wrote_force) {
                print "ForceWifi=true"
                wrote_force = 1
            }
            in_dev = ($0 == "[DeveloperSettings]")
            if (in_dev) {
                saw_dev = 1
                wrote_force = 0
            }
            print
            next
        }
        {
            if (in_dev && $0 ~ /^ForceWifi=/) {
                if (!wrote_force) {
                    print "ForceWifi=true"
                    wrote_force = 1
                }
                next
            }
            print
        }
        END {
            if (in_dev && !wrote_force) {
                print "ForceWifi=true"
            }
            if (!saw_dev) {
                print ""
                print "[DeveloperSettings]"
                print "ForceWifi=true"
            }
        }
    ' "${conf_path}" >"${tmp_path}"

    mv "${tmp_path}" "${conf_path}"
}

write_nm_config() {
    local config_path="$1"
    local tmp_path
    mkdir -p "$(dirname "${config_path}")"
    tmp_path="$(mktemp "${TMPDIR:-/tmp}/n306-nm.XXXXXX")"

    if [ -f "${config_path}" ]; then
        awk '
            $0 ~ /^menu_item:main:Hadisplay:/ { next }
            { print }
        ' "${config_path}" >"${tmp_path}"
    fi

    printf '%s\n' "${HADISPLAY_MENU_ENTRY}" >>"${tmp_path}"
    mv "${tmp_path}" "${config_path}"
}

extract_koreader() {
    local zip_path="$1"
    local output_dir="$2"
    mkdir -p "${output_dir}"
    ditto -x -k "${zip_path}" "${output_dir}"
}

merge_koboroots() {
    local stuff_root="$1"
    local nickelmenu_root="$2"
    local ssh_pubkey="$3"
    local output_tgz="$4"
    local merge_dir="$5"

    mkdir -p "${merge_dir}"
    tar -xzf "${stuff_root}" -C "${merge_dir}"
    tar -xzf "${nickelmenu_root}" -C "${merge_dir}"

    mkdir -p \
        "${merge_dir}/usr/local/niluje/usbnet/etc" \
        "${merge_dir}/mnt/onboard/.niluje/usbnet/etc"

    install -m 600 "${ssh_pubkey}" "${merge_dir}/usr/local/niluje/usbnet/etc/authorized_keys"
    install -m 600 "${ssh_pubkey}" "${merge_dir}/mnt/onboard/.niluje/usbnet/etc/authorized_keys"

    tar -C "${merge_dir}" -czf "${output_tgz}" .
}

stage_hadisplay_payload() {
    local mountpoint="$1"
    local target_name="$2"
    local binary_path="$3"
    local target_file="${PROJECT_ROOT}/targets/${target_name}.env"
    local remote_dir="${mountpoint}/.adds/hadisplay"

    [ -f "${target_file}" ] || die "Target file not found: ${target_file}"

    mkdir -p "${remote_dir}"
    cp "${binary_path}" "${remote_dir}/hadisplay"
    cp "${PROJECT_ROOT}/scripts/run-hadisplay.sh" "${remote_dir}/run-hadisplay.sh"
    cp "${PROJECT_ROOT}/scripts/hadisplay-target.sh" "${remote_dir}/hadisplay-target.sh"
    cp "${target_file}" "${remote_dir}/${target_name}.env"
    cp "${target_file}" "${remote_dir}/target.env"

    if config_file="$(resolve_config_file)"; then
        cp "${config_file}" "${remote_dir}/hadisplay-config.json"
    fi
}

stage_koreader() {
    local mountpoint="$1"
    local source_root="$2"
    local target_dir="${mountpoint}/.adds/koreader"

    mkdir -p "${mountpoint}/.adds"
    rm -rf "${target_dir}"
    cp -R "${source_root}" "${target_dir}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --mount)
            [ $# -ge 2 ] || die "Missing value for --mount"
            MOUNTPOINT="$2"
            shift 2
            ;;
        --ssh-pubkey)
            [ $# -ge 2 ] || die "Missing value for --ssh-pubkey"
            SSH_PUBKEY="$2"
            shift 2
            ;;
        --binary)
            [ $# -ge 2 ] || die "Missing value for --binary"
            HADISPLAY_BINARY="$2"
            shift 2
            ;;
        --target)
            [ $# -ge 2 ] || die "Missing value for --target"
            TARGET_NAME="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown argument: $1"
            ;;
    esac
done

require_cmd awk
require_cmd curl
require_cmd ditto
require_cmd install
require_cmd mktemp
require_cmd tar

[ -f "${SSH_PUBKEY}" ] || die "SSH public key not found: ${SSH_PUBKEY}"

if [ -z "${MOUNTPOINT}" ]; then
    MOUNTPOINT="$(detect_mountpoint)"
fi

[ -d "${MOUNTPOINT}" ] || die "Mountpoint does not exist: ${MOUNTPOINT}"
[ -f "${MOUNTPOINT}/.kobo/version" ] || die "Missing ${MOUNTPOINT}/.kobo/version"
grep -q 'N306' "${MOUNTPOINT}/.kobo/version" || die "Mounted device is not an N306: ${MOUNTPOINT}"

HADISPLAY_BINARY="$(ensure_hadisplay_binary)"
[ -f "${HADISPLAY_BINARY}" ] || die "Hadisplay binary not found after build/detect: ${HADISPLAY_BINARY}"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hadisplay-n306.XXXXXX")"
cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

log "Using mountpoint: ${MOUNTPOINT}"
log "Using target: ${TARGET_NAME}"
log "Using SSH public key: ${SSH_PUBKEY}"
log "Using hadisplay binary: ${HADISPLAY_BINARY}"
log "Working directory: ${WORK_DIR}"

log "Downloading NickelMenu"
download_file "${NICKELMENU_URL}" "${WORK_DIR}/nickelmenu-KoboRoot.tgz"

log "Downloading Kobo Stuff"
download_file "${KOBO_STUFF_URL}" "${WORK_DIR}/kobo-stuff.tar.xz"

log "Downloading KOReader"
download_file "${KOREADER_URL}" "${WORK_DIR}/koreader.zip"

log "Extracting Kobo Stuff"
mkdir -p "${WORK_DIR}/stuff"
tar -xf "${WORK_DIR}/kobo-stuff.tar.xz" -C "${WORK_DIR}/stuff"
KOBO_STUFF_KOBOROOT="$(find "${WORK_DIR}/stuff" -name 'KoboRoot.tgz' -print -quit)"
[ -n "${KOBO_STUFF_KOBOROOT}" ] || die "Could not find KoboRoot.tgz inside Kobo Stuff archive"

log "Merging Kobo Stuff and NickelMenu installers"
merge_koboroots \
    "${KOBO_STUFF_KOBOROOT}" \
    "${WORK_DIR}/nickelmenu-KoboRoot.tgz" \
    "${SSH_PUBKEY}" \
    "${WORK_DIR}/KoboRoot.tgz" \
    "${WORK_DIR}/merge"

log "Extracting KOReader"
extract_koreader "${WORK_DIR}/koreader.zip" "${WORK_DIR}/koreader-unpack"
if [ -d "${WORK_DIR}/koreader-unpack/koreader" ]; then
    KOREADER_SOURCE="${WORK_DIR}/koreader-unpack/koreader"
else
    KOREADER_SOURCE="$(find "${WORK_DIR}/koreader-unpack" -mindepth 1 -maxdepth 1 -type d -print -quit)"
fi
[ -n "${KOREADER_SOURCE:-}" ] || die "Could not find extracted KOReader directory"

log "Staging merged KoboRoot.tgz"
mkdir -p "${MOUNTPOINT}/.kobo"
cp "${WORK_DIR}/KoboRoot.tgz" "${MOUNTPOINT}/.kobo/KoboRoot.tgz"

log "Staging KOReader"
stage_koreader "${MOUNTPOINT}" "${KOREADER_SOURCE}"

log "Staging hadisplay payload"
stage_hadisplay_payload "${MOUNTPOINT}" "${TARGET_NAME}" "${HADISPLAY_BINARY}"

log "Writing NickelMenu entry"
write_nm_config "${MOUNTPOINT}/.adds/nm/config"

KOBO_CONF="${MOUNTPOINT}/.kobo/Kobo/Kobo eReader.conf"
[ -f "${KOBO_CONF}" ] || die "Missing Kobo config: ${KOBO_CONF}"
backup_if_missing "${KOBO_CONF}" "${KOBO_CONF}.pre-n306-ready"

log "Ensuring ForceWifi=true"
ensure_force_wifi "${KOBO_CONF}"

log "N306 staging complete"
cat <<EOF

Next steps:
  1. Safely eject ${MOUNTPOINT}
  2. Let the Kobo reboot and apply .kobo/KoboRoot.tgz
  3. Wait for it to rejoin Wi-Fi
  4. Test SSH: ssh hackspace-kobo
  5. Deploy OTA: ./scripts/deploy.sh --target ${TARGET_NAME}

If SSH does not come back, the Kobo likely picked up a different DHCP lease than ~/.ssh/config expects.
EOF
