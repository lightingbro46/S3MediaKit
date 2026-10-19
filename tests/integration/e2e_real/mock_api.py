#!/usr/bin/env python3
"""Service API giả cho kiểm thử E2E.

Khác bản của stress_vms ở chỗ hook kiểm quyền camera (`/api/camera/check-camera-of-user`,
tức `hook.on_play`) có thể **từ chối**: nếu `user_id` trong JWT của header Authorization nằm
trong `<work>/deny_users.json` thì trả HTTP 403. JWT không được kiểm chữ ký ở đây —
MediaServer đã kiểm trước khi gọi hook.

`<work>/hook_fault.json` (đọc lại mỗi request, không có thì bỏ qua) làm hook đó chậm hoặc lỗi để
thử cơ chế gia hạn quyền: `{"delay_sec": 10}` trả lời sau 10 s, `{"status": 503}` trả 503 cho
mọi user, `{"max_concurrency": 16}` chỉ phục vụ 16 request hook đó cùng lúc (phần còn lại chờ,
giả VMS quá tải). `deny_users.json` vẫn được xét trước. Mỗi request hook đó ghi `conc`: số request
hook đó đang mở tại lúc tới (gồm cả đang chờ), để đối chiếu giới hạn `hook.maxConcurrent`.

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
CONC_LOCK = threading.Lock()
CONC = [0]
# Recreated when max_concurrency changes.
GATE = {"limit": 0, "sem": None}
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


def hook_fault():
    try:
        return json.load(open(os.path.join(WORK, "hook_fault.json")))
    except (OSError, ValueError):
        return {}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _reply(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        path = self.path.split("?", 1)[0]
        code = 0
        user = ""
        conc = 0
        arrived = time.time()
        if path == CHECK_CAMERA:
            user = jwt_user(self.headers.get("Authorization", ""))
            fault = hook_fault()
            with CONC_LOCK:
                CONC[0] += 1
                conc = CONC[0]
                limit = int(fault.get("max_concurrency") or 0)
                if GATE["limit"] != limit:
                    GATE["limit"] = limit
                    GATE["sem"] = threading.Semaphore(limit) if limit else None
                sem = GATE["sem"]
            try:
                if sem:
                    sem.acquire()
                try:
                    if fault.get("delay_sec"):
                        time.sleep(float(fault["delay_sec"]))
                finally:
                    if sem:
                        sem.release()
            finally:
                with CONC_LOCK:
                    CONC[0] -= 1
            if denied(user):
                code = 403
            elif fault.get("status"):
                code = int(fault["status"])
        record = {"t": time.time(), "arrived": arrived, "path": path, "code": code}
        if path == CHECK_CAMERA:
            record["conc"] = conc
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
        self.send_response(code or 200)
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
