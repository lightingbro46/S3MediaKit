#!/usr/bin/env bash
# Two-node ESC DB sync smoke test (node-a / node-b on 127.0.0.1).
#
# Requires a Debug MediaServer (Debug builds read ./api-config.json when the
# configuration hook succeeds) built with ENABLE_SYNC_CHANGES=ON.
#
# Usage:
#   run_cluster.sh [work_dir]            # full scenario: phase1 → phase2 → restart → phase3
#   run_cluster.sh [work_dir] stop       # stop nodes and mock API started from work_dir
#   run_cluster.sh [work_dir] workerapis # one node: check APIs that run on the API worker pool
#   run_cluster.sh [work_dir] failover   # recording camera: failover to node-b while node-a is
#                                        # offline, move back, cross-node timeline, retention
#
# Env: MEDIASERVER_DIR (default: <repo>/release/linux/Debug), WAIT_SEC (default: 60)
#      failover: FO_WAIT_SEC (default: 90), FO_KEEP_HOURS (default: 1),
#                FO_RETENTION_WAIT=1 to also wait for the retention expiry (hours)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${1:-/tmp/s3mk_sync_cluster}"
BIN_DIR="${MEDIASERVER_DIR:-$REPO/release/linux/Debug}"
WAIT_SEC="${WAIT_SEC:-60}"
NODES="node-a node-b"

# Gracefully stop the MediaServer nodes (SIGINT, then SIGKILL after 30s).
# Optional arguments name the nodes to stop (default: all).
stop_nodes() {
    local pidfiles=""
    if [ $# -gt 0 ]; then
        for n in "$@"; do pidfiles="$pidfiles $WORK/$n/server.pid"; done
    else
        pidfiles="$(echo "$WORK"/node-*/server.pid)"
    fi
    for pidfile in $pidfiles; do
        [ -f "$pidfile" ] && kill -INT "$(cat "$pidfile")" 2>/dev/null
    done
    # MediaServer sleeps ~10s during shutdown
    for _ in $(seq 1 30); do
        alive=0
        for pidfile in $pidfiles; do
            [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null && alive=1
        done
        [ "$alive" = 0 ] && break
        sleep 1
    done
    for pidfile in $pidfiles; do
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "WARN: $(dirname "$pidfile") did not stop on SIGINT, killing"
            kill -9 "$(cat "$pidfile")" 2>/dev/null
        fi
        rm -f "$pidfile"
    done
}

stop_all() {
    stop_nodes
    for p in mock_api rtsp_source; do
        if [ -f "$WORK/$p.pid" ]; then
            kill "$(cat "$WORK/$p.pid")" 2>/dev/null
            rm -f "$WORK/$p.pid"
        fi
    done
}

wait_healthy() {
    local port="$1"
    for _ in $(seq 1 60); do
        curl -s -m 2 -o /dev/null "http://127.0.0.1:$port/media/mserver/healthcheck" && return 0
        sleep 1
    done
    echo "WARN: node on port $port is not healthy after 60s"
    return 1
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
# failover: production default of cluster.failoverActiveDelaySec (phase4 needs a short one).
[ "${2:-}" = "failover" ] && export FAILOVER_ACTIVE_DELAY_SEC=1800
python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" || exit 1

nohup python3 "$HERE/mock_api.py" 18090 > "$WORK/mock_api.log" 2>&1 &
echo $! > "$WORK/mock_api.pid"
trap stop_all EXIT

if [ "${2:-}" = "workerapis" ]; then
    start_node node-a init
    for _ in $(seq 1 60); do
        curl -s -m 2 -o /dev/null http://127.0.0.1:18081/media/mserver/healthcheck && break
        sleep 1
    done
    # The /media/api/* family is behind CHECK_SECRET(); hand the node's secret to the checker.
    # MediaServer dumps config.ini with CRLF line endings, hence the tr.
    API_SECRET="$(sed -n 's/^secret=//p' "$WORK/node-a/config.ini" | head -1 | tr -d '\r')" \
        python3 "$HERE/check_worker_apis.py" http://127.0.0.1:18081 node-a
    exit $?
fi

if [ "${2:-}" = "failover" ]; then
    FO_WAIT_SEC="${FO_WAIT_SEC:-90}"
    command -v ffmpeg > /dev/null || { echo "ffmpeg is required for the failover scenario"; exit 1; }
    # 640x360 H.264, GOP 2s: short segments are enough, the checks decode a few seconds.
    ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=25:duration=10" \
        -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 50 -keyint_min 50 -sc_threshold 0 \
        -b:v 400k -bsf:v h264_mp4toannexb -f h264 "$WORK/fo_source.h264" -y || exit 1
    nohup python3 "$REPO/tests/integration/stress_vms/rtsp_source.py" "$WORK/fo_source.h264" \
        --port 18554 --fps 25 --stats-sec 5 --state "$WORK/rtsp_state.json" \
        > "$WORK/rtsp_source.log" 2>&1 &
    echo $! > "$WORK/rtsp_source.pid"
    rm -f "$WORK/failover_state.json"
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo1
    fail=0
    check() { python3 "$HERE/check_failover.py" "$WORK" "$@" || fail=$((fail + $?)); }

    echo "fo1: camera recorded by node-a (primary)"
    for n in $NODES; do start_node "$n" init; done
    check mark:t_a_start
    sleep "$FO_WAIT_SEC"; report fo1_a_live; check a_live

    echo "fo2: node-a goes offline, the service hands the camera to node-b"
    stop_nodes node-a
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo2
    check mark:t_b_start
    sleep "$FO_WAIT_SEC"; check b_failover

    echo "fo3: node-a is back, the camera moves back to it"
    start_node node-a before_back
    wait_healthy 18081
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo3
    check mark:t_back
    sleep "$FO_WAIT_SEC"; report fo3_a_back; check a_back

    echo "fo4: node-b keeps the failover recordings until the camera retention expires"
    check retention
    report fo4_retention
    echo "failover scenario: $fail check(s) failed"
    exit "$fail"
fi

for n in $NODES; do start_node "$n" init; done

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

echo "phase4: camera deleted from the cluster"
python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" phase4
sleep "$WAIT_SEC"; report phase4
