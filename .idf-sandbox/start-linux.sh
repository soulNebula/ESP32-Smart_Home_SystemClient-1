#!/bin/sh
# ===========================================================================
#  ESP32 sandbox -- LINUX launcher
#
#      sh .idf-sandbox/start-linux.sh              # GUI console
#      sh .idf-sandbox/start-linux.sh build        # build only
#      sh .idf-sandbox/start-linux.sh --help
#
#  On Windows use  qi-dong-Windows.cmd  (the Chinese-named .cmd) instead.
#
#  If this file was copied from a USB stick / FAT partition the executable
#  bit may be gone, so running it as `sh start-linux.sh` is the safe way.
#
#  The sandbox ships its own Linux toolchain, but no Python (every distro
#  already has python3).  So we look for a bundled python first, then fall
#  back to the system one.
# ===========================================================================
set -u

# Work out this script's own directory without calling anything external.
# (dirname would be fine on a real Linux, but this way the script also works
# in minimal shells and under Git-for-Windows.)
case "$0" in
    */*) DIR=${0%/*} ;;
    *)   DIR=. ;;
esac
DIR=$(CDPATH= cd -- "$DIR" && pwd)
SCRIPT="$DIR/start.py"

if [ ! -f "$SCRIPT" ]; then
    echo "[X] start.py not found: $SCRIPT" >&2
    exit 2
fi

PY=""
for candidate in \
    "$DIR/python-linux/bin/python3" \
    "$DIR/python-linux/bin/python"
do
    if [ -x "$candidate" ]; then PY="$candidate"; break; fi
done

if [ -z "$PY" ]; then
    if command -v python3 >/dev/null 2>&1; then
        PY=$(command -v python3)
    elif command -v python >/dev/null 2>&1; then
        PY=$(command -v python)
    fi
fi

if [ -z "$PY" ]; then
    echo "[X] python3 not found." >&2
    echo "    Install one first:" >&2
    echo "      Debian/Ubuntu : sudo apt install python3 python3-venv" >&2
    echo "      Fedora        : sudo dnf install python3" >&2
    exit 2
fi

exec "$PY" "$SCRIPT" --os linux "$@"
