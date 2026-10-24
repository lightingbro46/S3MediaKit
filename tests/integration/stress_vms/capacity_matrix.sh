#!/usr/bin/env bash
# Dieu khien cac luot do nguong may Recording tu host (docs/recording-server-capacity-test.md muc 5).
# Moi kich ban la mot lan `docker run ... /opt/media/ccu_test.sh` (bundle da chep san trong image); chay
# mot hoac nhieu kich ban theo thu tu tren dong lenh, roi in bang ket qua va danh sach tarball can gui lai.
# CAP_DIR (mac dinh /data/cap) mount vao /work: thu muc lam viec ccu_work va tarball ket qua nam o do.
#
# Dung (tren host):
#   ./capacity_matrix.sh [tuy chon] <kich ban> [kich ban ...]
#   ./capacity_matrix.sh --list                 # liet ke kich ban
#   ./capacity_matrix.sh all                    # smoke G C4 C8 M16 M8
#   ./capacity_matrix.sh C4-b:200               # mot buoc, ep N = 200 camera
#   ./capacity_matrix.sh smoke G C8-a ab        # nhieu kich ban
#
# Kich ban:
#   smoke              20/50 camera, giu 60 s; truot thi dung ca chuoi
#   <L>                ca luot: a, b (G, H them c). L = G C4 C8 C24 M16 M8 H, va C28 (chi voi --client-host)
#   <L>-a              cam_step: nguong camera
#   <L>-b[:N]          live_step tren N camera; khong co N thi lay theo nguong <L>-a (300 neu >= 300,
#                      neu khong thi lam tron xuong boi cua 50), tu lan chay nay hoac tarball moi nhat
#                      trong CAP_DIR; khong co ca hai thi dung 300
#   <L>-c[:N]          live_replay (20% xem lai) tren N camera, cach chon N nhu b
#   ab                 baseline A/B (muc 5.6.2): Gc_base Gc_new Ga_base Ga_new
#   Gc_base Gc_new Ga_base Ga_new   tung lan cua ab
#   close              Ga_close4 Ga_close8 (record.closeThreads 4/8 o 700 camera)
#   dirty              Ga_dirty: dat sysctl dirty nho tren host, chay G 500-700 camera, tra lai (can root)
#   P6 (chay tren host, khong qua docker, can root; muc 5.6.1):
#   diskinfo           storcli64 / smartctl / lsblk -D / iostat: RAID, cache, model SSD, TRIM
#   trim               fstrim -v tren mount chua CAP_DIR (admin phai dong y truoc)
#   fio-seq            fio ghi tuan tu truc tiep 200 GB (~15-20 phut); chay lan 2 sau mot luot xoa lon
#   fio-rec            fio 16 job x 40 file ghi noi qua page cache, giong ghi hinh
#   p6                 diskinfo trim fio-seq fio-rec dirty
#   all                smoke G C4 C8 M16 M8
#
# Tuy chon:
#   --dry-run          chi in lenh
#   --suffix S         hau to them vao moi tag, vd _r3
#   --set k=v          ghi de config.ini cua MediaServer o moi kich ban (lap lai duoc)
#   --cap-dir DIR      thu muc mount vao /work, noi ghi ccu_work va tarball [/data/cap]
#   --hdd-dir DIR      thu muc tren HDD cho luot H, mount vao /hdd [/mnt/hdd/cap]
#   --image IMG        image test [s3mk-captest]
#   --bin PATH         MediaServer trong image [/opt/media/bin/MediaServer]
#   --ccu PATH         bundle ccu_test.sh trong image [/opt/media/ccu_test.sh]
#   --client-host H    nguon RTSP va viewer chay tren may H cung dai mang (ssh khong mat khau tu host,
#                      python3 >= 3.7): luong camera va luong xem di qua NIC that. Them _net vao moi tag,
#                      mount --ssh-dir vao /root/.ssh trong container
#   --ssh-dir DIR      khoa ssh va known_hosts dung de vao may client [$HOME/.ssh]
#   --viewer-workers N so worker viewer tren may client [nproc/2 cua may do]
#
# Ket qua: bang o cuoi man hinh va trong $CAP_DIR/matrix_<thoi gian>.txt; log tung kich ban va iostat
# luc ranh trong $CAP_DIR/matrix_logs_<thoi gian>/. Ctrl+C dung container va van in danh sach.
set -u

SELF="$0"
CAP_DIR=/data/cap
HDD_DIR=/mnt/hdd/cap
IMAGE=s3mk-captest
BIN=/opt/media/bin/MediaServer
CCU=/opt/media/ccu_test.sh
SUFFIX=""
DRY_RUN=0
SETS=()
CLIENT_HOST=""
SSH_DIR="$HOME/.ssh"
NET=()
NAME=s3mk-cap
CCU_STEPS_LIVE="0,100,200,300,400,500,600,750,1000,1250,1500,2000"
STAMP=$(date +%Y%m%d_%H%M%S)

usage() { sed -n '2,50p' "$SELF" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --suffix) SUFFIX="$2"; shift 2 ;;
        --set) SETS+=(--set "$2"); shift 2 ;;
        --cap-dir) CAP_DIR="$2"; shift 2 ;;
        --hdd-dir) HDD_DIR="$2"; shift 2 ;;
        --image) IMAGE="$2"; shift 2 ;;
        --bin) BIN="$2"; shift 2 ;;
        --ccu) CCU="$2"; shift 2 ;;
        --client-host) CLIENT_HOST="$2"; shift 2 ;;
        --ssh-dir) SSH_DIR="$2"; shift 2 ;;
        --viewer-workers) NET+=(--viewer-workers "$2"); shift 2 ;;
        --list|-h|--help) usage 0 ;;
        -*) echo "Tuy chon khong ro: $1"; usage 2 ;;
        *) ARGS+=("$1"); shift ;;
    esac
done
[ ${#ARGS[@]} -gt 0 ] || usage 2
if [ -n "$CLIENT_HOST" ]; then
    NET+=(--client-host "$CLIENT_HOST")
    SUFFIX="_net$SUFFIX"
else
    NET=()
fi

LIST="$CAP_DIR/matrix_$STAMP.txt"
LOGDIR="$CAP_DIR/matrix_logs_$STAMP"
G_CPUS=(--ms-cpus 0-7,16-23 --gen-cpus 8-15,24-31)
OFF=(--set record.replayPollerThreads=0 --set record.segmentStartStagger=0)

say() { echo "[matrix $(date +%H:%M:%S)] $*"; }

# Luot -> "ms_cpus|gen_cpus|docker_extra|tag goc"
lot_spec() {
    case "$1" in
        G)   echo "0-7,16-23|8-15,24-31||G_ssd_c16_m32" ;;
        C4)  echo "0-1,16-17|8-15,24-31||C4_ssd_c4_m32" ;;
        C8)  echo "0-3,16-19|8-15,24-31||C8_ssd_c8_m32" ;;
        C24) echo "0-11,16-27|12-15,28-31||C24_ssd_c24_m32" ;;
        # bo sinh tai o may client nen may nay chi con runner/mock/check: MediaServer lay 14/16 loi
        C28) echo "0-13,16-29|14-15,30-31||C28_ssd_c28_m32" ;;
        M16) echo "0-7,16-23|8-15,24-31|--memory 16g --memory-swap 16g|M16_ssd_c16_m16" ;;
        M8)  echo "0-7,16-23|8-15,24-31|--memory 8g --memory-swap 8g --shm-size=6g|M8_ssd_c16_m8" ;;
        H)   echo "0-7,16-23|8-15,24-31|-v $HDD_DIR:/hdd|H_hdd_c16_m32" ;;
        *)   return 1 ;;
    esac
}

# Mo rong tham so dong lenh thanh danh sach kich ban don
STEPS=()
expand() {
    local x="$1" lot
    case "$x" in
        all) for y in smoke G C4 C8 M16 M8; do expand "$y"; done ;;
        ab) STEPS+=(Gc_base Gc_new Ga_base Ga_new) ;;
        p6) STEPS+=(diskinfo trim fio-seq fio-rec Ga_dirty) ;;
        diskinfo|trim|fio-seq|fio-rec) STEPS+=("$x") ;;
        close) STEPS+=(Ga_close4 Ga_close8) ;;
        dirty|smoke|Gc_base|Gc_new|Ga_base|Ga_new|Ga_close4|Ga_close8|Ga_dirty)
            [ "$x" = dirty ] && x=Ga_dirty
            STEPS+=("$x") ;;
        *-a|*-b|*-b:*|*-c|*-c:*)
            lot="${x%%-*}"
            lot_spec "$lot" > /dev/null || { echo "Luot khong ro trong '$x'"; exit 2; }
            [ "$lot" = C28 ] && [ -z "$CLIENT_HOST" ] && { echo "C28 chi chay voi --client-host"; exit 2; }
            case "$x" in *-c*) case "$lot" in G|H) ;; *) echo "Buoc c chi co o G va H: $x"; exit 2 ;; esac ;; esac
            case "$x" in *:*) [[ "${x#*:}" =~ ^[1-9][0-9]*$ ]] || { echo "N khong hop le: $x"; exit 2; } ;; esac
            STEPS+=("$x") ;;
        *)
            lot_spec "$x" > /dev/null || { echo "Kich ban khong ro: $x (xem --list)"; exit 2; }
            [ "$x" = C28 ] && [ -z "$CLIENT_HOST" ] && { echo "C28 chi chay voi --client-host"; exit 2; }
            STEPS+=("$x-a" "$x-b")
            case "$x" in G|H) STEPS+=("$x-c") ;; esac ;;
    esac
}
for a in "${ARGS[@]}"; do expand "$a"; done

if [ "$DRY_RUN" != 1 ]; then
    command -v docker > /dev/null || { echo "Khong co docker"; exit 2; }
    mkdir -p "$CAP_DIR" || exit 2
    # bundle va MediaServer nam trong image; in commit cua bundle (BUNDLE_INFO trong payload) de doi
    # chieu voi commit cua binary
    BUNDLE_INFO=$(docker run --rm "$IMAGE" bash -c '
        [ -x "$1" ] || { echo "khong co $1"; exit 1; }
        [ -x "$2" ] || { echo "khong co $2"; exit 1; }
        sed -n "/^__CCU_PAYLOAD__\$/,\$p" "$1" | sed 1d | base64 -d 2>/dev/null \
            | tar -xzO ./BUNDLE_INFO 2>/dev/null | paste -sd ";" -' _ "$CCU" "$BIN" 2>&1) \
        || { echo "Image $IMAGE: $BUNDLE_INFO"; exit 2; }
    if docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
        echo "Container $NAME dang chay; dung no truoc."; exit 2
    fi
    case " ${STEPS[*]} " in *" H-"*) [ -d "$HDD_DIR" ] || { echo "Khong thay --hdd-dir $HDD_DIR"; exit 2; } ;; esac
    case " ${STEPS[*]} " in *" Ga_dirty "*|*" diskinfo "*|*" trim "*|*" fio-"*)
        [ "$(id -u)" = 0 ] || { echo "Ga_dirty va cac buoc P6 can root"; exit 2; } ;; esac
    case " ${STEPS[*]} " in *" fio-"*) command -v fio > /dev/null || { echo "Host chua cai fio"; exit 2; } ;; esac
    if [ -n "$CLIENT_HOST" ]; then
        # container dung khoa va known_hosts cua host: ssh mot lan tu host truoc de nhan host key
        [ -d "$SSH_DIR" ] || { echo "Khong thay --ssh-dir $SSH_DIR"; exit 2; }
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$CLIENT_HOST" true \
            || { echo "Khong ssh duoc (khong mat khau) tu host toi $CLIENT_HOST"; exit 2; }
        docker run --rm "$IMAGE" bash -c 'command -v ssh && command -v scp' > /dev/null \
            || { echo "Image $IMAGE thieu openssh-client (ssh/scp)"; exit 2; }
    fi
    mkdir -p "$LOGDIR"
fi
say "kich ban: ${STEPS[*]}"
[ "$DRY_RUN" = 1 ] || say "bundle $CCU: ${BUNDLE_INFO:-khong doc duoc BUNDLE_INFO}"

ROWS=""        # "kich ban|tag|ket qua|tarball" moi dong
declare -A TH  # nguong camera cua buoc a theo luot, trong lan chay nay
SYSCTL_OLD=""

add_row() { ROWS="$ROWS$1|$2|$3|$4"$'\n'; }

summary_of() { [ -n "$1" ] && [ -f "$1" ] && tar -xzOf "$1" --wildcards '*/SUMMARY.md' 2>/dev/null; }

cam_threshold() {
    summary_of "$1" | sed -n 's/.*Nguong camera[^:]*: \([0-9][0-9]*\).*/\1/p' | head -1
}

latest_tarball() {
    ls -t "$CAP_DIR"/ccu_result_*_"$1"_[0-9]*.tar.gz 2>/dev/null | head -1
}

verdict() {
    local tb="$1" rc="$2" s
    [ "$DRY_RUN" = 1 ] && { echo "-"; return; }
    [ -n "$tb" ] || { echo "loi (rc=$rc, khong co tarball)"; return; }
    s=$(summary_of "$tb")
    if echo "$s" | grep -q "Nguong camera"; then
        echo "$s" | sed -n 's/.*\*\*Nguong camera[^:]*: \([^*]*\)\*\* (\(.*\))/nguong \1 (\2)/p' | head -1
    elif echo "$s" | grep -q "Replay 20%"; then
        echo "$s" | grep -m2 -E "Replay 20%|Phiên xem lại đạt" | sed 's/^- \*\*//; s/\*\*//g' | paste -sd ';' -
    else
        echo "$s" | grep -m1 '^- \*\*CCU đạt' | sed 's/^- \*\*//; s/\*\*//g'
    fi
}

# run_one <kich ban> <tag> <docker extra> <tham so ccu_test.sh...>; dat TARBALL va RC
run_one() {
    local id="$1" tag="$2" extra="$3"; shift 3
    local cmd=(docker run --rm --name "$NAME" --network host --privileged
               --shm-size=20g --ulimit nofile=1048576:1048576)
    # shellcheck disable=SC2206
    [ -n "$extra" ] && cmd+=($extra)    # --shm-size o day ghi de gia tri mac dinh phia tren
    [ -n "$CLIENT_HOST" ] && cmd+=(-v "$SSH_DIR:/root/.ssh:ro")
    cmd+=(-v "$CAP_DIR:/work" -w /work "$IMAGE")
    TARBALL=""; RC=0
    if [ "$DRY_RUN" = 1 ]; then
        echo "${cmd[*]} $* ${SETS[*]} ${NET[*]} --tag $tag --yes"
        add_row "$id" "$tag" - -
        return 0
    fi
    say "===== $id ($tag) ====="
    iostat -x 5 3 > "$LOGDIR/$tag.iostat_idle.txt" 2>&1 || true
    local marker="$LOGDIR/.$tag.start"; : > "$marker"
    "${cmd[@]}" "$@" "${SETS[@]}" ${NET[@]+"${NET[@]}"} --tag "$tag" --yes > "$LOGDIR/$tag.log" 2>&1
    RC=$?
    TARBALL=$(find "$CAP_DIR" -maxdepth 1 -name "ccu_result_*_${tag}_[0-9]*.tar.gz" -newer "$marker" \
              2>/dev/null | sort | tail -1)
    say "$id xong rc=$RC, tarball: ${TARBALL:-KHONG CO} (log: $LOGDIR/$tag.log)"
    add_row "$id" "$tag" "$(verdict "$TARBALL" "$RC")" "${TARBALL:--}"
}

# N cho buoc b/c cua luot: N ep tren dong lenh > nguong a lan nay > tarball a moi nhat > 300
pick_n() {
    local lot="$1" forced="$2" base="$3" th=""
    if [ -n "$forced" ]; then echo "$forced"; return; fi
    th="${TH[$lot]:-}"
    if [ -z "$th" ] && [ "$DRY_RUN" != 1 ]; then th=$(cam_threshold "$(latest_tarball "$base$SUFFIX")"); fi
    if [ -z "$th" ]; then echo 300; return; fi
    if [ "$th" -ge 300 ]; then echo 300; else echo $(( th / 50 * 50 )); fi
}

# Buoc P6 tren host: ghi output vao $P6DIR, dong goi thanh mot tarball rieng o cuoi
P6DIR="$LOGDIR/p6"
FIO_SEQ_N=0
host_step() {
    local id="$1" out
    if [ "$DRY_RUN" = 1 ]; then
        case "$id" in
            diskinfo) echo "storcli64 /c0 show; storcli64 /c0/vall show all; storcli64 /c0/cv|bbu show all; smartctl -i -d megaraid,N /dev/sda; lsblk -D; iostat -x 5 12" ;;
            trim) echo "fstrim -v \$(df --output=target $CAP_DIR | tail -1)" ;;
            fio-seq) echo "fio --name=seq --rw=write --bs=1M --size=200G --direct=1 --ioengine=libaio --iodepth=16 (trong $CAP_DIR/fio)" ;;
            fio-rec) echo "fio --name=rec --rw=write --bs=64k --numjobs=16 --nrfiles=40 --size=12G --direct=0 --end_fsync=1 (trong $CAP_DIR/fio)" ;;
        esac
        add_row "$id" - - -; return 0
    fi
    mkdir -p "$P6DIR"
    say "===== $id (host) ====="
    case "$id" in
    diskinfo)
        out="$P6DIR/diskinfo.txt"
        {
            for c in "storcli64 /c0 show" "storcli64 /c0/vall show all" "storcli64 /c0/cv show all" \
                     "storcli64 /c0/bbu show all" "lsblk -D" "lsblk -o NAME,ROTA,SIZE,MODEL,MOUNTPOINT" \
                     "df -hT $CAP_DIR"; do
                echo "== $c"; $c 2>&1 || echo "(loi hoac khong co lenh)"
            done
            for i in 0 1 2 3 4 5 6 7; do
                echo "== smartctl -i -d megaraid,$i /dev/sda"; smartctl -i -d megaraid,"$i" /dev/sda 2>&1 | sed -n '4,20p'
            done
            echo "== iostat -x 5 12"; iostat -x 5 12 2>&1
        } > "$out" 2>&1
        add_row "$id" - "xem $(basename "$out")" - ;;
    trim)
        out="$P6DIR/fstrim.txt"
        local mnt; mnt=$(df --output=target "$CAP_DIR" | tail -1)
        { echo "== lsblk -D"; lsblk -D; echo "== fstrim -v $mnt"; fstrim -v "$mnt"; echo "rc=$?"; } > "$out" 2>&1
        add_row "$id" - "$(grep -m1 -E 'trimmed|not supported|fstrim:' "$out" | head -c 120)" - ;;
    fio-seq|fio-rec)
        mkdir -p "$CAP_DIR/fio"
        if [ "$id" = fio-seq ]; then
            FIO_SEQ_N=$((FIO_SEQ_N + 1)); out="$P6DIR/fio_seq_$FIO_SEQ_N.txt"
            ( cd "$CAP_DIR/fio" && fio --name=seq --rw=write --bs=1M --size=200G --direct=1 --ioengine=libaio \
                  --iodepth=16 --status-interval=30 --write_bw_log="$P6DIR/seq_$FIO_SEQ_N" --log_avg_msec=5000 ) > "$out" 2>&1
        else
            out="$P6DIR/fio_rec.txt"
            ( cd "$CAP_DIR/fio" && fio --name=rec --rw=write --bs=64k --numjobs=16 --nrfiles=40 --size=12G \
                  --direct=0 --end_fsync=1 --status-interval=30 ) > "$out" 2>&1
        fi
        rm -f "$CAP_DIR"/fio/seq.* "$CAP_DIR"/fio/rec.*; rmdir "$CAP_DIR/fio" 2>/dev/null
        add_row "$id" - "$(grep -m1 -E '^ *WRITE:' "$out" | sed 's/^ *//' | head -c 140)" - ;;
    esac
    say "$id xong: $out"
}

restore_sysctl() {
    [ -n "$SYSCTL_OLD" ] || return 0
    # ghi *_ratio thi kernel tu xoa *_bytes
    # shellcheck disable=SC2086
    sysctl -q -w $SYSCTL_OLD && say "da tra lai sysctl: $SYSCTL_OLD"
    SYSCTL_OLD=""
}

print_list() {
    if [ -d "$P6DIR" ]; then
        local tb="$CAP_DIR/p6_$(hostname -s)_$STAMP.tar.gz"
        cp "$LOGDIR"/*.iostat_idle.txt "$P6DIR"/ 2>/dev/null
        [ -f "$LOGDIR/sysctl_old.txt" ] && cp "$LOGDIR/sysctl_old.txt" "$P6DIR"/
        tar czf "$tb" -C "$LOGDIR" p6 && add_row "P6 (host)" - "output diskinfo/trim/fio" "$tb"
    fi
    {
        echo "# Do nguong - $(hostname -s), bat dau $STAMP, xong $(date +%Y%m%d_%H%M%S)"
        echo
        echo "Image $IMAGE, bundle $CCU: ${BUNDLE_INFO:--}"
        echo "Nguon RTSP + viewer: ${CLIENT_HOST:-cung may (loopback)}"
        echo
        echo "| kich ban | tag | ket qua | tarball |"
        echo "|---|---|---|---|"
        printf '%s' "$ROWS" | while IFS='|' read -r a b c d; do
            [ -n "$a" ] && echo "| $a | $b | $c | $(basename "$d") |"
        done
        echo
        echo "Tarball can gui lai:"
        printf '%s' "$ROWS" | cut -d'|' -f4 | grep -v -e '^-$' -e '^$' | while read -r f; do
            echo "  $f ($(du -h "$f" | cut -f1))"
        done
        echo "Log tung kich ban va iostat luc ranh: $LOGDIR"
    } | tee "$LIST"
    say "danh sach: $LIST"
}

trap 'say "dung theo yeu cau"; docker stop "$NAME" > /dev/null 2>&1; restore_sysctl;
      [ "$DRY_RUN" = 1 ] || print_list; exit 130' INT TERM

for id in "${STEPS[@]}"; do
    case "$id" in
    smoke)
        run_one smoke "smoke$SUFFIX" "" env CAM_HOLD_SEC=60 PHASE_SEC=60 "$CCU" "$BIN" \
            --only cam_step --cam-steps 20,50 "${G_CPUS[@]}"
        if [ "$DRY_RUN" != 1 ] && [ "$(cam_threshold "$TARBALL")" != 50 ]; then
            say "smoke khong dat 20 va 50 camera - dung. Xem $LOGDIR/smoke$SUFFIX.log"
            break
        fi ;;
    diskinfo|trim|fio-seq|fio-rec) host_step "$id" ;;
    Gc_base) run_one "$id" "Gc_base$SUFFIX" "" "$CCU" "$BIN" --only live_replay --cameras 300 "${G_CPUS[@]}" "${OFF[@]}" ;;
    Gc_new)  run_one "$id" "Gc_new$SUFFIX"  "" "$CCU" "$BIN" --only live_replay --cameras 300 "${G_CPUS[@]}" ;;
    Ga_base) run_one "$id" "Ga_base$SUFFIX" "" "$CCU" "$BIN" --only cam_step --cam-steps 500,600,700 "${G_CPUS[@]}" "${OFF[@]}" ;;
    Ga_new)  run_one "$id" "Ga_new$SUFFIX"  "" "$CCU" "$BIN" --only cam_step --cam-steps 500,600,700 "${G_CPUS[@]}" ;;
    Ga_close4|Ga_close8)
        run_one "$id" "$id$SUFFIX" "" "$CCU" "$BIN" --only cam_step --cam-steps 700 "${G_CPUS[@]}" \
            --set "record.closeThreads=${id#Ga_close}" ;;
    Ga_dirty)
        if [ "$DRY_RUN" = 1 ]; then
            echo "sysctl -w vm.dirty_background_bytes=268435456 vm.dirty_bytes=2147483648 vm.swappiness=10"
        else
            SYSCTL_OLD="vm.dirty_ratio=$(sysctl -n vm.dirty_ratio) vm.dirty_background_ratio=$(sysctl -n vm.dirty_background_ratio) vm.swappiness=$(sysctl -n vm.swappiness)"
            echo "$SYSCTL_OLD" > "$LOGDIR/sysctl_old.txt"
            sysctl -q -w vm.dirty_background_bytes=268435456 vm.dirty_bytes=2147483648 vm.swappiness=10
        fi
        run_one "$id" "Ga_dirty$SUFFIX" "" "$CCU" "$BIN" --only cam_step --cam-steps 500,600,700 "${G_CPUS[@]}"
        restore_sysctl ;;
    *)
        lot="${id%%-*}"; rest="${id#*-}"; step="${rest%%:*}"; forced=""
        case "$rest" in *:*) forced="${rest#*:}" ;; esac
        IFS='|' read -r ms gen extra base <<< "$(lot_spec "$lot")"
        cpus=(--ms-cpus "$ms" --gen-cpus "$gen")
        work=(); [ "$lot" = H ] && work=(--work /hdd/ccu_work)
        case "$step" in
        a)
            run_one "$id" "$base$SUFFIX" "$extra" "$CCU" "$BIN" --only cam_step "${cpus[@]}" "${work[@]}"
            [ "$DRY_RUN" = 1 ] || TH[$lot]=$(cam_threshold "$TARBALL")
            if [ "$DRY_RUN" != 1 ] && [ -z "${TH[$lot]}" ]; then
                say "$lot-a khong co bac dat"; TH[$lot]=0
            fi ;;
        b|c)
            n=$(pick_n "$lot" "$forced" "$base")
            if [ "$n" -lt 50 ]; then
                say "$id: nguong a < 50 camera - bo qua"; add_row "$id" - "bo qua (nguong a < 50)" -; continue
            fi
            sfx=""; [ "$n" != 300 ] && sfx="$n"
            if [ "$step" = b ]; then
                run_one "$id (N=$n)" "${base}_view$sfx$SUFFIX" "$extra" env CCU_STEPS="$CCU_STEPS_LIVE" \
                    "$CCU" "$BIN" --only live_step --cameras "$n" --accept 500 "${cpus[@]}" "${work[@]}"
            else
                run_one "$id (N=$n)" "${base}_replay$sfx$SUFFIX" "$extra" \
                    "$CCU" "$BIN" --only live_replay --cameras "$n" "${cpus[@]}" "${work[@]}"
            fi ;;
        esac ;;
    esac
done

[ "$DRY_RUN" = 1 ] && exit 0
print_list
