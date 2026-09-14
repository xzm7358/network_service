#!/bin/sh
set -eu

SCRIPT=${1:-packaging/S40network_service}
TMP=${TMPDIR:-/tmp}/network_service_bootstrap_test_$$
EMPTY_SEED="$TMP/missing.seed"

cleanup() {
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM HUP
mkdir -p "$TMP"

run_bootstrap() {
  config_dir=$1
  event_dir=$2
  seed=$3
  NETWORK_SERVICE_CONFIG_DIR="$config_dir" \
  NETWORK_SERVICE_EVENT_DIR="$event_dir" \
  NETWORK_SERVICE_WPA_SEED="$seed" \
  sh "$SCRIPT" bootstrap >/dev/null
}

DEFAULT_DATA="$TMP/default-data"
DEFAULT_EVENT="$TMP/default-wpa"
run_bootstrap "$DEFAULT_DATA" "$DEFAULT_EVENT" "$EMPTY_SEED"

ETHERNET="$DEFAULT_DATA/network-service/ethernet.json"
WPA="$DEFAULT_DATA/network-service/wpa_supplicant.conf"

[ "$(cat "$ETHERNET")" = '{"mode":"dhcp"}' ] || {
  echo "network_service_bootstrap_test: default Ethernet config is not DHCP" >&2
  exit 1
}
grep -qx "ctrl_interface=$DEFAULT_EVENT" "$WPA" || {
  echo "network_service_bootstrap_test: default Wi-Fi ctrl_interface is wrong" >&2
  exit 1
}
grep -qx 'update_config=1' "$WPA" || {
  echo "network_service_bootstrap_test: default Wi-Fi config is not writable" >&2
  exit 1
}

# Bootstrap is create-only. Never replace a user-owned or previously persisted
# configuration on later service restarts.
printf '%s\n' '{"mode":"static","sentinel":true}' > "$ETHERNET"
printf '%s\n' 'sentinel=user-wifi' > "$WPA"
run_bootstrap "$DEFAULT_DATA" "$DEFAULT_EVENT" "$EMPTY_SEED"
[ "$(cat "$ETHERNET")" = '{"mode":"static","sentinel":true}' ] || {
  echo "network_service_bootstrap_test: existing Ethernet config was overwritten" >&2
  exit 1
}
[ "$(cat "$WPA")" = 'sentinel=user-wifi' ] || {
  echo "network_service_bootstrap_test: existing Wi-Fi config was overwritten" >&2
  exit 1
}

SEED="$TMP/factory.seed"
cat > "$SEED" <<'EOF'
update_config=0
persistent_reconnect=1
network={
  ssid="factory-network"
  key_mgmt=NONE
}
EOF

SEEDED_DATA="$TMP/seeded-data"
SEEDED_EVENT="$TMP/seeded-wpa"
run_bootstrap "$SEEDED_DATA" "$SEEDED_EVENT" "$SEED"
SEEDED_WPA="$SEEDED_DATA/network-service/wpa_supplicant.conf"
grep -qx "ctrl_interface=$SEEDED_EVENT" "$SEEDED_WPA" || {
  echo "network_service_bootstrap_test: imported seed lacks runtime ctrl_interface" >&2
  exit 1
}
grep -qx 'update_config=1' "$SEEDED_WPA" || {
  echo "network_service_bootstrap_test: imported seed was not made writable" >&2
  exit 1
}
grep -q 'ssid="factory-network"' "$SEEDED_WPA" || {
  echo "network_service_bootstrap_test: factory network was not imported" >&2
  exit 1
}
if grep -q '^update_config=0$' "$SEEDED_WPA"; then
  echo "network_service_bootstrap_test: read-only seed setting survived import" >&2
  exit 1
fi

trap - EXIT INT TERM HUP
rm -rf "$TMP"
echo "network_service_bootstrap_test: PASS"
