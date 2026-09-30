#!/usr/bin/env python3
"""Máy phát RTSP tổng hợp cho stress test: phục vụ N luồng từ một file H.264 dựng sẵn.

Mỗi URL (bất kể path) đều được chấp nhận và trả về cùng một vòng lặp video đã được
đóng gói RTP sẵn, nên chi phí CPU cho mỗi luồng chỉ là vá seq/timestamp rồi ghi socket.
Chỉ hỗ trợ RTP/AVP/TCP (interleaved) để không phải mở thêm cổng UDP cho 600 luồng.

Dùng:
    rtsp_source.py <h264_file> [--sub-media <h264_file>] [--port 8554] [--fps 5]
                   [--procs N] [--stats-sec 10] [--drop-spec PATH] [--state PATH]

--sub-media: luồng có path kết thúc bằng "-sub" phát file này (luồng phụ độ phân giải thấp),
còn lại phát <h264_file>. Không có thì mọi luồng phát cùng một file.

Mọi phiên phát chung một bộ đếm frame: frame hiện tại được đóng gói RTP **một lần** cho mỗi
file rồi ghi cùng bytes đó cho mọi phiên (seq/timestamp liên tục với từng phiên vì bộ đếm chỉ
tăng). Phiên mới vào giữa GOP như camera thật; server chờ keyframe kế tiếp.

--procs N: N tiến trình con cùng nghe một cổng (SO_REUSEPORT, kernel chia kết nối). Tiến trình
cha gộp state của các con vào --state. State có thêm `tick_late_p99_ms` (độ lệch nhịp gửi) và
`cpu_pct_max` (tiến trình con bận nhất, % một lõi) — hai số này cho biết chính nguồn có phải
nút cổ chai không (docs/ccu-capacity-test-design.md mục 2.1).

--drop-spec trỏ tới file JSON do kịch bản ghi ra để mô phỏng camera lỗi:
    {"offline": ["cam-0007-main"], "flaky": ["cam-0012-sub"], "flaky_sec": 20}
File được đọc lại mỗi giây, cho phép đổi điều kiện giữa chừng mà không phải restart.
"""
import argparse
import asyncio
import base64
import json
import os
import struct
import sys
import time

MTU_PAYLOAD = 1400
CLOCK_RATE = 90000


# ---------------------------------------------------------------- H.264 parsing
def split_annexb(data):
    """Tách bytes Annex-B thành danh sách NAL (không gồm start code)."""
    nals, i, n = [], 0, len(data)
    start = None
    while i < n - 2:
        if data[i] == 0 and data[i + 1] == 0:
            if data[i + 2] == 1:
                sc = 3
            elif i < n - 3 and data[i + 2] == 0 and data[i + 3] == 1:
                sc = 4
            else:
                i += 1
                continue
            if start is not None:
                nals.append(data[start:i])
            i += sc
            start = i
            continue
        i += 1
    if start is not None:
        nals.append(data[start:])
    return [x for x in nals if x]


def group_access_units(nals):
    """Gộp NAL thành access unit: SPS/PPS/SEI đi kèm VCL NAL ngay sau nó."""
    aus, pending = [], []
    for nal in nals:
        t = nal[0] & 0x1F
        pending.append(nal)
        if t in (1, 5):  # VCL NAL kết thúc một access unit
            aus.append(pending)
            pending = []
    if pending:
        aus.append(pending)
    return aus


def packetize(nal, seq_ts_marker):
    """Trả về danh sách payload RTP (chưa có header) cho một NAL."""
    if len(nal) <= MTU_PAYLOAD:
        return [nal]
    # FU-A
    indicator = (nal[0] & 0xE0) | 28
    nal_type = nal[0] & 0x1F
    body, out, first = nal[1:], [], True
    step = MTU_PAYLOAD - 2
    for off in range(0, len(body), step):
        chunk = body[off:off + step]
        last = off + step >= len(body)
        header = (0x80 if first else 0) | (0x40 if last else 0) | nal_type
        out.append(bytes([indicator, header]) + chunk)
        first = False
    return out


class Loop(object):
    """Vòng lặp video đã đóng gói sẵn thành các gói RTP theo từng frame."""

    def __init__(self, path, fps):
        raw = open(path, "rb").read()
        nals = split_annexb(raw)
        self.sps = next((n for n in nals if (n[0] & 0x1F) == 7), None)
        self.pps = next((n for n in nals if (n[0] & 0x1F) == 8), None)
        if not self.sps or not self.pps:
            raise SystemExit("Không tìm thấy SPS/PPS trong %s" % path)
        self.fps = fps
        self.ts_step = CLOCK_RATE // fps
        # frames[i] = danh sách (payload, marker)
        self.frames = []
        for au in group_access_units(nals):
            pkts = []
            for nal in au:
                for payload in packetize(nal, None):
                    pkts.append(payload)
            if not pkts:
                continue
            self.frames.append(pkts)
        if not self.frames:
            raise SystemExit("Không dựng được frame nào từ %s" % path)

    def sdp(self, session_ctrl):
        profile = "".join("%02x" % b for b in self.sps[1:4])
        sprop = "%s,%s" % (base64.b64encode(self.sps).decode(),
                           base64.b64encode(self.pps).decode())
        return "\r\n".join([
            "v=0",
            "o=- 0 0 IN IP4 127.0.0.1",
            "s=S3MediaKit stress source",
            "c=IN IP4 0.0.0.0",
            "t=0 0",
            "a=control:*",
            "m=video 0 RTP/AVP 96",
            "a=rtpmap:96 H264/90000",
            "a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s" % (profile, sprop),
            "a=control:%s" % session_ctrl,
            "",
        ])


# ---------------------------------------------------------------- session state
class Session(object):
    __slots__ = ("sid", "path", "writer", "channel", "media", "playing", "started",
                 "packets", "closed")

    def __init__(self, sid, path, writer, media):
        self.sid = sid
        self.path = path
        self.writer = writer
        self.channel = 0
        self.media = media
        self.playing = False
        self.started = 0.0
        self.packets = 0
        self.closed = False


class Server(object):
    SSRC = 0x53334D4B

    def __init__(self, loop_media, args, sub_media=None):
        self.media = loop_media
        self.sub_media = sub_media or loop_media
        self.args = args
        # bộ đếm chung: frame thứ bao nhiêu, seq/ts kế tiếp của từng file
        self.frame_no = 0
        self.seq = {id(self.media): 0, id(self.sub_media): 0}
        self.ts = {id(self.media): 0, id(self.sub_media): 0}
        self.late = []
        self.sessions = {}
        self.next_sid = 1
        self.drop = {"offline": set(), "flaky": set(), "flaky_sec": 30}
        self.drop_mtime = None
        self.stat_packets = 0
        self.stat_bytes = 0
        self.stat_plays = 0
        self.stat_teardowns = 0
        self.started = time.time()

    # -------------------------------------------------- điều kiện lỗi từ file
    def reload_drop_spec(self):
        path = self.args.drop_spec
        if not path:
            return
        try:
            st = os.stat(path)
        except OSError:
            return
        if self.drop_mtime == st.st_mtime:
            return
        self.drop_mtime = st.st_mtime
        try:
            spec = json.load(open(path))
        except (ValueError, OSError):
            return
        self.drop = {
            "offline": set(spec.get("offline", [])),
            "flaky": set(spec.get("flaky", [])),
            "flaky_sec": float(spec.get("flaky_sec", 30)),
        }

    def stream_key(self, path):
        """Tên luồng trong URL. URL của SETUP/PLAY là <content-base>/trackID=0, nên phải bỏ
        thành phần track, nếu không mọi phiên đều mang khoá "trackID=0" (luồng -sub phát nhầm
        file chính, danh sách flaky không bao giờ khớp)."""
        parts = path.split("?", 1)[0].rstrip("/").split("/")
        if len(parts) > 1 and parts[-1].lower().startswith("trackid="):
            parts.pop()
        return parts[-1]

    def media_for(self, path):
        return self.sub_media if self.stream_key(path).endswith("-sub") else self.media

    def build_frame(self, media, channel):
        """Frame hiện tại của `media` thành bytes interleaved, dùng chung cho mọi phiên."""
        frame = media.frames[self.frame_no % len(media.frames)]
        key = id(media)
        seq, ts = self.seq[key], self.ts[key]
        buf = bytearray()
        last = len(frame) - 1
        for i, payload in enumerate(frame):
            hdr = struct.pack("!BBHII", 0x80, 96 | (0x80 if i == last else 0),
                              (seq + i) & 0xFFFF, ts & 0xFFFFFFFF, self.SSRC)
            pkt = hdr + payload
            buf += struct.pack("!BBH", 0x24, channel, len(pkt)) + pkt
        return bytes(buf), len(frame)

    # -------------------------------------------------- vòng phát
    async def ticker(self):
        interval = 1.0 / self.media.fps
        next_at = time.monotonic()
        last_reload = 0.0
        while True:
            next_at += interval
            delay = next_at - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            now = time.monotonic()
            self.late.append((now - next_at) * 1000.0)
            if len(self.late) > 4000:
                self.late = self.late[-2000:]
            if now - next_at > interval:
                next_at = now  # tụt nhịp: bỏ qua phần trễ
            if now - last_reload > 1.0:
                last_reload = now
                self.reload_drop_spec()
            self.tick(now)

    def tick(self, now):
        flaky_sec = self.drop["flaky_sec"]
        dead = []
        cache = {}
        for sess in self.sessions.values():
            if not sess.playing or sess.closed:
                continue
            key = self.stream_key(sess.path)
            if key in self.drop["flaky"] and flaky_sec > 0:
                # ngắt luồng định kỳ để buộc camera phải reconnect
                if now - sess.started > flaky_sec:
                    dead.append(sess)
                    continue
            ck = (id(sess.media), sess.channel)
            built = cache.get(ck)
            if built is None:
                built = cache[ck] = self.build_frame(sess.media, sess.channel)
            data, npkt = built
            sess.packets += npkt
            self.stat_packets += npkt
            self.stat_bytes += len(data)
            try:
                sess.writer.write(data)
            except (ConnectionError, OSError):
                dead.append(sess)
        for sess in dead:
            self.close_session(sess)
        # tiến bộ đếm chung sau khi mọi phiên đã nhận frame này
        self.frame_no += 1
        for media in {id(self.media): self.media, id(self.sub_media): self.sub_media}.values():
            frame = media.frames[(self.frame_no - 1) % len(media.frames)]
            self.seq[id(media)] += len(frame)
            self.ts[id(media)] += media.ts_step

    def close_session(self, sess):
        sess.closed = True
        sess.playing = False
        self.sessions.pop(sess.sid, None)
        try:
            sess.writer.close()
        except (ConnectionError, OSError):
            pass

    # -------------------------------------------------- RTSP
    async def handle(self, reader, writer):
        owned = []
        try:
            while True:
                first = await reader.read(1)
                if not first:
                    break
                if first == b"$":
                    # RTCP interleaved on the RTSP connection ($, channel, length): skip it,
                    # otherwise it is parsed as a malformed request and the session drops.
                    hdr = await reader.readexactly(3)
                    await reader.readexactly(struct.unpack("!H", hdr[1:])[0])
                    continue
                line = first + await reader.readline()
                try:
                    request = line.decode("utf-8", "replace").strip()
                except UnicodeDecodeError:
                    break
                if not request:
                    continue
                parts = request.split()
                if len(parts) < 3:
                    break
                method, url = parts[0], parts[1]
                headers = {}
                while True:
                    hl = await reader.readline()
                    if not hl or hl in (b"\r\n", b"\n"):
                        break
                    if b":" in hl:
                        k, v = hl.decode("utf-8", "replace").split(":", 1)
                        headers[k.strip().lower()] = v.strip()
                body_len = int(headers.get("content-length", 0) or 0)
                if body_len:
                    await reader.readexactly(body_len)
                await self.respond(method, url, headers, writer, owned)
        except (ConnectionError, OSError, asyncio.IncompleteReadError):
            pass
        finally:
            for sess in owned:
                self.close_session(sess)
            try:
                writer.close()
            except (ConnectionError, OSError):
                pass

    async def respond(self, method, url, headers, writer, owned):
        cseq = headers.get("cseq", "0")
        base = "RTSP/1.0 200 OK\r\nCSeq: %s\r\nServer: s3mk-stress\r\n" % cseq
        key = self.stream_key(url)

        if key in self.drop["offline"]:
            writer.write(("RTSP/1.0 404 Not Found\r\nCSeq: %s\r\n\r\n" % cseq).encode())
            await writer.drain()
            return

        if method == "OPTIONS":
            writer.write((base + "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n\r\n").encode())
        elif method == "DESCRIBE":
            sdp = self.media_for(url).sdp("trackID=0")
            writer.write((base + "Content-Type: application/sdp\r\nContent-Base: %s/\r\n"
                          "Content-Length: %d\r\n\r\n" % (url, len(sdp))).encode() + sdp.encode())
        elif method == "SETUP":
            transport = headers.get("transport", "")
            if "TCP" not in transport.upper():
                writer.write(("RTSP/1.0 461 Unsupported Transport\r\nCSeq: %s\r\n\r\n" % cseq).encode())
                await writer.drain()
                return
            sid = str(self.next_sid)
            self.next_sid += 1
            sess = Session(sid, url, writer, self.media_for(url))
            channel = 0
            for token in transport.split(";"):
                if token.strip().startswith("interleaved="):
                    channel = int(token.split("=", 1)[1].split("-")[0])
            sess.channel = channel
            self.sessions[sid] = sess
            owned.append(sess)
            writer.write((base + "Session: %s;timeout=60\r\n"
                          "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n\r\n"
                          % (sid, channel, channel + 1)).encode())
        elif method == "PLAY":
            sid = headers.get("session", "").split(";")[0]
            sess = self.sessions.get(sid)
            if sess:
                sess.playing = True
                sess.started = time.monotonic()
                self.stat_plays += 1
            key = id(sess.media) if sess else id(self.media)
            writer.write((base + "Session: %s\r\nRange: npt=0.000-\r\n"
                          "RTP-Info: url=%s/trackID=0;seq=%d;rtptime=%d\r\n\r\n"
                          % (sid, url, self.seq[key] & 0xFFFF, self.ts[key] & 0xFFFFFFFF)).encode())
        elif method in ("TEARDOWN", "PAUSE"):
            sid = headers.get("session", "").split(";")[0]
            sess = self.sessions.get(sid)
            if sess:
                self.stat_teardowns += 1
                self.close_session(sess)
            writer.write((base + "Session: %s\r\n\r\n" % sid).encode())
        elif method == "GET_PARAMETER":
            writer.write((base + "\r\n").encode())
        else:
            writer.write(("RTSP/1.0 405 Method Not Allowed\r\nCSeq: %s\r\n\r\n" % cseq).encode())
        await writer.drain()

    async def stats(self):
        last_p, last_b, last_t = 0, 0, time.time()
        last_cpu = time.process_time()
        while True:
            await asyncio.sleep(self.args.stats_sec)
            now = time.time()
            dt = max(0.001, now - last_t)
            playing = sum(1 for s in self.sessions.values() if s.playing)
            cpu = time.process_time()
            cpu_pct = (cpu - last_cpu) / dt * 100.0
            last_cpu = cpu
            late = sorted(self.late)
            self.late = []
            late_p99 = late[int(len(late) * 0.99)] if late else 0.0
            late_max = late[-1] if late else 0.0
            line = ("[rtsp-source] t=%5.0fs sessions=%d playing=%d plays=%d teardowns=%d "
                    "pps=%.0f kbps=%.0f" %
                    (now - self.started, len(self.sessions), playing, self.stat_plays,
                     self.stat_teardowns, (self.stat_packets - last_p) / dt,
                     (self.stat_bytes - last_b) * 8 / dt / 1000))
            print(line, flush=True)
            if self.args.state:
                state = {"sessions": len(self.sessions), "playing": playing,
                         "plays": self.stat_plays, "teardowns": self.stat_teardowns,
                         "packets": self.stat_packets, "bytes": self.stat_bytes,
                         "kbps": round((self.stat_bytes - last_b) * 8 / dt / 1000),
                         "cpu_pct": round(cpu_pct, 1), "tick_late_p99_ms": round(late_p99, 1),
                         "tick_late_max_ms": round(late_max, 1),
                         "uptime_sec": now - self.started, "at": now}
                # Ghi file ngoài event loop: khi server ghi hình dồn đĩa, write/rename có thể
                # bị chặn vài giây và kéo theo cả vòng phát của mọi phiên.
                await asyncio.get_event_loop().run_in_executor(None, write_state, self.args.state, state)
            last_p, last_b, last_t = self.stat_packets, self.stat_bytes, now


def write_state(path, state):
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(state, fh)
    os.replace(tmp, path)


def merge_states(paths, out, started):
    """Gộp state của các tiến trình con vào một file cho check_stress.py/check_ccu.py."""
    total = {"sessions": 0, "playing": 0, "plays": 0, "teardowns": 0, "packets": 0,
             "bytes": 0, "kbps": 0, "procs": len(paths), "procs_fresh": 0,
             "cpu_pct_max": 0.0, "tick_late_p99_ms": 0.0, "tick_late_max_ms": 0.0}
    for p in paths:
        try:
            st = json.load(open(p))
        except (OSError, ValueError):
            continue
        if time.time() - st.get("at", 0) < 60:
            total["procs_fresh"] += 1
        for k in ("sessions", "playing", "plays", "teardowns", "packets", "bytes", "kbps"):
            total[k] += st.get(k, 0)
        total["cpu_pct_max"] = max(total["cpu_pct_max"], st.get("cpu_pct", 0))
        total["tick_late_p99_ms"] = max(total["tick_late_p99_ms"], st.get("tick_late_p99_ms", 0))
        total["tick_late_max_ms"] = max(total["tick_late_max_ms"], st.get("tick_late_max_ms", 0))
    total["uptime_sec"] = time.time() - started
    tmp = out + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(total, fh)
    os.replace(tmp, out)
    return total


def supervise(args):
    """--procs N: chạy N tiến trình con cùng cổng, gộp state, dừng cả nhóm khi bị dừng."""
    import signal
    import subprocess
    started = time.time()
    state = args.state or "rtsp_state.json"
    children, paths = [], []
    for i in range(args.procs):
        child_state = "%s.%d" % (state, i)
        paths.append(child_state)
        cmd = [sys.executable, os.path.abspath(__file__), args.media, "--port", str(args.port),
               "--host", args.host, "--fps", str(args.fps), "--stats-sec", str(args.stats_sec),
               "--state", child_state, "--reuse-port"]
        if args.sub_media:
            cmd += ["--sub-media", args.sub_media]
        if args.drop_spec:
            cmd += ["--drop-spec", args.drop_spec]
        children.append(subprocess.Popen(cmd))

    def stop(*_):
        for c in children:
            if c.poll() is None:
                c.terminate()
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    print("[rtsp-source] %d tiến trình trên cổng %d" % (args.procs, args.port), flush=True)
    while True:
        time.sleep(args.stats_sec)
        t = merge_states(paths, state, started)
        print("[rtsp-source] tổng: sessions=%d playing=%d kbps=%d cpu_max=%.0f%% late_p99=%.0fms late_max=%.0fms"
              % (t["sessions"], t["playing"], t["kbps"], t["cpu_pct_max"], t["tick_late_p99_ms"],
                 t["tick_late_max_ms"]), flush=True)
        if any(c.poll() is not None for c in children):
            print("[rtsp-source] một tiến trình con đã chết, dừng cả nhóm", flush=True)
            stop()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("media")
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--fps", type=int, default=5)
    ap.add_argument("--stats-sec", type=float, default=10.0)
    ap.add_argument("--drop-spec", default=None)
    ap.add_argument("--state", default=None)
    ap.add_argument("--sub-media", default=None, help="file cho luồng có path kết thúc bằng -sub")
    ap.add_argument("--procs", type=int, default=1, help="số tiến trình con cùng cổng")
    ap.add_argument("--reuse-port", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.procs > 1:
        supervise(args)
        return

    media = Loop(args.media, args.fps)
    sub = Loop(args.sub_media, args.fps) if args.sub_media else None
    srv = Server(media, args, sub)
    print("[rtsp-source] %d frame, %d fps, nghe tại %s:%d%s"
          % (len(media.frames), media.fps, args.host, args.port,
             " (luồng -sub: %d frame)" % len(sub.frames) if sub else ""), flush=True)
    server = await asyncio.start_server(srv.handle, args.host, args.port, backlog=1024,
                                        reuse_port=args.reuse_port or None)
    asyncio.ensure_future(srv.ticker())
    asyncio.ensure_future(srv.stats())
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
