#!/usr/bin/env python3
"""Service API giả cho kiểm thử E2E.

Khác bản của stress_vms ở chỗ hook kiểm quyền camera (`/api/camera/check-camera-of-user`,
tức `hook.on_play`) có thể **từ chối**: nếu `user_id` trong JWT của header Authorization nằm
trong `<work>/deny_users.json` thì trả HTTP 403. JWT không được kiểm chữ ký ở đây —
MediaServer đã kiểm trước khi gọi hook.

Mọi request được ghi (path, thời điểm, body rút gọn) vào `<work>/mock_api_events.jsonl`
để kịch bản đối chiếu hook nào đã tới, ví dụ callback kết quả job trích xuất.

Dùng: mock_api.py <work> <port>
"""
import base64
import json
import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WORK = sys.argv[1]
PORT = int(sys.argv[2])
LOCK = threading.Lock()
EVENTS = os.path.join(WORK, "mock_api_events.jsonl")
CHECK_CAMERA = "/api/camera/check-camera-of-user"
# Server truyền nguyên body JSON cho callback của hook và không xét `code` (phần kiểm code
# trong parse_http_response bị `#if 0`); chỉ HTTP status khác 2xx mới là từ chối.
# Có manager.apiSecret thì body hook cấu hình bị coi là bản mã hoá, trừ khi có khoá
# `media_server` ở top-level (decryptServerConfigJson). Bản Debug sau đó đọc cấu hình thật
# từ ./api-config.json, nên chỉ cần một object rỗng.
CONFIG_HOOKS = ("/api/media-server/configuration", "/api/media-server/configuration-2nd-gen")


def jwt_user(header):
    token = header.split("Bearer", 1)[-1].strip() if header else ""
    parts = token.split(".")
    if len(parts) != 3:
        return ""
    pad = "=" * (-len(parts[1]) % 4)
    try:
        return json.loads(base64.urlsafe_b64decode(parts[1] + pad)).get("user_id", "")
    except ValueError:
        return ""


def denied(user):
    try:
        return user in json.load(open(os.path.join(WORK, "deny_users.json")))
    except (OSError, ValueError):
        return False


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _reply(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        path = self.path.split("?", 1)[0]
        code = 0
        user = ""
        if path == CHECK_CAMERA:
            user = jwt_user(self.headers.get("Authorization", ""))
            if denied(user):
                code = 403
        record = {"t": time.time(), "path": path, "code": code}
        if user:
            record["user"] = user
        if not path.endswith(("heartbeat", "report", "configuration", "configuration-2nd-gen")):
            record["body"] = raw[:2000].decode("utf-8", "replace")
        with LOCK, open(EVENTS, "a") as fh:
            fh.write(json.dumps(record) + "\n")
        reply = {"code": code, "msg": "denied" if code else "ok"}
        if path in CONFIG_HOOKS:
            reply["media_server"] = {}
        body = json.dumps(reply).encode()
        self.send_response(403 if code else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = do_POST = do_PUT = _reply

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print("[mock-api] 127.0.0.1:%d" % PORT, flush=True)
    srv.serve_forever()
