#!/bin/sh
# Build hadisplay for Kobo, deploy via rsync, and restart on device.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build-kobo"
REMOTE="kobo"
REMOTE_DIR="/mnt/onboard/.adds/hadisplay"

# Build.
if [ ! -d "${BUILD_DIR}" ]; then
    echo "Configuring build-kobo..."
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${PROJECT_DIR}/cmake/toolchains/kobo.cmake" \
        -DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
fi

echo "Building hadisplay..."
cmake --build "${BUILD_DIR}" --target hadisplay -j"$(nproc)"

# Deploy (--no-owner --no-group avoids chown on FAT32).
echo "Deploying to ${REMOTE}:${REMOTE_DIR}..."
rsync -avz --no-owner --no-group \
    "${BUILD_DIR}/hadisplay" \
    "${PROJECT_DIR}/scripts/run-hadisplay.sh" \
    "${REMOTE}:${REMOTE_DIR}/"

# Restart on device.
echo "Restarting hadisplay on device..."
ssh "${REMOTE}" "killall hadisplay 2>/dev/null || true; sleep 1; cd ${REMOTE_DIR} && chmod +x run-hadisplay.sh && > log.txt && nohup /bin/sh ./run-hadisplay.sh >> log.txt 2>&1 &"

echo "Done. Tailing log..."
sleep 1
ssh "${REMOTE}" "cat ${REMOTE_DIR}/log.txt"
