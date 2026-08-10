#!/usr/bin/env bash

# Xoa timefile/index va thu muc record cu hon mot moc ngay.
# Layout:
#   <storage>/<device>/YYYY-MM-DD.s3db[.idx]
#   <storage>/<device>/YYYY-MM-DD_sd.s3db[.idx]
#   <storage>/<device>/<stream>/YYYY-MM-DD/

set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  cleanup_record_day.sh [--dry-run] <storage-directory> <cutoff-YYYY-MM-DD>

Examples:
  cleanup_record_day.sh --dry-run /data/record 2026-08-01
  cleanup_record_day.sh /data/record 2026-08-01

The script removes dates strictly older than the cutoff. The cutoff date
itself is kept.
EOF
}

dry_run=0
if [[ "${1:-}" == "--dry-run" ]]; then
    dry_run=1
    shift
fi

if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi

storage_dir=$1
cutoff_date=$2

if [[ ! -d "$storage_dir" ]]; then
    echo "Error: storage directory does not exist or is not a directory: $storage_dir" >&2
    exit 1
fi

if [[ ! "$cutoff_date" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]]; then
    echo "Error: date must use YYYY-MM-DD: $cutoff_date" >&2
    exit 2
fi

# Validate that the date is real (not, for example, 2026-02-31).
if ! date -d "$cutoff_date" '+%Y-%m-%d' 2>/dev/null | grep -qxF "$cutoff_date"; then
    echo "Error: invalid calendar date: $cutoff_date" >&2
    exit 2
fi

storage_dir=$(realpath -- "$storage_dir")
if [[ "$storage_dir" == "/" ]]; then
    echo "Error: refusing to operate on the filesystem root" >&2
    exit 1
fi

declare -a files=()
declare -a directories=()

is_before_cutoff() {
    [[ "$1" < "$cutoff_date" ]]
}

# ISO date strings sort chronologically when their format is fixed.
while IFS= read -r -d '' path; do
    name=${path##*/}
    if [[ "$name" =~ ^([0-9]{4}-[0-9]{2}-[0-9]{2})(_sd)?\.(s3db|idx)$ ]] && \
       is_before_cutoff "${BASH_REMATCH[1]}"; then
        files+=("$path")
    fi
done < <(find "$storage_dir" -type f \( -name '*.s3db' -o -name '*.idx' \) -print0)

# Find exact YYYY-MM-DD folders. Exclude the storage root itself explicitly.
while IFS= read -r -d '' path; do
    name=${path##*/}
    if [[ "$path" != "$storage_dir" ]] && is_before_cutoff "$name"; then
        directories+=("$path")
    fi
done < <(find "$storage_dir" -mindepth 1 -type d -regextype posix-extended \
    -regex '.*/[0-9]{4}-[0-9]{2}-[0-9]{2}' -print0)

total=$(( ${#files[@]} + ${#directories[@]} ))
if (( total == 0 )); then
    echo "No record data older than $cutoff_date under $storage_dir"
    exit 0
fi

if (( dry_run )); then
    echo "Dry-run: the following $total item(s) would be removed:"
else
    echo "Removing $total item(s) older than $cutoff_date under $storage_dir:"
fi

for path in "${files[@]}" "${directories[@]}"; do
    echo "  $path"
done

if (( dry_run )); then
    exit 0
fi

# Remove files first, then date directories.
for path in "${files[@]}"; do
    rm -f -- "$path"
done
for path in "${directories[@]}"; do
    rm -rf -- "$path"
done

echo "Done. Removed $total item(s)."
