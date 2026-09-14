#!/bin/sh
set -eu

SCRIPT=${1:-packaging/S40network_service}
TMP=${TMPDIR:-/tmp}/network_service_supervisor_test_$$
mkdir -p "$TMP"

FAKE="$TMP/fake_network_service.sh"
COUNT="$TMP/launch_count"
SUP_PIDFILE="$TMP/supervisor.pid"
CHILD_PIDFILE="$TMP/child.pid"
LOCKDIR="$TMP/supervisor.lock"
SOCKET="$TMP/network.sock"
WPA_SEED="$TMP/wpa_supplicant.seed"

cat > "$WPA_SEED" <<'EOF'
update_config=0
persistent_reconnect=1
network={
  ssid="factory-network"
  key_mgmt=NONE
}
EOF

cat > "$FAKE" <<'EOF'
#!/bin/sh
COUNT_FILE=${NETWORK_SERVICE_TEST_COUNT:?}
count=0
if [ -r "$COUNT_FILE" ]; then
  IFS= read -r count < "$COUNT_FILE" || count=0
fi
count=$((count + 1))
printf '%s\n' "$count" > "$COUNT_FILE"

# First launch simulates an unexpected daemon crash. Later launches remain alive
# until the supervisor explicitly stops them.
if [ "$count" -eq 1 ]; then
  exit 42
fi

trap 'exit 0' TERM INT HUP
while :; do
  sleep 1
done
EOF
chmod +x "$FAKE"

run_init() {
  NETWORK_SERVICE_BIN="$FAKE" \
  NETWORK_SERVICE_SOCKET="$SOCKET" \
  NETWORK_SERVICE_CONFIG_DIR="$TMP/config" \
  NETWORK_SERVICE_WPA_SEED="$WPA_SEED" \
  NETWORK_SERVICE_EVENT_DIR="$TMP/wpa" \
  NETWORK_SERVICE_RESTART_DELAY=1 \
  NETWORK_SERVICE_START_CHECK_SECONDS=5 \
  NETWORK_SERVICE_SUPERVISOR_PIDFILE="$SUP_PIDFILE" \
  NETWORK_SERVICE_CHILD_PIDFILE="$CHILD_PIDFILE" \
  NETWORK_SERVICE_SUPERVISOR_LOCKDIR="$LOCKDIR" \
  NETWORK_SERVICE_TEST_COUNT="$COUNT" \
  sh "$SCRIPT" "$1"
}

cleanup() {
  set +e
  run_init stop >/dev/null 2>&1
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM HUP

wait_for_child() {
  previous=${1:-}
  WAITED_CHILD=
  attempt=0
  while [ "$attempt" -lt 10 ]; do
    if [ -r "$CHILD_PIDFILE" ]; then
      IFS= read -r pid < "$CHILD_PIDFILE" || pid=
      if [ -n "$pid" ] && [ "$pid" != "$previous" ] && kill -0 "$pid" 2>/dev/null; then
        WAITED_CHILD=$pid
        return 0
      fi
    fi
    sleep 1
    attempt=$((attempt + 1))
  done
  return 1
}

run_init start >/dev/null

# A factory-flashed device has an empty writable /data partition. The init
# path must materialize both authoritative files before the daemon is allowed
# to run, otherwise Ethernet remains an in-memory default and SAVE_CONFIG has
# no writable Wi-Fi destination.
if [ "$(cat "$TMP/config/network-service/ethernet.json" 2>/dev/null || true)" != \
     '{"mode":"dhcp"}' ]; then
  echo "network_service_supervisor_test: empty data did not bootstrap ethernet.json" >&2
  exit 1
fi
if ! grep -q '^ctrl_interface=' \
    "$TMP/config/network-service/wpa_supplicant.conf" 2>/dev/null ||
   ! grep -q '^update_config=1$' \
    "$TMP/config/network-service/wpa_supplicant.conf" 2>/dev/null; then
  echo "network_service_supervisor_test: empty data did not bootstrap writable Wi-Fi config" >&2
  exit 1
fi
if ! grep -q 'ssid="factory-network"' \
    "$TMP/config/network-service/wpa_supplicant.conf"; then
  echo "network_service_supervisor_test: factory Wi-Fi seed was not imported" >&2
  exit 1
fi

# The first fake daemon exits with 42. A successful start therefore proves the
# supervisor restarted it and published a later live child.
wait_for_child "" || {
  echo "network_service_supervisor_test: no live child after initial crash" >&2
  exit 1
}
child=$WAITED_CHILD

count=0
IFS= read -r count < "$COUNT"
if [ "$count" -lt 2 ]; then
  echo "network_service_supervisor_test: child was not restarted after initial crash" >&2
  exit 1
fi

run_init status >/dev/null

# Kill the stable child and require a distinct replacement PID.
kill -KILL "$child"
wait_for_child "$child" || {
  echo "network_service_supervisor_test: child was not restarted after SIGKILL" >&2
  exit 1
}
replacement=$WAITED_CHILD

if [ "$replacement" = "$child" ]; then
  echo "network_service_supervisor_test: replacement PID did not change" >&2
  exit 1
fi

count=0
IFS= read -r count < "$COUNT"
if [ "$count" -lt 3 ]; then
  echo "network_service_supervisor_test: expected at least three launches" >&2
  exit 1
fi

run_init stop >/dev/null
sleep 1

if [ -e "$SUP_PIDFILE" ] || [ -e "$CHILD_PIDFILE" ] || [ -d "$LOCKDIR" ]; then
  echo "network_service_supervisor_test: stop left ownership artifacts" >&2
  exit 1
fi

if kill -0 "$replacement" 2>/dev/null; then
  echo "network_service_supervisor_test: stop left child process running" >&2
  exit 1
fi

trap - EXIT INT TERM HUP
rm -rf "$TMP"
echo "network_service_supervisor_test: PASS"
