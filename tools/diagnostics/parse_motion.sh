#!/usr/bin/env bash
# parse_motion.sh — Parse S3MediaKit .mblk block files and .idx index files
#
# Usage:
#   parse_motion.sh [-v] <file.mblk>
#   parse_motion.sh [-v] <file.idx>
#   parse_motion.sh [-v] <file.mblk> <file.idx>
#   parse_motion.sh [-v] <directory/>
#
# Requires: bash >= 4, od, awk, date, tr, grep, stat

set -uo pipefail

# ──────────────────────────────────────────────────────────────────────────────
# Constants (must match C++ packed structs)
# ──────────────────────────────────────────────────────────────────────────────
readonly MOTION_MAGIC=1297044558        # 0x4D4F544E  "MOTN"
readonly INDEX_MAGIC=1480870222         # 0x5844494E  "INDX"
readonly BLOCK_HEADER_SIZE=24           # sizeof(BlockHeader)
readonly INDEX_HEADER_SIZE=56           # sizeof(IndexHeader)
readonly INDEX_ENTRY_SIZE=24            # sizeof(MotionIndexEntry)
readonly BLOCK_TYPE_EVENT=2
readonly BLOCK_TYPE_SUMMARY=1

# ──────────────────────────────────────────────────────────────────────────────
# ANSI colours (disabled when stdout is not a terminal)
# ──────────────────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
    CR=$'\033[0m'  CB=$'\033[1m'  CC=$'\033[36m'
    CG=$'\033[32m' CY=$'\033[33m' CRED=$'\033[31m' CD=$'\033[2m'
else
    CR='' CB='' CC='' CG='' CY='' CRED='' CD=''
fi
export CR CB CC CG CY CRED CD

# ──────────────────────────────────────────────────────────────────────────────
# Shared awk helper functions (injected into every awk program via $AWK_COMMON)
# All operate on global array b[].
# ──────────────────────────────────────────────────────────────────────────────
AWK_COMMON='
function u16(i)  { return b[i] + b[i+1]*256 }
function u32(i)  { return b[i] + b[i+1]*256 + b[i+2]*65536 + b[i+3]*16777216 }
function u64(i) {
    return b[i]       + b[i+1]*256             + b[i+2]*65536          + b[i+3]*16777216 \
         + b[i+4]*4294967296  + b[i+5]*1099511627776 \
         + b[i+6]*281474976710656 + b[i+7]*72057594037927936
}
function ms2str(ms,   sec, rem, r, cmd) {
    if (ms == 0) return "0"
    sec = int(ms / 1000); rem = ms % 1000
    cmd = "date -u -d @" sec " +\"%Y-%m-%d %H:%M:%S\" 2>/dev/null"
    cmd | getline r; close(cmd)
    return sprintf("%s.%03dZ", r, rem)
}
function type_name(t) {
    if (t == 1) return "Summary"
    if (t == 2) return "Event"
    return sprintf("0x%04X", t)
}
'

file_size() { stat -c%s "$1" 2>/dev/null || wc -c < "$1"; }

# Pipe a file through od as a stream of unsigned decimal bytes (one per line)
od_bytes() { od -v -A n -t u1 -- "$1" | tr -s ' ' '\n' | grep -v '^$'; }

# ──────────────────────────────────────────────────────────────────────────────
# parse_idx <file>
# ──────────────────────────────────────────────────────────────────────────────
parse_idx() {
    local file="$1"
    local fsz; fsz=$(file_size "$file")
    printf "\n%s=== IDX: %s  (%s bytes) ===%s\n" "${CB}${CC}" "$file" "$fsz" "$CR"

    od_bytes "$file" | awk \
        -v IDX_MAGIC="$INDEX_MAGIC" \
        -v IDX_HDR="$INDEX_HEADER_SIZE" \
        -v ENTRY_SZ="$INDEX_ENTRY_SIZE" \
        -v EVT="$BLOCK_TYPE_EVENT" \
        'BEGIN {
            n = 0
            CR=ENVIRON["CR"]; CG=ENVIRON["CG"]; CY=ENVIRON["CY"]
            CRED=ENVIRON["CRED"]; CD=ENVIRON["CD"]
        }
        { b[n++] = $1 + 0 }
        '"$AWK_COMMON"'
        END {
            magic = u32(0)
            if (magic != IDX_MAGIC+0) {
                printf CRED "  [bad index magic %d, expected %d]" CR "\n", magic, IDX_MAGIC
                exit 1
            }
            version     = u16(4)
            entry_size  = u16(6)
            entry_count = u64(8)
            capacity    = u64(16)
            printf "  magic=0x%08X (INDX)  version=%d  entry_size=%d  entry_count=%.0f  capacity=%.0f\n", \
                magic, version, entry_size, entry_count, capacity
            if (entry_size != ENTRY_SZ+0)
                printf CY "  [warning: entry_size=%d, expected %d]" CR "\n", entry_size, ENTRY_SZ
            printf "\n"
            printf CD "  %4s  %18s  %-26s  %12s  %-8s\n" CR, \
                "#", "Stamp (ms)", "DateTime (UTC)", "Offset", "Type"
            printf CD "  %s\n" CR, "-----------------------------------------------------------------------"
            for (i = 0; i < entry_count; i++) {
                off    = IDX_HDR + i * ENTRY_SZ
                stamp  = u64(off)
                offset = u64(off+8)
                btype  = u16(off+16)
                tname  = type_name(btype)
                color  = (btype == EVT+0) ? CG : CY
                printf "  %4d  %18.0f  %-26s  %12.0f  " color "%-8s" CR "\n", \
                    i, stamp, ms2str(stamp), offset, tname
            }
            printf CG "\n  %.0f entries parsed.\n" CR, entry_count
        }'
}

# ──────────────────────────────────────────────────────────────────────────────
# parse_mblk <file> [verbose=0|1]
# ──────────────────────────────────────────────────────────────────────────────
parse_mblk() {
    local file="$1"
    local verbose="${2:-0}"
    local fsz; fsz=$(file_size "$file")
    printf "\n%s=== MBLK: %s  (%s bytes) ===%s\n" "${CB}${CC}" "$file" "$fsz" "$CR"
    printf "%s  %4s  %12s  %-8s  %18s  %-26s  %5s  %6s  %4s  %4s  %6s  %s%s\n" \
        "$CD" "#" "Offset" "Type" "Stamp (ms)" "DateTime (UTC)" \
        "hdrsz" "paysz" "Rows" "Cols" "Active" "Info" "$CR"
    printf "%s  %s%s\n" "$CD" "$(printf '%0.s-' {1..112})" "$CR"

    od_bytes "$file" | awk \
        -v MMAGIC="$MOTION_MAGIC" \
        -v BLK_HDR="$BLOCK_HEADER_SIZE" \
        -v EVT="$BLOCK_TYPE_EVENT" \
        -v SUM="$BLOCK_TYPE_SUMMARY" \
        -v verbose="$verbose" \
        'BEGIN {
            n = 0
            CR=ENVIRON["CR"]; CG=ENVIRON["CG"]; CY=ENVIRON["CY"]
            CRED=ENVIRON["CRED"]; CD=ENVIRON["CD"]
        }
        { b[n++] = $1 + 0 }
        '"$AWK_COMMON"'
        END {
            file_offset = 0; block_idx = 0; errors = 0
            while (file_offset + BLK_HDR <= n) {
                magic = u32(file_offset)
                if (magic != MMAGIC+0) {
                    printf CRED "  [bad magic %d at offset %d, expected %d]\n" CR, \
                        magic, file_offset, MMAGIC
                    errors++; break
                }
                btype    = u16(file_offset+4)
                hdr_size = u16(file_offset+6)
                pay_size = u32(file_offset+8)
                stamp    = u64(file_offset+12)
                crc_v    = u32(file_offset+20)
                ext_off  = file_offset + BLK_HDR
                ext_size = hdr_size - BLK_HDR
                rows = 0; cols = 0; active = 0
                info = sprintf("crc=0x%08X", crc_v)

                if (btype == EVT+0 && ext_size >= 8) {
                    rows   = u16(ext_off)
                    cols   = u16(ext_off+2)
                    active = u16(ext_off+4)
                } else if (btype == SUM+0 && ext_size >= 16) {
                    end_s  = u64(ext_off)
                    rows   = u16(ext_off+8)
                    cols   = u16(ext_off+10)
                    active = u16(ext_off+12)
                    info   = "end=" ms2str(end_s)
                }

                tname = type_name(btype)
                tcolor = (btype == EVT+0) ? CG : CY
                printf "  %4d  %12d  " tcolor "%-8s" CR "  %18.0f  %-26s  %5d  %6d  %4d  %4d  %6d  %s\n", \
                    block_idx, file_offset, tname, stamp, ms2str(stamp), \
                    hdr_size, pay_size, rows, cols, active, info

                if (verbose+0 == 1 && btype == EVT+0 && pay_size > 0) {
                    pay_off  = file_offset + hdr_size
                    dump_len = (pay_size > 256) ? 256 : pay_size
                    printf CD
                    for (bi = 0; bi < dump_len; bi++) {
                        if (bi % 16 == 0) printf "    "
                        printf "%02X ", b[pay_off + bi]
                        if (bi % 16 == 15) printf "\n"
                    }
                    if ((dump_len % 16) != 0) printf "\n"
                    if (pay_size > 256) printf "    ... (%d more bytes)\n", pay_size - 256
                    printf CR
                }

                file_offset += hdr_size + pay_size
                block_idx++
            }
            csum = (errors > 0) ? CRED : CG
            printf csum "\n  %d blocks parsed, %d error(s).\n" CR, block_idx, errors
        }'
}

# ──────────────────────────────────────────────────────────────────────────────
# cross_validate <file.mblk> <file.idx>
# ──────────────────────────────────────────────────────────────────────────────
cross_validate() {
    local mblk="$1" idx="$2"
    printf "\n%s=== Cross-validation ===%s\n" "${CB}${CC}" "$CR"

    # Load idx bytes in awk BEGIN via getline, then compare against mblk stream
    od_bytes "$mblk" | awk \
        -v idx_file="$idx" \
        -v MMAGIC="$MOTION_MAGIC" \
        -v BLK_HDR="$BLOCK_HEADER_SIZE" \
        -v IDX_HDR="$INDEX_HEADER_SIZE" \
        -v ENTRY_SZ="$INDEX_ENTRY_SIZE" \
        'BEGIN {
            CR=ENVIRON["CR"]; CG=ENVIRON["CG"]; CY=ENVIRON["CY"]; CRED=ENVIRON["CRED"]
            # Load index bytes
            cmd = "od -v -A n -t u1 -- \"" idx_file "\" | tr -s \" \" \"\\n\" | grep -v \"^$\""
            idx_n = 0
            while ((cmd | getline val) > 0) ib[idx_n++] = val + 0
            close(cmd)
            n = 0
        }
        { b[n++] = $1 + 0 }
        function u16b(i) { return b[i]  + b[i+1]*256 }
        function u32b(i) { return b[i]  + b[i+1]*256 + b[i+2]*65536 + b[i+3]*16777216 }
        function u64b(i) {
            return b[i]  + b[i+1]*256   + b[i+2]*65536          + b[i+3]*16777216 \
                 + b[i+4]*4294967296 + b[i+5]*1099511627776 \
                 + b[i+6]*281474976710656 + b[i+7]*72057594037927936
        }
        function u16i(i) { return ib[i] + ib[i+1]*256 }
        function u64i(i) {
            return ib[i] + ib[i+1]*256  + ib[i+2]*65536         + ib[i+3]*16777216 \
                 + ib[i+4]*4294967296 + ib[i+5]*1099511627776 \
                 + ib[i+6]*281474976710656 + ib[i+7]*72057594037927936
        }
        END {
            entry_count = u64i(8)
            mismatches = 0; blk_i = 0; file_offset = 0
            while (file_offset + BLK_HDR <= n && blk_i < entry_count) {
                magic = u32b(file_offset)
                if (magic != MMAGIC+0) break
                btype    = u16b(file_offset+4)
                hdr_size = u16b(file_offset+6)
                pay_size = u32b(file_offset+8)
                stamp    = u64b(file_offset+12)
                off       = IDX_HDR + blk_i * ENTRY_SZ
                idx_stamp = u64i(off)
                idx_off   = u64i(off+8)
                idx_type  = u16i(off+16)
                if (stamp != idx_stamp || file_offset != idx_off || btype != idx_type) {
                    printf CRED "  [#%d] mismatch: stamp=%.0f(blk)/%.0f(idx)  offset=%d(blk)/%.0f(idx)  type=%d(blk)/%d(idx)\n" CR, \
                        blk_i, stamp, idx_stamp, file_offset, idx_off, btype, idx_type
                    mismatches++
                }
                file_offset += hdr_size + pay_size
                blk_i++
            }
            if (mismatches == 0)
                printf CG "  All %d entries match.\n" CR, blk_i
            else
                printf CRED "  %d mismatch(es) found.\n" CR, mismatches
        }'
}

# ──────────────────────────────────────────────────────────────────────────────
# scan_directory <dir> [verbose]
# ──────────────────────────────────────────────────────────────────────────────
scan_directory() {
    local dir="$1" verbose="${2:-0}"
    local found=0
    for mblk in "$dir"/*.mblk; do
        [[ -f "$mblk" ]] || continue
        found=1
        parse_mblk "$mblk" "$verbose"
        local idx="${mblk%.mblk}.idx"
        if [[ -f "$idx" ]]; then
            parse_idx "$idx"
            cross_validate "$mblk" "$idx"
        else
            printf "%s  (no matching .idx for %s)%s\n" "$CY" "$(basename "$mblk")" "$CR"
        fi
    done
    [[ "$found" -eq 0 ]] && printf "%sNo .mblk files found in %s%s\n" "$CY" "$dir" "$CR"
}

# ──────────────────────────────────────────────────────────────────────────────
# usage
# ──────────────────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] PATH...

Parse S3MediaKit motion block (.mblk) and index (.idx) files.

Arguments:
  PATH    .mblk file, .idx file, or directory containing them

Options:
  -v      Verbose: hex-dump bitmap payload of Event blocks (max 256 bytes)
  -h      Show this help

Examples:
  $(basename "$0") 20260313.mblk
  $(basename "$0") 20260313.idx
  $(basename "$0") 20260313.mblk 20260313.idx
  $(basename "$0") /var/record/motion/live/cam1/
  $(basename "$0") -v 20260313.mblk
EOF
    exit 0
}

# ──────────────────────────────────────────────────────────────────────────────
# main
# ──────────────────────────────────────────────────────────────────────────────
main() {
    local verbose=0
    local -a mblk_files=() idx_files=() dirs=()

    while getopts "vh" opt; do
        case "$opt" in
            v) verbose=1 ;;
            h) usage ;;
            *) usage ;;
        esac
    done
    shift $(( OPTIND - 1 ))
    [[ $# -eq 0 ]] && usage

    for path in "$@"; do
        if [[ -d "$path" ]]; then
            dirs+=("$path")
        elif [[ "$path" == *.mblk ]]; then
            mblk_files+=("$path")
        elif [[ "$path" == *.idx ]]; then
            idx_files+=("$path")
        else
            printf "%sUnknown file type: %s%s\n" "$CRED" "$path" "$CR" >&2
            exit 1
        fi
    done

    if [[ ${#dirs[@]} -gt 0 ]]; then
        for d in "${dirs[@]}"; do scan_directory "$d" "$verbose"; done
    fi

    if [[ ${#mblk_files[@]} -eq 1 && ${#idx_files[@]} -eq 1 ]]; then
        # Paired: parse both + cross-validate
        parse_mblk "${mblk_files[0]}" "$verbose"
        parse_idx "${idx_files[0]}"
        cross_validate "${mblk_files[0]}" "${idx_files[0]}"
    else
        for f in "${mblk_files[@]+"${mblk_files[@]}"}"; do
            parse_mblk "$f" "$verbose"
            local idx="${f%.mblk}.idx"
            if [[ -f "$idx" ]]; then
                parse_idx "$idx"
                cross_validate "$f" "$idx"
            fi
        done
        for f in "${idx_files[@]+"${idx_files[@]}"}"; do
            parse_idx "$f"
        done
    fi
}

main "$@"
