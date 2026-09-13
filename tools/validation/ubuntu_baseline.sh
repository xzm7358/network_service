#!/bin/sh
set -eu

MGMT_IFACE=${NS_VAL_MGMT_IFACE:-}
ETH_IFACE=${NS_VAL_ETH_IFACE:-}
WIFI_IFACE=${NS_VAL_WIFI_IFACE:-}
BIN=${NS_VAL_BINARY:-./app/smartcontrol/dnake/bin/network_service}
NETWORKCTL=${NS_VAL_NETWORKCTL:-./tools/networkctl.py}
SOCKET_PATH=${NS_VAL_SOCKET:-/tmp/ns_val_network.sock}
WPA_CTRL_DIR=${NS_VAL_WPA_CTRL_DIR:-/var/run/wpa_supplicant}
EVIDENCE_ROOT=${NS_VAL_EVIDENCE_ROOT:-evidence/ubuntu}
DURATION=${NS_VAL_DURATION_SECONDS:-60}
INTERVAL=${NS_VAL_INTERVAL_SECONDS:-1}
KEEP_DAEMON=${NS_VAL_KEEP_DAEMON:-0}
START_TIMEOUT=${NS_VAL_START_TIMEOUT_SECONDS:-10}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFLIGHT="$SCRIPT_DIR/ubuntu_preflight.sh"
COLLECTOR="$SCRIPT_DIR/ubuntu_collect_baseline.sh"

commit=unknown
if command -v git >/dev/null 2>&1; then
  commit=$(git rev-parse --verify HEAD 2>/dev/null || printf 'unknown')
fi
short_commit=$(printf '%s' "$commit" | cut -c1-12)
stamp=$(date -u +%Y%m%dT%H%M%SZ)
EVIDENCE_DIR=${NS_VAL_EVIDENCE_DIR:-$EVIDENCE_ROOT/${stamp}_${short_commit}}
CONFIG_DIR=${NS_VAL_CONFIG_DIR:-$EVIDENCE_DIR/runtime-config}
LOG="$EVIDENCE_DIR/network_service.log"

mkdir -p "$EVIDENCE_DIR" "$CONFIG_DIR"

export NS_VAL_MGMT_IFACE="$MGMT_IFACE"
export NS_VAL_ETH_IFACE="$ETH_IFACE"
export NS_VAL_WIFI_IFACE="$WIFI_IFACE"
export NS_VAL_BINARY="$BIN"
export NS_VAL_SOCKET="$SOCKET_PATH"
export NS_VAL_WPA_CTRL_DIR="$WPA_CTRL_DIR"
export NS_VAL_EVIDENCE_DIR="$EVIDENCE_DIR"
export NS_VAL_PREFLIGHT_REPORT="$EVIDENCE_DIR/preflight.log"

sh "$PREFLIGHT"

capture_network_state() {
  phase=$1
  {
    echo "=== $phase $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    echo "--- ip -4 addr ---"
    ip -4 addr show dev "$MGMT_IFACE" 2>&1 || true
    ip -4 addr show dev "$ETH_IFACE" 2>&1 || true
    ip -4 addr show dev "$WIFI_IFACE" 2>&1 || true
    echo "--- ip route ---"
    ip route show 2>&1 || true
  } >> "$EVIDENCE_DIR/network-state.log"

  {
    echo "=== $phase $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    cat /etc/resolv.conf 2>&1 || true
  } >> "$EVIDENCE_DIR/resolv.conf.log"
}

capture_processes() {
  phase=$1
  {
    echo "=== $phase $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    for proc in /proc/[0-9]*; do
      [ -r "$proc/cmdline" ] || continue
      cmdline=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null || true)
      case "$cmdline" in
        *network_service*|*udhcpc*|*wpa_supplicant*)
          printf '%s|%s\n' "${proc##*/}" "$cmdline"
          ;;
      esac
    done
  } >> "$EVIDENCE_DIR/processes.log"
}

cat > "$EVIDENCE_DIR/manifest.env" <<EOF
schema=network-service-ubuntu-baseline-v1
started_utc=$stamp
git_commit=$commit
hostname=$(hostname 2>/dev/null || printf unknown)
uname=$(uname -a 2>/dev/null | tr '\n' ' ')
mgmt_iface=$MGMT_IFACE
eth_iface=$ETH_IFACE
wifi_iface=$WIFI_IFACE
socket=$SOCKET_PATH
wpa_ctrl_dir=$WPA_CTRL_DIR
duration_seconds=$DURATION
interval_seconds=$INTERVAL
binary=$BIN
EOF

capture_network_state before
capture_processes before

DAEMON_PID=
cleanup() {
  rc=$?
  trap - EXIT INT TERM HUP
  if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    if [ "$KEEP_DAEMON" = "1" ]; then
      echo "ubuntu_baseline: leaving network_service running pid=$DAEMON_PID"
    else
      kill -TERM "$DAEMON_PID" 2>/dev/null || true
      waited=0
      while kill -0 "$DAEMON_PID" 2>/dev/null && [ "$waited" -lt 5 ]; do
        sleep 1
        waited=$((waited + 1))
      done
      if kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
      fi
      wait "$DAEMON_PID" 2>/dev/null || true
    fi
  fi
  [ "$KEEP_DAEMON" = "1" ] || rm -f "$SOCKET_PATH"
  capture_network_state after
  capture_processes after
  exit "$rc"
}
trap cleanup EXIT INT TERM HUP

"$BIN" \
  --socket "$SOCKET_PATH" \
  --eth "$ETH_IFACE" \
  --wifi "$WIFI_IFACE" \
  --config-dir "$CONFIG_DIR" \
  --event-dir "$WPA_CTRL_DIR" \
  > "$LOG" 2>&1 &
DAEMON_PID=$!
echo "$DAEMON_PID" > "$EVIDENCE_DIR/network_service.pid"

ready=0
elapsed=0
while [ "$elapsed" -lt "$START_TIMEOUT" ]; do
  if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
    wait "$DAEMON_PID" 2>/dev/null || true
    echo "ubuntu_baseline: network_service exited during startup; see $LOG" >&2
    exit 3
  fi

  if [ -S "$SOCKET_PATH" ]; then
    if python3 "$NETWORKCTL" --socket "$SOCKET_PATH" network.ping \
        > "$EVIDENCE_DIR/ping.json" 2> "$EVIDENCE_DIR/ping.err"; then
      ready=1
      break
    fi
  fi
  sleep 1
  elapsed=$((elapsed + 1))
done

if [ "$ready" -ne 1 ]; then
  echo "ubuntu_baseline: NetworkService did not become IPC-ready" >&2
  exit 4
fi

# Allow asynchronous WPA STATUS/Netlink initialization to settle before resource
# drift is measured. This keeps startup allocations out of the idle baseline.
sleep 2

export NS_VAL_PID="$DAEMON_PID"
export NS_VAL_NETWORKCTL="$NETWORKCTL"
export NS_VAL_DURATION_SECONDS="$DURATION"
export NS_VAL_INTERVAL_SECONDS="$INTERVAL"
sh "$COLLECTOR"

python3 "$NETWORKCTL" --socket "$SOCKET_PATH" network.snapshot \
  > "$EVIDENCE_DIR/final-snapshot.json" 2> "$EVIDENCE_DIR/final-snapshot.err" || true

RESOURCE="$EVIDENCE_DIR/resource.csv"
SUMMARY="$EVIDENCE_DIR/result.env"

awk -F, '
NR == 2 {
  samples=1; rss_start=$2; rss_end=$2; rss_peak=$2;
  fd_start=$7; fd_end=$7; fd_peak=$7;
  th_start=$8; th_end=$8; th_peak=$8;
  dup=($11 > 1 || $12 > 1) ? 1 : 0;
  next
}
NR > 2 {
  samples++;
  rss_end=$2; if ($2 > rss_peak) rss_peak=$2;
  fd_end=$7; if ($7 > fd_peak) fd_peak=$7;
  th_end=$8; if ($8 > th_peak) th_peak=$8;
  if ($11 > 1 || $12 > 1) dup=1;
}
END {
  if (samples == 0) exit 2;
  printf "samples=%d\n", samples;
  printf "rss_start_kb=%d\n", rss_start;
  printf "rss_end_kb=%d\n", rss_end;
  printf "rss_peak_kb=%d\n", rss_peak;
  printf "rss_drift_kb=%d\n", rss_end-rss_start;
  printf "fd_start=%d\n", fd_start;
  printf "fd_end=%d\n", fd_end;
  printf "fd_peak=%d\n", fd_peak;
  printf "fd_drift=%d\n", fd_end-fd_start;
  printf "threads_start=%d\n", th_start;
  printf "threads_end=%d\n", th_end;
  printf "threads_peak=%d\n", th_peak;
  printf "thread_drift=%d\n", th_end-th_start;
  printf "duplicate_udhcpc_seen=%d\n", dup;
}
' "$RESOURCE" > "$SUMMARY"

fd_drift=$(awk -F= '$1=="fd_drift" {print $2}' "$SUMMARY")
thread_drift=$(awk -F= '$1=="thread_drift" {print $2}' "$SUMMARY")
duplicate=$(awk -F= '$1=="duplicate_udhcpc_seen" {print $2}' "$SUMMARY")

result=PASS
reason=baseline_stable
if [ "$duplicate" -ne 0 ]; then
  result=FAIL
  reason=duplicate_udhcpc
elif [ "$fd_drift" -ne 0 ]; then
  result=FAIL
  reason=fd_drift
elif [ "$thread_drift" -ne 0 ]; then
  result=FAIL
  reason=thread_drift
fi

{
  printf 'result=%s\n' "$result"
  printf 'reason=%s\n' "$reason"
  printf 'daemon_pid=%s\n' "$DAEMON_PID"
  printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >> "$SUMMARY"

capture_network_state measured
capture_processes measured

if [ "$result" != "PASS" ]; then
  echo "ubuntu_baseline: FAIL reason=$reason evidence=$EVIDENCE_DIR" >&2
  exit 5
fi

echo "ubuntu_baseline: PASS evidence=$EVIDENCE_DIR"
