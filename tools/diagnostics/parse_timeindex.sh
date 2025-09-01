#!/bin/bash
# Usage: ./parse_idx.sh file.idx

file="$1"
if [ -z "$file" ]; then
    echo "Usage: $0 file.idx"
    exit 1
fi

entry_size=16

od -An -t u8 -w$entry_size -v "$file" | while read start_time offset; do
    local_time=$(date -d @"$start_time" +"%Y-%m-%d %H:%M:%S")
    echo "start_time: $start_time ($local_time), offset: $offset"
done