#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build-kobo-docker}"
IMAGE_TAG="${HADISPLAY_KOBO_DOCKER_IMAGE:-hadisplay-kobo-build:latest}"
TOOLCHAIN_RELEASE="${KOBO_TOOLCHAIN_RELEASE:-2025.05}"
TOOLCHAIN_BASE="${PROJECT_DIR}/.deps/koxtoolchain/${TOOLCHAIN_RELEASE}"
TOOLCHAIN_ROOT="${TOOLCHAIN_BASE}/x-tools/arm-kobo-linux-gnueabihf"
CONTAINER_BUILD_DIR="/workspace/$(basename "${BUILD_DIR}")"

detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
    else
        echo 4
    fi
}

JOBS="${HADISPLAY_JOBS:-$(detect_jobs)}"

mkdir -p "${TOOLCHAIN_BASE}"

echo "Building Docker image ${IMAGE_TAG}..."
docker build --platform linux/amd64 \
    -f "${PROJECT_DIR}/docker/kobo-build.Dockerfile" \
    -t "${IMAGE_TAG}" \
    "${PROJECT_DIR}"

echo "Building Kobo target in Docker..."
docker run --rm --platform linux/amd64 \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -e KOBO_TOOLCHAIN_RELEASE="${TOOLCHAIN_RELEASE}" \
    -e HADISPLAY_JOBS="${JOBS}" \
    -v "${PROJECT_DIR}:/workspace" \
    -w /workspace \
    "${IMAGE_TAG}" \
    bash -lc '
        set -euo pipefail

        build_dir="'"${CONTAINER_BUILD_DIR}"'"
        toolchain_base="/workspace/.deps/koxtoolchain/${KOBO_TOOLCHAIN_RELEASE}"
        toolchain_root="${toolchain_base}/x-tools/arm-kobo-linux-gnueabihf"
        archive="${toolchain_base}/kobo.tar.gz"

        if [ ! -x "${toolchain_root}/bin/arm-kobo-linux-gnueabihf-gcc" ]; then
            mkdir -p "${toolchain_base}"
            if [ ! -f "${archive}" ]; then
                echo "Downloading KOReader koxtoolchain ${KOBO_TOOLCHAIN_RELEASE}..."
                curl -fL --retry 3 \
                    -o "${archive}" \
                    "https://github.com/koreader/koxtoolchain/releases/download/${KOBO_TOOLCHAIN_RELEASE}/kobo.tar.gz"
            fi
            rm -rf "${toolchain_root}"
            tar -xzf "${archive}" -C "${toolchain_base}"
        fi

        if [ -f "${build_dir}/CMakeCache.txt" ] && ! grep -q "^CMAKE_GENERATOR:INTERNAL=Unix Makefiles$" "${build_dir}/CMakeCache.txt"; then
            rm -rf "${build_dir}"
        fi

        cmake -S /workspace \
            -B "${build_dir}" \
            -DCMAKE_TOOLCHAIN_FILE=/workspace/cmake/toolchains/kobo.cmake \
            -DKOBO_TOOLCHAIN_ROOT="${toolchain_root}" \
            -DHADISPLAY_BUILD_FBINK_MIRROR_X11=OFF

        cmake --build "${build_dir}" --target hadisplay --parallel "${HADISPLAY_JOBS}"
    '

echo "Docker build complete: ${BUILD_DIR}/hadisplay"
