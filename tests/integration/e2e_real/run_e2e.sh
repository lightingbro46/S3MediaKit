#!/usr/bin/env bash
# Điều phối kiểm thử E2E với camera thật.
#
#   run_e2e.sh <work> layout          # tạo work dir (cần E2E_RTSP_MAIN / E2E_RTSP_SUB)
#   run_e2e.sh <work> start           # bật mock API + MediaServer
#   run_e2e.sh <work> stop            # SIGINT MediaServer (SIGKILL sau 30 s) + tắt mock
#   run_e2e.sh <work> restart
#   run_e2e.sh <work> kill9           # kill -9 MediaServer (REC-13), rồi `start` lại
#   run_e2e.sh <work> check <nhóm..>  # chạy e2e_check.py (xem --help)
#
# Env: MEDIASERVER_DIR (mặc định <repo>/release/linux/Debug), PORT_BASE (19080)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
WORK="${1:?cần work dir}"
CMD="${2:-}"
BIN_DIR="${MEDIASERVER_DIR:-$REPO/release/linux/Debug}"
PORT_BASE="${PORT_BASE:-19080}"

port_of() { python3 -c "import json;print(json.load(open('$WORK/inventory.json'))['ports']['$1'])"; }

port_busy() { ss -ltn "sport = :$1" 2>/dev/null | grep -q LISTEN; }

start() {
    [ -f "$WORK/inventory.json" ] || { echo "Chưa layout: $0 $WORK layout"; exit 1; }
    for name in http rtsp rtmp api; do
        if port_busy "$(port_of $name)"; then
            echo "Cổng $name=$(port_of $name) đang bận; dừng tiến trình cũ trước"; exit 1
        fi
    done
    nohup python3 "$HERE/mock_api.py" "$WORK" "$(port_of api)" > "$WORK/mock_api.log" 2>&1 &
    echo $! > "$WORK/mock_api.pid"
    [ -f "$WORK/stdout.log" ] && mv "$WORK/stdout.log" "$WORK/stdout.$(date +%s).log"
    # cd để MediaServer đọc ./api-config.json của work dir; $! phải là pid MediaServer.
    cd "$WORK" || exit 1
    LD_LIBRARY_PATH="$BIN_DIR" nohup "$BIN_DIR/MediaServer" -c "$WORK/config.ini" \
        -s "$BIN_DIR/default.pem" -l 0 -t 4 --log-dir "$WORK/log/" > "$WORK/stdout.log" 2>&1 &
    echo $! > "$WORK/server.pid"
    cd - > /dev/null
    local url="http://127.0.0.1:$(port_of http)/media/mserver/healthcheck"
    for _ in $(seq 1 60); do
        curl -s -o /dev/null --max-time 2 "$url" && { echo "MediaServer sẵn sàng (pid $(cat "$WORK/server.pid"))"; return 0; }
        kill -0 "$(cat "$WORK/server.pid")" 2>/dev/null || { echo "MediaServer đã thoát, xem $WORK/stdout.log"; exit 1; }
        sleep 1
    done
    echo "MediaServer không trả healthcheck sau 60 s"; exit 1
}

stop_server() {
    local pidfile="$WORK/server.pid"
    [ -f "$pidfile" ] || return 0
    local pid; pid="$(cat "$pidfile")"
    kill -INT "$pid" 2>/dev/null
    for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then
        echo "WARN: MediaServer không dừng bằng SIGINT sau 30 s, kill -9"
        kill -9 "$pid" 2>/dev/null
    fi
    rm -f "$pidfile"
}

stop_all() {
    stop_server
    if [ -f "$WORK/mock_api.pid" ]; then
        kill "$(cat "$WORK/mock_api.pid")" 2>/dev/null
        rm -f "$WORK/mock_api.pid"
    fi
    grep -l "## signal:" "$WORK"/stdout*.log 2>/dev/null | sed 's/^/CRASH trong: /'
}

case "$CMD" in
    layout)  python3 "$HERE/setup_e2e.py" "$REPO" "$WORK" layout --port-base "$PORT_BASE" "${@:3}" ;;
    camera)  python3 "$HERE/setup_e2e.py" "$REPO" "$WORK" camera "${@:3}" ;;
    start)   start ;;
    stop)    stop_all ;;
    restart) stop_server; kill "$(cat "$WORK/mock_api.pid" 2>/dev/null)" 2>/dev/null; start ;;
    kill9)   kill -9 "$(cat "$WORK/server.pid")" && rm -f "$WORK/server.pid"
             kill "$(cat "$WORK/mock_api.pid" 2>/dev/null)" 2>/dev/null; rm -f "$WORK/mock_api.pid" ;;
    check)   python3 "$HERE/e2e_check.py" "$WORK" "${@:3}" ;;
    *)       sed -n 2,11p "$0"; exit 1 ;;
esac
