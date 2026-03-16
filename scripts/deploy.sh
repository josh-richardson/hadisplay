#!/bin/sh
# Build hadisplay for Kobo, deploy via rsync, and restart on device.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REMOTE="kobo"
REMOTE_DIR="/mnt/onboard/.adds/hadisplay"

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

JOBS="${HADISPLAY_JOBS:-$(detect_jobs)}"
NATIVE_BUILD_DIR="${PROJECT_DIR}/build-kobo"
DOCKER_BUILD_DIR="${PROJECT_DIR}/build-kobo-docker"
BUILD_DIR=""

if command -v arm-kobo-linux-gnueabihf-gcc >/dev/null 2>&1 && command -v arm-kobo-linux-gnueabihf-g++ >/dev/null 2>&1; then
    BUILD_DIR="${NATIVE_BUILD_DIR}"
    if [ ! -d "${BUILD_DIR}" ]; then
        echo "Configuring native Kobo build..."
        cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
            -DCMAKE_TOOLCHAIN_FILE="${PROJECT_DIR}/cmake/toolchains/kobo.cmake" \
            -DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF
    fi

    echo "Building hadisplay..."
    cmake --build "${BUILD_DIR}" --target hadisplay --parallel "${JOBS}"
else
    BUILD_DIR="${DOCKER_BUILD_DIR}"
    BUILD_DIR="${BUILD_DIR}" HADISPLAY_JOBS="${JOBS}" "${SCRIPT_DIR}/build-kobo-in-docker.sh"
fi

# Stop the current app before updating the binary.
ssh "${REMOTE}" "mkdir -p ${REMOTE_DIR}; killall hadisplay 2>/dev/null || true"

# Deploy (--no-owner/--no-group/--no-perms avoids VFAT noise on the device).
echo "Deploying to ${REMOTE}:${REMOTE_DIR}..."
rsync -rtvP --inplace --no-perms --no-owner --no-group \
    --rsync-path=/usr/bin/rsync -e ssh \
    "${BUILD_DIR}/hadisplay" \
    "${REMOTE}:${REMOTE_DIR}/hadisplay.new"
rsync -rtvP --inplace --no-perms --no-owner --no-group \
    --rsync-path=/usr/bin/rsync -e ssh \
    "${PROJECT_DIR}/scripts/run-hadisplay.sh" \
    "${REMOTE}:${REMOTE_DIR}/run-hadisplay.sh"
if [ -f "${PROJECT_DIR}/.env" ]; then
    rsync -rtvP --inplace --no-perms --no-owner --no-group \
        --rsync-path=/usr/bin/rsync -e ssh \
        "${PROJECT_DIR}/.env" \
        "${REMOTE}:${REMOTE_DIR}/.env"
else
    echo "No .env found at ${PROJECT_DIR}/.env; skipping."
fi

# Restart on device.
echo "Restarting hadisplay on device..."
ssh "${REMOTE}" "mv ${REMOTE_DIR}/hadisplay.new ${REMOTE_DIR}/hadisplay && sleep 1 && cd ${REMOTE_DIR} && chmod +x hadisplay run-hadisplay.sh && > log.txt && nohup /bin/sh ./run-hadisplay.sh >> log.txt 2>&1 &"

echo "Done. Tailing log..."
sleep 1
ssh "${REMOTE}" "cat ${REMOTE_DIR}/log.txt"
