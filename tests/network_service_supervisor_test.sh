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
  attempt=0
  while [ "$attempt" -lt 10 ]; do
    if [ -r "$CHILD_PIDFILE" ]; then
      IFS= read -r pid < "$CHILD_PIDFILE" || pid=
      if [ -n "$pid" ] && [ "$pid" != "$previous" ] && kill -0 "$pid" 2>/dev/null; then
        printf '%s\n' "$pid"
        return 0
      fi
    fi
    sleep 1
    attempt=$((attempt + 1))
  done
  return 1
}

run_init start >/dev/null

# The first fake daemon exits with 42. A successful start therefore proves the
# supervisor restarted it and published a later live child.
child=$(wait_for_child "") || {
  echo "network_service_supervisor_test: no live child after initial crash" >&2
  exit 1
}

count=0
IFS= read -r count < "$COUNT"
if [ "$count" -lt 2 ]; then
  echo "network_service_supervisor_test: child was not restarted after initial crash" >&2
  exit 1
fi

run_init status >/dev/null

# Kill the stable child and require a distinct replacement PID.
kill -KILL "$child"
replacement=$(wait_for_child "$child") || {
  echo "network_service_supervisor_test: child was not restarted after SIGKILL" >&2
  exit 1
}

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
