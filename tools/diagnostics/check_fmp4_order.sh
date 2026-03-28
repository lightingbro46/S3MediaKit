#!/usr/bin/env bash
# ============================================================
# check_fmp4_order.sh
# Kết nối đến luồng FMP4 (VOD/live) của S3MediaKit
# và kiểm tra trật tự:  init(moov) → moof+mdat → init mới(moov) → moof+mdat
#
# Cách dùng:
#   ./check_fmp4_order.sh <URL> [timeout_giây] [max_moof]
#
# Ví dụ:
#   ./check_fmp4_order.sh http://127.0.0.1:8080/media/record/my_vod.live.mp4
#   ./check_fmp4_order.sh http://127.0.0.1:8080/media/record/my_vod.live.mp4 60 100
#
# URL format của S3MediaKit:  http://<host>:<port>/media/<app>/<stream>.live.mp4
# ============================================================
set -euo pipefail

URL="${1:?Thiếu URL. VD: http://127.0.0.1:8080/media/record/stream.live.mp4}"
TIMEOUT="${2:-0}"     # (không dùng nữa — giữ tham số để tương thích ngược)
MAX_MOOF="${3:-0}"    # (không dùng nữa — script chạy đến khi bị ngắt/EOF)

python3 - "$URL" "$TIMEOUT" "$MAX_MOOF" <<'PYEOF'
import sys
import struct
import urllib.request
import urllib.error
import time

url      = sys.argv[1]
timeout  = float(sys.argv[2])
max_moof = int(sys.argv[3])

# ── ANSI colours ────────────────────────────────────────────
RESET  = '\033[0m'
BOLD   = '\033[1m'
GREEN  = '\033[92m'
YELLOW = '\033[93m'
RED    = '\033[91m'
CYAN   = '\033[96m'
DIM    = '\033[2m'

def green(s):  return GREEN  + s + RESET
def yellow(s): return YELLOW + s + RESET
def red(s):    return RED    + s + RESET
def cyan(s):   return CYAN   + s + RESET
def bold(s):   return BOLD   + s + RESET
def dim(s):    return DIM    + s + RESET

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
        """Returns (btype:bytes, total_size:int).
        Payload is consumed but not returned to keep memory low."""
        hdr = self.read_exact(8)
        size  = struct.unpack('>I', hdr[:4])[0]
        btype = hdr[4:8]

        if size == 1:            # 64-bit extended size
            ext       = self.read_exact(8)
            size      = struct.unpack('>Q', ext)[0]
            body_len  = size - 16
        elif size == 0:          # box extends to EOF (shouldn't happen in HTTP stream)
            self._s.read()
            return btype, 0
        else:
            body_len  = size - 8

        if body_len < 0:
            raise ValueError(f"Invalid box size {size} for type {btype}")

        # Consume payload in chunks (saves memory vs loading giant mdat)
        remaining = body_len
        while remaining > 0:
            chunk = self.read_exact(min(remaining, 65536))
            remaining -= len(chunk)

        return btype, size


# ── Sequence validator ──────────────────────────────────────
def validate(events):
    """
    events: list of box type strings in order received.
    Expected valid sequence:
        moov  (init #1)
        [moof mdat]+   (≥1 media segment)
        moov  (init #2 — codec/track change)
        [moof mdat]+   (≥1 media segment with new codec)

    Returns list of (status, message).
    status: 'PASS' | 'FAIL' | 'WARN' | 'INFO'
    """
    results = []
    i = 0
    n = len(events)

    # Skip leading ftyp/styp/sidx/free (allowed before first moov)
    while i < n and events[i] in ('ftyp', 'styp', 'sidx', 'free', 'skip'):
        i += 1

    # ── Check 1: first box must be moov ──────────────────────
    if i >= n:
        results.append(('FAIL', 'Không nhận được box nào'))
        return results

    if events[i] != 'moov':
        results.append(('FAIL',
            f'Box đầu tiên không phải moov mà là "{events[i]}" '
            f'— stream KHÔNG bắt đầu bằng init segment'))
        return results

    results.append(('PASS', f'[#{i}] Init segment đầu tiên (moov) đúng vị trí'))
    first_moov_idx = i
    i += 1

    # ── Check 2: ≥1 moof+mdat pairs after first moov ────────
    while i < n and events[i] in ('styp', 'sidx', 'free', 'skip'):
        i += 1

    seg_pairs = 0
    bad_order = False
    expect_mdat = False
    j = i
    while j < n and events[j] not in ('moov', 'ftyp'):
        if events[j] == 'moof':
            if expect_mdat:
                bad_order = True
            expect_mdat = True
            seg_pairs += 1
        elif events[j] == 'mdat':
            if not expect_mdat:
                bad_order = True
            expect_mdat = False
        j += 1

    if seg_pairs == 0:
        results.append(('WARN',
            'Không có media segment (moof+mdat) nào sau init đầu tiên '
            '— có thể stream chưa có dữ liệu'))
    else:
        status = 'PASS' if not bad_order else 'WARN'
        msg    = f'{seg_pairs} moof+mdat pair(s) sau init đầu tiên'
        if bad_order:
            msg += ' (cảnh báo: một số moof/mdat không theo đúng cặp)'
        results.append((status, msg))

    i = j

    # ── Check 3: second moov (codec/track change) ────────────
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

    # Kiểm tra không có moof/mdat giữa hai moov (sẽ là lỗi thứ tự)
    between = [e for e in events[first_moov_idx+1:second_moov_idx]
               if e in ('moof', 'mdat')]
    if not between:
        results.append(('WARN',
            f'[#{second_moov_idx}] Init segment thứ hai nhận được '
            'nhưng KHÔNG có media segment nào ở giữa — '
            'có thể codec đổi ngay lập tức (stream rất ngắn)'))
    else:
        results.append(('PASS',
            f'[#{second_moov_idx}] Init segment thứ hai (moov) nhận được '
            'đúng sau các media segment — codec change OK'))

    i = second_moov_idx + 1

    # ── Check 4: ≥1 moof+mdat sau moov thứ hai ──────────────
    while i < n and events[i] in ('styp', 'sidx', 'free', 'skip'):
        i += 1

    seg_pairs2  = 0
    bad_order2  = False
    expect_mdat = False
    while i < n:
        if events[i] == 'moov':
            # thêm moov nữa = codec đổi thêm lần nữa, chỉ ghi nhận
            results.append(('INFO',
                f'[#{i}] Init segment thứ {sum(1 for e in events[:i+1] if e=="moov")} '
                f'phát hiện (codec đổi nhiều lần)'))
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
            'Không nhận được media segment nào sau init segment mới '
            '— cần nhận thêm dữ liệu để xác nhận'))
    else:
        status = 'PASS' if not bad_order2 else 'WARN'
        msg    = (f'{seg_pairs2} moof+mdat pair(s) sau init segment '
                  'mới (codec mới) hoạt động đúng')
        if bad_order2:
            msg += ' (cảnh báo: một số moof/mdat không đúng thứ tự)'
        results.append((status, msg))

    return results


# ── Main ────────────────────────────────────────────────────
def main():
    print(bold('=' * 64))
    print(bold('  FMP4 Stream Order Checker — S3MediaKit'))
    print(bold('=' * 64))
    print(f'  URL     : {cyan(url)}')
    print(f'  Timeout : {timeout}s')
    print(f'  Max moof: {max_moof}')
    print()

    req = urllib.request.Request(url, headers={
        'User-Agent': 'S3MediaKit-FMP4Checker/1.0',
        'Accept':     '*/*',
    })

    events     = []   # list of box type strings (e.g. 'moov', 'moof', …)
    moof_count = 0
    start      = time.time()

    print(f"  {'#':>5}  {'Box type':<36}  {'Size':>10}  {'t(s)':>7}")
    print('  ' + '-' * 62)

    try:
        with urllib.request.urlopen(req) as resp:
            print(f'  {green(f"HTTP {resp.status} {resp.reason}")}')
            ct = resp.headers.get('Content-Type', 'n/a')
            print(f'  Content-Type: {ct}')
            print()

            reader = BoxReader(resp)

            while True:
                try:
                    btype, size = reader.next_box()
                except EOFError:
                    print()
                    print(dim('  [stream kết thúc (EOF)]'))
                    break

                btype_str = btype.decode('ascii', errors='replace')
                label     = BOX_LABEL.get(btype, f'{btype_str} (unknown)')
                elapsed   = time.time() - start
                idx       = len(events)
                events.append(btype_str)

                if btype == b'moov':
                    moov_n = sum(1 for e in events if e == 'moov')
                    row = (f'  {idx:>5}  '
                           f'{yellow(bold(f"#{moov_n} {label}")):<52}  '
                           f'{size:>10}  {elapsed:>6.2f}s')
                elif btype == b'moof':
                    moof_count += 1
                    row = (f'  {idx:>5}  {label:<36}  {size:>10}  '
                           f'{elapsed:>6.2f}s  (seg #{moof_count})')
                else:
                    row = f'  {idx:>5}  {dim(label):<46}  {size:>10}  {elapsed:>6.2f}s'

                print(row)

    except urllib.error.URLError as exc:
        print(red(f'  [Lỗi kết nối: {exc.reason}]'))
        sys.exit(2)
    except KeyboardInterrupt:
        print()
        print(yellow('  [ngắt bởi người dùng]'))

    # ── Print results ────────────────────────────────────────
    print()
    print(bold('=' * 64))
    print(bold('  KẾT QUẢ KIỂM TRA'))
    print(bold('=' * 64))
    print()

    if not events:
        print(red('  FAIL: Không nhận được dữ liệu nào từ server'))
        sys.exit(1)

    moov_total = sum(1 for e in events if e == 'moov')
    moof_total = sum(1 for e in events if e == 'moof')
    print(f'  Tổng boxes nhận: {len(events)}  |  '
          f'moov: {moov_total}  |  moof: {moof_total}')
    print()

    results  = validate(events)
    all_pass = True

    for status, msg in results:
        if status == 'PASS':
            print(green(f'  ✓ PASS  {msg}'))
        elif status == 'FAIL':
            print(red(  f'  ✗ FAIL  {msg}'))
            all_pass = False
        elif status == 'WARN':
            print(yellow(f'  ! WARN  {msg}'))
        else:
            print(cyan(  f'  i INFO  {msg}'))

    print()
    if all_pass:
        print(bold(green('  OVERALL: PASS — Thứ tự FMP4 hợp lệ ✓')))
        sys.exit(0)
    else:
        print(bold(red('  OVERALL: FAIL — Thứ tự FMP4 KHÔNG hợp lệ ✗')))
        sys.exit(1)


main()
PYEOF
