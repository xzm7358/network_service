#!/bin/sh
set -eu

SCRIPT=${1:?migration script is required}
REAL_SERVICE=${2:?network_service binary is required}
TMP=${TMPDIR:-/tmp}/rcs_ethernet_migration_test_$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

RCS="$TMP/rcS"
NEW_SERVICE="$TMP/network_service_new"
OLD_SERVICE="$TMP/network_service_old"
CONFIG_DIR="$TMP/data"
mkdir -p "$CONFIG_DIR/network-service"
printf '%s\n' '{"mode":"dhcp"}' > "$CONFIG_DIR/network-service/ethernet.json"

cat > "$NEW_SERVICE" <<'EOF'
#!/bin/sh
if [ "$#" -eq 2 ] && [ "$1" = "--check-capability" ] && \
   [ "$2" = "ethernet-json-v1" ]; then
  exit 0
fi
if [ "$#" -eq 5 ] && [ "$1" = "--config-dir" ] && \
   [ "$3" = "--eth" ] && [ "$4" = "eth0" ] && \
   [ "$5" = "--validate-ethernet-config" ] && \
   [ -r "$2/network-service/ethernet.json" ]; then
  exit 0
fi
exit 1
EOF
cat > "$OLD_SERVICE" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$NEW_SERVICE" "$OLD_SERVICE"

cat > "$RCS" <<'EOF'
#!/bin/sh
ifconfig lo 127.0.0.1
ifconfig eth0 192.168.68.90
ifconfig eth0 192.168.68.91
# ifconfig eth0 192.168.68.90
ifconfig eth0 192.168.68.90 netmask 255.255.255.0
start_application
EOF
chmod 0750 "$RCS"
mode_before=$(ls -ld "$RCS" | awk '{ print $1 }')

before=$(cksum "$RCS")
if NETWORK_SERVICE_BIN="$OLD_SERVICE" NETWORK_SERVICE_CONFIG_DIR="$CONFIG_DIR" \
    RCS_PATH="$RCS" sh "$SCRIPT"; then
  echo "rcs_ethernet_migration_test: old service passed capability gate" >&2
  exit 1
fi
[ "$(cksum "$RCS")" = "$before" ] || {
  echo "rcs_ethernet_migration_test: failed gate changed rcS" >&2
  exit 1
}

NETWORK_SERVICE_BIN="$NEW_SERVICE" NETWORK_SERVICE_CONFIG_DIR="$CONFIG_DIR" \
  RCS_PATH="$RCS" sh "$SCRIPT"

if grep -x 'ifconfig eth0 192.168.68.90' "$RCS" >/dev/null; then
  echo "rcs_ethernet_migration_test: exact legacy assignment remains" >&2
  exit 1
fi
grep -x 'ifconfig eth0 192.168.68.91' "$RCS" >/dev/null
grep -x '# ifconfig eth0 192.168.68.90' "$RCS" >/dev/null
grep -x 'ifconfig eth0 192.168.68.90 netmask 255.255.255.0' "$RCS" >/dev/null
grep -x 'start_application' "$RCS" >/dev/null
[ -f "$RCS.pre-network-service" ] || {
  echo "rcs_ethernet_migration_test: recovery backup missing" >&2
  exit 1
}
[ "$(ls -ld "$RCS" | awk '{ print $1 }')" = "$mode_before" ] || {
  echo "rcs_ethernet_migration_test: rcS mode was not preserved" >&2
  exit 1
}

after=$(cksum "$RCS")
NETWORK_SERVICE_BIN="$OLD_SERVICE" NETWORK_SERVICE_CONFIG_DIR="$CONFIG_DIR" \
  RCS_PATH="$RCS" sh "$SCRIPT"
[ "$(cksum "$RCS")" = "$after" ] || {
  echo "rcs_ethernet_migration_test: repeat run was not idempotent" >&2
  exit 1
}

INVALID_RCS="$TMP/rcS-invalid"
printf '%s\n' 'ifconfig eth0 192.168.68.90' > "$INVALID_RCS"
printf '%s\n' '{"mode":"static","address":"bad"}' > \
  "$CONFIG_DIR/network-service/ethernet.json"
invalid_before=$(cksum "$INVALID_RCS")
if NETWORK_SERVICE_BIN="$REAL_SERVICE" NETWORK_SERVICE_CONFIG_DIR="$CONFIG_DIR" \
    RCS_PATH="$INVALID_RCS" sh "$SCRIPT"; then
  echo "rcs_ethernet_migration_test: invalid config passed validation gate" >&2
  exit 1
fi
[ "$(cksum "$INVALID_RCS")" = "$invalid_before" ] || {
  echo "rcs_ethernet_migration_test: invalid config changed rcS" >&2
  exit 1
}

echo "rcs_ethernet_migration_test: PASS"
