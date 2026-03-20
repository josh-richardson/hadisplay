#!/bin/sh
# Fetch and display hadisplay logs from the Kobo.
#
# Usage:
#   ./logs.sh              Show the full log
#   ./logs.sh -f           Tail the log (follow)
#   ./logs.sh -n 50        Show the last 50 lines
#   ./logs.sh -n 50 -f     Tail starting from the last 50 lines

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1090
. "${SCRIPT_DIR}/hadisplay-target.sh"

FOLLOW=false
NUM_LINES=""
TARGET_NAME="${HADISPLAY_TARGET:-${HADISPLAY_DEFAULT_TARGET:-clara-colour}}"

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--target)
            if [ $# -lt 2 ]; then
                echo "Missing target name for $1" >&2
                exit 1
            fi
            TARGET_NAME="$2"
            shift 2
            ;;
        --target=*)
            TARGET_NAME="${1#*=}"
            shift
            ;;
        -f|--follow)
            FOLLOW=true
            shift
            ;;
        -n|--lines)
            if [ $# -lt 2 ]; then
                echo "Missing line count for $1" >&2
                exit 1
            fi
            NUM_LINES="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [-t TARGET] [-f] [-n NUM]"
            echo "  -t, --target T Show logs for target T"
            echo "  -f, --follow   Tail the log (follow new lines)"
            echo "  -n, --lines N  Show only the last N lines"
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

hadisplay_load_target "${TARGET_NAME}" || exit 1

REMOTE="${HADISPLAY_SSH_HOST}"
REMOTE_LOG="${HADISPLAY_REMOTE_LOG}"

if [ "$FOLLOW" = true ]; then
    if [ -n "$NUM_LINES" ]; then
        ssh "$REMOTE" "tail -n $NUM_LINES -f '$REMOTE_LOG'"
    else
        ssh "$REMOTE" "tail -f '$REMOTE_LOG'"
    fi
elif [ -n "$NUM_LINES" ]; then
    ssh "$REMOTE" "tail -n $NUM_LINES '$REMOTE_LOG'"
else
    ssh "$REMOTE" "cat '$REMOTE_LOG'"
fi
