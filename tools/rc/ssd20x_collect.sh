#!/bin/sh
set -eu

SCHEMA_VERSION=2
PROBE=${NETWORK_SERVICE_RC_PROBE:-/dnake/bin/network_service_rc_probe}
SOCKET=${NETWORK_SERVICE_SOCKET:-/tmp/smart_hmi_network.sock}
SERVICE_CMD=${NETWORK_SERVICE_SERVICE_CMD:-/etc/init.d/S40network_service}
NETWORK_BIN=${NETWORK_SERVICE_BIN:-/dnake/bin/network_service}
NETWORK_PROC=${NETWORK_SERVICE_PROCESS_NAME:-network_service}
SMARTCONTROL_PROC=${SMARTCONTROL_PROCESS_NAME:-desktop}
ETH_IFACE=${NETWORK_SERVICE_ETH:-eth0}
OUT=
PROVENANCE=
SCAN_SAMPLES=3
RESTART_SAMPLES=2
STEADY_SAMPLES=5
STEADY_INTERVAL_SEC=1
PROBE_TIMEOUT_MS=1500
SCAN_TIMEOUT_MS=10000
SCAN_POLL_MS=100

usage() {
    cat <<'EOF'
Usage: ssd20x_collect.sh --out DIR --provenance FILE [options]

Required:
  --out DIR                 Evidence bundle output directory
  --provenance FILE         Build provenance key=value file

Options:
  --probe PATH              RC probe binary
  --socket PATH             NetworkService AF_UNIX socket
  --service-cmd PATH        init script supporting restart
  --network-bin PATH        deployed NetworkService binary
  --eth IFACE               immutable Ethernet identity interface (default: eth0)
  --scan-samples N          physical Wi-Fi scan cycles (default: 3)
  --restart-samples N       service restart cycles (default: 2)
  --steady-samples N        steady resource samples (default: 5)
  --steady-interval SEC     interval between steady samples (default: 1)

Operator HIL assertions are taken from environment:
  RC_OPERATOR
  RC_UI_SCAN_RESULT=pass|fail|UNRECORDED
  RC_UI_RESTART_RESULT=pass|fail|UNRECORDED
  RC_NOTES
EOF
}

positive_int() {
    case "$1" in
        ''|*[!0-9]*|0) return 1 ;;
        *) return 0 ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out) OUT=$2; shift 2 ;;
        --provenance) PROVENANCE=$2; shift 2 ;;
        --probe) PROBE=$2; shift 2 ;;
        --socket) SOCKET=$2; shift 2 ;;
        --service-cmd) SERVICE_CMD=$2; shift 2 ;;
        --network-bin) NETWORK_BIN=$2; shift 2 ;;
        --eth) ETH_IFACE=$2; shift 2 ;;
        --scan-samples) SCAN_SAMPLES=$2; shift 2 ;;
        --restart-samples) RESTART_SAMPLES=$2; shift 2 ;;
        --steady-samples) STEADY_SAMPLES=$2; shift 2 ;;
        --steady-interval) STEADY_INTERVAL_SEC=$2; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$OUT" ] || { echo "--out is required" >&2; exit 2; }
[ -n "$PROVENANCE" ] || { echo "--provenance is required" >&2; exit 2; }
[ -f "$PROVENANCE" ] || { echo "provenance file not found: $PROVENANCE" >&2; exit 2; }
[ -x "$PROBE" ] || { echo "RC probe is not executable: $PROBE" >&2; exit 2; }
[ -x "$SERVICE_CMD" ] || { echo "service command is not executable: $SERVICE_CMD" >&2; exit 2; }
[ -x "$NETWORK_BIN" ] || { echo "NetworkService binary is not executable: $NETWORK_BIN" >&2; exit 2; }
[ -r "/sys/class/net/$ETH_IFACE/address" ] || {
    echo "Ethernet MAC sysfs identity is unavailable: /sys/class/net/$ETH_IFACE/address" >&2
    exit 2
}
positive_int "$SCAN_SAMPLES" || { echo "invalid --scan-samples" >&2; exit 2; }
positive_int "$RESTART_SAMPLES" || { echo "invalid --restart-samples" >&2; exit 2; }
positive_int "$STEADY_SAMPLES" || { echo "invalid --steady-samples" >&2; exit 2; }

for key in network_service_revision smartcontrol_revision compiler_id toolchain_id sysroot_id build_id; do
    grep -q "^${key}=..*" "$PROVENANCE" || {
        echo "provenance missing required key: $key" >&2
        exit 2
    }
done

sha256_cmd() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
    elif command -v busybox >/dev/null 2>&1; then
        busybox sha256sum "$1"
    else
        echo "sha256sum is required" >&2
        return 1
    fi
}

pid_for() {
    name=$1
    if command -v pidof >/dev/null 2>&1; then
        pidof "$name" 2>/dev/null | awk '{print $1}'
        return 0
    fi
    ps 2>/dev/null | awk -v n="$name" '$0 ~ n && $0 !~ /awk/ {print $1; exit}'
}

proc_value() {
    status=$1
    field=$2
    awk -v f="$field" '$1 == f ":" {print $2; exit}' "$status"
}

resource_row() {
    phase=$1
    name=$2
    pid=$3
    csv=$4
    [ -n "$pid" ] || return 0
    [ -r "/proc/$pid/status" ] || return 0
    now=$($PROBE clock-ms)
    rss=$(proc_value "/proc/$pid/status" VmRSS)
    hwm=$(proc_value "/proc/$pid/status" VmHWM)
    threads=$(proc_value "/proc/$pid/status" Threads)
    fd_count=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l | awk '{print $1}')
    : "${rss:=0}" "${hwm:=0}" "${threads:=0}" "${fd_count:=0}"
    echo "$phase,$now,$name,$pid,$rss,$hwm,$threads,$fd_count" >> "$csv"
}

mac_row() {
    phase=$1
    csv=$2
    mac=$(tr 'A-F' 'a-f' < "/sys/class/net/$ETH_IFACE/address" | tr -d '\r\n ')
    [ -n "$mac" ] || { echo "empty MAC identity for $ETH_IFACE" >&2; return 1; }
    now=$($PROBE clock-ms)
    echo "$phase,$now,$ETH_IFACE,$mac" >> "$csv"
}

single_line() {
    printf '%s' "$1" | tr '\r\n' '  '
}

rm -rf "$OUT"
mkdir -p "$OUT/raw"
cp "$PROVENANCE" "$OUT/provenance.env"

network_pid=$(pid_for "$NETWORK_PROC" || true)
[ -n "$network_pid" ] || { echo "NetworkService process not found: $NETWORK_PROC" >&2; exit 3; }
smartcontrol_pid=$(pid_for "$SMARTCONTROL_PROC" || true)

network_sha=$(sha256_cmd "$NETWORK_BIN" | awk '{print $1}')
probe_sha=$(sha256_cmd "$PROBE" | awk '{print $1}')

cat > "$OUT/manifest.env" <<EOF
schema_version=$SCHEMA_VERSION
collector_version=2
captured_at_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
target_arch=$(uname -m)
target_uname=$(single_line "$(uname -a)")
network_service_socket=$SOCKET
network_service_eth_iface=$ETH_IFACE
network_service_binary=$NETWORK_BIN
network_service_binary_sha256=$network_sha
network_service_process_name=$NETWORK_PROC
network_service_pid=$network_pid
smartcontrol_process_name=$SMARTCONTROL_PROC
smartcontrol_pid=$smartcontrol_pid
rc_probe_binary=$PROBE
rc_probe_sha256=$probe_sha
EOF

{
    sha256_cmd "$NETWORK_BIN"
    sha256_cmd "$PROBE"
    sha256_cmd "$SERVICE_CMD"
} > "$OUT/hashes.sha256"

cat > "$OUT/operator.env" <<EOF
operator=$(single_line "${RC_OPERATOR:-}")
ui_responsive_during_scan=$(single_line "${RC_UI_SCAN_RESULT:-UNRECORDED}")
ui_recovers_after_restart=$(single_line "${RC_UI_RESTART_RESULT:-UNRECORDED}")
notes=$(single_line "${RC_NOTES:-}")
EOF

$PROBE --socket "$SOCKET" --timeout-ms "$PROBE_TIMEOUT_MS" \
    --out "$OUT/raw/ready.json" ready > "$OUT/ready.metrics"
$PROBE --socket "$SOCKET" --timeout-ms "$PROBE_TIMEOUT_MS" \
    --out "$OUT/raw/snapshot.json" snapshot > "$OUT/snapshot.metrics"

echo 'phase,monotonic_ms,process,pid,rss_kb,hwm_kb,threads,fd_count' > "$OUT/resource_samples.csv"
echo 'phase,monotonic_ms,iface,mac' > "$OUT/mac_samples.csv"
resource_row baseline "$NETWORK_PROC" "$network_pid" "$OUT/resource_samples.csv"
resource_row baseline "$SMARTCONTROL_PROC" "$smartcontrol_pid" "$OUT/resource_samples.csv"
mac_row baseline "$OUT/mac_samples.csv"

echo 'sample,scan_id,start_latency_ms,completion_ms,final_state' > "$OUT/scan_samples.csv"
i=1
while [ "$i" -le "$SCAN_SAMPLES" ]; do
    prefix="$OUT/raw/scan_$i"
    row=$($PROBE --socket "$SOCKET" --timeout-ms "$PROBE_TIMEOUT_MS" \
        --poll-ms "$SCAN_POLL_MS" --scan-timeout-ms "$SCAN_TIMEOUT_MS" \
        --out-prefix "$prefix" scan-cycle)
    echo "$i,$row" >> "$OUT/scan_samples.csv"
    network_pid=$(pid_for "$NETWORK_PROC" || true)
    smartcontrol_pid=$(pid_for "$SMARTCONTROL_PROC" || true)
    resource_row "scan_$i" "$NETWORK_PROC" "$network_pid" "$OUT/resource_samples.csv"
    resource_row "scan_$i" "$SMARTCONTROL_PROC" "$smartcontrol_pid" "$OUT/resource_samples.csv"
    mac_row "scan_$i" "$OUT/mac_samples.csv"
    i=$((i + 1))
done

echo 'sample,restart_to_ready_ms,probe_wait_ready_ms' > "$OUT/restart_samples.csv"
i=1
while [ "$i" -le "$RESTART_SAMPLES" ]; do
    start_ms=$($PROBE clock-ms)
    "$SERVICE_CMD" restart
    metrics="$OUT/restart_$i.metrics"
    $PROBE --socket "$SOCKET" --timeout-ms "$PROBE_TIMEOUT_MS" \
        --poll-ms "$SCAN_POLL_MS" --scan-timeout-ms "$SCAN_TIMEOUT_MS" \
        --out "$OUT/raw/restart_$i.ready.json" wait-ready > "$metrics"
    end_ms=$($PROBE clock-ms)
    wait_ms=$(sed -n 's/^ready_latency_ms=//p' "$metrics")
    [ -n "$wait_ms" ] || { echo "restart probe did not emit ready latency" >&2; exit 4; }
    echo "$i,$((end_ms - start_ms)),$wait_ms" >> "$OUT/restart_samples.csv"
    network_pid=$(pid_for "$NETWORK_PROC" || true)
    [ -n "$network_pid" ] || { echo "NetworkService missing after restart" >&2; exit 4; }
    smartcontrol_pid=$(pid_for "$SMARTCONTROL_PROC" || true)
    resource_row "restart_$i" "$NETWORK_PROC" "$network_pid" "$OUT/resource_samples.csv"
    resource_row "restart_$i" "$SMARTCONTROL_PROC" "$smartcontrol_pid" "$OUT/resource_samples.csv"
    mac_row "restart_$i" "$OUT/mac_samples.csv"
    i=$((i + 1))
done

$PROBE --socket "$SOCKET" --timeout-ms "$PROBE_TIMEOUT_MS" \
    --out "$OUT/raw/post_restart_snapshot.json" snapshot > "$OUT/post_restart_snapshot.metrics"
mac_row final "$OUT/mac_samples.csv"

i=1
while [ "$i" -le "$STEADY_SAMPLES" ]; do
    network_pid=$(pid_for "$NETWORK_PROC" || true)
    smartcontrol_pid=$(pid_for "$SMARTCONTROL_PROC" || true)
    resource_row "steady_$i" "$NETWORK_PROC" "$network_pid" "$OUT/resource_samples.csv"
    resource_row "steady_$i" "$SMARTCONTROL_PROC" "$smartcontrol_pid" "$OUT/resource_samples.csv"
    [ "$i" -eq "$STEADY_SAMPLES" ] || sleep "$STEADY_INTERVAL_SEC"
    i=$((i + 1))
done

mac_unique=$(awk -F, 'NR > 1 {print $4}' "$OUT/mac_samples.csv" | sort -u | wc -l | awk '{print $1}')
if [ "$mac_unique" -ne 1 ]; then
    echo "immutable Ethernet MAC policy violated; see $OUT/mac_samples.csv" >&2
    exit 5
fi

cat > "$OUT/README.txt" <<'EOF'
SSD20x RC raw evidence bundle (schema v2).
Run the host validator from the matching NetworkService source tree:
  python3 tools/rc/validate_ssd20x_evidence.py <bundle> --structure-only
Schema v2 enforces immutable Ethernet MAC identity across baseline, physical scans, NetworkService restarts, and final capture.
For final RC proof, also pass a reviewed thresholds JSON file and ensure operator.env records PASS for both UI assertions.
EOF

printf '%s\n' "SSD20x RC evidence captured: $OUT"
printf '%s\n' "IMPORTANT: RC is not proven until host validation passes with frozen thresholds and operator HIL assertions are pass."
