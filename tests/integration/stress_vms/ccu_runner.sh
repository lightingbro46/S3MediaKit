#!/usr/bin/env bash
# Test ngưỡng CCU của MediaServer — bản một file, chạy trên server đích.
#
# File này là phần đầu của ccu_test.sh do make_ccu_bundle.sh sinh ra; phía sau dòng
# __CCU_PAYLOAD__ là tar.gz (base64) chứa harness tests/integration/stress_vms, migration,
# config.ini, default.pem và clip video dựng sẵn. Không cần repo, không cần ffmpeg.
#
# Dùng:
#   ./ccu_test.sh <MediaServer | thư mục chứa MediaServer> [tuỳ chọn]
#
# Tuỳ chọn:
#   --suite quick|standard|full  quick    ~45 phút: bậc tới 600 (hot), storm, admission
#                                standard ~2,5 giờ: + chia đôi ngưỡng, bậc uniform, churn 30 phút
#                                full     ~8 giờ : + 5 hỗn hợp thuần + soak 4 giờ
#                                [mặc định: standard]
#   --only a,b,...               chỉ chạy các kịch bản này (tên ở cuối --help)
#   --work DIR                   thư mục làm việc, cần nhiều dung lượng [./ccu_work]
#   --accept N                   mức nghiệm thu [400]
#   --profile full|mid|smoke     số camera: 300 / 120 / 30 [full]
#   --viewer-hosts h1,h2         chạy thêm worker viewer trên máy khác qua ssh (không mật khẩu)
#   --nic IFACE                  card mạng phục vụ viewer (mặc định lo, hoặc tự dò khi có --viewer-hosts)
#   --soak-hours H               thời lượng soak [4]
#   --keep-records               giữ file ghi hình sau mỗi kịch bản (mặc định xoá để đỡ tốn đĩa)
#   --yes                        không hỏi lại khi cảnh báo dung lượng
#
# Kết quả: ccu_result_<máy>_<thời gian>.tar.gz trong thư mục hiện tại. Gửi nguyên file đó.
set -u

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
START_TS=$(date +%s)
STAMP=$(date +%Y%m%d_%H%M%S)
HOST=$(hostname -s 2>/dev/null || hostname)

SUITE=standard
ONLY=""
WORK="$PWD/ccu_work"
ACCEPT=400
PROFILE=full
VIEWER_HOSTS=""
NIC=""
SOAK_HOURS=4
KEEP_RECORDS=0
ASSUME_YES=0
BIN=""

ALL_SCENARIOS="step_hot step_uniform storm churn admission mix_fmp4-sub mix_fmp4-main mix_rtsp mix_hls mix_replay soak"

usage() {
    sed -n '2,26p' "$SELF" | sed 's/^# \{0,1\}//'
    echo "Kịch bản: $ALL_SCENARIOS"
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --suite) SUITE="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --work) WORK="$2"; shift 2 ;;
        --accept) ACCEPT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --viewer-hosts) VIEWER_HOSTS="$2"; shift 2 ;;
        --nic) NIC="$2"; shift 2 ;;
        --soak-hours) SOAK_HOURS="$2"; shift 2 ;;
        --keep-records) KEEP_RECORDS=1; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        -h|--help) usage 0 ;;
        -*) echo "Tuỳ chọn lạ: $1"; usage 2 ;;
        *) BIN="$1"; shift ;;
    esac
done

[ -n "$BIN" ] || usage 2
[ -d "$BIN" ] && BIN="$BIN/MediaServer"
[ -x "$BIN" ] || { echo "Không thấy MediaServer chạy được: $BIN"; exit 2; }
BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
[ "$(basename "$BIN")" = MediaServer ] || { echo "Binary phải tên MediaServer (harness gọi <thư mục>/MediaServer)"; exit 2; }
case "$PROFILE" in full|mid|smoke) ;; *) echo "--profile không hợp lệ"; exit 2 ;; esac
case "$PROFILE" in full) CAMERAS=300 ;; mid) CAMERAS=120 ;; smoke) CAMERAS=30 ;; esac

case "$SUITE" in
    quick)    SCENARIOS="step_hot storm admission" ;;
    standard) SCENARIOS="step_hot step_uniform storm churn admission" ;;
    full)     SCENARIOS="step_hot step_uniform storm churn admission mix_fmp4-sub mix_fmp4-main mix_rtsp mix_hls mix_replay soak" ;;
    *) echo "--suite không hợp lệ: $SUITE"; exit 2 ;;
esac
[ -n "$ONLY" ] && SCENARIOS="${ONLY//,/ }"
for s in $SCENARIOS; do
    case " $ALL_SCENARIOS " in *" $s "*) ;; *) echo "Kịch bản lạ: $s"; exit 2 ;; esac
done

mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
OUT="$WORK/result_${HOST}_${STAMP}"
B="$WORK/bundle"
mkdir -p "$OUT"
exec > >(tee -a "$OUT/runner.log") 2>&1

say() { echo "[ccu $(date +%H:%M:%S)] $*"; }

# ------------------------------------------------------------------ kiểm môi trường
missing=""
for c in python3 curl ss tar gzip base64 awk sed df ip; do
    command -v "$c" > /dev/null || missing="$missing $c"
done
[ -z "$missing" ] || { echo "Thiếu lệnh:$missing"; exit 2; }
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 6) else 1)' \
    || { echo "Cần python3 >= 3.6"; exit 2; }
ulimit -n 1048576 2>/dev/null || ulimit -n "$(ulimit -Hn)" 2>/dev/null || true
if [ "$(ulimit -n)" -lt 65536 ]; then
    say "CẢNH BÁO: ulimit -n = $(ulimit -n) (< 65536). 300 camera + nhiều viewer sẽ hết fd."
    say "          Chạy bằng root hoặc nâng hard limit (/etc/security/limits.conf)."
fi
if grep -aq "api-config.json" "$BIN" 2>/dev/null; then
    say "CẢNH BÁO: binary có vẻ là bản Debug (đọc api-config.json). Số đo CPU của bản Debug không dùng được."
    IS_DEBUG=1
else
    IS_DEBUG=0
fi

# ------------------------------------------------------------------ giải nén harness
say "giải nén harness vào $B"
rm -rf "$B"
mkdir -p "$B"
line=$(awk '/^__CCU_PAYLOAD__$/ {print NR + 1; exit}' "$SELF")
[ -n "$line" ] || { echo "File không có payload — hãy dùng ccu_test.sh do make_ccu_bundle.sh sinh ra"; exit 2; }
tail -n +"$line" "$SELF" | base64 -d | tar xz -C "$B" || { echo "Giải nén payload lỗi"; exit 2; }
H="$B/tests/integration/stress_vms"
cat "$B/BUNDLE_INFO" 2>/dev/null | sed 's/^/  /'

# ------------------------------------------------------------------ mạng
SERVER_HOST=127.0.0.1
CTL_HOST=127.0.0.1
if [ -n "$VIEWER_HOSTS" ]; then
    first="${VIEWER_HOSTS%%,*}"
    route=$(ip route get "$(getent hosts "$first" | awk '{print $1}' | head -1 || echo "$first")" 2>/dev/null | head -1)
    [ -z "$NIC" ] && NIC=$(echo "$route" | awk '{for(i=1;i<=NF;i++) if($i=="dev") print $(i+1)}')
    SERVER_HOST=$(echo "$route" | awk '{for(i=1;i<=NF;i++) if($i=="src") print $(i+1)}')
    CTL_HOST="$SERVER_HOST"
    for h in ${VIEWER_HOSTS//,/ }; do
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$h" python3 -c 'print(1)' > /dev/null 2>&1 \
            || { echo "Không ssh được (không mật khẩu) tới $h hoặc máy đó thiếu python3"; exit 2; }
    done
fi
[ -z "$NIC" ] && NIC=lo

# ------------------------------------------------------------------ thông tin máy
{
    echo "== thời gian";   date -R
    echo "== máy";         uname -a
    echo "== binary";      ls -la "$BIN"; echo "debug_build=$IS_DEBUG"; sha256sum "$BIN" 2>/dev/null
    echo "== cpu";         lscpu 2>/dev/null || nproc
    echo "== ram";         free -m
    echo "== đĩa (work)";  df -h "$WORK"
    echo "== block";       lsblk -d -o NAME,ROTA,SIZE,MODEL 2>/dev/null
    echo "== mạng";        ip -br addr 2>/dev/null
    for i in $(ls /sys/class/net 2>/dev/null); do
        echo "  $i speed=$(cat /sys/class/net/$i/speed 2>/dev/null || echo ?)Mbps"
    done
    echo "== ulimit -n";   ulimit -n
    echo "== python";      python3 --version
    echo "== sysctl";      sysctl vm.dirty_ratio vm.dirty_background_ratio net.core.somaxconn 2>/dev/null
    echo "== PSI";         for k in cpu io memory; do echo "$k: $(tr '\n' ' ' < /proc/pressure/$k 2>/dev/null)"; done
    echo "== tham số";     echo "suite=$SUITE scenarios=$SCENARIOS accept=$ACCEPT profile=$PROFILE nic=$NIC viewer_hosts=$VIEWER_HOSTS"
} > "$OUT/system.txt" 2>&1

# ------------------------------------------------------------------ ước lượng dung lượng
# 300 camera × (3,7 + 0,5) Mbps ≈ 160 MB/s ghi đĩa. Mỗi kịch bản xoá bản ghi khi xong
# (trừ --keep-records), nên chỉ cần đủ cho kịch bản dài nhất.
per_sec_mb=$(( CAMERAS * 42 / 80 ))
longest_min=60
case " $SCENARIOS " in *" churn "*) longest_min=45 ;; esac
case " $SCENARIOS " in *" step_hot "*) longest_min=75 ;; esac
case " $SCENARIOS " in *" soak "*) longest_min=$(( SOAK_HOURS * 60 + 15 )) ;; esac
need_gb=$(( per_sec_mb * 60 * longest_min / 1024 + 10 ))
avail_gb=$(df -Pk "$WORK" | awk 'NR==2 {print int($4/1048576)}')
say "ghi hình ≈ ${per_sec_mb} MB/s; kịch bản dài nhất ~${longest_min} phút cần ≈ ${need_gb} GB; còn trống ${avail_gb} GB"
if [ "$avail_gb" -lt "$need_gb" ]; then
    say "CẢNH BÁO: có thể thiếu dung lượng. Server sẽ tự dọn khi đầy (cũng là một phần của test),"
    say "          nhưng watchdog sẽ dừng kịch bản nếu đĩa còn < 5% hoặc < 10 GB."
    if [ "$ASSUME_YES" != 1 ] && [ -t 0 ]; then
        read -r -p "Tiếp tục? [y/N] " ans
        case "$ans" in y|Y|yes) ;; *) exit 1 ;; esac
    fi
fi

# ------------------------------------------------------------------ giám sát nền
BG_PIDS=""
if command -v vmstat > /dev/null; then
    vmstat -t 5 > "$OUT/vmstat.log" 2>&1 & BG_PIDS="$BG_PIDS $!"
fi
if command -v iostat > /dev/null; then
    iostat -x -t 5 > "$OUT/iostat.log" 2>&1 & BG_PIDS="$BG_PIDS $!"
fi
(
    while true; do
        echo "$(date +%s) $(tr '\n' ' ' < /proc/pressure/io 2>/dev/null)"
        sleep 5
    done
) > "$OUT/psi_io.log" 2>&1 & BG_PIDS="$BG_PIDS $!"

CURRENT_FILE="$WORK/.current_scenario"
(
    while true; do
        sleep 30
        cur=$(cat "$CURRENT_FILE" 2>/dev/null)
        [ -n "$cur" ] || continue
        read -r pct avail <<< "$(df -Pk "$WORK" | awk 'NR==2 {gsub("%","",$5); print 100-$5, int($4/1048576)}')"
        if [ "$pct" -lt 5 ] || [ "$avail" -lt 10 ]; then
            echo "[watchdog $(date +%H:%M:%S)] đĩa còn ${pct}% (${avail} GB) — dừng kịch bản $cur" | tee -a "$OUT/runner.log"
            touch "$WORK/$cur.disk_abort"
            bash "$H/run_stress.sh" "$WORK/$cur" stop > /dev/null 2>&1
        fi
    done
) & BG_PIDS="$BG_PIDS $!"

cleanup() {
    for p in $BG_PIDS; do kill "$p" 2>/dev/null; done
    cur=$(cat "$CURRENT_FILE" 2>/dev/null)
    [ -n "$cur" ] && bash "$H/run_stress.sh" "$WORK/$cur" stop > /dev/null 2>&1
    rm -f "$CURRENT_FILE"
}
trap cleanup EXIT
trap 'say "bị ngắt — đóng gói phần đã có"; finish; exit 130' INT TERM

# ------------------------------------------------------------------ chạy một kịch bản
declare -A RC DURATION

scenario_env() {
    # In ra biến môi trường của kịch bản, mỗi dòng một biến. Biến đã đặt sẵn từ ngoài
    # (vd CCU_HOLD_SEC=200 ./ccu_test.sh ...) được giữ nguyên — dùng để tinh chỉnh khi cần.
    local s="$1" steps="" args="" hold=""
    echo "CCU_ACCEPT=$ACCEPT"
    case "$s" in
        step_hot)
            echo "CCU_MIX=vms"; echo "CCU_DIST=hot"
            [ "$SUITE" = quick ] && { steps=0,100,200,300,400,500,600; args="--bisect 0"; } ;;
        step_uniform)
            echo "CCU_MIX=vms"; echo "CCU_DIST=uniform"
            steps=0,200,400,500,600,800; args="--bisect 0" ;;
        mix_*)
            echo "CCU_MIX=${s#mix_}"; echo "CCU_DIST=hot"
            steps=0,200,400,800,1200; args="--bisect 0"; hold=120
            echo "CCU_ACCEPT_HOLD_SEC=${CCU_ACCEPT_HOLD_SEC:-120}" ;;
        storm)     echo "CCU_MIX=vms"; echo "CCU_DIST=hot" ;;
        churn)     echo "CCU_MIX=vms"; echo "CCU_DIST=hot"; echo "CCU_DURATION=${CCU_CHURN_SEC:-1800}" ;;
        soak)      echo "CCU_MIX=vms"; echo "CCU_DIST=hot"
                   echo "CCU_DURATION=${CCU_SOAK_SEC:-$(( SOAK_HOURS * 3600 ))}" ;;
        admission) echo "CCU_MIX=vms"; echo "CCU_DIST=uniform" ;;
    esac
    [ -n "$steps" ] && echo "CCU_STEPS=${CCU_STEPS:-$steps}"
    [ -n "$args" ] && echo "CCU_ARGS=${CCU_ARGS:-$args}"
    [ -n "$hold" ] && echo "CCU_HOLD_SEC=${CCU_HOLD_SEC:-$hold}"
}

harness_scenario() {
    case "$1" in
        step_*|mix_*) echo ccu ;;
        storm) echo ccu_storm ;;
        churn) echo ccu_churn ;;
        soak) echo ccu_soak ;;
        admission) echo ccu_admission ;;
    esac
}

collect() {
    # chép bằng chứng của một kịch bản sang OUT, rồi xoá bản ghi nếu không giữ
    local s="$1" w="$WORK/$1" o="$OUT/$1"
    mkdir -p "$o"
    for f in "$w"/ccu*_report.json "$w"/ccu*_report.md "$w"/report_*.json "$w"/phases.jsonl \
             "$w"/resources.jsonl "$w"/inventory.json "$w"/rtsp_state.json "$w"/api_state.json \
             "$w"/load_ccu.json "$w"/rtsp_source.log "$w"/viewer_ctl.log "$w"/viewer_single.log \
             "$w"/mock_api.log "$w"/rest_load.log "$w"/onvif_mock.log; do
        [ -f "$f" ] && cp "$f" "$o/"
    done
    cat "$w"/viewer_worker_*.log 2>/dev/null | tail -2000 > "$o/viewer_workers.log"
    for n in "$w"/node-*; do
        [ -d "$n" ] || continue
        local nn; nn=$(basename "$n")
        mkdir -p "$o/$nn"
        cp "$n/config.ini" "$o/$nn/" 2>/dev/null
        # log đầy đủ nén lại; thêm bản tóm tắt cảnh báo/lỗi theo vị trí code để đọc nhanh
        for lf in "$n"/stdout*.log; do
            [ -f "$lf" ] || continue
            sed 's/\x1b\[[0-9;]*m//g' "$lf" | gzip -c > "$o/$nn/$(basename "$lf").gz"
            # tên thread có dấu cách ("event poller 4") nên tìm cột File.cpp:dòng thay vì đếm cột
            sed 's/\x1b\[[0-9;]*m//g' "$lf" | awk '$3=="W" || $3=="E" {
                    for (i = 4; i <= NF; i++) if ($i ~ /\.(cpp|cc|c|h):[0-9]+$/) { print $3, $i, $(i+1); break }
                }' \
                | sort | uniq -c | sort -rn | head -60 > "$o/$nn/$(basename "$lf" .log).warn_summary.txt"
        done
        du -sh "$n/record" > "$o/$nn/record_size.txt" 2>/dev/null
        if [ "$KEEP_RECORDS" != 1 ]; then
            rm -rf "$n/record"
        fi
    done
    [ -f "$WORK/$s.disk_abort" ] && echo "đã dừng vì hết đĩa" > "$o/DISK_ABORT"
}

run_one() {
    local s="$1" hs t0 rc
    hs=$(harness_scenario "$s")
    say "===== $s ($hs) ====="
    rm -rf "$WORK/$s"
    mkdir -p "$WORK/$s"
    echo "$s" > "$CURRENT_FILE"
    t0=$(date +%s)
    (
        export MEDIASERVER_DIR="$BIN_DIR" MEDIA_PROFILE=realistic MEDIA_DIR="$B/media"
        export PHASE_SEC="${PHASE_SEC:-180}" CCU_NIC="$NIC" CCU_SERVER_HOST="$SERVER_HOST"
        export CCU_CTL_HOST="$CTL_HOST" VIEWER_HOSTS="$VIEWER_HOSTS"
        while IFS= read -r kv; do export "$kv"; done < <(scenario_env "$s")
        bash "$H/run_stress.sh" "$WORK/$s" "$PROFILE" "$hs"
    ) > "$OUT/$s.console.log" 2>&1
    rc=$?
    RC[$s]=$rc
    DURATION[$s]=$(( ($(date +%s) - t0) / 60 ))
    rm -f "$CURRENT_FILE"
    say "$s xong sau ${DURATION[$s]} phút, rc=$rc"
    grep -E "^\- \*\*|^[0-9]\. |^A2" "$WORK/$s"/ccu*_report.md 2>/dev/null | sed 's/^/    /'
    collect "$s"
}

# ------------------------------------------------------------------ tổng hợp
finish() {
    local sum="$OUT/SUMMARY.md"
    {
        echo "# Kết quả test CCU — $HOST, $(date '+%Y-%m-%d %H:%M')"
        echo
        echo "- Binary: \`$BIN\` (debug_build=$IS_DEBUG)"
        echo "- Suite: $([ -n "$ONLY" ] && echo "--only" || echo "$SUITE") — kịch bản: $SCENARIOS"
        echo "- Profile: $PROFILE ($CAMERAS camera / $(( CAMERAS * 2 )) luồng, bitrate thật), mức nghiệm thu $ACCEPT"
        echo "- Viewer: $([ -n "$VIEWER_HOSTS" ] && echo "máy này + $VIEWER_HOSTS (NIC $NIC)" || echo "chạy cùng máy (loopback)")"
        echo "- Tổng thời gian: $(( ($(date +%s) - START_TS) / 60 )) phút"
        echo
        echo "| Kịch bản | rc | phút | Kết luận |"
        echo "|---|---|---|---|"
        for s in $SCENARIOS; do
            local line=""
            if [ -f "$OUT/$s/ccu_report.md" ] || ls "$OUT/$s"/ccu_*_report.md > /dev/null 2>&1; then
                line=$(grep -hE "^\- \*\*(CCU đạt|Nghiệm thu|Yếu tố)|^\- \*\*[0-9A]" "$OUT/$s"/ccu*_report.md 2>/dev/null \
                       | sed 's/^- //; s/\*\*//g' | tr '\n' ';' | sed 's/;$//')
            fi
            [ -f "$OUT/$s/DISK_ABORT" ] && line="DỪNG VÌ HẾT ĐĨA; $line"
            echo "| $s | ${RC[$s]:--} | ${DURATION[$s]:--} | ${line:-chưa chạy / không có báo cáo} |"
        done
        echo
        for s in $SCENARIOS; do
            for f in "$OUT/$s"/ccu*_report.md; do
                [ -f "$f" ] || continue
                echo "## $s"
                echo
                sed '1d' "$f"
                echo
            done
        done
    } > "$sum"
    local tarball="$PWD/ccu_result_${HOST}_${STAMP}.tar.gz"
    tar czf "$tarball" -C "$WORK" "$(basename "$OUT")" 2>/dev/null
    say "tóm tắt: $sum"
    say "GỬI LẠI FILE NÀY: $tarball ($(du -h "$tarball" | cut -f1))"
}

say "chạy: $SCENARIOS (profile $PROFILE, nghiệm thu $ACCEPT, viewer NIC $NIC)"
for s in $SCENARIOS; do
    run_one "$s"
done
finish
exit 0

__CCU_PAYLOAD__
