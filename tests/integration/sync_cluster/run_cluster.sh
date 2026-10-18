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
#                                        # offline (refused while node-b has failover off), move back,
#                                        # cross-node timeline, retention, release when failover is turned off
#   run_cluster.sh [work_dir] speaker    # speaker config sync: add, change, move, restart,
#                                        # sync_config_to_db=0, delete
#   run_cluster.sh [work_dir] monitor    # /media/monitor cluster component: node-b down, back, hook API down
#   run_cluster.sh [work_dir] history    # node-a + recording camera: /media/monitor/history, open latency,
#                                        # baseline across restarts, coverage/gaps, restarts, RSS growth
#   run_cluster.sh [work_dir] stale      # node-a (Debug build): monitor poller stalls once, /media/monitor/*
#                                        # reports stale=true, then recovers (S3MK_TEST_MONITOR_STALL_SEC)
#   run_cluster.sh [work_dir] removal    # node-b removed from the cluster (HTTP 412), cleanup of
#                                        # its ESC data on node-a, node-b added back
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
    for p in mock_api rtsp_source fake_s3; do
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
# removal: short decommission/cleanup delays instead of the production 300 s / 3600 s.
if [ "${2:-}" = "removal" ]; then
    export DECOMMISSION_AFTER_SEC="${DECOMMISSION_AFTER_SEC:-15}"
    export REMOVED_PEER_CLEANUP_SEC="${REMOVED_PEER_CLEANUP_SEC:-30}"
fi
python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" || exit 1

nohup python3 "$HERE/mock_api.py" 18090 "$WORK/not_qualified.txt" > "$WORK/mock_api.log" 2>&1 &
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

    echo "fo2off: node-a goes offline, the service hands the camera to node-b, which has failover off"
    stop_nodes node-a
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo2off
    sleep "$FO_WAIT_SEC"; check b_no_failover

    echo "fo2: failover turned on for node-b, it takes the camera"
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

    echo "fo5: node-a offline again, node-b takes the camera, then failover is turned off on node-b"
    stop_nodes node-a
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo2
    sleep "$FO_WAIT_SEC"; check b_live
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo2off
    sleep "$FO_WAIT_SEC"; check b_released
    echo "failover scenario: $fail check(s) failed"
    exit "$fail"
fi

if [ "${2:-}" = "history" ]; then
    fail=0
    command -v ffprobe > /dev/null || { echo "ffmpeg/ffprobe are required for the history scenario"; exit 1; }
    check() { python3 "$HERE/check_history.py" "$WORK" "$@" || fail=$((fail + 1)); }
    # Own port: the scenario stops and restarts its source, which must not be a shared one.
    export FO_RTSP_PORT="${FO_RTSP_PORT:-18564}"
    source_start() {
        nohup python3 "$REPO/tests/integration/stress_vms/rtsp_source.py" "$WORK/fo_source.h264" \
            --port "$FO_RTSP_PORT" --fps 25 --stats-sec 5 --state "$WORK/rtsp_state.json" >> "$WORK/rtsp_source.log" 2>&1 &
        echo $! > "$WORK/rtsp_source.pid"
    }
    ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=640x360:rate=25:duration=10" \
        -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 50 -keyint_min 50 -sc_threshold 0 \
        -b:v 400k -bsf:v h264_mp4toannexb -f h264 "$WORK/fo_source.h264" -y || exit 1
    ss -ltn | grep -q ":$FO_RTSP_PORT " && { echo "port $FO_RTSP_PORT is busy"; exit 1; }
    source_start
    rm -f "$WORK/history_state.json"
    # Segments of 20 s: a 90 s outage of the source is a gap (> 2 x mp4_max_second).
    MP4_MAX_SECOND=20 python3 "$HERE/setup_nodes.py" "$REPO" "$WORK"
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" fo1
    start_node node-a init
    wait_healthy 18081
    echo "h0: monitor settings"; check h0_config 60
    echo "h1: history"; check h1_history 180
    echo "h2: open latency"; check h2_open 60
    echo "h3: baseline"; check h3_baseline 420
    echo "h4: coverage"; check h4_coverage 300
    echo "h5: source stopped 90 s"
    kill "$(cat "$WORK/rtsp_source.pid")"; sleep 90; source_start; sleep 2
    ss -ltn | grep -q ":$FO_RTSP_PORT " || echo "FAIL rtsp_source did not restart on $FO_RTSP_PORT"
    check h5_gap 300
    echo "h6: clean restart"
    stop_nodes node-a; start_node node-a clean; wait_healthy 18081; check h6_clean_restart 240
    echo "h7: kill -9"
    kill -9 "$(cat "$WORK/node-a/server.pid")"; sleep 2; start_node node-a killed; wait_healthy 18081
    check h7_unclean 240
    echo "h8: RSS growth"; check h8_rss 240
    grep -h "## signal:" "$WORK"/node-a/stdout*.log && echo "CRASH: node-a"
    echo "history scenario: $fail check(s) failed"
    exit "$fail"
fi

if [ "${2:-}" = "monitor" ]; then
    fail=0
    check() { python3 "$HERE/check_monitor.py" "$WORK" "$@" || fail=$((fail + 1)); }
    for n in $NODES; do start_node "$n" init; done
    wait_healthy 18081
    echo "m1: both nodes up"; check m1_ok 240
    # ClusterManager health-checks peers every 60 s, then monitor.sustainSamples cycles.
    echo "m2: node-b stopped"; stop_nodes node-b; check m2_peer_down 180
    echo "m3: node-b back"; start_node node-b back; check m3_peer_back 180
    echo "m4: hook API stopped"
    kill "$(cat "$WORK/mock_api.pid")"; check m4_hook_down 120
    nohup python3 "$HERE/mock_api.py" 18090 "$WORK/not_qualified.txt" >> "$WORK/mock_api.log" 2>&1 &
    echo $! > "$WORK/mock_api.pid"
    echo "m5: object storage pool on a fake S3"
    nohup python3 "$HERE/fake_s3.py" 19000 > "$WORK/fake_s3.log" 2>&1 &
    echo $! > "$WORK/fake_s3.pid"
    check m5_pool_ok 180
    echo "m6: object storage stopped"
    kill "$(cat "$WORK/fake_s3.pid")"; rm -f "$WORK/fake_s3.pid"; check m6_pool_down 240
    report monitor
    echo "monitor scenario: $fail check(s) failed"
    exit "$fail"
fi

if [ "${2:-}" = "stale" ]; then
    STALL_SEC="${STALL_SEC:-60}"
    # Only node-a: the hook reads these once at start (ENABLE_DEBUG builds; Release ignores them).
    export S3MK_TEST_MONITOR_STALL_SEC="$STALL_SEC" S3MK_TEST_MONITOR_STALL_AT_CYCLE=3
    start_node node-a init
    unset S3MK_TEST_MONITOR_STALL_SEC S3MK_TEST_MONITOR_STALL_AT_CYCLE
    wait_healthy 18081
    python3 "$HERE/check_stale.py" "$WORK" "$STALL_SEC"
    fail=$?
    # Stop after the recovery check: a SIGINT during the stall waits for the sleep to end.
    pid="$(cat "$WORK/node-a/server.pid" 2>/dev/null)"
    stop_nodes node-a
    if grep -q "## signal:" "$WORK/node-a/stdout.log" || [ -f "$BIN_DIR/crash.$pid" ]; then
        echo "CRASH: node-a (see $WORK/node-a/stdout.log)"; fail=$((fail + 1))
    fi
    echo "stale scenario: $fail check(s) failed"
    exit "$fail"
fi

if [ "${2:-}" = "removal" ]; then
    fail=0
    check() { python3 "$HERE/check_removal.py" "$WORK" "$@" || fail=$((fail + $?)); }
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" rm1
    for n in $NODES; do start_node "$n" init; done

    echo "rm1: node-b runs 2 cameras and a speaker"; check rm1; report rm1
    echo "rm2: camera moved to node-a, node-b removed from the cluster (412)"
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" rm2; check rm2
    echo "rm3: node-b added back"
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" rm3; check rm3; report rm3
    echo "removal scenario: $fail check(s) failed"
    exit "$fail"
fi

if [ "${2:-}" = "speaker" ]; then
    fail=0
    check() { python3 "$HERE/check_speaker.py" "$WORK" "$@" || fail=$((fail + $?)); }
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" sp1
    for n in $NODES; do start_node "$n" init; done

    echo "sp1: speaker on node-a"; check sp1; report sp1
    echo "sp2: speaker renamed on node-a"
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" sp2; check sp2; report sp2
    echo "sp3: speaker moved node-a -> node-b"
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" sp3; check sp3; report sp3
    echo "sp4: restart both nodes"
    stop_nodes
    for n in $NODES; do start_node "$n" before_restart; done
    check sp4; report sp4
    echo "sp5: node-b with camera/speaker sync_config_to_db=0"
    stop_nodes node-b
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" syncoff:node-b
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" sp5
    start_node node-b before_syncoff
    check sp5; report sp5
    echo "sp6: speaker deleted from the cluster (node-b back to sync on)"
    stop_nodes node-b
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" syncon:node-b
    python3 "$HERE/setup_nodes.py" "$REPO" "$WORK" sp6
    start_node node-b before_syncon
    check sp6; report sp6
    echo "speaker scenario: $fail check(s) failed"
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
