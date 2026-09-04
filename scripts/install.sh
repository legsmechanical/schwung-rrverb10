#!/usr/bin/env bash
# Install the RRVerb-10 module to a Move.
#
# Usage:
#   ./scripts/install.sh              deploy the module
#   ./scripts/install.sh --with-rom   also upload your local roms/rrv10.bin
#
# The ROM is NOT uploaded by default: it is your own dump, and pushing user
# assets around silently is how they get clobbered. Once it is on the device it
# survives subsequent module updates.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

MOVE_HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/audio_fx/rrverb10"
WITH_ROM=0
[ "$1" = "--with-rom" ] && WITH_ROM=1

if [ ! -d "dist/rrverb10" ]; then
    echo "Error: dist/rrverb10 not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing RRVerb-10 Module -> ${MOVE_HOST} ==="

# Module files deploy as `ableton`; the restart below SSHes as root itself.
ssh "ableton@${MOVE_HOST}" "mkdir -p ${DEST}/roms"
scp -r dist/rrverb10/* "ableton@${MOVE_HOST}:${DEST}/"

# scp -r COPIES, it never deletes -- so a canvas.js from an older install
# survives here forever. dAVEBOx loads that file straight off disk whenever it
# is present, regardless of what module.json declares, so a stale copy silently
# restores the suppressed Bank Editor on a Move-bus slot. Remove it explicitly.
ssh "ableton@${MOVE_HOST}" "rm -f ${DEST}/canvas.js"

if [ "$WITH_ROM" = "1" ]; then
    if [ -f roms/rrv10.bin ]; then
        echo "Uploading program ROM..."
        scp roms/rrv10.bin "ableton@${MOVE_HOST}:${DEST}/roms/rrv10.bin"
    else
        echo "  --with-rom given but roms/rrv10.bin is not present locally; skipping."
    fi
fi

ssh "ableton@${MOVE_HOST}" "chmod -R a+rw ${DEST}"

# A module deploy does NOT take effect until the host restarts -- swapping the
# effect out and back in leaves the old code live, and the deploy looks like a
# no-op. Do it here rather than relying on remembering.
if [ -x "../scripts/restart_move.sh" ]; then
    echo "Restarting Move host..."
    MOVE_HOST="root@${MOVE_HOST}" ../scripts/restart_move.sh
else
    echo ""
    echo "  NOTE: ../scripts/restart_move.sh not found."
    echo "  Restart the host before testing, or the old code stays live."
fi

echo ""
echo "=== Install Complete ==="
echo "Installed to: ${DEST}/"
[ "$WITH_ROM" = "1" ] || echo "ROM: supply roms/rrv10.bin (or re-run with --with-rom)."
