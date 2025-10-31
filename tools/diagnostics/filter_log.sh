#!/bin/bash
# Lọc log theo mốc thời gian hoặc khoảng thời gian và hiển thị tiến độ

usage() {
    echo "Cách dùng:"
    echo "  $0 <file_log> '<start_time>' ['<end_time>']"
    echo
    echo "Ví dụ:"
    echo "  $0 mediaserver.log '2025-10-30 18:57:58'"
    echo "  $0 mediaserver.log '2025-10-30 18:57:58' '2025-10-30 19:10:00'"
    exit 1
}

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    usage
fi

log_file="$1"
start_time="$2"
end_time="$3"

if [ ! -f "$log_file" ]; then
    echo "❌ File không tồn tại: $log_file"
    exit 1
fi

start_epoch=$(date -d "$start_time" +%s 2>/dev/null)
if [ -z "$start_epoch" ]; then
    echo "❌ Sai định dạng thời gian start_time: $start_time"
    exit 1
fi

if [ -n "$end_time" ]; then
    end_epoch=$(date -d "$end_time" +%s 2>/dev/null)
    if [ -z "$end_epoch" ]; then
        echo "❌ Sai định dạng thời gian end_time: $end_time"
        exit 1
    fi
else
    end_epoch=9999999999
fi

echo "🔍 Đang đọc file log: $log_file"
echo "📆 Khoảng thời gian: $start_time → ${end_time:-<tới hết>}"
echo "----------------------------------------------------"

line_count=0
match_count=0

while IFS= read -r line; do
    ((line_count++))
    # Cứ mỗi 1000 dòng in ra 1 lần tiến độ
    if (( line_count % 1000 == 0 )); then
        echo "📖 Đã đọc $line_count dòng..."
    fi

    ts=$(echo "$line" | awk '{print $1" "$2}' | grep -Eo '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?')
    if [ -n "$ts" ]; then
        ts_clean=$(echo "$ts" | cut -d. -f1)
        ts_epoch=$(date -d "$ts_clean" +%s 2>/dev/null)
        if [ -n "$ts_epoch" ]; then
            if [ "$ts_epoch" -ge "$start_epoch" ] && [ "$ts_epoch" -le "$end_epoch" ]; then
                echo "$line"
                ((match_count++))
            fi
        fi
    fi
done < "$log_file"

echo "----------------------------------------------------"
echo "✅ Hoàn thành! Đã đọc tổng cộng $line_count dòng, lọc được $match_count dòng hợp lệ."
