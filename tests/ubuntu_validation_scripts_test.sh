#!/bin/sh
set -eu

ROOT=${1:-.}
PRE="$ROOT/tools/validation/ubuntu_preflight.sh"
COLLECT="$ROOT/tools/validation/ubuntu_collect_baseline.sh"
BASELINE="$ROOT/tools/validation/ubuntu_baseline.sh"

sh -n "$PRE"
sh -n "$COLLECT"
sh -n "$BASELINE"

# The preflight must fail closed before any mutation when the three role
# interfaces are not distinct. Host CI does not need udhcpc or a real lab NIC for
# this check; we assert the diagnostic contract, not a successful physical run.
TMP=${TMPDIR:-/tmp}/network_service_ubuntu_validation_test_$$
mkdir -p "$TMP"
REPORT="$TMP/preflight.log"
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

set +e
NS_VAL_MGMT_IFACE=lo \
NS_VAL_ETH_IFACE=lo \
NS_VAL_WIFI_IFACE=lo \
NS_VAL_ALLOW_NONROOT=1 \
NS_VAL_REQUIRE_WIFI_CTRL=0 \
NS_VAL_BINARY=/bin/true \
NS_VAL_PREFLIGHT_REPORT="$REPORT" \
sh "$PRE" >/dev/null 2>&1
rc=$?
set -e

if [ "$rc" -eq 0 ]; then
  echo "ubuntu_validation_scripts_test: duplicate role interfaces unexpectedly passed" >&2
  exit 1
fi

if ! grep -F 'FAIL|INTERFACES_NOT_DISTINCT|' "$REPORT" >/dev/null 2>&1; then
  echo "ubuntu_validation_scripts_test: missing fail-closed interface diagnostic" >&2
  exit 1
fi

rm -rf "$TMP"
trap - EXIT INT TERM HUP
echo "ubuntu_validation_scripts_test: PASS"
