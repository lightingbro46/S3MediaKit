#!/usr/bin/env python3
"""Service API giả cho hook của MediaServer, bản chịu tải.

Khác với tests/integration/sync_cluster/mock_api.py: không log từng request (300
camera sẽ làm ngập stdout), bật keep-alive, và đếm hook theo path để kịch bản
kiểm được rằng luồng nào thực sự lên/xuống.

Webhook kết quả video config rule (`.../action-callback`) được giữ nguyên body và header
`Idempotency-Key`, đọc lại qua `GET /_mock/hooks?path=action-callback`.

Dùng: mock_api.py [port] [--state PATH] [--stats-sec 15]
"""
import argparse
import json
import os
import threading
import time
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

COUNTS = defaultdict(int)
STREAMS = {}
# Body của các hook cần kiểm nội dung, theo thứ tự nhận.
RECORDED = []
RECORD_SUFFIXES = ("action-callback",)
LOCK = threading.Lock()
STARTED = time.time()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "s3mk-stress-api"

    def _reply(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        path = self.path.split("?", 1)[0]
        if path == "/_mock/hooks":
            return self._send_recorded()
        with LOCK:
            COUNTS[path] += 1
            if path.endswith(RECORD_SUFFIXES):
                try:
                    body = json.loads(raw) if raw else None
                except ValueError:
                    body = raw.decode("utf-8", "replace")
                RECORDED.append({"path": path, "at": time.time(), "body": body,
                                 "idempotency_key": self.headers.get("Idempotency-Key")})
            # on_stream_changed cho biết luồng nào đang bật/tắt
            if path.endswith("on_stream_changed") and raw:
                try:
                    payload = json.loads(raw)
                    key = "%s/%s" % (payload.get("app", ""), payload.get("stream", ""))
                    STREAMS[key] = bool(payload.get("regist", False))
                except ValueError:
                    pass
        body = json.dumps({"code": 0, "ok": 1}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_recorded(self):
        with LOCK:
            data = json.dumps(RECORDED).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = _reply
    do_POST = _reply
    do_PUT = _reply

    def log_message(self, fmt, *args):
        pass


def reporter(args):
    last = 0
    while True:
        time.sleep(args.stats_sec)
        with LOCK:
            total = sum(COUNTS.values())
            snapshot = dict(COUNTS)
            live = sum(1 for v in STREAMS.values() if v)
            known = len(STREAMS)
        print("[mock-api] hooks=%d (+%d) streams_known=%d streams_live=%d"
              % (total, total - last, known, live), flush=True)
        last = total
        if args.state:
            tmp = args.state + ".tmp"
            with open(tmp, "w") as fh:
                json.dump({"hooks": total, "by_path": snapshot,
                           "streams_known": known, "streams_live": live,
                           "uptime_sec": time.time() - STARTED}, fh, indent=1)
            os.replace(tmp, args.state)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", type=int, default=18590)
    ap.add_argument("--state", default=None)
    ap.add_argument("--stats-sec", type=float, default=15.0)
    args = ap.parse_args()

    threading.Thread(target=reporter, args=(args,), daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    srv.daemon_threads = True
    srv.request_queue_size = 256
    print("[mock-api] nghe tại 127.0.0.1:%d" % args.port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
