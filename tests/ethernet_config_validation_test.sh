#!/bin/sh
set -eu

BIN=${1:?network_service binary is required}
TMP=${TMPDIR:-/tmp}/ethernet_config_validation_test_$$
CONFIG_DIR="$TMP/data"
mkdir -p "$CONFIG_DIR/network-service"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

printf '%s\n' '{"mode":"dhcp"}' > "$CONFIG_DIR/network-service/ethernet.json"
result=$("$BIN" --config-dir "$CONFIG_DIR" --eth eth0 --validate-ethernet-config)
[ "$result" = "dhcp" ] || {
  echo "ethernet_config_validation_test: valid DHCP config was not reported" >&2
  exit 1
}

printf '%s\n' '{"mode":"static","address":"bad"}' > \
  "$CONFIG_DIR/network-service/ethernet.json"
if "$BIN" --config-dir "$CONFIG_DIR" --eth eth0 \
    --validate-ethernet-config >/dev/null 2>&1; then
  echo "ethernet_config_validation_test: invalid static config passed" >&2
  exit 1
fi

echo "ethernet_config_validation_test: PASS"
