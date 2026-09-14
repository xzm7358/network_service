#!/bin/sh
set -eu

RCS=${RCS_PATH:-/etc/init.d/rcS}
BIN=${NETWORK_SERVICE_BIN:-/dnake/bin/network_service}
CONFIG_DIR=${NETWORK_SERVICE_CONFIG_DIR:-/data}
BACKUP=${RCS}.pre-network-service
TMP=${RCS}.network-service.$$

cleanup() {
  rm -f "$TMP"
}
trap cleanup EXIT INT TERM HUP

[ -f "$RCS" ] || {
  echo "network_service rcS migration: missing file: $RCS" >&2
  exit 2
}
[ ! -L "$RCS" ] || {
  echo "network_service rcS migration: refusing to replace symlink: $RCS" >&2
  exit 2
}

matches=$(awk '
  /^[[:space:]]*ifconfig[[:space:]]+eth0[[:space:]]+192[.]168[.]68[.]90[[:space:]]*$/ { count++ }
  END { print count + 0 }
' "$RCS")

if [ "$matches" -eq 0 ]; then
  echo "network_service rcS migration: legacy eth0 assignment already absent"
  exit 0
fi

[ -x "$BIN" ] || {
  echo "network_service rcS migration: service binary is not executable: $BIN" >&2
  exit 3
}
[ -r "$CONFIG_DIR/network-service/ethernet.json" ] || {
  echo "network_service rcS migration: missing authoritative ethernet.json" >&2
  exit 3
}
if ! "$BIN" --check-capability ethernet-json-v1 >/dev/null 2>&1; then
  echo "network_service rcS migration: ethernet-json-v1 capability is unavailable" >&2
  exit 3
fi
if ! "$BIN" --config-dir "$CONFIG_DIR" --eth eth0 \
    --validate-ethernet-config >/dev/null 2>&1; then
  echo "network_service rcS migration: authoritative ethernet.json is invalid" >&2
  exit 3
fi

if [ ! -e "$BACKUP" ]; then
  cp -p "$RCS" "$BACKUP"
fi
cp -p "$RCS" "$TMP"
awk '
  !/^[[:space:]]*ifconfig[[:space:]]+eth0[[:space:]]+192[.]168[.]68[.]90[[:space:]]*$/ { print }
' "$RCS" > "$TMP"

remaining=$(awk '
  /^[[:space:]]*ifconfig[[:space:]]+eth0[[:space:]]+192[.]168[.]68[.]90[[:space:]]*$/ { count++ }
  END { print count + 0 }
' "$TMP")
[ "$remaining" -eq 0 ] || {
  echo "network_service rcS migration: staged validation failed" >&2
  exit 4
}

mv "$TMP" "$RCS"
trap - EXIT INT TERM HUP
echo "network_service rcS migration: removed $matches legacy eth0 assignment(s)"
