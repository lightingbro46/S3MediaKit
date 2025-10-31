#!/bin/bash
# Usage: ./parse_timeblock.sh file.bin

file="$1"
if [ -z "$file" ]; then
    echo "Usage: $0 file.bin"
    exit 1
fi

offset=0
filesize=$(stat -c %s "$file")

while [ $offset -lt $filesize ]; do
    # Đọc 4 byte length (little-endian)
    len_hex=$(dd if="$file" bs=1 skip=$offset count=4 2>/dev/null | hexdump -v -e '1/1 "%02x"')
    # Đảo byte order
    len=$((0x${len_hex:6:2}${len_hex:4:2}${len_hex:2:2}${len_hex:0:2}))
    offset=$((offset + 4))
    # Đọc protobuf data
    dd if="$file" bs=1 skip=$offset count=$len 2>/dev/null > /tmp/timeblock.bin
    echo "---- TimeBlock at offset $((offset-4)) (length $len) ----"
    protoc --decode_raw < /tmp/timeblock.bin | while IFS= read -r line; do
        # Nếu dòng bắt đầu bằng "6:"
        if echo "$line" | grep -qE '^[[:space:]]*6:'; then
            value=$(echo "$line" | sed -n 's/^[[:space:]]*6:[[:space:]]*\(.*\)/\1/p' | xargs)
            decoded=$(echo "$value" | base64 --decode 2>/dev/null)
            if [ $? -eq 0 ] && [ -n "$decoded" ]; then
                echo "$line"
                echo "    → Base64 decoded: $decoded"
            else
                echo "$line"
            fi
        else
            echo "$line"
        fi
    done
    offset=$((offset + len))
done