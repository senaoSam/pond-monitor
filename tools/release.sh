#!/usr/bin/env bash
# Build, publish and point a device at a new firmware version.
#
# Each device polls its own RTDB entry, so devices are versioned independently:
# updating the pond node does not touch the watchdog, which lets a change be
# proven on one board before the other follows.
#
#   tools/release.sh <device> "<release notes>"
#
#     device  pond-site | watchdog        (matches DEVICE_ID in the firmware,
#                                          and the RTDB key under /firmware)
#             heartbeat-canary            the heartbeat boards registered as
#                                          canary (tools/register.sh --canary)
#             heartbeat                   every heartbeat board
#
# The heartbeat boards all share one image, so a release goes in two steps: to
# the canary boards first, then -- once they have run it a while -- to all of
# them. Promoting reuses the canary's published asset rather than rebuilding,
# so the boards that were proven on it and the rest run byte-identical code.
# Promoting also moves the canary entry, so canary boards are never behind.
#
#   tools/release.sh heartbeat-canary "notes"   build, publish, canary only
#   tools/release.sh heartbeat "notes"          everyone (reuses the asset)
#
# Release tags are <asset>-v<code>, so it is always clear which board a given
# release is for. The version code is read from the firmware source, so the two
# cannot drift: bump FW_VERSION_CODE and this follows.

set -euo pipefail

DEVICE="${1:-}"
NOTES="${2:-}"
RTDB="https://esp32-e7966-default-rtdb.asia-southeast1.firebasedatabase.app"
REPO="senaoSam/pond-monitor"

# ASSET names the tag and the .bin; ENTRIES are the /firmware keys pointed at it.
case "$DEVICE" in
  pond-site)        DIR=sensor-node;    ASSET=pond-site; ENTRIES="pond-site" ;;
  watchdog)         DIR=watchdog;       ASSET=watchdog;  ENTRIES="watchdog" ;;
  heartbeat-canary) DIR=heartbeat-node; ASSET=heartbeat; ENTRIES="heartbeat-canary" ;;
  heartbeat)        DIR=heartbeat-node; ASSET=heartbeat; ENTRIES="heartbeat heartbeat-canary" ;;
  *) echo "usage: $0 {pond-site|watchdog|heartbeat-canary|heartbeat} \"notes\"" >&2; exit 1 ;;
esac

cd "$(dirname "$0")/.."

# The firmware is the single source of truth for its own version.
CODE=$(grep -oE 'FW_VERSION_CODE = [0-9]+' "$DIR/src/main.cpp" | grep -oE '[0-9]+')
[ -n "$CODE" ] || { echo "could not read FW_VERSION_CODE from $DIR/src/main.cpp" >&2; exit 1; }

TAG="${ASSET}-v${CODE}"
URL="https://github.com/$REPO/releases/download/$TAG/${ASSET}.bin"
echo ">> $DEVICE version $CODE  (tag $TAG)"

# The .bin is built from the working tree, but the tag GitHub creates points at
# a commit -- so publishing with changes uncommitted ships a binary that no
# commit corresponds to, and "what is actually running on the board?" stops
# having an answer. Ignored files (secrets.h) are deliberately not counted.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "!! uncommitted changes -- commit before releasing, or the tag will not" >&2
  echo "   match the firmware:" >&2
  git status --short --untracked-files=no >&2
  exit 1
fi

# Promoting a heartbeat release that the canaries already run: nothing to build
# or upload, only the entries to move.
PROMOTE=false
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  if [ "$DEVICE" = heartbeat ]; then
    PROMOTE=true
    echo ">> $TAG already published -- promoting that same asset"
  else
    echo "!! $TAG already exists -- bump FW_VERSION_CODE first" >&2
    exit 1
  fi
fi

if ! $PROMOTE; then
  echo ">> building"
  ( cd "$DIR" && python -m platformio run -e esp32-s3-devkitc-1 >/dev/null )

  BIN="$DIR/.pio/build/esp32-s3-devkitc-1/firmware.bin"
  STAGE="$(mktemp -d)/${ASSET}.bin"
  cp "$BIN" "$STAGE"
  echo ">> $(stat -c%s "$STAGE") bytes"

  echo ">> publishing $TAG"
  gh release create "$TAG" --repo "$REPO" \
    --title "$ASSET v$CODE" \
    --notes "${NOTES:-No notes.}

Version code: $CODE" \
    "$STAGE" >/dev/null
  EXPECT=$(stat -c%s "$STAGE")
else
  EXPECT=""
fi

# Only point boards at the asset once it is actually downloadable -- otherwise a
# board could poll into a 404 and mark a good version bad.
echo ">> verifying the asset is fetchable"
SIZE=$(curl -sL --max-time 120 -o /dev/null -w '%{size_download}' "$URL")
if [ -n "$EXPECT" ] && [ "$SIZE" != "$EXPECT" ]; then
  echo "!! downloaded $SIZE bytes, expected $EXPECT -- not updating RTDB" >&2
  exit 1
fi
[ "${SIZE:-0}" -gt 100000 ] || { echo "!! asset looks empty ($SIZE bytes)" >&2; exit 1; }

for ENTRY in $ENTRIES; do
  echo ">> pointing /firmware/$ENTRY at v$CODE"
  curl -s --max-time 30 -X PUT "$RTDB/firmware/$ENTRY.json" \
    -H 'Content-Type: application/json' \
    -d "{\"version\":$CODE,\"url\":\"$URL\",\"note\":\"${NOTES:-}\"}" \
    -o /dev/null -w '   RTDB HTTP %{http_code}\n'
done

echo ">> done. boards poll a minute after boot, then every 30 min."
echo "   on the same network you can force it: curl http://<ip>/fwcheck"
