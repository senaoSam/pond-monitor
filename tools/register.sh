#!/usr/bin/env bash
# Flash a heartbeat board and register it under the label stuck on it.
#
#   tools/register.sh <ID> <PORT> [--canary] [--note "text"] [--no-flash] [--force]
#
#   ID        the label: pond letter + number, e.g. A03 (letters, digits, - _)
#   PORT      the board's serial port, e.g. COM6
#   --canary  this board takes releases first (tools/release.sh heartbeat-canary)
#   --note    what it watches, e.g. "3號池打氣機" (kept in the registry)
#   --no-flash  register only; the board already runs heartbeat firmware
#   --force   take over an ID that another MAC holds (replacing a dead board)
#
# The board finds out who it is from /registry/<MAC> at boot (see
# shared/Registry), so registering is what turns an anonymous board into A03.
# One already running reboots into its new identity within half an hour, or
# at once when power-cycled.
#
# The MAC printed here is the same one the board reports in meta.mac.

set -euo pipefail

RTDB="https://esp32-e7966-default-rtdb.asia-southeast1.firebasedatabase.app"

ID="${1:-}"; PORT="${2:-}"
[ -n "$ID" ] && [ -n "$PORT" ] || { sed -n '3,12p' "$0" >&2; exit 1; }
shift 2
CANARY=false; NOTE=""; FLASH=true; FORCE=false
while [ $# -gt 0 ]; do
  case "$1" in
    --canary) CANARY=true ;;
    --note) NOTE="${2:-}"; shift ;;
    --no-flash) FLASH=false ;;
    --force) FORCE=true ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
  shift
done

# Same rule the board applies (registryIdValid): the id becomes an RTDB key
# and Discord text.
[[ "$ID" =~ ^[A-Za-z0-9_-]{1,16}$ ]] || { echo "!! bad id '$ID'" >&2; exit 1; }

cd "$(dirname "$0")/.."

# esptool prints the MAC while it flashes, so it is taken from that output
# rather than by connecting a second time -- on a board without auto-reset
# every connection costs another BOOT+RST by hand.
LOG=$(mktemp)
if $FLASH; then
  echo ">> flashing heartbeat firmware to $PORT"
  echo "   (if it hangs at 'Connecting...', hold BOOT, tap RST, release BOOT)"
  ( cd heartbeat-node && python -m platformio run -e esp32-s3-devkitc-1 \
      -t upload --upload-port "$PORT" ) >"$LOG" 2>&1 ||
    { tail -20 "$LOG" >&2; echo "!! flash failed" >&2; exit 1; }
else
  echo ">> reading MAC on $PORT"
  python "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --port "$PORT" read_mac >"$LOG" 2>&1 || true
fi
MAC=$(grep -m1 -oiE 'MAC: ([0-9a-f]{2}:){5}[0-9a-f]{2}' "$LOG" |
      sed -E 's/MAC: //; s/://g' | tr 'a-f' 'A-F')
rm -f "$LOG"
[ ${#MAC} -eq 12 ] || { echo "!! could not read the MAC on $PORT" >&2; exit 1; }
echo "   MAC $MAC"

# One label, one board. Two boards under the same id would overwrite each
# other's heartbeat, and the watchdog could never tell which one is dead.
HOLDER=$(curl -s --max-time 20 "$RTDB/registry.json" | python -c "
import json,sys
d=json.load(sys.stdin) or {}
print(' '.join(m for m,v in d.items() if isinstance(v,dict) and v.get('id','').upper()=='$ID'.upper() and m!='$MAC'))")
if [ -n "$HOLDER" ]; then
  if $FORCE; then
    echo ">> $ID was on $HOLDER -- taking it over (--force)"
    for m in $HOLDER; do curl -s --max-time 20 -X DELETE "$RTDB/registry/$m.json" >/dev/null; done
  else
    echo "!! $ID already belongs to $HOLDER. Use --force when replacing that board." >&2
    exit 1
  fi
fi

# Whatever this board published under before -- its unregistered esp-<mac>
# name, or a previous label -- would otherwise sit in /devices going stale,
# and the watchdog would report it as a failed supply.
OLDID=$(curl -s --max-time 20 "$RTDB/registry/$MAC/id.json" | tr -d '"')
for OLD in "esp-$(echo "$MAC" | tr 'A-F' 'a-f')" "$OLDID"; do
  [ -n "$OLD" ] && [ "$OLD" != null ] && [ "$OLD" != "$ID" ] || continue
  if [ "$(curl -s --max-time 20 "$RTDB/devices/$OLD.json?shallow=true")" != null ]; then
    curl -s --max-time 20 -X DELETE "$RTDB/devices/$OLD.json" >/dev/null
    echo ">> removed old /devices/$OLD"
  fi
done

BODY=$(ID="$ID" NOTE="$NOTE" CANARY="$CANARY" python -c "
import json,os
b={'id':os.environ['ID'],'canary':os.environ['CANARY']=='true'}
if os.environ['NOTE']: b['note']=os.environ['NOTE']
print(json.dumps(b))  # \uXXXX escapes survive the Windows command line")
CODE=$(curl -s --max-time 20 -o /dev/null -w '%{http_code}' -X PUT \
       "$RTDB/registry/$MAC.json" -H 'Content-Type: application/json' -d "$BODY")
[ "$CODE" = 200 ] || { echo "!! registry write failed: HTTP $CODE" >&2; exit 1; }
echo ">> registered $MAC as $ID$($CANARY && echo ' [canary]')"
echo "   it reports as /devices/$ID once it is up -- watch for the green pulse"
