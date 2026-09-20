#!/usr/bin/env bash
# Stress test VMS: 300 camera / 600 luồng trên 1-2 node MediaServer.
#
# Yêu cầu: MediaServer bản Debug (bản Debug mới đọc ./api-config.json). Muốn chạy
# kịch bản đồng bộ DB thì bản build phải có ENABLE_SYNC_CHANGES=ON; muốn chạy kịch
# bản StorageManager thì cần thêm một bản ENABLE_TIER_STORAGE=OFF (xem README).
#
# Dùng:
#   run_stress.sh [work_dir] [profile] [scenario]
#     profile : smoke (30 cam) | mid (120 cam) | full (300 cam)   [mặc định: mid]
#     scenario: all | ramp | steady | onvif | flap | schedule | retention
#               | failover | storage | recover                    [mặc định: all]
#   run_stress.sh [work_dir] stop
#
# Env:
#   MEDIASERVER_DIR  thư mục chứa MediaServer (mặc định <repo>/release/linux/Debug)
#   NODES            số node (mặc định 2; kịch bản storage luôn dùng 1)
#   PHASE_SEC        thời lượng mỗi phase (mặc định theo profile)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${1:-/tmp/s3mk_stress}"
PROFILE="${2:-mid}"
SCENARIO="${3:-all}"
BIN_DIR="${MEDIASERVER_DIR:-$REPO/release/linux/Debug}"

RTSP_PORT=18554
ONVIF_PORT=18580
API_PORT=18590

case "$PROFILE" in
    smoke) CAMERAS=30;  DEF_PHASE=60  ;;
    mid)   CAMERAS=120; DEF_PHASE=120 ;;
    full)  CAMERAS=300; DEF_PHASE=180 ;;
    stop)  CAMERAS=0;   DEF_PHASE=0   ;;
    *)     echo "profile không hợp lệ: $PROFILE (smoke|mid|full)"; exit 2 ;;
esac
PHASE_SEC="${PHASE_SEC:-$DEF_PHASE}"
NODES="${NODES:-2}"
[ "$SCENARIO" = "storage" ] && NODES=1

node_list() {
    local i=0 out=""
    while [ "$i" -lt "$NODES" ]; do
        out="$out node-$(printf "\\$(printf '%03o' $((97 + i)))")"
        i=$((i + 1))
    done
    echo "$out"
}

# ------------------------------------------------------------------ dừng tiến trình
stop_nodes() {
    for pidfile in "$WORK"/node-*/server.pid; do
        [ -f "$pidfile" ] && kill -INT "$(cat "$pidfile")" 2>/dev/null
    done
    for _ in $(seq 1 40); do
        alive=0
        for pidfile in "$WORK"/node-*/server.pid; do
            [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null && alive=1
        done
        [ "$alive" = 0 ] && break
        sleep 1
    done
    for pidfile in "$WORK"/node-*/server.pid; do
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "WARN: $(dirname "$pidfile") không dừng bằng SIGINT, phải kill -9"
            kill -9 "$(cat "$pidfile")" 2>/dev/null
        fi
        rm -f "$pidfile"
    done
}

stop_helpers() {
    for name in rtsp_source onvif_mock mock_api sampler; do
        if [ -f "$WORK/$name.pid" ]; then
            kill "$(cat "$WORK/$name.pid")" 2>/dev/null
            rm -f "$WORK/$name.pid"
        fi
    done
}

stop_all() { stop_nodes; stop_helpers; }

# `stop` cũng phải hạ luôn kịch bản đang chạy dở của lần trước: nếu chỉ giết node mà
# để kịch bản cũ sống, nó vẫn chạy tiếp các phase và ghi đè api-config.json của lần
# chạy mới.
stop_previous_run() {
    local f="$WORK/run_stress.pid" old
    [ -f "$f" ] || return 0
    old="$(cat "$f")"
    if [ -n "$old" ] && [ "$old" != "$$" ] && kill -0 "$old" 2>/dev/null; then
        echo "dừng kịch bản còn chạy dở (pid $old)"
        kill -9 "$old" 2>/dev/null
        sleep 1
    fi
    rm -f "$f"
}

if [ "${2:-}" = "stop" ] || [ "$PROFILE" = "stop" ]; then
    stop_previous_run
    stop_all
    echo "đã dừng"
    exit 0
fi

# ------------------------------------------------------------------ khởi động
# Một MediaServer còn sót từ lần chạy trước sẽ chiếm cổng của node mới. Node mới
# chết lúc bind, nhưng `wait_http` lại thấy cổng HTTP có người trả lời (chính là tiến
# trình cũ) nên kịch bản vẫn chạy tiếp và cho số liệu sai. Vì vậy phải chặn từ đầu.
preflight_ports() {
    local busy="" p
    for p in $(seq 18081 $((18081 + NODES - 1))) \
             $(seq 18551 $((18551 + NODES - 1))) \
             $(seq 18351 $((18351 + NODES - 1))) \
             "$RTSP_PORT" "$ONVIF_PORT" "$API_PORT"; do
        if ss -lnt 2>/dev/null | awk 'NR>1{print $4}' | grep -qE "[:.]$p\$"; then
            busy="$busy $p"
        fi
    done
    if [ -n "$busy" ]; then
        echo "FAIL: các cổng sau đang bị chiếm:$busy"
        echo "      Còn tiến trình của lần chạy trước? Thử:"
        echo "        $0 $WORK stop   (và kiểm tra: pgrep -ax MediaServer)"
        exit 1
    fi
}

start_node() {
    local n="$1" tag="$2" d="$WORK/$n"
    [ -f "$d/stdout.log" ] && mv "$d/stdout.log" "$d/stdout.$tag.log"
    cd "$d" || return 1
    LD_LIBRARY_PATH="$BIN_DIR" nohup "$BIN_DIR/MediaServer" -c "$d/config.ini" \
        -s "$BIN_DIR/default.pem" -l 1 -t 4 --affinity 0 --log-dir "$d/log/" \
        > "$d/stdout.log" 2>&1 &
    echo $! > "$d/server.pid"
    cd - > /dev/null
}

# Chờ node lên. Kiểm cả pid: nếu tiến trình vừa khởi động đã chết (ví dụ bind lỗi)
# thì dừng ngay thay vì chờ hết timeout hoặc nhận nhầm câu trả lời của tiến trình khác.
wait_http() {
    local node="$1" port="$2" tries="${3:-120}"
    local pidfile="$WORK/$node/server.pid"
    for _ in $(seq 1 "$tries"); do
        if [ -f "$pidfile" ] && ! kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "FAIL: $node đã thoát ngay sau khi khởi động; 20 dòng cuối:"
            sed 's/\x1b\[[0-9;]*m//g' "$WORK/$node/stdout.log" | tail -20
            return 1
        fi
        curl -s -m 2 -o /dev/null "http://127.0.0.1:$port/media/mserver/healthcheck" && return 0
        sleep 1
    done
    echo "FAIL: $node không trả lời healthcheck trên cổng $port"
    return 1
}

mark() {
    # ghi mốc thời gian của phase để check_stress.py cắt log theo phase
    echo "{\"phase\": \"$1\", \"at\": $(date +%s)}" >> "$WORK/phases.jsonl"
    echo ""
    echo "===== phase: $1 ($(date +%H:%M:%S)) ====="
}

report() {
    python3 "$HERE/check_stress.py" "$WORK" --phase "$1"
}

drive_load() {
    local secs="$1" first_port
    first_port=$(python3 -c "import json;print(json.load(open('$WORK/inventory.json'))['ports']['node-a']['http'])")
    python3 "$HERE/load_driver.py" --base "http://127.0.0.1:$first_port" \
        --inventory "$WORK/inventory.json" --duration "$secs" --owner node-a \
        --state "$WORK/load_$2.json" 2>&1 | sed 's/^/  /'
}

# ------------------------------------------------------------------ chuẩn bị
[ -x "$BIN_DIR/MediaServer" ] || { echo "Không thấy MediaServer trong $BIN_DIR"; exit 1; }
command -v ffmpeg > /dev/null || { echo "Cần ffmpeg để dựng file video mẫu"; exit 1; }

mkdir -p "$WORK"
stop_previous_run
stop_all
sleep 2
preflight_ports
echo $$ > "$WORK/run_stress.pid"
rm -f "$WORK/phases.jsonl"

MEDIA="$WORK/loop.h264"
if [ ! -f "$MEDIA" ]; then
    echo "dựng file video mẫu..."
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=176x144:rate=5:duration=8" \
        -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 10 -keyint_min 10 \
        -sc_threshold 0 -b:v 48k -bsf:v h264_mp4toannexb -f h264 "$MEDIA" -y || exit 1
fi

python3 "$HERE/setup_stress.py" "$REPO" "$WORK" layout \
    --cameras "$CAMERAS" --nodes "$NODES" \
    --rtsp-port "$RTSP_PORT" --onvif-port "$ONVIF_PORT" --api-port "$API_PORT" \
    ${STORAGE_ARGS:-} || exit 1

trap stop_all EXIT

nohup python3 "$HERE/rtsp_source.py" "$MEDIA" --port "$RTSP_PORT" --fps 5 \
    --drop-spec "$WORK/drop_spec.json" --state "$WORK/rtsp_state.json" \
    > "$WORK/rtsp_source.log" 2>&1 &
echo $! > "$WORK/rtsp_source.pid"

nohup python3 "$HERE/onvif_mock.py" --port "$ONVIF_PORT" \
    --fault-spec "$WORK/onvif_fault_spec.json" --state "$WORK/onvif_state.json" \
    > "$WORK/onvif_mock.log" 2>&1 &
echo $! > "$WORK/onvif_mock.pid"

nohup python3 "$HERE/mock_api.py" "$API_PORT" --state "$WORK/api_state.json" \
    > "$WORK/mock_api.log" 2>&1 &
echo $! > "$WORK/mock_api.pid"

sleep 2

# lấy mẫu tài nguyên của từng node mỗi 5s
(
    while true; do
        for pidfile in "$WORK"/node-*/server.pid; do
            [ -f "$pidfile" ] || continue
            pid=$(cat "$pidfile"); node=$(basename "$(dirname "$pidfile")")
            [ -d "/proc/$pid" ] || continue
            rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null)
            threads=$(awk '/Threads/{print $2}' "/proc/$pid/status" 2>/dev/null)
            fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
            utime=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)
            echo "{\"at\":$(date +%s),\"node\":\"$node\",\"rss_kb\":${rss:-0},\"threads\":${threads:-0},\"fds\":$fds,\"cpu_ticks\":${utime:-0}}"
        done
        sleep 5
    done
) >> "$WORK/resources.jsonl" 2>/dev/null &
echo $! > "$WORK/sampler.pid"

# ------------------------------------------------------------------ kịch bản
run_phase() {
    local name="$1" secs="$2" setup_phase="${3:-}"
    mark "$name"
    if [ -n "$setup_phase" ]; then
        python3 "$HERE/setup_stress.py" "$REPO" "$WORK" phase "$setup_phase" \
            --cameras "$CAMERAS" --nodes "$NODES" \
            --rtsp-port "$RTSP_PORT" --onvif-port "$ONVIF_PORT" --api-port "$API_PORT"
    fi
    drive_load "$secs" "$name"
    report "$name"
}

echo "profile=$PROFILE cameras=$CAMERAS nodes=$NODES phase_sec=${PHASE_SEC}s binary=$BIN_DIR"

mark ramp
for n in $(node_list); do start_node "$n" init; done
for n in $(node_list); do
    port=$(python3 -c "import json;print(json.load(open('$WORK/inventory.json'))['ports']['$n']['http'])")
    wait_http "$n" "$port" 180 || exit 1
done
echo "các node đã lên, chờ ${PHASE_SEC}s cho camera kết nối..."
sleep "$PHASE_SEC"
report ramp

case "$SCENARIO" in
    ramp) ;;
    steady)    run_phase steady    "$PHASE_SEC" ;;
    onvif)     run_phase onvif     "$PHASE_SEC" onvif_storm ;;
    flap)      run_phase flap      "$PHASE_SEC" source_flap ;;
    schedule)  run_phase schedule  "$PHASE_SEC" schedule_flip ;;
    retention) run_phase retention "$PHASE_SEC" retention_drop ;;
    failover)  run_phase failover  "$PHASE_SEC" failover ;;
    recover)   run_phase recover   "$PHASE_SEC" recover ;;
    storage)
        # StorageManager::enforceStoragePolicy chạy theo timer 300s, nên mỗi phase
        # dưới đây phải dài hơn một chu kỳ mới quan sát được kết quả.
        local_wait=$((PHASE_SEC > 360 ? PHASE_SEC : 360))
        run_phase steady "$PHASE_SEC" ""
        # giữ 1h: chưa có đoạn nào quá hạn -> không được xoá gì
        run_phase retention "$local_wait" retention_drop
        # giữ 0h: mọi đoạn đã ghi xong đều quá hạn -> phải xoá và dựng lại timefile
        run_phase purge "$local_wait" retention_purge
        ;;
    all)
        run_phase steady    "$PHASE_SEC" ""
        run_phase onvif     "$PHASE_SEC" onvif_storm
        run_phase flap      "$PHASE_SEC" source_flap
        run_phase schedule  "$PHASE_SEC" schedule_flip
        run_phase retention "$PHASE_SEC" retention_drop
        if [ "$NODES" -gt 1 ]; then
            run_phase failover "$PHASE_SEC" failover
            run_phase rebalance "$PHASE_SEC" rebalance
        fi
        run_phase recover "$PHASE_SEC" recover
        ;;
    *) echo "scenario không hợp lệ: $SCENARIO"; exit 2 ;;
esac

mark done
python3 "$HERE/check_stress.py" "$WORK" --final
