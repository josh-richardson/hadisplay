#!/bin/sh
# Fetch and display hadisplay logs from the Kobo.
#
# Usage:
#   ./logs.sh              Show the full log
#   ./logs.sh -f           Tail the log (follow)
#   ./logs.sh -n 50        Show the last 50 lines
#   ./logs.sh -n 50 -f     Tail starting from the last 50 lines

REMOTE="kobo"
REMOTE_LOG="/mnt/onboard/.adds/hadisplay/hadisplay.log"

FOLLOW=false
NUM_LINES=""

while [ $# -gt 0 ]; do
    case "$1" in
        -f|--follow)
            FOLLOW=true
            shift
            ;;
        -n|--lines)
            NUM_LINES="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [-f] [-n NUM]"
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
