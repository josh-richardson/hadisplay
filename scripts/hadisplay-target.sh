#!/bin/sh
# Shared target metadata helpers for hadisplay shell entrypoints.

hadisplay_project_dir() {
    if [ -n "${HADISPLAY_PROJECT_DIR:-}" ] && [ -d "${HADISPLAY_PROJECT_DIR}" ]; then
        printf '%s\n' "${HADISPLAY_PROJECT_DIR}"
        return 0
    fi

    helper_dir="$(cd "$(dirname "$0")" && pwd)"
    printf '%s\n' "$(cd "${helper_dir}/.." && pwd)"
}

hadisplay_validate_target_name() {
    case "$1" in
        ''|*[!A-Za-z0-9_-]*)
            return 1
            ;;
    esac
    return 0
}

hadisplay_default_target_name() {
    printf '%s\n' "${HADISPLAY_DEFAULT_TARGET:-clara-colour}"
}

hadisplay_target_dir() {
    printf '%s/targets\n' "$(hadisplay_project_dir)"
}

hadisplay_target_file() {
    printf '%s/%s.env\n' "$(hadisplay_target_dir)" "$1"
}

hadisplay_load_target() {
    target_name="${1:-$(hadisplay_default_target_name)}"
    if ! hadisplay_validate_target_name "${target_name}"; then
        printf '%s\n' "Invalid target name: ${target_name}" >&2
        return 1
    fi

    target_file="$(hadisplay_target_file "${target_name}")"
    if [ ! -r "${target_file}" ]; then
        printf '%s\n' "Unknown target: ${target_name} (${target_file} not found)" >&2
        return 1
    fi

    # shellcheck disable=SC1090
    . "${target_file}"

    hadisplay_apply_target_defaults "${target_name}"
}

hadisplay_apply_target_defaults() {
    target_name="${1:-${HADISPLAY_TARGET_NAME:-$(hadisplay_default_target_name)}}"
    : "${HADISPLAY_TARGET_NAME:=${target_name}}"
    : "${HADISPLAY_SSH_HOST:=kobo}"
    : "${HADISPLAY_REMOTE_DIR:=/mnt/onboard/.adds/hadisplay}"
    : "${HADISPLAY_REMOTE_LOG:=/mnt/onboard/.adds/hadisplay/hadisplay.log}"
    : "${HADISPLAY_APP_DIR:=/mnt/onboard/.adds/hadisplay}"
    : "${HADISPLAY_APP_BIN:=hadisplay}"
    : "${HADISPLAY_APP_LOG:=log.txt}"
    : "${HADISPLAY_LD_LIBRARY_PATH:=/mnt/onboard/.niluje/usbnet/lib}"
    : "${HADISPLAY_NICKEL_BIN:=/usr/local/Kobo/nickel}"
    : "${HADISPLAY_NICKEL_LD_LIBRARY_PATH:=/usr/local/Kobo:/lib:/usr/lib}"
    : "${HADISPLAY_WIFI_INTERFACE:=wlan0}"
    : "${HADISPLAY_WIFI_HELPER_DIR:=/mnt/onboard/.adds/koreader}"
    : "${HADISPLAY_DISABLE_WIFI_SCRIPT:=${HADISPLAY_WIFI_HELPER_DIR}/disable-wifi.sh}"
    : "${HADISPLAY_ENABLE_WIFI_SCRIPT:=${HADISPLAY_WIFI_HELPER_DIR}/enable-wifi.sh}"
    : "${HADISPLAY_OBTAIN_IP_SCRIPT:=${HADISPLAY_WIFI_HELPER_DIR}/obtain-ip.sh}"
    : "${HADISPLAY_KEEPALIVE_RELOAD_MODULE:=true}"
    : "${HADISPLAY_KEEPALIVE_MODULE_ROOT:=/drivers/${PLATFORM}/wifi}"
    : "${HADISPLAY_SHELL_LOG_PREFIX:=hadisplay}"
}
