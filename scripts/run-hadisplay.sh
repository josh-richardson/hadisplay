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

# Run hadisplay.
if [ -n "${ORIG_LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}:${ORIG_LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="${HADISPLAY_LD_LIBRARY_PATH}"
fi
cd "${HADISPLAY_DIR}" || exit 1
./hadisplay >>"${LOGFILE}" 2>&1
RETVAL=$?

# Restart Nickel.
if [ -n "${ORIG_LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH="/usr/local/Kobo:/lib:/usr/lib:${ORIG_LD_LIBRARY_PATH}"
else
    export LD_LIBRARY_PATH="/usr/local/Kobo:/lib:/usr/lib"
fi
/usr/local/Kobo/nickel -platform kobo -skipFontLoad >>"${LOGFILE}" 2>&1 &
sync

exit ${RETVAL}
