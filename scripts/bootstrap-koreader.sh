#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DEPS_DIR="${ROOT_DIR}/.deps"
KOREADER_DIR="${DEPS_DIR}/koreader"
KO_READER_REF=${KO_READER_REF:-v2025.10}

mkdir -p "${DEPS_DIR}"

if [[ ! -d "${KOREADER_DIR}/.git" ]]; then
  git clone --depth 1 --branch "${KO_READER_REF}" https://github.com/koreader/koreader.git "${KOREADER_DIR}"
fi

cd "${KOREADER_DIR}"
./kodev build
