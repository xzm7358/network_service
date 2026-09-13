#!/bin/sh
set -eu

MGMT_IFACE=${NS_VAL_MGMT_IFACE:-}
ETH_IFACE=${NS_VAL_ETH_IFACE:-}
WIFI_IFACE=${NS_VAL_WIFI_IFACE:-}
BIN=${NS_VAL_BINARY:-./app/smartcontrol/dnake/bin/network_service}
SOCKET_PATH=${NS_VAL_SOCKET:-/tmp/ns_val_network.sock}
WPA_CTRL_DIR=${NS_VAL_WPA_CTRL_DIR:-/var/run/wpa_supplicant}
REPORT=${NS_VAL_PREFLIGHT_REPORT:-}
ALLOW_NONROOT=${NS_VAL_ALLOW_NONROOT:-0}
REQUIRE_WIFI_CTRL=${NS_VAL_REQUIRE_WIFI_CTRL:-1}

failures=0
warnings=0

emit() {
  level=$1
  code=$2
  shift 2
  message=$*
  printf '%s|%s|%s\n' "$level" "$code" "$message"
  if [ -n "$REPORT" ]; then
    printf '%s|%s|%s\n' "$level" "$code" "$message" >> "$REPORT"
  fi
}

pass() { emit PASS "$1" "$2"; }
warn() { warnings=$((warnings + 1)); emit WARN "$1" "$2"; }
fail() { failures=$((failures + 1)); emit FAIL "$1" "$2"; }

require_value() {
  name=$1
  value=$2
  if [ -z "$value" ]; then
    fail "${name}_REQUIRED" "$name must be set explicitly"
    return 1
  fi
  return 0
}

command_required() {
  name=$1
  if command -v "$name" >/dev/null 2>&1; then
    pass "COMMAND_${name}" "found $(command -v "$name")"
  else
    fail "COMMAND_${name}_MISSING" "$name is required for physical validation"
  fi
}

iface_exists() {
  iface=$1
  [ -n "$iface" ] && [ -d "/sys/class/net/$iface" ]
}

nm_managed() {
  iface=$1
  command -v nmcli >/dev/null 2>&1 || return 1
  value=$(nmcli -t -f GENERAL.MANAGED device show "$iface" 2>/dev/null | head -n 1 || true)
  value=${value#*:}
  case "$value" in
    yes|true|1) return 0 ;;
    *) return 1 ;;
  esac
}

if [ -n "$REPORT" ]; then
  report_dir=${REPORT%/*}
  if [ "$report_dir" != "$REPORT" ]; then
    mkdir -p "$report_dir"
  fi
  : > "$REPORT"
fi

if [ "$(uname -s 2>/dev/null || true)" = "Linux" ]; then
  pass OS_LINUX "Linux host detected"
else
  fail OS_NOT_LINUX "Ubuntu physical validation requires Linux"
fi

if [ "$ALLOW_NONROOT" = "1" ] || [ "$(id -u)" -eq 0 ]; then
  pass PRIVILEGE "root privilege available or explicitly waived for dry validation"
else
  fail ROOT_REQUIRED "run with root privileges (for example sudo -E); preflight itself performs no network mutation"
fi

require_value NS_VAL_MGMT_IFACE "$MGMT_IFACE" || true
require_value NS_VAL_ETH_IFACE "$ETH_IFACE" || true
require_value NS_VAL_WIFI_IFACE "$WIFI_IFACE" || true

if [ -n "$MGMT_IFACE" ] && [ -n "$ETH_IFACE" ] && [ -n "$WIFI_IFACE" ]; then
  if [ "$MGMT_IFACE" = "$ETH_IFACE" ] || [ "$MGMT_IFACE" = "$WIFI_IFACE" ] || [ "$ETH_IFACE" = "$WIFI_IFACE" ]; then
    fail INTERFACES_NOT_DISTINCT "management, test Ethernet, and test Wi-Fi interfaces must be three different devices"
  else
    pass INTERFACES_DISTINCT "management=$MGMT_IFACE ethernet=$ETH_IFACE wifi=$WIFI_IFACE"
  fi
fi

for iface in "$MGMT_IFACE" "$ETH_IFACE" "$WIFI_IFACE"; do
  [ -n "$iface" ] || continue
  if iface_exists "$iface"; then
    pass "IFACE_${iface}" "interface exists"
  else
    fail "IFACE_${iface}_MISSING" "interface does not exist under /sys/class/net"
  fi
done

for cmd in ip ifconfig route udhcpc python3; do
  command_required "$cmd"
done

if [ -x "$BIN" ]; then
  pass BINARY_EXECUTABLE "network_service binary is executable: $BIN"
elif [ -f "$BIN" ]; then
  fail BINARY_NOT_EXECUTABLE "network_service exists but is not executable: $BIN"
else
  fail BINARY_MISSING "network_service binary not found: $BIN"
fi

if command -v ip >/dev/null 2>&1 && [ -n "$MGMT_IFACE" ]; then
  default_routes=$(ip route show default 2>/dev/null || true)
  if printf '%s\n' "$default_routes" | grep -F " dev $MGMT_IFACE" >/dev/null 2>&1; then
    pass MGMT_DEFAULT_ROUTE "at least one default route uses management interface $MGMT_IFACE"
  else
    fail MGMT_DEFAULT_ROUTE_MISSING "no default route uses management interface $MGMT_IFACE"
  fi

  for iface in "$ETH_IFACE" "$WIFI_IFACE"; do
    [ -n "$iface" ] || continue
    if printf '%s\n' "$default_routes" | grep -F " dev $iface" >/dev/null 2>&1; then
      fail "TEST_IFACE_${iface}_DEFAULT_ROUTE" "test interface already owns a default route; isolate it before validation"
    else
      pass "TEST_IFACE_${iface}_NO_DEFAULT_ROUTE" "test interface is not carrying the host default route"
    fi
  done
fi

if command -v ip >/dev/null 2>&1 && [ -n "${SSH_CONNECTION:-}" ] && [ -n "$MGMT_IFACE" ]; then
  ssh_client=${SSH_CONNECTION%% *}
  ssh_route=$(ip route get "$ssh_client" 2>/dev/null | head -n 1 || true)
  if printf '%s\n' "$ssh_route" | grep -F " dev $MGMT_IFACE" >/dev/null 2>&1; then
    pass SSH_USES_MGMT "SSH client route uses $MGMT_IFACE"
  else
    fail SSH_NOT_ON_MGMT "current SSH path is not proven to use $MGMT_IFACE: $ssh_route"
  fi
else
  warn SSH_ROUTE_UNVERIFIED "SSH_CONNECTION unavailable; management-path safety relies on the default-route checks"
fi

if command -v nmcli >/dev/null 2>&1; then
  pass NETWORKMANAGER_DETECTED "nmcli is present; checking test-interface ownership"
  for iface in "$ETH_IFACE" "$WIFI_IFACE"; do
    [ -n "$iface" ] || continue
    if nm_managed "$iface"; then
      fail "NETWORKMANAGER_OWNS_${iface}" "NetworkManager manages test interface $iface; mark it unmanaged before running NetworkService"
    else
      pass "NETWORKMANAGER_NOT_OWNER_${iface}" "NetworkManager does not report ownership of $iface"
    fi
  done
else
  warn NETWORKMANAGER_NOT_DETECTED "nmcli not found; verify no other network manager owns the test interfaces"
fi

if command -v networkctl >/dev/null 2>&1; then
  for iface in "$ETH_IFACE" "$WIFI_IFACE"; do
    [ -n "$iface" ] || continue
    output=$(networkctl status "$iface" --no-pager 2>/dev/null || true)
    if printf '%s\n' "$output" | grep -E 'State:.*(routable|configured)|Setup State: configured' >/dev/null 2>&1; then
      warn "NETWORKD_POSSIBLE_OWNER_${iface}" "systemd-networkd may have configured $iface; inspect ownership before destructive tests"
    fi
  done
fi

if [ "$REQUIRE_WIFI_CTRL" = "1" ] && [ -n "$WIFI_IFACE" ]; then
  ctrl_path="$WPA_CTRL_DIR/$WIFI_IFACE"
  if [ -S "$ctrl_path" ]; then
    pass WPA_CTRL_SOCKET "wpa_supplicant ctrl socket exists: $ctrl_path"
  else
    fail WPA_CTRL_SOCKET_MISSING "expected wpa_supplicant ctrl socket is missing: $ctrl_path"
  fi
fi

if [ -e "$SOCKET_PATH" ]; then
  fail VALIDATION_SOCKET_BUSY "validation socket already exists: $SOCKET_PATH"
else
  pass VALIDATION_SOCKET_FREE "validation socket is free: $SOCKET_PATH"
fi

if [ "$failures" -eq 0 ]; then
  emit RESULT PREFLIGHT_PASS "failures=0 warnings=$warnings"
  exit 0
fi

emit RESULT PREFLIGHT_FAIL "failures=$failures warnings=$warnings"
exit 1
