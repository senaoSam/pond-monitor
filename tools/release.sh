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
#
# Release tags are <device>-v<code>, so it is always clear which board a given
# release is for. The version code is read from the firmware source, so the two
# cannot drift: bump FW_VERSION_CODE and this follows.

set -euo pipefail

DEVICE="${1:-}"
NOTES="${2:-}"
RTDB="https://esp32-e7966-default-rtdb.asia-southeast1.firebasedatabase.app"
REPO="senaoSam/pond-monitor"

case "$DEVICE" in
  pond-site) DIR=sensor-node ;;
  watchdog)  DIR=watchdog ;;
  *) echo "usage: $0 {pond-site|watchdog} \"notes\"" >&2; exit 1 ;;
esac

cd "$(dirname "$0")/.."

# The firmware is the single source of truth for its own version.
CODE=$(grep -oE 'FW_VERSION_CODE = [0-9]+' "$DIR/src/main.cpp" | grep -oE '[0-9]+')
[ -n "$CODE" ] || { echo "could not read FW_VERSION_CODE from $DIR/src/main.cpp" >&2; exit 1; }

TAG="${DEVICE}-v${CODE}"
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

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo "!! $TAG already exists -- bump FW_VERSION_CODE first" >&2
  exit 1
fi

echo ">> building"
( cd "$DIR" && python -m platformio run -e esp32-s3-devkitc-1 >/dev/null )

BIN="$DIR/.pio/build/esp32-s3-devkitc-1/firmware.bin"
STAGE="$(mktemp -d)/${DEVICE}.bin"
cp "$BIN" "$STAGE"
echo ">> $(stat -c%s "$STAGE") bytes"

echo ">> publishing $TAG"
gh release create "$TAG" --repo "$REPO" \
  --title "$DEVICE v$CODE" \
  --notes "${NOTES:-No notes.}

Version code: $CODE" \
  "$STAGE" >/dev/null

URL="https://github.com/$REPO/releases/download/$TAG/${DEVICE}.bin"

# Only point the device at the asset once it is actually downloadable --
# otherwise a board could poll into a 404 and mark a good version bad.
echo ">> verifying the asset is fetchable"
SIZE=$(curl -sL --max-time 120 -o /dev/null -w '%{size_download}' "$URL")
[ "$SIZE" = "$(stat -c%s "$STAGE")" ] || {
  echo "!! downloaded $SIZE bytes, expected $(stat -c%s "$STAGE") -- not updating RTDB" >&2
  exit 1
}

echo ">> pointing /firmware/$DEVICE at v$CODE"
curl -s --max-time 30 -X PUT "$RTDB/firmware/$DEVICE.json" \
  -H 'Content-Type: application/json' \
  -d "{\"version\":$CODE,\"url\":\"$URL\",\"note\":\"${NOTES:-}\"}" \
  -o /dev/null -w '   RTDB HTTP %{http_code}\n'

echo ">> done. boards poll a minute after boot, then every 30 min."
echo "   on the same network you can force it: curl http://<ip>/fwcheck"
