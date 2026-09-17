#!/usr/bin/env bash
# Two-node ESC DB sync smoke test (node-a / node-b on 127.0.0.1).
#
# Requires a Debug MediaServer (Debug builds read ./api-config.json when the
# configuration hook succeeds) built with ENABLE_SYNC_CHANGES=ON.
#
# Usage:
#   run_cluster.sh [work_dir]            # full scenario: phase1 → phase2 → restart → phase3
#   run_cluster.sh [work_dir] stop       # stop nodes and mock API started from work_dir
#
# Env: MEDIASERVER_DIR (default: <repo>/release/linux/Debug), WAIT_SEC (default: 60)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${1:-/tmp/s3mk_sync_cluster}"
BIN_DIR="${MEDIASERVER_DIR:-$REPO/release/linux/Debug}"
WAIT_SEC="${WAIT_SEC:-60}"
NODES="node-a node-b"

# Gracefully stop the MediaServer nodes (SIGINT, then SIGKILL after 30s).
stop_nodes() {
    for pidfile in "$WORK"/node-*/server.pid; do
        [ -f "$pidfile" ] && kill -INT "$(cat "$pidfile")" 2>/dev/null
    done
    # MediaServer sleeps ~10s during shutdown
    for _ in $(seq 1 30); do
        alive=0
        for pidfile in "$WORK"/node-*/server.pid; do
            [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null && alive=1
        done
        [ "$alive" = 0 ] && break
        sleep 1
    done
    for pidfile in "$WORK"/node-*/server.pid; do
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "WARN: $(dirname "$pidfile") did not stop on SIGINT, killing"
            kill -9 "$(cat "$pidfile")" 2>/dev/null
        fi
        rm -f "$pidfile"
    done
}

stop_all() {
    stop_nodes
    if [ -f "$WORK/mock_api.pid" ]; then
        kill "$(cat "$WORK/mock_api.pid")" 2>/dev/null
        rm -f "$WORK/mock_api.pid"
    fi
}

start_node() {
    local n="$1" tag="$2"
    local d="$WORK/$n"
    [ -f "$d/stdout.log" ] && mv "$d/stdout.log" "$d/stdout.$tag.log"
    # cd first so that $! is the MediaServer pid (not a background subshell, which ignores SIGINT).
    cd "$d" || return 1
    LD_LIBRARY_PATH="$BIN_DIR" nohup "$BIN_DIR/MediaServer" -c "$d/config.ini" \
        -s "$BIN_DIR/default.pem" -l 0 -t 4 --affinity 0 --log-dir "$d/log/" > "$d/stdout.log" 2>&1 &
    echo $! > "$d/server.pid"
    cd - > /dev/null
}

report() {
    local label="$1"
    echo "---- $label"
    for n in $NODES; do
        local f="$WORK/$n/stdout.log"
        grep -q "## signal:" "$f" && echo "CRASH: $n (see $f)"
        printf "%s locked=%s failed_apply=%s relay_recv=%s relay_fail=%s stale_skip=%s\n" "$n" \
            "$(grep -c 'database is locked' "$f")" "$(grep -c 'Failed to apply' "$f")" \
            "$(grep -c 'Relay received' "$f")" "$(grep -c 'Relay pull from .* failed' "$f")" \
            "$(grep -c 'skip stale' "$f")"
    done
    python3 "$HERE/compare_db.py" "$WORK"
}

if [ "${2:-}" = "stop" ]; then
    stop_all
    exit 0
fi

[ -x "$BIN_DIR/MediaServer" ] || { echo "MediaServer not found in $BIN_DIR"; exit 1; }
stop_all
mkdir -p "$WORK"
python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" || exit 1

nohup python3 "$HERE/mock_api.py" 18090 > "$WORK/mock_api.log" 2>&1 &
echo $! > "$WORK/mock_api.pid"
for n in $NODES; do start_node "$n" init; done
trap stop_all EXIT

echo "phase1: camera on node-a"; sleep "$WAIT_SEC"; report phase1

echo "phase2: move camera node-a -> node-b"
python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" phase2
sleep "$WAIT_SEC"; report phase2

echo "restart both nodes (persisted bootstrap state)"
stop_nodes
for n in $NODES; do start_node "$n" before_restart; done
sleep "$WAIT_SEC"; report restart

echo "phase3: config change on node-b"
python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" phase3
sleep "$WAIT_SEC"; report phase3
