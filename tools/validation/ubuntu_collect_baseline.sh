#!/bin/sh
set -eu

PID=${NS_VAL_PID:-}
EVIDENCE_DIR=${NS_VAL_EVIDENCE_DIR:-}
ETH_IFACE=${NS_VAL_ETH_IFACE:-}
WIFI_IFACE=${NS_VAL_WIFI_IFACE:-}
SOCKET_PATH=${NS_VAL_SOCKET:-/tmp/ns_val_network.sock}
NETWORKCTL=${NS_VAL_NETWORKCTL:-./tools/networkctl.py}
DURATION=${NS_VAL_DURATION_SECONDS:-60}
INTERVAL=${NS_VAL_INTERVAL_SECONDS:-1}

if [ -z "$PID" ] || [ -z "$EVIDENCE_DIR" ]; then
  echo "ubuntu_collect_baseline: NS_VAL_PID and NS_VAL_EVIDENCE_DIR are required" >&2
  exit 2
fi

case "$PID" in
  *[!0-9]*|'') echo "ubuntu_collect_baseline: invalid PID: $PID" >&2; exit 2 ;;
esac

mkdir -p "$EVIDENCE_DIR"
RESOURCE="$EVIDENCE_DIR/resource.csv"
SNAPSHOT="$EVIDENCE_DIR/snapshot.log"
PROCESS="$EVIDENCE_DIR/process.log"

printf '%s\n' 'epoch_s,rss_kb,vmsize_kb,vmdata_kb,vmstk_kb,threads_status,fd_count,task_count,utime_ticks,stime_ticks,eth_udhcpc_count,wifi_udhcpc_count' > "$RESOURCE"
: > "$SNAPSHOT"
: > "$PROCESS"

status_value() {
  key=$1
  awk -v k="$key" '$1 == k ":" { print $2; exit }' "/proc/$PID/status" 2>/dev/null || true
}

count_entries() {
  dir=$1
  count=0
  for item in "$dir"/*; do
    [ -e "$item" ] || continue
    count=$((count + 1))
  done
  printf '%s\n' "$count"
}

count_udhcpc_for_iface() {
  iface=$1
  [ -n "$iface" ] || { printf '0\n'; return; }
  count=0
  for proc in /proc/[0-9]*; do
    [ -r "$proc/cmdline" ] || continue
    cmdline=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null || true)
    case " $cmdline " in
      *" udhcpc "*" -i $iface "*) count=$((count + 1)) ;;
    esac
  done
  printf '%s\n' "$count"
}

sample=0
start=$(date +%s)
end=$((start + DURATION))

while :; do
  now=$(date +%s)
  [ "$now" -le "$end" ] || break

  if [ ! -r "/proc/$PID/status" ]; then
    printf '%s|PROCESS_EXITED|pid=%s\n' "$now" "$PID" >> "$PROCESS"
    echo "ubuntu_collect_baseline: network_service exited during collection" >&2
    exit 3
  fi

  rss=$(status_value VmRSS); rss=${rss:-0}
  vmsize=$(status_value VmSize); vmsize=${vmsize:-0}
  vmdata=$(status_value VmData); vmdata=${vmdata:-0}
  vmstk=$(status_value VmStk); vmstk=${vmstk:-0}
  threads=$(status_value Threads); threads=${threads:-0}
  fd_count=$(count_entries "/proc/$PID/fd")
  task_count=$(count_entries "/proc/$PID/task")

  stat=$(cat "/proc/$PID/stat")
  utime=$(printf '%s\n' "$stat" | awk '{print $14}')
  stime=$(printf '%s\n' "$stat" | awk '{print $15}')

  eth_udhcpc=$(count_udhcpc_for_iface "$ETH_IFACE")
  wifi_udhcpc=$(count_udhcpc_for_iface "$WIFI_IFACE")

  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$now" "$rss" "$vmsize" "$vmdata" "$vmstk" "$threads" \
    "$fd_count" "$task_count" "$utime" "$stime" \
    "$eth_udhcpc" "$wifi_udhcpc" >> "$RESOURCE"

  printf '%s|pid=%s|fd=%s|tasks=%s|eth_udhcpc=%s|wifi_udhcpc=%s\n' \
    "$now" "$PID" "$fd_count" "$task_count" "$eth_udhcpc" "$wifi_udhcpc" >> "$PROCESS"

  if [ -f "$NETWORKCTL" ]; then
    snapshot=$(python3 "$NETWORKCTL" --socket "$SOCKET_PATH" network.snapshot 2>&1 || true)
    snapshot=$(printf '%s' "$snapshot" | tr '\n' ' ')
    printf '%s|%s\n' "$now" "$snapshot" >> "$SNAPSHOT"
  fi

  sample=$((sample + 1))
  [ "$now" -eq "$end" ] && break
  sleep "$INTERVAL"
done

printf '%s|COLLECTION_COMPLETE|samples=%s|duration_seconds=%s\n' \
  "$(date +%s)" "$sample" "$DURATION" >> "$PROCESS"

echo "ubuntu_collect_baseline: PASS samples=$sample"
