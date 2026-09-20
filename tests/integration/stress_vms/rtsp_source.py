#!/usr/bin/env python3
"""Máy phát RTSP tổng hợp cho stress test: phục vụ N luồng từ một file H.264 dựng sẵn.

Mỗi URL (bất kể path) đều được chấp nhận và trả về cùng một vòng lặp video đã được
đóng gói RTP sẵn, nên chi phí CPU cho mỗi luồng chỉ là vá seq/timestamp rồi ghi socket.
Chỉ hỗ trợ RTP/AVP/TCP (interleaved) để không phải mở thêm cổng UDP cho 600 luồng.

Dùng:
    rtsp_source.py <h264_file> [--port 8554] [--fps 5] [--stats-sec 10]
                   [--drop-spec PATH] [--state PATH]

--drop-spec trỏ tới file JSON do kịch bản ghi ra để mô phỏng camera lỗi:
    {"offline": ["cam-0007-main"], "flaky": ["cam-0012-sub"], "flaky_sec": 20}
File được đọc lại mỗi giây, cho phép đổi điều kiện giữa chừng mà không phải restart.
"""
import argparse
import asyncio
import base64
import json
import os
import random
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
    __slots__ = ("sid", "path", "writer", "channel", "seq", "ts", "ssrc",
                 "frame_idx", "playing", "started", "packets", "closed")

    def __init__(self, sid, path, writer):
        self.sid = sid
        self.path = path
        self.writer = writer
        self.channel = 0
        self.seq = random.randint(0, 0xFFFF)
        self.ts = random.randint(0, 0x7FFFFFFF)
        self.ssrc = random.randint(1, 0xFFFFFFFF)
        self.frame_idx = 0
        self.playing = False
        self.started = 0.0
        self.packets = 0
        self.closed = False


class Server(object):
    def __init__(self, loop_media, args):
        self.media = loop_media
        self.args = args
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
        return path.rstrip("/").rsplit("/", 1)[-1].split("?", 1)[0]

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
            else:
                next_at = time.monotonic()  # tụt nhịp: bỏ qua phần trễ
            now = time.monotonic()
            if now - last_reload > 1.0:
                last_reload = now
                self.reload_drop_spec()
            self.tick(now)

    def tick(self, now):
        flaky_sec = self.drop["flaky_sec"]
        dead = []
        for sess in self.sessions.values():
            if not sess.playing or sess.closed:
                continue
            key = self.stream_key(sess.path)
            if key in self.drop["flaky"] and flaky_sec > 0:
                # ngắt luồng định kỳ để buộc camera phải reconnect
                if now - sess.started > flaky_sec:
                    dead.append(sess)
                    continue
            frame = self.media.frames[sess.frame_idx % len(self.media.frames)]
            sess.frame_idx += 1
            buf = bytearray()
            last = len(frame) - 1
            for i, payload in enumerate(frame):
                hdr = struct.pack("!BBHII", 0x80, 96 | (0x80 if i == last else 0),
                                  sess.seq & 0xFFFF, sess.ts & 0xFFFFFFFF, sess.ssrc)
                sess.seq += 1
                pkt = hdr + payload
                buf += struct.pack("!BBH", 0x24, sess.channel, len(pkt)) + pkt
                sess.packets += 1
                self.stat_packets += 1
            sess.ts += self.media.ts_step
            self.stat_bytes += len(buf)
            try:
                sess.writer.write(bytes(buf))
            except (ConnectionError, OSError):
                dead.append(sess)
        for sess in dead:
            self.close_session(sess)

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
                line = await reader.readline()
                if not line:
                    break
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
            sdp = self.media.sdp("trackID=0")
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
            sess = Session(sid, url, writer)
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
            writer.write((base + "Session: %s\r\nRange: npt=0.000-\r\n"
                          "RTP-Info: url=%s/trackID=0;seq=%d;rtptime=%d\r\n\r\n"
                          % (sid, url, sess.seq if sess else 0, sess.ts if sess else 0)).encode())
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
        while True:
            await asyncio.sleep(self.args.stats_sec)
            now = time.time()
            dt = max(0.001, now - last_t)
            playing = sum(1 for s in self.sessions.values() if s.playing)
            line = ("[rtsp-source] t=%5.0fs sessions=%d playing=%d plays=%d teardowns=%d "
                    "pps=%.0f kbps=%.0f" %
                    (now - self.started, len(self.sessions), playing, self.stat_plays,
                     self.stat_teardowns, (self.stat_packets - last_p) / dt,
                     (self.stat_bytes - last_b) * 8 / dt / 1000))
            print(line, flush=True)
            if self.args.state:
                tmp = self.args.state + ".tmp"
                with open(tmp, "w") as fh:
                    json.dump({"sessions": len(self.sessions), "playing": playing,
                               "plays": self.stat_plays, "teardowns": self.stat_teardowns,
                               "packets": self.stat_packets, "bytes": self.stat_bytes,
                               "uptime_sec": now - self.started}, fh)
                os.replace(tmp, self.args.state)
            last_p, last_b, last_t = self.stat_packets, self.stat_bytes, now


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("media")
    ap.add_argument("--port", type=int, default=8554)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--fps", type=int, default=5)
    ap.add_argument("--stats-sec", type=float, default=10.0)
    ap.add_argument("--drop-spec", default=None)
    ap.add_argument("--state", default=None)
    args = ap.parse_args()

    media = Loop(args.media, args.fps)
    srv = Server(media, args)
    print("[rtsp-source] %d frame, %d fps, nghe tại %s:%d"
          % (len(media.frames), media.fps, args.host, args.port), flush=True)
    server = await asyncio.start_server(srv.handle, args.host, args.port, backlog=1024)
    asyncio.ensure_future(srv.ticker())
    asyncio.ensure_future(srv.stats())
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
