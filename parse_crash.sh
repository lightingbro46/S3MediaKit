#!/bin/bash

if [ $# -lt 2 ]; then
    echo "Usage: $0 <binary> <crash_log>"
    exit 1
fi

BINARY=$1
LOGFILE=$2

while IFS= read -r line; do
    # tìm địa chỉ trong ngoặc ()
    if [[ $line =~ \(\+0x[0-9a-fA-F]+\) ]]; then
        # cắt lấy giá trị bên trong ngoặc ()
        ADDR_HEX=$(echo "$line" | sed -n 's/.*(+\(0x[0-9a-fA-F]\+\)).*/\1/p')
        SRC=$(addr2line -e "$BINARY" -f -C -p "$ADDR_HEX")
        echo "$line"
        echo "  => $SRC"
        echo
    else
        echo "$line"
    fi
done < "$LOGFILE"