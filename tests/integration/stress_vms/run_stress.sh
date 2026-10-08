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
#               | failover | storage | recover | soak | videorule
#               | ccu | ccu_storm | ccu_churn | ccu_soak | ccu_admission  [mặc định: all]
#   run_stress.sh [work_dir] stop
#
# Env:
#   MEDIASERVER_DIR  thư mục chứa MediaServer (mặc định <repo>/release/linux/Debug)
#   NODES            số node (mặc định 2; kịch bản storage luôn dùng 1)
#   PHASE_SEC        thời lượng mỗi phase (mặc định theo profile)
#   CYCLES           số chu kỳ failover<->rebalance của kịch bản soak (mặc định 4)
#
# Env riêng cho kịch bản ccu* (docs/ccu-capacity-test-design.md):
#   CCU_MIX          vms | fmp4-sub | fmp4-main | rtsp | hls | replay   [vms]
#   CCU_DIST         hot | uniform                                       [hot]
#   CCU_STEPS        danh sách bậc                     [0,100,200,300,400,500,600,800,...]
#   CCU_HOLD_SEC     giữ mỗi bậc (bậc nghiệm thu giữ CCU_ACCEPT_HOLD_SEC)  [150]
#   CCU_ACCEPT       mức nghiệm thu                                      [400]
#   CCU_LEVEL        mức cho storm/churn/soak                            [= CCU_ACCEPT]
#   CCU_DURATION     thời lượng churn/soak (giây)                        [1800 / 14400]
#   CCU_JOIN_RATE    viewer mở mỗi giây                                  [20]
#   CCU_WORKERS      số worker viewer trên máy này                        [nproc/2]
#   CCU_NIC          card mạng phục vụ viewer, để đo egress               [lo]
#   CCU_REST_LOAD    1 = chạy thêm tải REST nhẹ (PTZ/storage) làm nền     [1]
#   CCU_PRODUCTION_ARGS 1 = chạy MediaServer không -t 4 --affinity 0     [1 với ccu*]
#   VIEWER_HOSTS     máy client khác (ssh), mỗi máy VIEWER_WORKERS_PER_HOST worker [4]
#   CCU_CTL_HOST     địa chỉ controller mà máy client gọi về    [IP đầu của máy này]
#   CCU_ARGS         tham số thêm cho check_ccu.py (vd "--th long_stall_session_ratio=0.02")
#   MEDIA_PROFILE    light (176x144 5fps 48kbps, mặc định) | realistic (main 1080p25 4Mbps,
#                    sub 640x360 25fps 512kbps, GOP 2 s) — đo CCU phải dùng realistic
#   RTSP_PROCS       số tiến trình nguồn RTSP cùng cổng [1 với light, nproc/2 với realistic]
#   MEDIA_DIR        thư mục có sẵn main_1080p.h264/sub_360p.h264 (khỏi cần ffmpeg)
#   CONFIG_BASE      repo (conf/config.ini) | code (mặc định trong code, như production)
#                    [code với ccu*, repo với kịch bản khác]
#   SSL_PEM          chứng chỉ cho -s [<MEDIASERVER_DIR>/default.pem, không có thì <repo>/default.pem]
set -u
# 300 camera + vài nghìn viewer + file ghi: giới hạn fd mặc định 1024 không đủ
ulimit -n 1048576 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

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
CYCLES="${CYCLES:-4}"
[ "$SCENARIO" = "storage" ] && NODES=1
[ "$SCENARIO" = "videorule" ] && NODES=1
case "$SCENARIO" in
    ccu*) NODES=1; CCU_PRODUCTION_ARGS="${CCU_PRODUCTION_ARGS:-1}"; CONFIG_BASE="${CONFIG_BASE:-code}" ;;
    gapfill)
        # Rớt mạng đồng loạt rồi bù từ thẻ SD (onvif_mock --profile-g). Mock khai báo giờ UTC0,
        # server cộng chênh múi giờ máy vào khoảng tìm kiếm, nên cả rig chạy TZ=UTC.
        NODES=1; CONFIG_BASE="${CONFIG_BASE:-code}"
        export TZ=UTC STRESS_GAP_FILL=1 STRESS_BLOCKING="${STRESS_BLOCKING:-code}"
        # --stream-base: URI replay của mock trỏ về nguồn RTSP của harness (path replay-cam-NNNN-*).
        ONVIF_MOCK_ARGS="${ONVIF_MOCK_ARGS:-} --profile-g --stream-base rtsp://127.0.0.1:$RTSP_PORT" ;;
esac
CCU_PORT=18600
CCU_SINGLE_PORT=18601

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
    # HLS nằm trên /dev/shm (setup_stress.py hls_dir): xoá để không giữ RAM sau khi dừng
    for link in "$WORK"/node-*/hls_shm; do
        [ -L "$link" ] && rm -rf "$(readlink "$link")"
    done
}

stop_helpers() {
    # viewer controller trước: nó báo worker dừng rồi mới thoát
    for name in viewer_ctl viewer_single rest_load; do
        if [ -f "$WORK/$name.pid" ]; then
            kill "$(cat "$WORK/$name.pid")" 2>/dev/null
            rm -f "$WORK/$name.pid"
        fi
    done
    if [ -f "$WORK/viewer_remote.pids" ]; then
        kill $(cat "$WORK/viewer_remote.pids") 2>/dev/null
        rm -f "$WORK/viewer_remote.pids"
    fi
    for name in rtsp_source onvif_mock mock_api sampler blocking_sampler; do
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
        # Con của nó (vd check_video_rule.py) sống tiếp sau khi cha bị kill -9 và vẫn
        # ghi vào fault spec của lần chạy mới, nên phải hạ trước.
        pkill -9 -P "$old" 2>/dev/null
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
             "$RTSP_PORT" "$ONVIF_PORT" "$API_PORT" \
             $(case "$SCENARIO" in ccu*) echo "$CCU_PORT $CCU_SINGLE_PORT" ;; esac); do
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
    # Đo CCU phải chạy như production: số poller mặc định (= số lõi), không ghim CPU.
    local threads="-t 4 --affinity 0"
    [ "${CCU_PRODUCTION_ARGS:-0}" = 1 ] && threads=""
    LD_LIBRARY_PATH="$BIN_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" nohup "$BIN_DIR/MediaServer" -c "$d/config.ini" \
        -s "$PEM" -l 1 $threads --log-dir "$d/log/" \
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

# ------------------------------------------------------------------ CCU
node_port() {
    python3 -c "import json;print(json.load(open('$WORK/inventory.json'))['ports']['$1']['$2'])"
}

start_viewers() {
    # $1 = cổng controller, $2 = tên pid, còn lại = tham số thêm cho controller
    local port="$1" name="$2"; shift 2
    local local_workers="${CCU_WORKERS:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}"
    local per_host="${VIEWER_WORKERS_PER_HOST:-4}" hosts="" total
    [ -n "${VIEWER_HOSTS:-}" ] && [ "$name" = viewer_ctl ] && hosts="${VIEWER_HOSTS//,/ }"
    total=$local_workers
    for h in $hosts; do total=$((total + per_host)); done
    nohup python3 "$HERE/viewer_load.py" controller --port "$port" \
        --inventory "$WORK/inventory.json" --host "${CCU_SERVER_HOST:-127.0.0.1}" \
        --http-port "$(node_port node-a http)" --rtsp-port "$(node_port node-a rtsp)" \
        --mix "${CCU_MIX:-vms}" --dist "${CCU_DIST:-hot}" --join-rate "${CCU_JOIN_RATE:-20}" \
        --replay-earliest "$REPLAY_EARLIEST" --local-workers "$local_workers" --total "$total" \
        --log-dir "$WORK" "$@" > "$WORK/$name.log" 2>&1 &
    echo $! > "$WORK/$name.pid"
    # Worker ở máy khác: script đẩy qua stdin nên máy client không cần checkout repo.
    local idx=$local_workers ctl="${CCU_CTL_HOST:-$(hostname -I | awk '{print $1}')}"
    for h in $hosts; do
        for _ in $(seq 1 "$per_host"); do
            ssh -o BatchMode=yes "$h" python3 - worker --control "http://$ctl:$port" \
                --index "$idx" --total "$total" < "$HERE/viewer_load.py" \
                >> "$WORK/viewer_remote_$h.log" 2>&1 &
            echo $! >> "$WORK/viewer_remote.pids"
            idx=$((idx + 1))
        done
    done
    for _ in $(seq 1 30); do
        curl -s -m 2 -o /dev/null "http://127.0.0.1:$port/target" && return 0
        sleep 1
    done
    echo "FAIL: viewer controller không lên (xem $WORK/$name.log)"; return 1
}

run_ccu() {
    local kind="${1#ccu}" accept="${CCU_ACCEPT:-400}" rc
    kind="${kind#_}"; [ -z "$kind" ] && kind=step
    local level="${CCU_LEVEL:-$accept}" duration
    case "$kind" in soak) duration="${CCU_DURATION:-14400}" ;; *) duration="${CCU_DURATION:-1800}" ;; esac
    mark "ccu_$kind"
    # Viewer replay lùi 180-600 s; phải có đủ 180 s bản ghi, nếu không mốc bị kẹp sát cạnh bản
    # ghi mới nhất và replay phải chờ file đang ghi đóng lại (đo nhầm thành khựng).
    case "${CCU_MIX:-vms}" in vms|replay)
        local wait_rec=$(( REPLAY_EARLIEST + 180 - $(date +%s) ))
        if [ "$wait_rec" -gt 0 ]; then
            echo "chờ ${wait_rec}s cho đủ 180 s bản ghi (viewer replay)..."
            sleep "$wait_rec"
        fi ;;
    esac
    start_viewers "$CCU_PORT" viewer_ctl || return 1
    local extra=""
    if [ "$kind" = admission ]; then
        # giới hạn theo camera: mọi viewer dồn vào cam-0000 (cohort always)
        start_viewers "$CCU_SINGLE_PORT" viewer_single --single-camera cam-0000 --dist uniform || return 1
        extra="--single-controller http://127.0.0.1:$CCU_SINGLE_PORT"
    fi
    if [ "${CCU_REST_LOAD:-1}" = 1 ]; then
        nohup python3 "$HERE/load_driver.py" --base "http://127.0.0.1:$(node_port node-a http)" \
            --inventory "$WORK/inventory.json" --duration 999999 --owner node-a \
            --ptz-workers 2 --storage-workers 2 --probe-workers 0 --ptz-pace 1 --storage-pace 1 \
            --state "$WORK/load_ccu.json" > "$WORK/rest_load.log" 2>&1 &
        echo $! > "$WORK/rest_load.pid"
    fi
    python3 "$HERE/check_ccu.py" "$WORK" --controller "http://127.0.0.1:$CCU_PORT" $extra \
        --scenario "$kind" --mix "${CCU_MIX:-vms}" --dist "${CCU_DIST:-hot}" \
        --steps "${CCU_STEPS:-0,100,200,300,400,500,600,800,1000,1250,1500,2000}" \
        --hold "${CCU_HOLD_SEC:-150}" --accept "$accept" --accept-hold "${CCU_ACCEPT_HOLD_SEC:-300}" \
        --join-rate "${CCU_JOIN_RATE:-20}" --level "$level" --duration "$duration" \
        ${CCU_ARGS:-}
    rc=$?
    curl -s -m 5 -X PUT -d '{"stop": true}' "http://127.0.0.1:$CCU_PORT/target" > /dev/null
    curl -s -m 5 -X PUT -d '{"stop": true}' "http://127.0.0.1:$CCU_SINGLE_PORT/target" > /dev/null 2>&1
    return $rc
}

# ------------------------------------------------------------------ chuẩn bị
[ -x "$BIN_DIR/MediaServer" ] || { echo "Không thấy MediaServer trong $BIN_DIR"; exit 1; }
PEM="${SSL_PEM:-$BIN_DIR/default.pem}"
[ -f "$PEM" ] || PEM="$REPO/default.pem"
need_ffmpeg() {
    command -v ffmpeg > /dev/null || { echo "Cần ffmpeg để dựng file video mẫu (hoặc đặt MEDIA_DIR)"; exit 1; }
}

mkdir -p "$WORK"
stop_previous_run
stop_all
sleep 2
preflight_ports
echo $$ > "$WORK/run_stress.pid"
rm -f "$WORK/phases.jsonl"

MEDIA_PROFILE="${MEDIA_PROFILE:-light}"
SUB_MEDIA=""
SOURCE_FPS=5
if [ "$MEDIA_PROFILE" = realistic ]; then
    # Bitrate thật (docs/ccu-capacity-test-design.md mục 2.1). Nhiễu theo thời gian để x264
    # không nén testsrc2 xuống vài trăm kbps; 10 s = 5 GOP để vòng lặp khớp keyframe.
    SOURCE_FPS=25
    MEDIA="$WORK/main_1080p.h264"
    SUB_MEDIA="$WORK/sub_360p.h264"
    if [ -n "${MEDIA_DIR:-}" ]; then
        for f in main_1080p.h264 sub_360p.h264; do
            [ -f "$WORK/$f" ] || cp "$MEDIA_DIR/$f" "$WORK/$f" 2>/dev/null
        done
    fi
    for spec in "$MEDIA:1920x1080:4000k" "$SUB_MEDIA:640x360:512k"; do
        f="${spec%%:*}"; rest="${spec#*:}"; size="${rest%%:*}"; rate="${rest#*:}"
        [ -f "$f" ] && continue
        need_ffmpeg
        echo "dựng $(basename "$f") ($size, $rate)..."
        ffmpeg -hide_banner -loglevel error -f lavfi \
            -i "testsrc2=size=$size:rate=25:duration=10" -vf "noise=alls=12:allf=t" \
            -c:v libx264 -preset veryfast -profile:v main -pix_fmt yuv420p \
            -g 50 -keyint_min 50 -sc_threshold 0 -b:v "$rate" -maxrate "$rate" -bufsize "$rate" \
            -bsf:v h264_mp4toannexb -f h264 "$f" -y || exit 1
    done
elif [ "$MEDIA_PROFILE" != light ]; then
    echo "MEDIA_PROFILE không hợp lệ: $MEDIA_PROFILE (light|realistic)"; exit 2
fi
RTSP_PROCS="${RTSP_PROCS:-$([ "$MEDIA_PROFILE" = realistic ] && echo $(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 )) || echo 1)}"

MEDIA="${MEDIA:-$WORK/loop.h264}"
if [ "$MEDIA_PROFILE" = light ] && [ ! -f "$MEDIA" ]; then
    need_ffmpeg
    echo "dựng file video mẫu..."
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=176x144:rate=5:duration=8" \
        -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 10 -keyint_min 10 \
        -sc_threshold 0 -b:v 48k -bsf:v h264_mp4toannexb -f h264 "$MEDIA" -y || exit 1
fi

python3 "$HERE/setup_stress.py" "$REPO" "$WORK" layout \
    --cameras "$CAMERAS" --nodes "$NODES" --config-base "${CONFIG_BASE:-repo}" \
    --rtsp-port "$RTSP_PORT" --onvif-port "$ONVIF_PORT" --api-port "$API_PORT" \
    ${STORAGE_ARGS:-} || exit 1

trap stop_all EXIT

nohup python3 "$HERE/rtsp_source.py" "$MEDIA" --port "$RTSP_PORT" --fps "$SOURCE_FPS" \
    ${SUB_MEDIA:+--sub-media "$SUB_MEDIA"} --procs "$RTSP_PROCS" \
    --drop-spec "$WORK/drop_spec.json" --state "$WORK/rtsp_state.json" \
    --replay-speed "${REPLAY_SPEED:-4}" \
    > "$WORK/rtsp_source.log" 2>&1 &
echo $! > "$WORK/rtsp_source.pid"

ONVIF_EXTRA=""
# Video config rule tìm profile chính bằng URI luồng, nên mock phải trả đúng URL camera.
[ "$SCENARIO" = "videorule" ] && ONVIF_EXTRA="--stream-base rtsp://127.0.0.1:$RTSP_PORT"
# vd ONVIF_MOCK_ARGS="--auth digest --auth-pass stress-pass": camera chỉ nhận HTTP Digest.
ONVIF_EXTRA="$ONVIF_EXTRA ${ONVIF_MOCK_ARGS:-}"
nohup python3 "$HERE/onvif_mock.py" --port "$ONVIF_PORT" $ONVIF_EXTRA \
    --fault-spec "$WORK/onvif_fault_spec.json" --state "$WORK/onvif_state.json" \
    > "$WORK/onvif_mock.log" 2>&1 &
echo $! > "$WORK/onvif_mock.pid"

# --config-root: bản Release lấy danh sách camera qua hook này thay vì api-config.json
nohup python3 "$HERE/mock_api.py" "$API_PORT" --state "$WORK/api_state.json" --config-root "$WORK" \
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
            utime=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)
            # Phân loại fd theo đích của symlink. Chỉ đếm tổng thì biết fd tăng mà không
            # biết rò loại nào, nên một lần `ls -l` trả cả tổng lẫn từng loại.
            fd_info=$(ls -l "/proc/$pid/fd" 2>/dev/null | awk '
                / -> / {
                    t = $NF; n++
                    if      (t ~ /^socket:/)                 k = "socket"
                    else if (t ~ /^pipe:/)                   k = "pipe"
                    else if (t ~ /^anon_inode:\[?eventfd/)   k = "eventfd"
                    else if (t ~ /^anon_inode:\[?eventpoll/) k = "eventpoll"
                    else if (t ~ /^anon_inode:\[?timerfd/)   k = "timerfd"
                    else if (t ~ /^anon_inode:\[?inotify/)   k = "inotify"
                    else if (t ~ /^anon_inode:/)             k = "anon"
                    else if (t ~ /^\//)                      k = "file"
                    else                                     k = "other"
                    c[k]++
                }
                END {
                    printf "%d ", n + 0
                    printf "{"
                    first = 1
                    for (k in c) { if (!first) printf ","; printf "\"%s\":%d", k, c[k]; first = 0 }
                    printf "}"
                }')
            fds="${fd_info%% *}"; fd_kinds="${fd_info#* }"
            echo "{\"at\":$(date +%s),\"node\":\"$node\",\"rss_kb\":${rss:-0},\"threads\":${threads:-0},\"fds\":${fds:-0},\"fd_kinds\":${fd_kinds:-{\}},\"cpu_ticks\":${utime:-0}}"
        done
        sleep 5
    done
) >> "$WORK/resources.jsonl" 2>/dev/null &
echo $! > "$WORK/sampler.pid"

# Hàng đợi và thời gian bận của các pool BlockingExecutors, mỗi 5 s (cửa sổ reset mỗi lần gọi).
(
    while true; do
        for n in $(node_list); do
            port=$(python3 -c "import json;print(json.load(open('$WORK/inventory.json'))['ports']['$n']['http'])" 2>/dev/null)
            secret=$(awk -F= '/^secret=/{sub(/\r$/, "", $2); print $2; exit}' "$WORK/$n/config.ini" 2>/dev/null)
            body=$(curl -s -m 3 "http://127.0.0.1:$port/index/api/getBlockingExecutors?secret=$secret" | tr -d '\n')
            [ -n "$body" ] && echo "{\"at\":$(date +%s),\"node\":\"$n\",\"resp\":$body}"
        done
        sleep 5
    done
) >> "$WORK/blocking.jsonl" 2>/dev/null &
echo $! > "$WORK/blocking_sampler.pid"

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

# Replay cần bản ghi đã có: luồng lên trong ~30 s đầu của ramp.
REPLAY_EARLIEST=$(( $(date +%s) - PHASE_SEC + 30 ))

case "$SCENARIO" in
    ramp) ;;
    ccu|ccu_storm|ccu_churn|ccu_soak|ccu_admission)
        run_ccu "$SCENARIO"
        ccu_rc=$?
        mark done
        python3 "$HERE/check_stress.py" "$WORK" --final
        [ "$ccu_rc" = 0 ] || { echo "FAIL: $SCENARIO (xem $WORK/ccu*_report.md)"; exit 1; }
        exit 0
        ;;
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
    soak)
        # V5: lặp chu kỳ failover <-> rebalance để phân biệt fd rò thật với fd bão hoà.
        # Một lần failover/rebalance (kịch bản `all`) không nói được gì: phải thấy fd tăng
        # ĐƠN ĐIỆU theo số chu kỳ mới là rò. Đọc cột "fd đầu→cuối" của từng phase.
        if [ "$NODES" -lt 2 ]; then
            echo "kịch bản soak cần NODES>=2 (nó dựa vào failover giữa các node)"; exit 2
        fi
        run_phase steady "$PHASE_SEC" ""
        cycle=1
        while [ "$cycle" -le "$CYCLES" ]; do
            run_phase "failover_$cycle"  "$PHASE_SEC" failover
            run_phase "rebalance_$cycle" "$PHASE_SEC" rebalance
            cycle=$((cycle + 1))
        done
        run_phase recover "$PHASE_SEC" recover
        ;;
    gapfill)
        # nền -> rớt mạng đồng loạt OUTAGE_SEC -> có mạng lại, theo dõi bù khoảng hở GAPFILL_WATCH_SEC
        run_phase steady "$PHASE_SEC" ""
        run_phase net_outage "${OUTAGE_SEC:-180}" net_outage
        run_phase net_recover "${GAPFILL_WATCH_SEC:-1500}" recover
        python3 "$HERE/check_gapfill.py" "$WORK" | tee "$WORK/report_gapfill.txt"
        ;;
    videorule)
        # Checklist chạy thật của video config rule trên onvif_mock.py (không cần camera).
        # Phần rule chạy với enableAuthorize=0; phần auth restart node với secret để kiểm 401.
        mark videorule
        python3 "$HERE/check_video_rule.py" "$WORK" --part rule
        rule_rc=$?
        mark videorule_auth
        stop_nodes
        python3 "$HERE/setup_stress.py" "$REPO" "$WORK" phase videorule_auth \
            --cameras "$CAMERAS" --nodes "$NODES" \
            --rtsp-port "$RTSP_PORT" --onvif-port "$ONVIF_PORT" --api-port "$API_PORT"
        start_node node-a auth
        port=$(python3 -c "import json;print(json.load(open('$WORK/inventory.json'))['ports']['node-a']['http'])")
        wait_http node-a "$port" 180 || exit 1
        python3 "$HERE/check_video_rule.py" "$WORK" --part auth
        auth_rc=$?
        [ "$rule_rc" = 0 ] && [ "$auth_rc" = 0 ] || { echo "FAIL: video config rule"; exit 1; }
        echo "video config rule: đạt"
        # Bỏ qua bản kiểm tra cuối chung: node vừa restart cho phần auth nên luồng chưa kịp lên
        # lại, bất biến "luồng còn phát" không nói gì về kịch bản này.
        mark done
        exit 0
        ;;
    *) echo "scenario không hợp lệ: $SCENARIO"; exit 2 ;;
esac

mark done
python3 "$HERE/check_stress.py" "$WORK" --final
