#!/usr/bin/env python3
"""Bộ sinh người xem (viewer) cho test đo ngưỡng CCU — docs/ccu-capacity-test-design.md.

Hai vai, cùng một file:

  controller  Giữ mức viewer mục tiêu, sinh "plan" (viewer thứ i xem gì), gộp số liệu từ
              worker. Điều khiển qua HTTP để worker ở máy khác cũng dùng được:
                GET  /plan            plan viewer (không đổi trong một lần chạy)
                GET  /target          mức mục tiêu + mốc cửa sổ đo
                PUT  /target          {"level", "join_rate", "churn_mean_sec", "stop"}
                POST /mark            mở cửa sổ đo mới, trả {"mark": id}
                POST /report          worker gửi số liệu
                GET  /stats           số liệu gộp của cửa sổ hiện tại
  worker      Mở/đóng viewer thật (asyncio) theo mục tiêu, đo và báo về controller.

Viewer i thuộc worker (i % tổng_worker). Mức N nghĩa là viewer 0..N-1 đang mở, nên tăng bậc
giữ nguyên viewer cũ, giảm bậc đóng viewer có chỉ số cao nhất.

Không giải mã video. Mỗi viewer đo từ lớp đóng gói:
  fMP4 (live, replay)  moof/tfdt + trun  -> thời gian media
  RTSP/TCP             RTP timestamp     -> thời gian media
  HLS                  #EXTINF           -> thời gian media, mỗi segment tải xong
rồi chạy một mô hình player (bộ đệm khởi động `prebuffer`) để ra thời gian khựng hình.

Dùng:
    viewer_load.py controller --port 18600 --inventory work/inventory.json \
        --host 127.0.0.1 --http-port 18081 --rtsp-port 18551 --local-workers 3
    viewer_load.py worker --control http://10.0.0.5:18600 --index 3 --total 5
"""
import argparse
import asyncio
import bisect
import json
import os
import random
import signal
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Cohort có nguồn ổn định (xem setup_stress.py). flaky/offline không dùng cho viewer: lỗi của
# nguồn sẽ bị tính nhầm thành lỗi phía phát.
LIVE_COHORTS = ("always", "office", "motion_night", "lowres_motion", "retention_auto",
                "long_retention", "no_record")
# Cohort ghi liên tục luồng chính cả ngày — replay luôn có dữ liệu.
REPLAY_COHORTS = ("always", "retention_auto", "long_retention")

MIXES = {
    # replay lấy đúng mức trần REPLAY_MAX_SHARE: trường hợp xấu nhất được phép ở production
    "vms": {"fmp4-sub": 45, "fmp4-main": 15, "hls": 10, "rtsp": 10, "replay": 20},
    "fmp4-sub": {"fmp4-sub": 100},
    "fmp4-main": {"fmp4-main": 100},
    "rtsp": {"rtsp": 100},
    "hls": {"hls": 100},
    "replay": {"replay": 100},
}
KINDS = ("fmp4-sub", "fmp4-main", "hls", "rtsp", "replay")
# Người xem replay chiếm tối đa 20% số người xem (điều kiện nghiệm thu). Áp cho mọi tiền tố của
# plan vì bậc N dùng N viewer đầu; hỗn hợp chỉ có replay (khoanh vùng) thì không áp.
REPLAY_MAX_SHARE = 0.20

TTFF_TIMEOUT = 15.0     # chưa có frame đầu sau chừng này thì tính là mở thất bại
DATA_TIMEOUT = 10.0     # đang phát mà không nhận gì chừng này thì coi như đứt
LONG_STALL = 2.0        # khựng dài hơn mức này là "khựng dài"
REPORT_SEC = 3.0


def now():
    return time.monotonic()


# ================================================================== plan
def build_plan(inv, mix, dist, max_viewers, seed, replay_earliest, single_camera=""):
    """Danh sách viewer, phần tử i = viewer thứ i. Xác định theo seed."""
    rng = random.Random(seed)
    by_cohort = inv.get("by_cohort", {})
    live = sorted(c for co in LIVE_COHORTS for c in by_cohort.get(co, []))
    replay = sorted(c for co in REPLAY_COHORTS for c in by_cohort.get(co, []))
    if single_camera:
        live = replay = [single_camera]
    if not live:
        raise SystemExit("inventory không có camera nào dùng được cho viewer")
    if not replay:
        replay = live

    def picker(cams):
        if dist == "uniform" or len(cams) == 1:
            return lambda: rng.choice(cams)
        # hot: Zipf s=1.1 trên thứ tự camera đã xáo — ~20% camera nhận ~80% viewer
        order = list(cams)
        rng.shuffle(order)
        weights = [1.0 / ((k + 1) ** 1.1) for k in range(len(order))]
        cum, acc = [], 0.0
        for w in weights:
            acc += w
            cum.append(acc)
        return lambda: order[min(len(order) - 1, bisect.bisect_left(cum, rng.random() * acc))]

    pick_live, pick_replay = picker(live), picker(replay)
    weights = MIXES[mix]
    kinds = sorted(weights)
    capped = [k for k in kinds if k != "replay"]
    total_w = float(sum(weights.values()))
    capped_w = float(sum(weights[k] for k in capped))
    plan = []
    n_replay = 0
    for i in range(max_viewers):
        r, acc, kind = rng.random() * total_w, 0.0, kinds[-1]
        for k in kinds:
            acc += weights[k]
            if r < acc:
                kind = k
                break
        if kind == "replay" and capped_w and n_replay + 1 > REPLAY_MAX_SHARE * (i + 1):
            r, acc, kind = rng.random() * capped_w, 0.0, capped[-1]
            for k in capped:
                acc += weights[k]
                if r < acc:
                    kind = k
                    break
        n_replay += kind == "replay"
        if kind == "replay":
            cam = pick_replay()
            stream = "main"
            # Lùi ít nhất 3 × fileSecond (60 s): replay đuổi theo bản ghi mới, mốc sát cạnh
            # bản ghi thì viewer phải chờ file đang ghi đóng lại -> đo nhầm thành khựng/trôi.
            back = rng.randint(180, 600)
            spec = {"kind": kind, "cam": cam, "stream": "%s-main" % cam, "back": back}
        else:
            cam = pick_live()
            if kind == "fmp4-sub":
                stream = "sub"
            elif kind == "fmp4-main":
                stream = "main"
            else:
                stream = rng.choice(("main", "sub"))
            spec = {"kind": kind, "cam": cam, "stream": "%s-%s" % (cam, stream)}
        plan.append(spec)
    return {"plan": plan, "replay_earliest": replay_earliest, "mix": mix, "dist": dist}


# ================================================================== player
class Player(object):
    """Mô hình player tối giản: phát theo đồng hồ thực, dừng khi hết dữ liệu đã nhận.

    `prebuffer` là độ trễ khởi động (giây media giữ sẵn trước khi phát). Khựng = thời gian
    đầu phát vượt quá phần media đã nhận.
    """

    def __init__(self, prebuffer):
        self.prebuffer = prebuffer
        self.started = False
        self.last = 0.0
        self.playhead = 0.0
        self.buffered = 0.0
        self.stall_sec = 0.0
        self.long_stalls = 0
        self.stalled = False
        self.stall_began = 0.0   # thời điểm (đồng hồ thực) bộ đệm cạn

    def advance(self, t):
        if not self.started:
            return
        dt = t - self.last
        self.last = t
        if dt <= 0:
            return
        cand = self.playhead + dt
        if cand > self.buffered:
            if not self.stalled:
                self.stalled = True
                self.stall_began = t - (cand - self.buffered)
            self.stall_sec += cand - max(self.playhead, self.buffered)
            self.playhead = self.buffered
        else:
            self.playhead = cand

    def on_media(self, t, start, end):
        if not self.started:
            self.started = True
            self.last = t
            self.playhead = start - self.prebuffer
            self.buffered = end
            return
        self.advance(t)
        if end > self.buffered:
            if self.stalled:
                if t - self.stall_began > LONG_STALL:
                    self.long_stalls += 1
                self.stalled = False
            self.buffered = end


# ================================================================== fMP4
def iter_boxes(data, off, end):
    while off + 8 <= end:
        size, typ = struct.unpack_from(">I4s", data, off)
        hdr = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, off + 8)[0]
            hdr = 16
        elif size == 0:
            size = end - off
        if size < hdr or off + size > end:
            return
        yield typ, off + hdr, off + size
        off += size


def find_box(data, off, end, typ):
    for t, s, e in iter_boxes(data, off, end):
        if t == typ:
            return s, e
    return None


class FMP4Parser(object):
    """Đọc luồng fMP4 tăng dần: chỉ giữ moov/moof, bỏ qua payload mdat mà không sao chép."""

    def __init__(self, on_fragment):
        self.buf = bytearray()
        self.skip = 0
        self.on_fragment = on_fragment
        self.video_track = None
        self.timescale = {}
        self.trex_duration = {}
        self.got_moov = False

    def feed(self, data):
        mv = memoryview(data)
        pos = 0
        n = len(data)
        while pos < n:
            if self.skip:
                k = min(self.skip, n - pos)
                self.skip -= k
                pos += k
                continue
            self.buf += mv[pos:]
            pos = n
            self._drain()

    def _drain(self):
        buf = self.buf
        off = 0
        while len(buf) - off >= 8:
            size, typ = struct.unpack_from(">I4s", buf, off)
            hdr = 8
            if size == 1:
                if len(buf) - off < 16:
                    break
                size = struct.unpack_from(">Q", buf, off + 8)[0]
                hdr = 16
            if size < hdr:
                raise ValueError("box hỏng: size=%d" % size)
            if typ in (b"moov", b"moof"):
                if len(buf) - off < size:
                    break
                if typ == b"moov":
                    self._parse_moov(buf, off + hdr, off + size)
                else:
                    self._parse_moof(buf, off + hdr, off + size)
                off += size
                continue
            avail = len(buf) - off
            if avail >= size:
                off += size
            else:
                self.skip = size - avail
                off = len(buf)
                break
        del buf[:off]

    def _parse_moov(self, d, s, e):
        self.got_moov = True
        tracks = []
        for t, ts, te in iter_boxes(d, s, e):
            if t == b"trak":
                tid, scale, handler = None, None, None
                tkhd = find_box(d, ts, te, b"tkhd")
                if tkhd:
                    ver = d[tkhd[0]]
                    tid = struct.unpack_from(">I", d, tkhd[0] + (20 if ver == 1 else 12))[0]
                mdia = find_box(d, ts, te, b"mdia")
                if mdia:
                    mdhd = find_box(d, mdia[0], mdia[1], b"mdhd")
                    if mdhd:
                        ver = d[mdhd[0]]
                        scale = struct.unpack_from(">I", d, mdhd[0] + (20 if ver == 1 else 12))[0]
                    hdlr = find_box(d, mdia[0], mdia[1], b"hdlr")
                    if hdlr:
                        handler = bytes(d[hdlr[0] + 8:hdlr[0] + 12])
                if tid is not None and scale:
                    tracks.append((tid, scale, handler))
            elif t == b"mvex":
                for mt, ms, me in iter_boxes(d, ts, te):
                    if mt == b"trex":
                        tid, _desc, dur = struct.unpack_from(">III", d, ms + 4)
                        self.trex_duration[tid] = dur
        for tid, scale, handler in tracks:
            self.timescale[tid] = scale
            if handler == b"vide" and self.video_track is None:
                self.video_track = tid
        if self.video_track is None and tracks:
            self.video_track = tracks[0][0]

    def _parse_moof(self, d, s, e):
        for t, ts, te in iter_boxes(d, s, e):
            if t != b"traf":
                continue
            tid, default_dur, base, total = None, None, None, 0
            for bt, bs, be in iter_boxes(d, ts, te):
                if bt == b"tfhd":
                    flags = struct.unpack_from(">I", d, bs)[0] & 0xFFFFFF
                    tid = struct.unpack_from(">I", d, bs + 4)[0]
                    p = bs + 8
                    if flags & 0x1:
                        p += 8
                    if flags & 0x2:
                        p += 4
                    if flags & 0x8:
                        default_dur = struct.unpack_from(">I", d, p)[0]
                elif bt == b"tfdt":
                    ver = d[bs]
                    base = (struct.unpack_from(">Q", d, bs + 4)[0] if ver == 1
                            else struct.unpack_from(">I", d, bs + 4)[0])
                elif bt == b"trun":
                    flags = struct.unpack_from(">I", d, bs)[0] & 0xFFFFFF
                    count = struct.unpack_from(">I", d, bs + 4)[0]
                    p = bs + 8
                    if flags & 0x1:
                        p += 4
                    if flags & 0x4:
                        p += 4
                    if flags & 0x100:
                        stride = 4 * sum(1 for f in (0x100, 0x200, 0x400, 0x800) if flags & f)
                        for k in range(count):
                            total += struct.unpack_from(">I", d, p + k * stride)[0]
                    else:
                        dur = default_dur if default_dur is not None else self.trex_duration.get(tid, 0)
                        total += dur * count
            if tid is None or base is None:
                continue
            if self.video_track is not None and tid != self.video_track:
                continue
            scale = float(self.timescale.get(tid) or 90000)
            self.on_fragment(base / scale, (base + total) / scale)


# ================================================================== HTTP
class HttpStream(object):
    """HTTP/1.1 tối giản trên asyncio: một request, đọc body dạng stream."""

    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.status = 0
        self.headers = {}
        self.chunked = False
        self.remaining = None   # None = đọc tới EOF
        self._chunk_left = 0
        self._done = False

    @classmethod
    async def open(cls, host, port, path, timeout, keep=None):
        if keep is not None:
            reader, writer = keep
        else:
            reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
        s = cls(reader, writer)
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: keep-alive\r\n"
               "User-Agent: s3mk-viewer-load\r\n\r\n" % (path, host, port))
        writer.write(req.encode())
        line = await asyncio.wait_for(reader.readline(), timeout)
        if not line:
            raise ConnectionError("server đóng kết nối trước khi trả lời")
        parts = line.split()
        s.status = int(parts[1]) if len(parts) > 1 else 0
        while True:
            hl = await asyncio.wait_for(reader.readline(), timeout)
            if not hl or hl in (b"\r\n", b"\n"):
                break
            k, _, v = hl.decode("latin-1").partition(":")
            s.headers[k.strip().lower()] = v.strip()
        if "chunked" in s.headers.get("transfer-encoding", "").lower():
            s.chunked = True
        elif "content-length" in s.headers:
            s.remaining = int(s.headers["content-length"])
        return s

    async def read(self, timeout, n=65536):
        """Trả bytes của body; b"" khi hết body."""
        if self._done:
            return b""
        if self.chunked:
            if self._chunk_left == 0:
                size_line = await asyncio.wait_for(self.reader.readline(), timeout)
                if not size_line:
                    self._done = True
                    return b""
                size = int(size_line.strip().split(b";")[0] or b"0", 16)
                if size == 0:
                    await asyncio.wait_for(self.reader.readline(), timeout)
                    self._done = True
                    return b""
                self._chunk_left = size
            data = await asyncio.wait_for(self.reader.read(min(n, self._chunk_left)), timeout)
            if not data:
                self._done = True
                return b""
            self._chunk_left -= len(data)
            if self._chunk_left == 0:
                await asyncio.wait_for(self.reader.readexactly(2), timeout)
            return data
        if self.remaining is not None:
            if self.remaining <= 0:
                self._done = True
                return b""
            data = await asyncio.wait_for(self.reader.read(min(n, self.remaining)), timeout)
            if not data:
                self._done = True
                return b""
            self.remaining -= len(data)
            return data
        data = await asyncio.wait_for(self.reader.read(n), timeout)
        if not data:
            self._done = True
        return data

    async def read_all(self, timeout):
        out = []
        while True:
            d = await self.read(timeout)
            if not d:
                return b"".join(out)
            out.append(d)

    def close(self):
        try:
            self.writer.close()
        except (OSError, RuntimeError):
            pass


class Rejected(Exception):
    """Server từ chối có chủ đích (HTTP/RTSP status khác 200)."""


# ================================================================== viewer
class Viewer(object):
    def __init__(self, worker, slot, spec):
        self.w = worker
        self.slot = slot
        self.spec = spec
        self.kind = spec["kind"]
        self.player = Player(1.0 if self.kind == "hls" else 0.5)
        self.task = None
        self.closing = False
        self.playing = False
        self.t_first = None
        self.bytes = 0
        self.window = None  # (t, stall_sec, long_stalls, buffered) lúc bắt đầu cửa sổ
        self.last_data = now()
        self.seg_dur = 0.0  # HLS: media đến theo từng segment

    # ---- đo
    def _first_media(self, t, t_start):
        self.playing = True
        self.t_first = t
        self.w.on_join_ok(self, (t - t_start) * 1000.0)

    def media(self, start, end):
        t = now()
        self.last_data = t
        self.player.on_media(t, start, end)
        if self.window is None and self.w.mark is not None:
            p = self.player
            self.window = (t, p.stall_sec, p.long_stalls, p.buffered)

    def window_row(self, t):
        """(kind, giây xem, giây khựng, số lần khựng dài, trôi, tốc độ media) trong cửa sổ."""
        if not self.playing or self.window is None:
            return None
        p = self.player
        p.advance(t)
        t0, s0, l0, b0 = self.window
        watch = t - t0
        if watch < 1.0:
            return None
        media = p.buffered - b0
        drift = watch - media
        if self.seg_dur:
            # HLS nhận media theo bậc một segment: lệch trong ±1 segment là lượng tử, không phải trôi
            q = self.seg_dur
            drift = 0.0 if abs(drift) <= q else drift - q * (1 if drift > 0 else -1)
        return [self.kind, round(watch, 2), round(p.stall_sec - s0, 3), p.long_stalls - l0,
                round(drift, 3), round((watch - drift) / watch, 4)]

    # ---- vòng đời
    def url_query(self, extra=None):
        q = {}
        if self.w.token:
            q["token"] = self.w.token
        if extra:
            q.update(extra)
        return ("?" + urllib.parse.urlencode(q)) if q else ""

    async def run(self):
        backoff = 1.0
        while not self.closing:
            t_start = now()
            reason = None
            try:
                if self.kind in ("fmp4-sub", "fmp4-main", "replay"):
                    await self._run_fmp4(t_start)
                elif self.kind == "rtsp":
                    await self._run_rtsp(t_start)
                else:
                    await self._run_hls(t_start)
                reason = "eof"
            except asyncio.CancelledError:
                raise
            except Rejected as exc:
                reason = str(exc)
            except asyncio.TimeoutError:
                reason = "ttff_timeout" if self.t_first is None else "data_timeout"
            except (ConnectionError, OSError, asyncio.IncompleteReadError) as exc:
                reason = "conn:" + type(exc).__name__
            except (ValueError, IndexError, struct.error) as exc:
                reason = "parse:" + type(exc).__name__
            if self.closing:
                break
            if self.t_first is None:
                self.w.on_join_fail(self, reason)
            else:
                self.w.on_unexpected_close(self, reason)
            # Player thật thử lại; giữ nguyên slot để mức CCU không bị hụt.
            self.playing = False
            self.t_first = None
            self.window = None
            self.player = Player(self.player.prebuffer)
            await asyncio.sleep(backoff * (0.5 + random.random()))
            backoff = min(backoff * 2, 10.0)

    async def _stream_loop(self, read, t_start):
        while not self.closing:
            timeout = TTFF_TIMEOUT if self.t_first is None else DATA_TIMEOUT
            if self.t_first is None:
                timeout = max(0.1, TTFF_TIMEOUT - (now() - t_start))
            data = await read(timeout)
            if not data:
                return
            self.bytes += len(data)
            self.w.bytes += len(data)
            yield data

    async def _run_fmp4(self, t_start):
        w = self.w
        if self.kind == "replay":
            stamp = int(time.time()) - self.spec.get("back", 120)
            earliest = w.replay_earliest
            if earliest and stamp < earliest:
                stamp = earliest
            path = "/media/record/%s/%s/vod/%d.live.mp4" % (self.spec["cam"], self.spec["stream"], stamp)
        else:
            path = "/media/live/%s/%s.live.mp4" % (self.spec["cam"], self.spec["stream"])
        path += self.url_query()
        s = await HttpStream.open(w.host, w.http_port, path, TTFF_TIMEOUT)
        try:
            if s.status != 200:
                body = b""
                try:
                    body = await asyncio.wait_for(s.read_all(3), 3)
                except (asyncio.TimeoutError, ConnectionError, OSError, ValueError):
                    pass
                raise Rejected("http%d%s" % (s.status, short_reason(body)))
            parser = FMP4Parser(lambda a, b: self._on_frag(a, b, t_start))
            async for data in self._stream_loop(s.read, t_start):
                parser.feed(data)
        finally:
            s.close()

    def _on_frag(self, start, end, t_start):
        if self.t_first is None:
            self._first_media(now(), t_start)
        self.media(start, end)

    async def _run_rtsp(self, t_start):
        w = self.w
        url = "rtsp://%s:%d/%s/%s%s" % (w.host, w.rtsp_port, self.spec["cam"], self.spec["stream"],
                                       self.url_query())
        reader, writer = await asyncio.wait_for(asyncio.open_connection(w.host, w.rtsp_port), TTFF_TIMEOUT)
        cseq = [0]

        async def request(method, target, headers=None):
            cseq[0] += 1
            lines = ["%s %s RTSP/1.0" % (method, target), "CSeq: %d" % cseq[0],
                     "User-Agent: s3mk-viewer-load"]
            for k, v in (headers or {}).items():
                lines.append("%s: %s" % (k, v))
            writer.write(("\r\n".join(lines) + "\r\n\r\n").encode())
            status, hdrs, body = await read_rtsp_response(reader, TTFF_TIMEOUT)
            if status != 200:
                raise Rejected("rtsp%d" % status)
            return hdrs, body

        try:
            hdrs, body = await request("DESCRIBE", url, {"Accept": "application/sdp"})
            base = hdrs.get("content-base") or url
            control, clock = sdp_video_control(body.decode("utf-8", "replace"))
            if control.startswith("rtsp://"):
                track_url = control
            else:
                b = base.split("?", 1)[0]
                track_url = b + ("" if b.endswith("/") else "/") + control
            hdrs, _ = await request("SETUP", track_url,
                                    {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
            session = hdrs.get("session", "").split(";")[0].strip()
            await request("PLAY", base, {"Session": session, "Range": "npt=0.000-"})

            buf = bytearray()
            unwrap = [None, 0]  # ts trước, phần bù tràn 32 bit
            last_keepalive = now()
            async for data in self._stream_loop(lambda t: asyncio.wait_for(reader.read(65536), t), t_start):
                buf += data
                off = 0
                n = len(buf)
                while off < n:
                    if buf[off] == 0x24:  # '$'
                        if n - off < 4:
                            break
                        ch, ln = buf[off + 1], (buf[off + 2] << 8) | buf[off + 3]
                        if n - off < 4 + ln:
                            break
                        if ch == 0 and ln >= 12:
                            ts = struct.unpack_from(">I", buf, off + 8)[0]
                            prev = unwrap[0]
                            if prev is not None and ts < prev and prev - ts > 0x80000000:
                                unwrap[1] += 1 << 32
                            unwrap[0] = ts
                            m = (ts + unwrap[1]) / float(clock)
                            if self.t_first is None:
                                self._first_media(now(), t_start)
                            self.media(m, m)
                        off += 4 + ln
                    else:
                        # Trả lời RTSP chen giữa luồng (keepalive): bỏ qua cả header và body
                        end = buf.find(b"\r\n\r\n", off)
                        if end < 0:
                            break
                        head = bytes(buf[off:end]).decode("latin-1")
                        clen = 0
                        for line in head.split("\r\n"):
                            if line.lower().startswith("content-length:"):
                                clen = int(line.split(":", 1)[1])
                        if n - (end + 4) < clen:
                            break
                        off = end + 4 + clen
                del buf[:off]
                t = now()
                if t - last_keepalive > 25:
                    last_keepalive = t
                    cseq[0] += 1
                    writer.write(("GET_PARAMETER %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n"
                                  % (base, cseq[0], session)).encode())
            if self.closing:
                cseq[0] += 1
                writer.write(("TEARDOWN %s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n"
                              % (base, cseq[0], session)).encode())
        finally:
            try:
                writer.close()
            except (OSError, RuntimeError):
                pass

    async def _run_hls(self, t_start):
        w = self.w
        sid = "ccu-%d-%d-%d" % (w.index, self.slot, random.randint(0, 1 << 30))
        pl_path = ("/media/live/%s/%s/hls.m3u8" % (self.spec["cam"], self.spec["stream"])
                   + self.url_query({"session_id": sid}))
        conn = [None]

        async def get(path, timeout):
            keep = conn[0]
            for attempt in (0, 1):
                try:
                    s = await HttpStream.open(w.host, w.http_port, path, timeout, keep=keep)
                    body = await s.read_all(timeout)
                    if s.headers.get("connection", "").lower() == "close":
                        s.close()
                        conn[0] = None
                    else:
                        conn[0] = (s.reader, s.writer)
                    return s, body
                except (ConnectionError, OSError, asyncio.IncompleteReadError):
                    if keep is None or attempt:
                        raise
                    keep = conn[0] = None

        last_seq = None
        cursor = 0.0
        seg_dur = 2.0
        try:
            while not self.closing:
                timeout = DATA_TIMEOUT if self.t_first else max(0.1, TTFF_TIMEOUT - (now() - t_start))
                s, body = await get(pl_path, timeout)
                if s.status in (301, 302) and s.headers.get("location"):
                    pl_path = relative_path(pl_path, s.headers["location"])
                    continue
                if s.status != 200:
                    raise Rejected("http%d%s" % (s.status, short_reason(body)))
                self.bytes += len(body)
                w.bytes += len(body)
                seq0, target, segs = parse_m3u8(body.decode("utf-8", "replace"))
                seg_dur = target or seg_dur
                self.seg_dur = seg_dur
                if not segs:
                    await asyncio.sleep(0.5)
                    if now() - t_start > TTFF_TIMEOUT and self.t_first is None:
                        raise asyncio.TimeoutError()
                    continue
                if last_seq is None:
                    # Như player thật: bắt đầu cách cạnh live 3 segment.
                    first = max(0, len(segs) - 3)
                    last_seq = seq0 + first - 1
                progressed = False
                for k, (dur, uri) in enumerate(segs):
                    seq = seq0 + k
                    if seq <= last_seq:
                        continue
                    if last_seq is not None and seq > last_seq + 1 and self.t_first is not None:
                        w.count_event("hls_skipped_segments", seq - last_seq - 1)
                    seg_path = relative_path(pl_path, uri)
                    if "session_id=" not in seg_path:
                        seg_path += ("&" if "?" in seg_path else "?") + "session_id=" + sid
                    ss, data = await get(seg_path, DATA_TIMEOUT)
                    if ss.status != 200:
                        raise Rejected("seg_http%d" % ss.status)
                    self.bytes += len(data)
                    w.bytes += len(data)
                    self.last_data = now()
                    if self.t_first is None:
                        self._first_media(now(), t_start)
                    self.media(cursor, cursor + dur)
                    cursor += dur
                    last_seq = seq
                    progressed = True
                if not progressed:
                    if now() - self.last_data > DATA_TIMEOUT and self.t_first is not None:
                        raise asyncio.TimeoutError()
                    await asyncio.sleep(max(0.3, seg_dur / 2.0))
        finally:
            if conn[0]:
                try:
                    conn[0][1].close()
                except (OSError, RuntimeError):
                    pass

    def stop(self):
        self.closing = True
        if self.task:
            self.task.cancel()


def short_reason(body):
    try:
        obj = json.loads(body.decode("utf-8", "replace"))
        msg = obj.get("msg") or obj.get("message") or ""
        return (":" + str(msg)[:40]) if msg else ""
    except (ValueError, AttributeError):
        text = body.decode("utf-8", "replace").strip()
        return (":" + text[:40]) if text else ""


def relative_path(base_path, ref):
    if ref.startswith("http://") or ref.startswith("https://"):
        p = urllib.parse.urlsplit(ref)
        return p.path + (("?" + p.query) if p.query else "")
    if ref.startswith("/"):
        return ref
    base = base_path.split("?", 1)[0]
    return base.rsplit("/", 1)[0] + "/" + ref


def parse_m3u8(text):
    seq0, target, segs, dur = 0, 0.0, [], None
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#EXT-X-MEDIA-SEQUENCE:"):
            seq0 = int(line.split(":", 1)[1])
        elif line.startswith("#EXT-X-TARGETDURATION:"):
            target = float(line.split(":", 1)[1])
        elif line.startswith("#EXTINF:"):
            dur = float(line.split(":", 1)[1].split(",", 1)[0])
        elif line and not line.startswith("#"):
            segs.append((dur if dur is not None else target, line))
            dur = None
    return seq0, target, segs


async def read_rtsp_response(reader, timeout):
    line = await asyncio.wait_for(reader.readline(), timeout)
    if not line:
        raise ConnectionError("RTSP server đóng kết nối")
    parts = line.split()
    status = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    hdrs = {}
    while True:
        hl = await asyncio.wait_for(reader.readline(), timeout)
        if not hl or hl in (b"\r\n", b"\n"):
            break
        k, _, v = hl.decode("latin-1").partition(":")
        hdrs[k.strip().lower()] = v.strip()
    body = b""
    clen = int(hdrs.get("content-length", "0") or 0)
    if clen:
        body = await asyncio.wait_for(reader.readexactly(clen), timeout)
    return status, hdrs, body


def sdp_video_control(sdp):
    control, clock, in_video = None, 90000, False
    for line in sdp.splitlines():
        line = line.strip()
        if line.startswith("m="):
            in_video = line.startswith("m=video")
        elif in_video and line.startswith("a=control:") and control is None:
            control = line.split(":", 1)[1]
        elif in_video and line.startswith("a=rtpmap:"):
            try:
                clock = int(line.split("/")[1].split()[0])
            except (IndexError, ValueError):
                pass
    if control is None:
        raise ValueError("SDP không có track video")
    return control, clock


# ================================================================== worker
class Worker(object):
    def __init__(self, args):
        self.args = args
        self.index = args.index
        self.total = args.total
        self.control = args.control.rstrip("/")
        self.viewers = {}
        self.plan = []
        self.replay_earliest = 0
        self.host = self.http_port = self.rtsp_port = None
        self.token = ""
        if args.token_file:
            with open(args.token_file) as fh:
                self.token = fh.read().strip()
        self.mark = None
        self.target = {"level": 0}
        self.bytes = 0
        self._reset_window()
        self.loop_lag = []

    def _reset_window(self):
        self.joins = defaultdict(lambda: {"ok": 0, "fail": 0})
        self.fail_reasons = defaultdict(int)
        self.closes = defaultdict(int)
        self.ttff = defaultdict(list)
        self.events = defaultdict(int)
        self.bytes = 0
        self.intended_closes = 0
        for v in self.viewers.values():
            v.window = None
            if v.playing:
                p = v.player
                p.advance(now())
                v.window = (now(), p.stall_sec, p.long_stalls, p.buffered)
        self.window_t = now()
        self.cpu0 = time.process_time()
        self.loop_lag = []

    # ---- callback từ viewer
    def on_join_ok(self, v, ttff_ms):
        self.joins[v.kind]["ok"] += 1
        self.ttff[v.kind].append(round(ttff_ms, 1))

    def on_join_fail(self, v, reason):
        self.joins[v.kind]["fail"] += 1
        self.fail_reasons["%s:%s" % (v.kind, reason)] += 1

    def on_unexpected_close(self, v, reason):
        self.closes["%s:%s" % (v.kind, reason)] += 1

    def count_event(self, name, n=1):
        self.events[name] += n

    # ---- điều khiển
    def http(self, method, path, payload=None, timeout=10):
        data = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(self.control + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode())

    async def call(self, method, path, payload=None):
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self.http, method, path, payload)

    def my_slots(self, level):
        return range(self.index, level, self.total)

    async def control_loop(self):
        tokens = 0.0
        last = now()
        lost_since = None
        while True:
            try:
                tgt = await self.call("GET", "/target")
                lost_since = None
            except (OSError, ValueError) as exc:
                # controller đã tắt hẳn: tự dừng thay vì treo cùng viewer đang mở
                lost_since = lost_since or now()
                if now() - lost_since > 30:
                    print("[worker %d] mất controller quá 30s, dừng" % self.index, flush=True)
                    return
                print("[worker %d] mất controller: %s" % (self.index, exc), flush=True)
                await asyncio.sleep(1)
                continue
            if tgt.get("stop"):
                return
            if tgt.get("mark") != self.mark:
                self.mark = tgt.get("mark")
                self._reset_window()
            self.target = tgt
            level = min(int(tgt.get("level", 0)), len(self.plan))
            wanted = set(self.my_slots(level))
            # đóng slot vượt mức (chỉ số cao trước)
            for slot in sorted(set(self.viewers) - wanted, reverse=True):
                v = self.viewers.pop(slot)
                v.stop()
                self.intended_closes += 1
            # mở slot thiếu theo tốc độ vào cho phép
            rate = float(tgt.get("join_rate") or 20.0) / self.total
            t = now()
            tokens = min(tokens + (t - last) * rate, max(1.0, rate))
            last = t
            for slot in sorted(wanted - set(self.viewers)):
                if tokens < 1.0:
                    break
                tokens -= 1.0
                self.start_viewer(slot)
            # churn: viewer xem xong thì thoát, slot mở lại bằng phiên mới
            churn = float(tgt.get("churn_mean_sec") or 0)
            if churn > 0:
                for slot, v in list(self.viewers.items()):
                    if v.t_first is not None and not hasattr(v, "leave_at"):
                        v.leave_at = v.t_first + random.expovariate(1.0 / churn)
                    if getattr(v, "leave_at", None) and now() >= v.leave_at:
                        v.stop()
                        self.viewers.pop(slot)
                        self.intended_closes += 1
                        self.events["churn_leave"] += 1
            await asyncio.sleep(0.2 if wanted - set(self.viewers) else 1.0)

    def start_viewer(self, slot):
        v = Viewer(self, slot, self.plan[slot])
        v.task = asyncio.ensure_future(v.run())
        self.viewers[slot] = v

    async def lag_probe(self):
        while True:
            t = now()
            await asyncio.sleep(0.1)
            self.loop_lag.append((now() - t - 0.1) * 1000.0)
            if len(self.loop_lag) > 5000:
                self.loop_lag = self.loop_lag[-2500:]

    def report(self):
        t = now()
        span = max(0.001, t - self.window_t)
        rows = []
        active = defaultdict(int)
        playing = defaultdict(int)
        for v in self.viewers.values():
            active[v.kind] += 1
            if v.playing:
                playing[v.kind] += 1
                r = v.window_row(t)
                if r:
                    rows.append(r)
        lag = sorted(self.loop_lag)
        return {
            "worker": self.index, "host": os.uname()[1], "pid": os.getpid(), "mark": self.mark,
            "span_sec": round(span, 1),
            "cpu_pct": round((time.process_time() - self.cpu0) / span * 100.0, 1),
            "loop_lag_p99_ms": round(lag[int(len(lag) * 0.99)] if lag else 0.0, 1),
            "active": dict(active), "playing": dict(playing),
            "joins": {k: dict(v) for k, v in self.joins.items()},
            "fail_reasons": dict(self.fail_reasons), "closes": dict(self.closes),
            "events": dict(self.events), "intended_closes": self.intended_closes,
            "ttff_ms": {k: v for k, v in self.ttff.items()},
            "bytes": self.bytes, "viewers": rows,
        }

    async def report_loop(self):
        while True:
            await asyncio.sleep(REPORT_SEC)
            try:
                await self.call("POST", "/report", self.report())
            except (OSError, ValueError) as exc:
                print("[worker %d] gửi report lỗi: %s" % (self.index, exc), flush=True)

    async def main(self):
        for _ in range(60):
            try:
                cfg = await self.call("GET", "/plan")
                break
            except (OSError, ValueError):
                await asyncio.sleep(1)
        else:
            raise SystemExit("không lấy được plan từ controller")
        self.plan = cfg["plan"]
        self.replay_earliest = cfg.get("replay_earliest") or 0
        self.host, self.http_port, self.rtsp_port = cfg["host"], cfg["http_port"], cfg["rtsp_port"]
        if not self.token and cfg.get("token"):
            self.token = cfg["token"]
        bg = [asyncio.ensure_future(x) for x in (self.lag_probe(), self.report_loop())]
        try:
            await self.control_loop()
        finally:
            for v in list(self.viewers.values()):
                v.stop()
            try:
                await self.call("POST", "/report", self.report())
            except (OSError, ValueError):
                pass
            for b in bg:
                b.cancel()


# ================================================================== controller
class Controller(object):
    def __init__(self, args):
        self.args = args
        inv = json.load(open(args.inventory))
        self.plan = build_plan(inv, args.mix, args.dist, args.max_viewers, args.seed,
                               args.replay_earliest, args.single_camera)
        self.plan.update({"host": args.host, "http_port": args.http_port, "rtsp_port": args.rtsp_port})
        if args.token_file:
            with open(args.token_file) as fh:
                self.plan["token"] = fh.read().strip()
        self.lock = threading.Lock()
        self.target = {"level": 0, "join_rate": args.join_rate, "churn_mean_sec": 0,
                       "mark": 0, "stop": False}
        self.reports = {}
        self.procs = []

    def merged(self):
        with self.lock:
            mark = self.target["mark"]
            reps = dict(self.reports)
        t = time.time()
        out = {"mark": mark, "level": self.target["level"], "workers_expected": self.args.total,
               "workers": [], "active": defaultdict(int), "playing": defaultdict(int),
               "joins": defaultdict(lambda: {"ok": 0, "fail": 0}), "fail_reasons": defaultdict(int),
               "closes": defaultdict(int), "events": defaultdict(int), "ttff_ms": defaultdict(list),
               "bytes": 0, "viewers": [], "intended_closes": 0, "span_sec": 0.0}
        for idx, (at, r) in sorted(reps.items()):
            fresh = t - at < 3 * REPORT_SEC + 2
            out["workers"].append({"worker": idx, "host": r.get("host"), "fresh": fresh,
                                   "same_mark": r.get("mark") == mark, "cpu_pct": r.get("cpu_pct"),
                                   "loop_lag_p99_ms": r.get("loop_lag_p99_ms")})
            if r.get("mark") != mark:
                continue
            out["span_sec"] = max(out["span_sec"], r.get("span_sec", 0))
            for key in ("active", "playing", "fail_reasons", "closes", "events"):
                for k, v in (r.get(key) or {}).items():
                    out[key][k] += v
            for k, v in (r.get("joins") or {}).items():
                out["joins"][k]["ok"] += v.get("ok", 0)
                out["joins"][k]["fail"] += v.get("fail", 0)
            for k, v in (r.get("ttff_ms") or {}).items():
                out["ttff_ms"][k].extend(v)
            out["bytes"] += r.get("bytes", 0)
            out["intended_closes"] += r.get("intended_closes", 0)
            out["viewers"].extend(r.get("viewers") or [])
        for key in ("active", "playing", "fail_reasons", "closes", "events", "ttff_ms"):
            out[key] = dict(out[key])
        out["joins"] = {k: dict(v) for k, v in out["joins"].items()}
        return out

    def handler(self):
        ctl = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def _send(self, obj, code=200):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _body(self):
                n = int(self.headers.get("Content-Length") or 0)
                return json.loads(self.rfile.read(n) or b"{}") if n else {}

            def do_GET(self):
                if self.path == "/plan":
                    return self._send(ctl.plan)
                if self.path == "/target":
                    with ctl.lock:
                        return self._send(dict(ctl.target))
                if self.path == "/stats":
                    return self._send(ctl.merged())
                self._send({"error": "not found"}, 404)

            def do_PUT(self):
                if self.path != "/target":
                    return self._send({"error": "not found"}, 404)
                body = self._body()
                with ctl.lock:
                    for k in ("level", "join_rate", "churn_mean_sec", "stop"):
                        if k in body:
                            ctl.target[k] = body[k]
                    self._send(dict(ctl.target))

            def do_POST(self):
                if self.path == "/mark":
                    with ctl.lock:
                        ctl.target["mark"] += 1
                        return self._send({"mark": ctl.target["mark"]})
                if self.path == "/report":
                    r = self._body()
                    with ctl.lock:
                        ctl.reports[r.get("worker")] = (time.time(), r)
                    return self._send({"ok": 1})
                self._send({"error": "not found"}, 404)

            def log_message(self, fmt, *a):
                pass

        return H

    def spawn_local(self):
        for i in range(self.args.local_workers):
            cmd = [sys.executable, os.path.abspath(__file__), "worker",
                   "--control", "http://127.0.0.1:%d" % self.args.port,
                   "--index", str(self.args.first_index + i), "--total", str(self.args.total)]
            log = open(os.path.join(self.args.log_dir, "viewer_worker_%d.log"
                                    % (self.args.first_index + i)), "a")
            self.procs.append(subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT))

    def run(self):
        srv = ThreadingHTTPServer(("0.0.0.0", self.args.port), self.handler())
        srv.daemon_threads = True

        def stop(*_):
            with self.lock:
                self.target["stop"] = True

            def later():
                # giữ HTTP tới khi worker cục bộ đọc được "stop" và thoát (tối đa 10 s)
                deadline = time.time() + 10
                while time.time() < deadline and any(p.poll() is None for p in self.procs):
                    time.sleep(0.2)
                srv.shutdown()
            threading.Thread(target=later, daemon=True).start()

        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        self.spawn_local()
        print("[viewer] controller :%d, plan %d viewer (mix=%s dist=%s), %d worker cục bộ / %d tổng"
              % (self.args.port, len(self.plan["plan"]), self.args.mix, self.args.dist,
                 self.args.local_workers, self.args.total), flush=True)
        try:
            srv.serve_forever(poll_interval=0.5)
        finally:
            # cho worker thấy stop và đóng viewer gọn gàng
            deadline = time.time() + 10
            for p in self.procs:
                try:
                    p.wait(timeout=max(0.1, deadline - time.time()))
                except subprocess.TimeoutExpired:
                    p.kill()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="role")
    c = sub.add_parser("controller")
    c.add_argument("--port", type=int, default=18600)
    c.add_argument("--inventory", required=True)
    c.add_argument("--host", default="127.0.0.1", help="địa chỉ MediaServer mà viewer kết nối tới")
    c.add_argument("--http-port", type=int, required=True)
    c.add_argument("--rtsp-port", type=int, required=True)
    c.add_argument("--mix", default="vms", choices=sorted(MIXES))
    c.add_argument("--dist", default="hot", choices=("hot", "uniform"))
    c.add_argument("--single-camera", default="", help="dồn mọi viewer vào một camera (kiểm giới hạn theo camera)")
    c.add_argument("--max-viewers", type=int, default=5000)
    c.add_argument("--seed", type=int, default=20260929)
    c.add_argument("--join-rate", type=float, default=20.0)
    c.add_argument("--replay-earliest", type=int, default=0, help="epoch sớm nhất có bản ghi")
    c.add_argument("--local-workers", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    c.add_argument("--total", type=int, default=0, help="tổng worker kể cả máy khác (mặc định = cục bộ)")
    c.add_argument("--first-index", type=int, default=0)
    c.add_argument("--token-file", default="")
    c.add_argument("--log-dir", default=".")
    w = sub.add_parser("worker")
    w.add_argument("--control", required=True)
    w.add_argument("--index", type=int, required=True)
    w.add_argument("--total", type=int, required=True)
    w.add_argument("--token-file", default="")
    args = ap.parse_args()
    if args.role == "controller":
        if not args.total:
            args.total = args.local_workers
        Controller(args).run()
    elif args.role == "worker":
        try:
            asyncio.run(Worker(args).main())
        except KeyboardInterrupt:
            pass
    else:
        ap.print_help()
        sys.exit(2)


if __name__ == "__main__":
    main()
