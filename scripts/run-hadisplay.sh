#!/bin/sh
# Launch hadisplay on the Kobo.
# Stops Nickel, runs the app, then restarts Nickel on exit.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1090
. "${SCRIPT_DIR}/hadisplay-target.sh"

if [ -r "${SCRIPT_DIR}/target.env" ]; then
    # shellcheck disable=SC1091
    . "${SCRIPT_DIR}/target.env"
fi

hadisplay_apply_target_defaults

HADISPLAY_DIR="${HADISPLAY_APP_DIR}"
LOGFILE="${HADISPLAY_DIR}/${HADISPLAY_APP_LOG}"
ORIG_LD_LIBRARY_PATH="${LD_LIBRARY_PATH}"
HADISPLAY_LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}"
NICKEL_ENV_PID=""

# Siphon env from Nickel before we kill it (needed for WiFi, dbus, etc.)
if pkill -0 nickel 2>/dev/null; then
    NICKEL_ENV_PID="$(pidof -s nickel)"
    eval "$(grep -s -E -e '^(DBUS_SESSION_BUS_ADDRESS|NICKEL_HOME|WIFI_MODULE|LANG|INTERFACE|PLATFORM|QT_GSTREAMER_PLAYBIN_AUDIOSINK|QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER|PATH)=' \
        "/proc/${NICKEL_ENV_PID}/environ" | sed 's/^/export /')"

    sync

    # Stop Nickel and its helpers.
    killall -q -TERM nickel hindenburg sickel fickel strickel fontickel dhcpcd-dbus dhcpcd

    # Wait up to 4 seconds for Nickel to die.
    timeout=0
    while pkill -0 nickel 2>/dev/null; do
        if [ ${timeout} -ge 16 ]; then
            break
        fi
        usleep 250000
        timeout=$((timeout + 1))
    done

    rm -f /tmp/nickel-hardware-status
fi

# Disable WiFi driver-level power save to prevent disconnects.
WIFI_IFACE="${INTERFACE:-${HADISPLAY_WIFI_INTERFACE}}"
WIFI_LOG="${HADISPLAY_DIR}/hadisplay.log"
NICKEL_BIN="${HADISPLAY_NICKEL_BIN}"

log_msg() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [INFO] [${HADISPLAY_SHELL_LOG_PREFIX}] $1" >>"$WIFI_LOG"
}
log_warn() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [WARN] [${HADISPLAY_SHELL_LOG_PREFIX}] $1" >>"$WIFI_LOG"
}

if iw "$WIFI_IFACE" set power_save off 2>/dev/null; then
    log_msg "WiFi power save disabled via iw"
elif iwconfig "$WIFI_IFACE" power off 2>/dev/null; then
    log_msg "WiFi power save disabled via iwconfig"
else
    log_warn "Failed to disable WiFi power save"
fi

# Background WiFi keepalive: pings the gateway every 60s to prevent
# inactivity-based WiFi teardown, and attempts to reconnect if WiFi drops.
wifi_keepalive() {
    while true; do
        sleep 60
        # Check if the interface is still up (driver may report UP, UNKNOWN, or DORMANT).
        IFACE_STATE=$(cat "/sys/class/net/$WIFI_IFACE/operstate" 2>/dev/null)
        case "$IFACE_STATE" in
            up|unknown|dormant) WIFI_OK=true ;;
            *) WIFI_OK=false ;;
        esac
        if [ "$WIFI_OK" = false ]; then
            log_warn "WiFi interface $WIFI_IFACE is down, attempting recovery"
            # Try to reload WiFi modules and reconnect.
            if [ "${HADISPLAY_KEEPALIVE_RELOAD_MODULE}" = true ] && [ -n "${WIFI_MODULE}" ]; then
                insmod "/drivers/${PLATFORM}/wifi/sdio_wifi_pwr.ko" 2>/dev/null
                insmod "${HADISPLAY_KEEPALIVE_MODULE_ROOT}/${WIFI_MODULE}" 2>/dev/null
                sleep 2
            fi
            ifconfig "$WIFI_IFACE" up 2>/dev/null
            wpa_cli -i "$WIFI_IFACE" reconnect 2>/dev/null
            sleep 5
            udhcpc -i "$WIFI_IFACE" -t 5 -q 2>/dev/null
            IFACE_STATE=$(cat "/sys/class/net/$WIFI_IFACE/operstate" 2>/dev/null)
            case "$IFACE_STATE" in up|unknown|dormant) RECOVER_OK=true ;; *) RECOVER_OK=false ;; esac
            if [ "$RECOVER_OK" = true ]; then
                log_msg "WiFi recovery successful"
            else
                log_warn "WiFi recovery failed, will retry in 60s"
            fi
        fi
        # Generate traffic to prevent inactivity timeout.
        GATEWAY=$(ip route | awk '/default/ {print $3}' | head -1)
        if [ -n "$GATEWAY" ]; then
            if ! ping -c 1 -W 5 "$GATEWAY" >/dev/null 2>&1; then
                log_warn "Gateway ping failed ($GATEWAY)"
            fi
        fi
    done
}
wifi_keepalive &
KEEPALIVE_PID=$!
export HADISPLAY_KEEPALIVE_PID="${KEEPALIVE_PID}"

# Run hadisplay.
if [ -n "${ORIG_LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}:${ORIG_LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}"
fi
cd "${HADISPLAY_DIR}" || exit 1
if "./${HADISPLAY_APP_BIN}" >>"${LOGFILE}" 2>&1; then
    RETVAL=0
else
    RETVAL=$?
    if [ "${RETVAL}" -eq 126 ]; then
        log_warn "Launch returned 126, retrying once after a short delay"
        sleep 2
        if "./${HADISPLAY_APP_BIN}" >>"${LOGFILE}" 2>&1; then
            RETVAL=0
        else
            RETVAL=$?
        fi
    fi
fi

# Stop the keepalive when hadisplay exits.
kill "$KEEPALIVE_PID" 2>/dev/null
wait "$KEEPALIVE_PID" 2>/dev/null

# Restart Nickel.
export LD_LIBRARY_PATH="/usr/local/Kobo"
rm -f /tmp/nickel-hardware-status
mkfifo /tmp/nickel-hardware-status
/usr/local/Kobo/hindenburg >>"${LOGFILE}" 2>&1 &
LIBC_FATAL_STDERR_=1 "${NICKEL_BIN}" -platform kobo -skipFontLoad >>"${LOGFILE}" 2>&1 &
[ "${PLATFORM}" != "freescale" ] && udevadm trigger >>"${LOGFILE}" 2>&1 &
sync

exit ${RETVAL}
