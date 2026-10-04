#!/usr/bin/env python3
"""Mock RTSP backchannel kiểu ONVIF (Profile T/S audio output) để kiểm BackchannelPusher.

Trình tự client phải đi:
    DESCRIBE rtsp://host:port/<path>   (Require: www.onvif.org/ver20/backchannel)
    SETUP    rtsp://host:port/<path>/audioback  (RTP/AVP/TCP;unicast;interleaved=0-1)
    PLAY     rtsp://host:port/<path>
    ... RTP (kênh 0) / RTCP (kênh 1) interleaved "$" trên cùng kết nối ...
    TEARDOWN rtsp://host:port/<path>

Digest (không qop) như camera ONVIF: request đầu không có Authorization nhận 401
`Digest realm="...", nonce="..."`. DESCRIBE thiếu header Require nhận 551 Option not supported.
SDP chỉ có một track audio sendonly PCMU/8000/1 (hoặc PCMA với --codec pcma).

Dùng:
    rtsp_backchannel_mock.py [--port 18554] [--user admin] [--password admin123]
                             [--codec pcmu|pcma] [--state PATH] [--audio-out PATH]

--state ghi JSON sau mỗi request và khi kết nối đóng: danh sách request (method, uri), số gói/byte
payload RTP, payload type, seq có liên tục không, số gói RTCP, phiên đang mở hay không.
--audio-out ghi nối payload RTP (G.711 thô) nhận được.
"""
import argparse
import hashlib
import json
import os
import socketserver
import struct
import threading
import time

REALM = "rtsp-backchannel-mock"
REQUIRE = "www.onvif.org/ver20/backchannel"


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


class State:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.requests = []
        self.rtp_packets = 0
        self.rtp_bytes = 0
        self.payload_types = []
        self.seq_gaps = 0
        self.rtcp_packets = 0
        self.sessions_open = 0
        self.teardowns = 0

    def dump(self):
        if not self.args.state:
            return
        with self.lock:
            data = {
                "requests": self.requests,
                "rtp_packets": self.rtp_packets,
                "rtp_bytes": self.rtp_bytes,
                "payload_types": self.payload_types,
                "seq_gaps": self.seq_gaps,
                "rtcp_packets": self.rtcp_packets,
                "session_open": self.sessions_open > 0,
                "teardowns": self.teardowns,
            }
        tmp = self.args.state + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1)
        os.replace(tmp, self.args.state)


def make_handler(state, args):
    pt, codec = (8, "PCMA") if args.codec == "pcma" else (0, "PCMU")
    nonce = md5("%f" % time.time())

    class Handler(socketserver.StreamRequestHandler):
        def setup(self):
            super().setup()
            self.session = None
            self.playing = False
            self.last_seq = None

        def read_exact(self, n):
            data = b""
            while len(data) < n:
                chunk = self.rfile.read(n - len(data))
                if not chunk:
                    return None
                data += chunk
            return data

        def authorized(self, method, headers):
            auth = headers.get("authorization", "")
            if not auth.startswith("Digest "):
                return False
            import re
            fields = dict(re.findall(r'(\w+)="([^"]*)"', auth[7:]))
            if fields.get("username") != args.user or fields.get("nonce") != nonce:
                return False
            ha1 = md5("%s:%s:%s" % (args.user, REALM, args.password))
            ha2 = md5("%s:%s" % (method, fields.get("uri", "")))
            return fields.get("response") == md5("%s:%s:%s" % (ha1, nonce, ha2))

        def reply(self, code, reason, cseq, extra=None, body=b""):
            lines = ["RTSP/1.0 %d %s" % (code, reason), "CSeq: %s" % cseq]
            for k, v in (extra or {}).items():
                lines.append("%s: %s" % (k, v))
            if body:
                lines.append("Content-Length: %d" % len(body))
            self.wfile.write(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
            self.wfile.flush()

        def on_interleaved(self):
            hdr = self.read_exact(3)
            if hdr is None:
                return False
            channel, length = hdr[0], struct.unpack(">H", hdr[1:3])[0]
            data = self.read_exact(length)
            if data is None:
                return False
            if channel == 0 and self.playing and len(data) >= 12:
                payload_type = data[1] & 0x7F
                seq = struct.unpack(">H", data[2:4])[0]
                csrc = data[0] & 0x0F
                payload = data[12 + 4 * csrc:]
                with state.lock:
                    state.rtp_packets += 1
                    state.rtp_bytes += len(payload)
                    if payload_type not in state.payload_types:
                        state.payload_types.append(payload_type)
                    if self.last_seq is not None and seq != (self.last_seq + 1) & 0xFFFF:
                        state.seq_gaps += 1
                self.last_seq = seq
                if args.audio_out:
                    with open(args.audio_out, "ab") as f:
                        f.write(payload)
            elif channel == 1:
                with state.lock:
                    state.rtcp_packets += 1
            return True

        def on_request(self, first):
            raw = first
            while not raw.endswith(b"\r\n\r\n"):
                line = self.rfile.readline()
                if not line:
                    return False
                raw += line
            lines = raw.decode("utf-8", "replace").split("\r\n")
            method, uri = lines[0].split(" ")[:2]
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            if int(headers.get("content-length", "0")):
                self.read_exact(int(headers["content-length"]))
            cseq = headers.get("cseq", "0")
            with state.lock:
                state.requests.append({"method": method, "uri": uri,
                                       "auth": "authorization" in headers})
            try:
                if not self.authorized(method, headers):
                    self.reply(401, "Unauthorized", cseq,
                               {"WWW-Authenticate": 'Digest realm="%s", nonce="%s"' % (REALM, nonce)})
                    return True
                if method in ("DESCRIBE", "PLAY") and REQUIRE not in headers.get("require", ""):
                    self.reply(551, "Option not supported", cseq, {"Unsupported": REQUIRE})
                    return True
                if method == "OPTIONS":
                    self.reply(200, "OK", cseq, {"Public": "DESCRIBE, SETUP, PLAY, TEARDOWN"})
                elif method == "DESCRIBE":
                    sdp = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=ONVIF backchannel mock\r\nt=0 0\r\n"
                           "m=audio 0 RTP/AVP %d\r\na=rtpmap:%d %s/8000/1\r\na=control:audioback\r\n"
                           "a=sendonly\r\n" % (pt, pt, codec)).encode()
                    self.reply(200, "OK", cseq, {"Content-Type": "application/sdp",
                                                 "Content-Base": uri.rstrip("/") + "/"}, sdp)
                elif method == "SETUP":
                    if not uri.endswith("/audioback"):
                        self.reply(404, "Not Found", cseq)
                        return True
                    self.session = "%08X" % (int(time.time() * 1000) & 0xFFFFFFFF)
                    self.reply(200, "OK", cseq, {"Session": self.session + ";timeout=60",
                                                 "Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
                elif method == "PLAY":
                    if not self.session or headers.get("session", "").split(";")[0] != self.session:
                        self.reply(454, "Session Not Found", cseq)
                        return True
                    self.playing = True
                    with state.lock:
                        state.sessions_open += 1
                    self.reply(200, "OK", cseq, {"Session": self.session})
                elif method == "TEARDOWN":
                    self.reply(200, "OK", cseq, {"Session": self.session or ""})
                    with state.lock:
                        state.teardowns += 1
                    self.end_session()
                else:
                    self.reply(405, "Method Not Allowed", cseq)
            finally:
                state.dump()
            return True

        def end_session(self):
            if self.playing:
                self.playing = False
                with state.lock:
                    state.sessions_open -= 1

        def handle(self):
            try:
                while True:
                    first = self.rfile.read(1)
                    if not first:
                        break
                    ok = self.on_interleaved() if first == b"$" else self.on_request(first + self.rfile.readline())
                    if not ok:
                        break
            except (ConnectionError, OSError):
                pass
            finally:
                self.end_session()
                state.dump()

    return Handler


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=18554)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin123")
    ap.add_argument("--codec", choices=["pcmu", "pcma"], default="pcmu")
    ap.add_argument("--state", default=None)
    ap.add_argument("--audio-out", default=None)
    args = ap.parse_args()
    state = State(args)
    state.dump()
    server = Server((args.host, args.port), make_handler(state, args))
    print("rtsp_backchannel_mock listening on %s:%d (%s)" % (args.host, args.port, args.codec), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
