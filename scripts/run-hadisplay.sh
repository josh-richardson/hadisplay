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
HADISPLAY_EXIT_SHUTDOWN=64

# Siphon env from Nickel before we kill it (needed for WiFi, dbus, etc.)
if pkill -0 nickel 2>/dev/null; then
    NICKEL_ENV_PID="$(pidof -s nickel)"
    # Capture ALL of Nickel's environment, not just a subset.
    # Missing variables (especially PRODUCT) can prevent Nickel from
    # rendering correctly on restart.  Filter for valid KEY=VALUE lines
    # to avoid breakage from multi-line values or binary data.
    eval "$(tr '\0' '\n' < "/proc/${NICKEL_ENV_PID}/environ" | grep -E '^[A-Za-z_][A-Za-z_0-9]*=' | sed "s/'/'\\\\''/g; s/=\\(.*\\)/='\\1'/" | sed 's/^/export /')"

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

if [ "${RETVAL}" -eq "${HADISPLAY_EXIT_SHUTDOWN}" ]; then
    log_msg "hadisplay requested device shutdown"
    sync
    if poweroff >>"${LOGFILE}" 2>&1; then
        exit 0
    else
        log_warn "poweroff failed, falling back to Nickel restart"
    fi
fi

# Tear down WiFi before restarting Nickel.
# Nickel does not handle a pre-existing WiFi connection gracefully and will
# fail to repaint the framebuffer if WiFi is still up (KOReader #1520).
TEARDOWN_IFACE="${INTERFACE:-${WIFI_IFACE}}"
if [ -n "${WIFI_MODULE}" ] && grep -q "^${WIFI_MODULE} " "/proc/modules"; then
    log_msg "Tearing down WiFi before Nickel restart (module: ${WIFI_MODULE}, iface: ${TEARDOWN_IFACE})"

    if [ -x "/sbin/dhcpcd" ]; then
        dhcpcd -d -k "${TEARDOWN_IFACE}" 2>/dev/null
        killall -q -TERM udhcpc default.script
    else
        killall -q -TERM udhcpc default.script dhcpcd
    fi

    kill_timeout=0
    while pkill -0 udhcpc 2>/dev/null; do
        if [ ${kill_timeout} -ge 20 ]; then
            break
        fi
        usleep 250000
        kill_timeout=$((kill_timeout + 1))
    done

    wpa_cli -i "${TEARDOWN_IFACE}" terminate 2>/dev/null

    [ "${WIFI_MODULE}" = "dhd" ] && wlarm_le -i "${TEARDOWN_IFACE}" down 2>/dev/null
    ifconfig "${TEARDOWN_IFACE}" down 2>/dev/null

    WIFI_DEP_MOD=""
    POWER_TOGGLE="module"
    SKIP_UNLOAD=""
    case "${WIFI_MODULE}" in
        "moal")
            WIFI_DEP_MOD="mlan"
            POWER_TOGGLE="ntx_io"
            ;;
        "wlan_drv_gen4m")
            POWER_TOGGLE="wmt"
            SKIP_UNLOAD="true"
            ;;
    esac

    if [ -z "${SKIP_UNLOAD}" ]; then
        usleep 250000
        rmmod "${WIFI_MODULE}" 2>/dev/null
        if [ -n "${WIFI_DEP_MOD}" ] && grep -q "^${WIFI_DEP_MOD} " "/proc/modules"; then
            usleep 250000
            rmmod "${WIFI_DEP_MOD}" 2>/dev/null
        fi
    fi

    case "${POWER_TOGGLE}" in
        "ntx_io")
            log_warn "ntx_io power toggle required but no tool available; skipping"
            ;;
        "wmt")
            echo 0 >/dev/wmtWifi 2>/dev/null
            ;;
        *)
            if grep -q "^sdio_wifi_pwr " "/proc/modules"; then
                # Restore Nickel-expected CPU frequency scaling on DVFS-capable i.MX devices.
                if [ -e "/sys/devices/platform/mxc_dvfs_core.0/enable" ]; then
                    echo "0" >"/sys/devices/platform/mxc_dvfs_core.0/enable"
                    echo "userspace" >"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
                    cat "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq" >"/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed"
                fi
                usleep 250000
                rmmod sdio_wifi_pwr 2>/dev/null
            fi
            ;;
    esac
else
    log_msg "WiFi module not loaded, skipping teardown"
fi

# Restart Nickel. Mirror Kobo's stock rcS launch path closely enough to avoid
# leaving Nickel on a blank framebuffer on the i.MX EPDC devices.
log_msg "Restarting Nickel via builtin launcher path"
export LD_LIBRARY_PATH="/usr/local/Kobo"
cd /
unset OLDPWD LC_ALL STARDICT_DATA_DIR EXT_FONT_DIR KO_DONT_GRAB_INPUT FBINK_FORCE_ROTA
(
    if [ "${PLATFORM}" = "freescale" ] || [ "${PLATFORM}" = "mx50-ntx" ] || [ "${PLATFORM}" = "mx6sl-ntx" ]; then
        usleep 400000
    fi
    /etc/init.d/on-animator.sh >>"${LOGFILE}" 2>&1
) &

rm -f /tmp/nickel-hardware-status
mkfifo /tmp/nickel-hardware-status

# Flush before launching Nickel.
sync

# Unmount SD card if present so Nickel can detect and mount it cleanly.
if [ -e "/dev/mmcblk1p1" ]; then
    umount /mnt/sd 2>/dev/null
fi

/usr/local/Kobo/hindenburg >>"${LOGFILE}" 2>&1 &
LIBC_FATAL_STDERR_=1 "${NICKEL_BIN}" -platform kobo -skipFontLoad >>"${LOGFILE}" 2>&1 &
[ "${PLATFORM}" != "freescale" ] && udevadm trigger >>"${LOGFILE}" 2>&1 &

exit ${RETVAL}
