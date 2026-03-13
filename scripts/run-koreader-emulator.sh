#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
KO_READER_DIR="${ROOT_DIR}/.deps/koreader"

if [[ ! -x "${KO_READER_DIR}/kodev" ]]; then
  echo "KOReader is not bootstrapped. Run ./scripts/bootstrap-koreader.sh first." >&2
  exit 1
fi

cd "${KO_READER_DIR}"
./kodev run "$@"
