#!/bin/sh
# Launch hadisplay on the Kobo.
# Stops Nickel, runs the app, then restarts Nickel on exit.

HADISPLAY_DIR="/mnt/onboard/.adds/hadisplay"
LOGFILE="${HADISPLAY_DIR}/log.txt"
ORIG_LD_LIBRARY_PATH="${LD_LIBRARY_PATH}"
HADISPLAY_LD_LIBRARY_PATH="/mnt/onboard/.niluje/usbnet/lib"

# Siphon env from Nickel before we kill it (needed for WiFi, dbus, etc.)
if pkill -0 nickel 2>/dev/null; then
    eval "$(grep -s -E -e '^(DBUS_SESSION_BUS_ADDRESS|NICKEL_HOME|WIFI_MODULE|LANG|INTERFACE)=' \
        "/proc/$(pidof -s nickel)/environ" | sed 's/^/export /')"

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
WIFI_IFACE="${INTERFACE:-wlan0}"
WIFI_LOG="${HADISPLAY_DIR}/hadisplay.log"

log_msg() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [INFO] [keepalive] $1" >>"$WIFI_LOG"
}
log_warn() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [WARN] [keepalive] $1" >>"$WIFI_LOG"
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
        # Check if the interface is still up.
        if ! ip link show "$WIFI_IFACE" up 2>/dev/null | grep -q "state UP"; then
            log_warn "WiFi interface $WIFI_IFACE is down, attempting recovery"
            # Try to reload WiFi modules and reconnect.
            if [ -n "${WIFI_MODULE}" ]; then
                insmod "/drivers/${PLATFORM}/wifi/sdio_wifi_pwr.ko" 2>/dev/null
                insmod "/drivers/${PLATFORM}/wifi/${WIFI_MODULE}" 2>/dev/null
                sleep 2
            fi
            ifconfig "$WIFI_IFACE" up 2>/dev/null
            wpa_cli -i "$WIFI_IFACE" reconnect 2>/dev/null
            sleep 5
            udhcpc -i "$WIFI_IFACE" -t 5 -q 2>/dev/null
            if ip link show "$WIFI_IFACE" up 2>/dev/null | grep -q "state UP"; then
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

# Run hadisplay.
if [ -n "${ORIG_LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}:${ORIG_LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}"
fi
cd "${HADISPLAY_DIR}" || exit 1
./hadisplay >>"${LOGFILE}" 2>&1
RETVAL=$?

# Stop the keepalive when hadisplay exits.
kill "$KEEPALIVE_PID" 2>/dev/null
wait "$KEEPALIVE_PID" 2>/dev/null

# Restart Nickel.
if [ -n "${ORIG_LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH="/usr/local/Kobo:/lib:/usr/lib:${ORIG_LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="/usr/local/Kobo:/lib:/usr/lib"
fi
/usr/local/Kobo/nickel -platform kobo -skipFontLoad >>"${LOGFILE}" 2>&1 &
sync

exit ${RETVAL}
