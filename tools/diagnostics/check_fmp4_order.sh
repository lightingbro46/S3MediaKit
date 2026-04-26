#!/usr/bin/env bash
# ============================================================
# check_fmp4_order.sh
# Kết nối đến luồng FMP4 (VOD/live) của S3MediaKit và kiểm tra:
#   1. Trật tự box:  moov → moof+mdat → moov mới → moof+mdat
#   2. DTS (baseMediaDecodeTime trong moof/traf/tfdt) có tăng đều không
#      — detect backward DTS (lỗi), DTS stall, và DTS jump lớn (gap)
#      — phân biệt từng track_id; reset sau mỗi init segment (moov)
#
# Cách dùng:
#   ./check_fmp4_order.sh <URL> [max_moof]
#
# Ví dụ:
#   ./check_fmp4_order.sh http://127.0.0.1:8080/media/record/my_vod.live.mp4
#   ./check_fmp4_order.sh http://127.0.0.1:8080/media/record/my_vod.live.mp4 100
#
# URL format của S3MediaKit:  http://<host>:<port>/media/<app>/<stream>.live.mp4
# Ctrl-C để dừng bất cứ lúc nào — kết quả sẽ được in ra.
# ============================================================
set -euo pipefail

URL="${1:?Thiếu URL. VD: http://127.0.0.1:8080/media/record/stream.live.mp4}"
MAX_MOOF="${2:-0}"    # 0 = không giới hạn; dùng Ctrl-C để dừng

python3 - "$URL" "$MAX_MOOF" <<'PYEOF'
import sys
import struct
import urllib.request
import urllib.error
import time

url      = sys.argv[1]
max_moof = int(sys.argv[2])

# ── ANSI colours ────────────────────────────────────────────
RESET   = '\033[0m'
BOLD    = '\033[1m'
GREEN   = '\033[92m'
YELLOW  = '\033[93m'
RED     = '\033[91m'
CYAN    = '\033[96m'
DIM     = '\033[2m'
MAGENTA = '\033[95m'

def green(s):    return GREEN   + str(s) + RESET
def yellow(s):   return YELLOW  + str(s) + RESET
def red(s):      return RED     + str(s) + RESET
def cyan(s):     return CYAN    + str(s) + RESET
def bold(s):     return BOLD    + str(s) + RESET
def dim(s):      return DIM     + str(s) + RESET
def magenta(s):  return MAGENTA + str(s) + RESET

# ── Friendly box names ──────────────────────────────────────
BOX_LABEL = {
    b'ftyp': 'ftyp  (file-type)',
    b'moov': 'moov  *** INIT SEGMENT ***',
    b'moof': 'moof  (fragment header)',
    b'mdat': 'mdat  (media data)',
    b'styp': 'styp  (segment type)',
    b'sidx': 'sidx  (segment index)',
    b'free': 'free',
    b'skip': 'skip',
}

# ── Streaming MP4 box reader ────────────────────────────────
class BoxReader:
    def __init__(self, stream):
        self._s   = stream
        self._buf = bytearray()

    def _fill(self, n):
        while len(self._buf) < n:
            chunk = self._s.read(max(n - len(self._buf), 8192))
            if not chunk:
                raise EOFError("stream ended")
            self._buf.extend(chunk)

    def read_exact(self, n):
        self._fill(n)
        data = bytes(self._buf[:n])
        del self._buf[:n]
        return data

    def next_box(self):
        """Read header → return (btype, total_size, body_len).
        Does NOT consume the body; caller must call consume_body()."""
        hdr   = self.read_exact(8)
        size  = struct.unpack('>I', hdr[:4])[0]
        btype = hdr[4:8]

        if size == 1:            # 64-bit extended size
            ext      = self.read_exact(8)
            size     = struct.unpack('>Q', ext)[0]
            body_len = size - 16
        elif size == 0:          # extends to EOF
            return btype, 0, 0
        else:
            body_len = size - 8

        if body_len < 0:
            raise ValueError(f"Invalid box size {size} for type {btype}")
        return btype, size, body_len

    def consume_body(self, body_len, keep=False):
        """Consume body_len bytes.  keep=True → return bytes; else discard."""
        if body_len == 0:
            return b'' if keep else None
        if keep:
            return self.read_exact(body_len)
        remaining = body_len
        while remaining > 0:
            remaining -= len(self.read_exact(min(remaining, 65536)))
        return None


# ── moof DTS parser (moof/traf/tfhd + tfdt) ────────────────
def parse_moof_dts(payload):
    """Walk moof payload; return [(track_id_or_None, base_decode_time), …]."""
    results = []
    data    = payload
    off     = 0
    while off + 8 <= len(data):
        sz = struct.unpack('>I', data[off:off+4])[0]
        bt = data[off+4:off+8]
        if sz < 8 or off + sz > len(data):
            break
        body = data[off+8:off+sz]
        if bt == b'traf':
            track_id = None
            base_dts = None
            t_off    = 0
            while t_off + 8 <= len(body):
                s2 = struct.unpack('>I', body[t_off:t_off+4])[0]
                t2 = body[t_off+4:t_off+8]
                if s2 < 8 or t_off + s2 > len(body):
                    break
                b2 = body[t_off+8:t_off+s2]
                if t2 == b'tfhd' and len(b2) >= 8:
                    # tfhd is a FullBox: version(1) + flags(3) + track_ID(4)
                    track_id = struct.unpack('>I', b2[4:8])[0]
                elif t2 == b'tfdt' and len(b2) >= 4:
                    ver = b2[0]
                    if ver == 0 and len(b2) >= 8:
                        base_dts = struct.unpack('>I', b2[4:8])[0]
                    elif ver == 1 and len(b2) >= 12:
                        base_dts = struct.unpack('>Q', b2[4:12])[0]
                t_off += s2
            if base_dts is not None:
                results.append((track_id, base_dts))
        off += sz
    return results


# ── DTS monotonicity tracker ────────────────────────────────
# Threshold: warn if DTS jumps more than 10 s worth of ticks.
# For timescale=1000 (ms) that is 10 000 ticks.
# For timescale=90000 (video) that is 900 000 ticks.
# We detect timescale heuristically from the first delta.
DTS_JUMP_WARN_SEC = 10.0   # warn if gap > 10 s

class DtsTracker:
    def __init__(self):
        # track_id → (moof_idx, prev_dts, estimated_timescale)
        self._last         = {}
        self.dts_fail      = 0   # backward / stalled
        self.dts_warn      = 0   # large jump
        self.dts_ok        = 0   # normal increment
        self.issues        = []  # (level, msg)

    def reset(self):
        """Called on new moov; DTS may legitimately restart."""
        self._last.clear()

    def feed(self, moof_idx, td_list):
        """
        td_list: [(track_id, dts), …] from one moof.
        Returns coloured display tokens for each track.
        """
        tokens = []
        for tid, dts in td_list:
            key    = tid if tid is not None else '__unk__'
            ts_str = str(tid) if tid is not None else '?'

            if key not in self._last:
                # First observation for this track (or after codec change)
                self._last[key] = (moof_idx, dts, None)
                tokens.append(cyan(f't{ts_str}:{dts}(init)'))
                continue

            prev_idx, prev_dts, est_ts = self._last[key]
            delta = dts - prev_dts

            # Estimate timescale from first non-zero delta (capped to sane range)
            if est_ts is None and delta > 0:
                # Guess: if delta looks like ms (< 2000) → ts=1000
                # else if delta looks like 90kHz video → ts=90000, etc.
                if delta <= 2000:
                    est_ts = 1000
                elif delta <= 4000:
                    est_ts = 44100
                else:
                    est_ts = 90000
                self._last[key] = (moof_idx, dts, est_ts)
            elif est_ts is not None:
                self._last[key] = (moof_idx, dts, est_ts)

            jump_thresh = int(DTS_JUMP_WARN_SEC * (est_ts or 1000))

            if delta < 0:
                msg = (f'moof #{moof_idx} t{ts_str}: DTS BACKWARD '
                       f'{prev_dts} → {dts}  (Δ={delta})')
                self.issues.append(('FAIL', msg))
                self.dts_fail += 1
                tokens.append(red(f't{ts_str}:{dts}(Δ{delta}!)'))
            elif delta == 0:
                msg = (f'moof #{moof_idx} t{ts_str}: DTS STALL at {dts} '
                       f'(repeated for ≥2 fragments)')
                self.issues.append(('WARN', msg))
                self.dts_warn += 1
                tokens.append(yellow(f't{ts_str}:{dts}(Δ0!)'))
            elif delta > jump_thresh:
                gap_sec = delta / (est_ts or 1000)
                msg = (f'moof #{moof_idx} t{ts_str}: DTS LARGE JUMP '
                       f'{prev_dts} → {dts}  (Δ={delta}, ~{gap_sec:.1f}s)')
                self.issues.append(('WARN', msg))
                self.dts_warn += 1
                tokens.append(yellow(f't{ts_str}:{dts}(Δ+{delta}~{gap_sec:.0f}s)'))
            else:
                self.dts_ok += 1
                tokens.append(green(f't{ts_str}:{dts}(Δ+{delta})'))

        return tokens


# ── Box-order validator ─────────────────────────────────────
def validate_order(events):
    """
    Returns list of (status, message).
    status ∈ {'PASS', 'FAIL', 'WARN', 'INFO'}
    """
    results = []
    i = 0
    n = len(events)

    # Skip leading ftyp/styp/sidx/free
    while i < n and events[i] in ('ftyp', 'styp', 'sidx', 'free', 'skip'):
        i += 1

    if i >= n:
        results.append(('FAIL', 'Không nhận được box nào'))
        return results

    if events[i] != 'moov':
        results.append(('FAIL',
            f'Box đầu tiên không phải moov mà là "{events[i]}" '
            '— stream KHÔNG bắt đầu bằng init segment'))
        return results

    results.append(('PASS', f'[#{i}] Init segment đầu tiên (moov) đúng vị trí'))
    first_moov_idx = i
    i += 1

    # ── ≥1 moof+mdat pairs after first moov ─────────────────
    while i < n and events[i] in ('styp', 'sidx', 'free', 'skip'):
        i += 1

    seg_pairs   = 0
    bad_order   = False
    expect_mdat = False
    j = i
    while j < n and events[j] not in ('moov', 'ftyp'):
        if events[j] == 'moof':
            if expect_mdat:
                bad_order = True
            expect_mdat = True
            seg_pairs  += 1
        elif events[j] == 'mdat':
            if not expect_mdat:
                bad_order = True
            expect_mdat = False
        j += 1

    if seg_pairs == 0:
        results.append(('WARN',
            'Không có media segment (moof+mdat) nào sau init đầu tiên '
            '— stream chưa có dữ liệu hoặc kết nối quá ngắn'))
    else:
        status = 'PASS' if not bad_order else 'WARN'
        msg    = f'{seg_pairs} moof+mdat pair(s) sau init đầu tiên'
        if bad_order:
            msg += '  [!] moof/mdat không đúng cặp'
        results.append((status, msg))

    i = j

    # ── Second moov (codec/track change) ────────────────────
    second_moov_idx = None
    while i < n:
        if events[i] == 'moov':
            second_moov_idx = i
            break
        i += 1

    if second_moov_idx is None:
        total_moov = sum(1 for e in events if e == 'moov')
        results.append(('INFO',
            f'Chỉ có {total_moov} moov trong stream — '
            'codec không đổi trong khoảng dữ liệu nhận được'))
        return results

    between = [e for e in events[first_moov_idx+1:second_moov_idx]
               if e in ('moof', 'mdat')]
    if not between:
        results.append(('WARN',
            f'[#{second_moov_idx}] Init segment thứ hai nhận được '
            'nhưng KHÔNG có media segment nào ở giữa'))
    else:
        results.append(('PASS',
            f'[#{second_moov_idx}] Init segment thứ hai (moov) đúng '
            'sau các media segment — codec change OK'))

    i = second_moov_idx + 1

    # ── ≥1 moof+mdat after second moov ──────────────────────
    while i < n and events[i] in ('styp', 'sidx', 'free', 'skip'):
        i += 1

    seg_pairs2  = 0
    bad_order2  = False
    expect_mdat = False
    while i < n:
        if events[i] == 'moov':
            cnt = sum(1 for e in events[:i+1] if e == 'moov')
            results.append(('INFO',
                f'[#{i}] Init segment thứ {cnt} (codec đổi thêm lần nữa)'))
            expect_mdat = False
        elif events[i] == 'moof':
            if expect_mdat:
                bad_order2 = True
            expect_mdat = True
            seg_pairs2 += 1
        elif events[i] == 'mdat':
            if not expect_mdat:
                bad_order2 = True
            expect_mdat = False
        i += 1

    if seg_pairs2 == 0:
        results.append(('WARN',
            'Không nhận được media segment nào sau init segment mới'))
    else:
        status = 'PASS' if not bad_order2 else 'WARN'
        msg    = f'{seg_pairs2} moof+mdat pair(s) sau init segment mới'
        if bad_order2:
            msg += '  [!] moof/mdat không đúng thứ tự'
        results.append((status, msg))

    return results


# ── Main ────────────────────────────────────────────────────
def main():
    print(bold('=' * 72))
    print(bold('  FMP4 Stream Order + DTS Checker — S3MediaKit'))
    print(bold('=' * 72))
    print(f'  URL     : {cyan(url)}')
    print(f'  Max moof: {max_moof if max_moof > 0 else "unlimited (Ctrl-C để dừng)"}')
    print()

    req = urllib.request.Request(url, headers={
        'User-Agent': 'S3MediaKit-FMP4Checker/2.0',
        'Accept':     '*/*',
    })

    events     = []
    moof_count = 0
    init_count = 0
    start      = time.time()
    dts_track  = DtsTracker()

    print(f"  {'#':>5}  {'Box':<36}  {'Size':>10}  {'t(s)':>7}  DTS (per-track)")
    print('  ' + '-' * 80)

    try:
        with urllib.request.urlopen(req) as resp:
            print(f'  {green(f"HTTP {resp.status} {resp.reason}")}')
            ct = resp.headers.get('Content-Type', 'n/a')
            print(f'  Content-Type: {ct}')
            print()

            reader = BoxReader(resp)

            while True:
                if max_moof > 0 and moof_count >= max_moof:
                    print()
                    print(dim(f'  [dừng sau {max_moof} moof]'))
                    break

                try:
                    btype, size, body_len = reader.next_box()
                except EOFError:
                    print()
                    print(dim('  [stream kết thúc (EOF)]'))
                    break

                btype_str = btype.decode('ascii', errors='replace')
                label     = BOX_LABEL.get(btype, f'{btype_str}')
                elapsed   = time.time() - start
                idx       = len(events)
                events.append(btype_str)

                dts_tokens = []

                if btype == b'moof':
                    moof_count += 1
                    # Load moof payload (header-only box, typically <4 KB)
                    payload = reader.consume_body(body_len, keep=True)
                    td_list = parse_moof_dts(payload) if payload else []
                    dts_tokens = dts_track.feed(moof_count, td_list)
                    dts_str = '  ' + '  '.join(dts_tokens) if dts_tokens else ''
                    row = (f'  {idx:>5}  {label:<36}  {size:>10}  '
                           f'{elapsed:>6.2f}s  #{moof_count}{dts_str}')
                elif btype == b'moov':
                    reader.consume_body(body_len, keep=False)
                    init_count += 1
                    dts_track.reset()
                    label_colored = yellow(bold(f'#{init_count} {label}'))
                    # pad label_colored for alignment (ANSI codes add invisible chars)
                    row = (f'  {idx:>5}  {label_colored}  '
                           f'{size:>10}  {elapsed:>6.2f}s'
                           f'  {magenta("── new init segment ──")}')
                else:
                    reader.consume_body(body_len, keep=False)
                    row = f'  {idx:>5}  {dim(label):<46}  {size:>10}  {elapsed:>6.2f}s'

                print(row)

    except urllib.error.URLError as exc:
        print(red(f'  [Lỗi kết nối: {exc.reason}]'))
        sys.exit(2)
    except KeyboardInterrupt:
        print()
        print(yellow('  [ngắt bởi người dùng]'))

    # ── Results ──────────────────────────────────────────────
    print()
    print(bold('=' * 72))
    print(bold('  KẾT QUẢ KIỂM TRA'))
    print(bold('=' * 72))
    print()

    if not events:
        print(red('  FAIL: Không nhận được dữ liệu nào từ server'))
        sys.exit(1)

    moov_total = sum(1 for e in events if e == 'moov')
    moof_total = sum(1 for e in events if e == 'moof')
    print(f'  Tổng boxes: {len(events)}   moov: {moov_total}   moof: {moof_total}')
    print()

    # ── Box order ────────────────────────────────────────────
    print(bold('  [1] Kiểm tra thứ tự box'))
    order_results = validate_order(events)
    order_pass    = True
    for status, msg in order_results:
        if status == 'PASS':
            print(green(f'      ✓ PASS  {msg}'))
        elif status == 'FAIL':
            print(red(  f'      ✗ FAIL  {msg}'))
            order_pass = False
        elif status == 'WARN':
            print(yellow(f'      ! WARN  {msg}'))
        else:
            print(cyan(  f'      i INFO  {msg}'))
    print()

    # ── DTS monotonicity ─────────────────────────────────────
    print(bold('  [2] Kiểm tra DTS tăng đều'))
    dts_pass = (dts_track.dts_fail == 0)
    print(f'      OK: {dts_track.dts_ok}   WARN: {dts_track.dts_warn}   FAIL: {dts_track.dts_fail}')

    if dts_track.issues:
        print()
        for level, msg in dts_track.issues[:30]:   # cap display at 30
            if level == 'FAIL':
                print(red(  f'      ✗ {msg}'))
            else:
                print(yellow(f'      ! {msg}'))
        if len(dts_track.issues) > 30:
            print(dim(f'      … {len(dts_track.issues) - 30} thêm vấn đề (ẩn)'))
    else:
        print(green('      DTS tăng đơn điệu — không có lỗi'))
    print()

    # ── Overall ──────────────────────────────────────────────
    overall = order_pass and dts_pass
    if overall:
        print(bold(green('  OVERALL: PASS ✓')))
        sys.exit(0)
    else:
        parts = []
        if not order_pass: parts.append('thứ tự box sai')
        if not dts_pass:   parts.append('DTS backward')
        print(bold(red(f'  OVERALL: FAIL ✗  ({", ".join(parts)})')))
        sys.exit(1)


main()
PYEOF
