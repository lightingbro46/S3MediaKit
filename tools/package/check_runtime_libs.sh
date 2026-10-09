#!/bin/bash
# Kiểm thư viện runtime của MediaServer/ffmpeg/ffprobe: thư viện tự build (FFmpeg _s3, AWS SDK)
# nằm trong <libdir>, thư viện hệ thống lấy từ gói apt.
# Chỉ kiểm, không chép, không vá.
#
#   check_runtime_libs.sh [--strict] <libdir> <binary>...
#       - không thư viện nào "not found" (binary và mọi .so trong <libdir>);
#       - thư viện tự build (FFmpeg *_s3, AWS SDK) phải resolve vào <libdir>;
#       - --strict: <libdir> chỉ chứa thư viện tự build (dùng cho /opt/media/lib trong Docker).
#   check_runtime_libs.sh --apt-packages <libdir> <binary>...
#       in các gói apt sở hữu thư viện còn lại (ngoài <libdir>, trừ glibc/libstdc++), để đối chiếu
#       danh sách gói runtime trong dockerfile.
set -euo pipefail

# Thư viện mình tự build: FFmpeg đổi tên _s3, AWS SDK và runtime của nó.
OWN_RE='^lib(avutil|avcodec|avformat|avfilter|swscale|swresample)_s3\.so|^libaws-|^libs2n\.so'
# Không thuộc gói nào cần cài thêm: loader, glibc, libstdc++/libgcc_s đã có trong mọi image.
BASE_RE='^(linux-vdso|ld-linux|libc|libm|libpthread|libdl|librt|libresolv|libutil|libstdc\+\+|libgcc_s)\.so'

mode=check
strict=0
case "${1:-}" in
    --apt-packages) mode=apt; shift ;;
    --strict) strict=1; shift ;;
esac
[ $# -ge 2 ] || { sed -n '2,13p' "$0"; exit 2; }
libdir=$(readlink -f "$1"); shift

targets=("$@")
while IFS= read -r so; do
    targets+=("$so")
done < <(find "$libdir" -maxdepth 1 -type f -name 'lib*.so*' | sort)

fail=0
declare -A others=()
for t in "${targets[@]}"; do
    [ -e "$t" ] || { echo "MISSING: $t"; fail=1; continue; }
    while IFS= read -r line; do
        name=$(awk '{print $1}' <<< "$line")
        path=$(awk '$2 == "=>" {print $3}' <<< "$line")
        if grep -q 'not found' <<< "$line"; then
            echo "NOT FOUND: $name (cần bởi $t)"
            fail=1
        elif [[ "$name" =~ $OWN_RE ]]; then
            if [ "$(dirname "$(readlink -f "$path")")" != "$libdir" ] && [ "$(dirname "$path")" != "$libdir" ]; then
                echo "WRONG PATH: $name -> $path (cần bởi $t), phải nằm trong $libdir"
                fail=1
            fi
        elif [ -n "$path" ] && ! [[ "$name" =~ $BASE_RE ]]; then
            others["$path"]=1
        fi
    done < <(ldd "$t" 2>&1 | grep -v 'not a dynamic executable' || true)
done

if [ $strict = 1 ]; then
    while IFS= read -r f; do
        if ! [[ "$(basename "$f")" =~ $OWN_RE ]]; then
            echo "NOT OWN: $f (chỉ thư viện tự build được nằm trong $libdir)"
            fail=1
        fi
    done < <(find "$libdir" -maxdepth 1 \( -type f -o -type l \) -name '*.so*')
fi

if [ $mode = apt ]; then
    for p in "${!others[@]}"; do
        dpkg -S "$(readlink -f "$p")" 2>/dev/null || dpkg -S "$p" 2>/dev/null || echo "?: $p"
    done | awk -F: '{print $1}' | sort -u
    exit $fail
fi

if [ $fail = 0 ]; then
    echo "OK: ${#targets[@]} file, thư viện tự build nằm trong $libdir, ${#others[@]} thư viện hệ thống"
fi
exit $fail
