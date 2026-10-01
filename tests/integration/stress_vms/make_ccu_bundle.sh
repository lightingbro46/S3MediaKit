#!/usr/bin/env bash
# Đóng gói test CCU thành MỘT file chạy được trên server đích: ccu_test.sh
#
# Phần đầu là ccu_runner.sh, phía sau dòng __CCU_PAYLOAD__ là tar.gz (base64) gồm:
#   tests/integration/stress_vms/*.py, run_stress.sh, README.md
#   migration/updates, migration/mserver_updates, conf/config.ini, default.pem
#   docs/ccu-capacity-test-design.md
#   media/main_1080p.h264, media/sub_360p.h264   (dựng bằng ffmpeg ở máy này, máy đích không cần)
#
# Dùng: make_ccu_bundle.sh [file_ra]     [mặc định: ./ccu_test.sh]
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-$PWD/ccu_test.sh}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/p/tests/integration/stress_vms" "$TMP/p/migration" "$TMP/p/conf" "$TMP/p/docs" "$TMP/p/media"
cp "$HERE"/*.py "$HERE"/run_stress.sh "$HERE"/README.md "$TMP/p/tests/integration/stress_vms/"
cp -r "$REPO/migration/updates" "$REPO/migration/mserver_updates" "$TMP/p/migration/"
cp "$REPO/conf/config.ini" "$TMP/p/conf/"
cp "$REPO/default.pem" "$TMP/p/"
cp "$REPO/docs/ccu-capacity-test-design.md" "$TMP/p/docs/"

# Clip giống hệt run_stress.sh (MEDIA_PROFILE=realistic): nhiễu theo thời gian để x264 không
# nén testsrc2 xuống vài trăm kbps; 10 s = 5 GOP để vòng lặp khớp keyframe.
command -v ffmpeg > /dev/null || { echo "Can ffmpeg tren may dong goi"; exit 1; }
for spec in "main_1080p.h264:1920x1080:4000k" "sub_360p.h264:640x360:512k"; do
    f="${spec%%:*}"; rest="${spec#*:}"; size="${rest%%:*}"; rate="${rest#*:}"
    echo "dung $f ($size, $rate)..."
    ffmpeg -hide_banner -loglevel error -f lavfi \
        -i "testsrc2=size=$size:rate=25:duration=10" -vf "noise=alls=12:allf=t" \
        -c:v libx264 -preset veryfast -profile:v main -pix_fmt yuv420p \
        -g 50 -keyint_min 50 -sc_threshold 0 -b:v "$rate" -maxrate "$rate" -bufsize "$rate" \
        -bsf:v h264_mp4toannexb -f h264 "$TMP/p/media/$f" -y
done

{
    echo "commit: $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo ?)$(git -C "$REPO" diff --quiet -- tests/integration/stress_vms 2>/dev/null || echo ' (+ thay doi chua commit trong stress_vms)')"
    echo "dong goi: $(date '+%Y-%m-%d %H:%M')"
} > "$TMP/p/BUNDLE_INFO"

find "$TMP/p" -name __pycache__ -prune -exec rm -rf {} +
tar czf "$TMP/payload.tgz" -C "$TMP/p" .
{
    cat "$HERE/ccu_runner.sh"
    base64 -w 76 "$TMP/payload.tgz"
} > "$OUT"
chmod +x "$OUT"
echo "da tao $OUT ($(du -h "$OUT" | cut -f1))"
