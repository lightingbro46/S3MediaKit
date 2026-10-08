#!/usr/bin/env python3
"""Mock API hãng của loa Bosch (ext-plugin/Bosch/BoschIpSpeaker) cho kênh API hãng của SpeakerController.

Phục vụ các endpoint plugin gọi: /api/login, /api/session, /api/settings, /api/audioSettings,
/api/info, /api/users, /api/logout, /api/key (âm lượng), /api/file, /api/messages[/<id>/play|stop].
Plugin dùng chung ip:port cho API hãng và ONVIF: /onvif/* được chuyển tiếp tới --onvif-upstream
(vd. onvif_mock.py), không có thì trả 404, tức loa không hỗ trợ ONVIF.

Dùng:
    bosch_api_mock.py [--port 18680] [--user admin] [--password admin]
                      [--onvif-upstream 127.0.0.1:18580] [--control PATH] [--state PATH]

--control là file JSON đọc lại mỗi request (thiếu khoá = false):
    {"api_down": true}      # mọi /api/* đóng kết nối không trả lời (loa mất API hãng)
    {"reject": true}        # /api/login trả 401 (sai mật khẩu)
    {"onvif_down": true}    # /onvif/* trả 404 dù có upstream
--state ghi bộ đếm (login, logout, volume, play, users, …) và âm lượng hiện tại sau mỗi request.
"""
import argparse
import http.client
import json
import os
import sys
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
STATE = {"counts": {}, "volume": -20, "users": [], "messages": [], "sessions": 0}


def bump(name):
    STATE["counts"][name] = STATE["counts"].get(name, 0) + 1


def control(args):
    if not args.control or not os.path.exists(args.control):
        return {}
    try:
        with open(args.control) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def save_state(args):
    if not args.state:
        return
    tmp = args.state + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(STATE, fh, indent=1)
    os.replace(tmp, args.state)


def make_handler(args):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            if args.verbose:
                sys.stderr.write("bosch_api_mock: " + fmt % a + "\n")

        def body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return self.rfile.read(n) if n > 0 else b""

        def reply(self, status, obj=None, headers=None):
            data = b"" if obj is None else json.dumps(obj).encode()
            self.send_response(status)
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            if obj is not None:
                self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def proxy_onvif(self, method):
            raw = self.body()
            ctl = control(args)
            if not args.onvif_upstream or ctl.get("onvif_down"):
                with LOCK:
                    bump("onvif_404")
                self.reply(404)
                return
            host, port = args.onvif_upstream.rsplit(":", 1)
            conn = http.client.HTTPConnection(host, int(port), timeout=30)
            try:
                hdr = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection")}
                conn.request(method, self.path, body=raw, headers=hdr)
                resp = conn.getresponse()
                data = resp.read()
                self.send_response(resp.status)
                for k, v in resp.getheaders():
                    if k.lower() not in ("content-length", "connection", "transfer-encoding"):
                        self.send_header(k, v)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except OSError:
                self.reply(502)
            finally:
                conn.close()

        def authed(self):
            return "SESSID=" in (self.headers.get("Cookie") or "")

        def handle_api(self, method):
            raw = self.body()
            ctl = control(args)
            path = self.path.split("?", 1)[0]
            if ctl.get("api_down"):
                with LOCK:
                    bump("api_down_drop")
                    save_state(args)
                self.close_connection = True
                self.connection.shutdown(2)
                return
            with LOCK:
                try:
                    self.route(method, path, raw, ctl)
                finally:
                    save_state(args)

        def route(self, method, path, raw, ctl):
            if path == "/api/login" and method == "POST":
                bump("login")
                try:
                    req = json.loads(raw or b"{}")
                except ValueError:
                    req = {}
                if ctl.get("reject") or req.get("username") != args.user or req.get("password") != args.password:
                    bump("login_rejected")
                    self.reply(401, {"error": "unauthorized"})
                    return
                STATE["sessions"] += 1
                sid = uuid.uuid4().hex
                self.reply(200, {"ok": True}, {"Set-Cookie": "SESSID=%s; Path=/; HttpOnly" % sid})
                return
            if not self.authed():
                self.reply(401, {"error": "no session"})
                return
            if path == "/api/session" and method == "GET":
                self.reply(200, {"id": "s1", "initialized": True})
            elif path == "/api/logout" and method == "POST":
                bump("logout")
                STATE["sessions"] = max(0, STATE["sessions"] - 1)
                self.reply(200, {})
            elif path == "/api/settings" and method == "GET":
                self.reply(200, {"productName": "LBB 1930/20 mock", "firmwareVersion": "1.0.0",
                                 "serialNumber": "MOCK-0001", "macAddress": "00:07:5F:00:00:01"})
            elif path == "/api/audioSettings" and method == "GET":
                self.reply(200, {"user_level": STATE["volume"]})
            elif path == "/api/info" and method == "GET":
                self.reply(200, {"freeProjectSpace": 50 * 1024 * 1024})
            elif path == "/api/users" and method == "GET":
                self.reply(200, STATE["users"])
            elif path == "/api/users" and method == "POST":
                bump("add_user")
                req = json.loads(raw or b"{}")
                STATE["users"].append({"id": uuid.uuid4().hex, "username": req.get("username", ""),
                                       "group": req.get("group", "")})
                self.reply(201, {})
            elif path == "/api/key" and method == "PATCH":
                bump("volume")
                try:
                    req = json.loads(raw or b"{}")
                    if req.get("key") == "audio/user_level":
                        STATE["volume"] = int(req["value"])
                except (ValueError, KeyError):
                    pass
                self.reply(200, {})
            elif path == "/api/file" and method == "POST":
                bump("upload_chunk")
                self.reply(200, {})
            elif path == "/api/messages" and method == "GET":
                self.reply(200, STATE["messages"])
            elif path == "/api/messages" and method == "POST":
                req = json.loads(raw or b"{}")
                STATE["messages"].append({"id": uuid.uuid4().hex, "fileName": req.get("fileName"),
                                          "label": req.get("label")})
                self.reply(201, {})
            elif path.startswith("/api/messages/") and method == "PUT":
                bump(path.rsplit("/", 1)[-1])
                self.reply(200, {})
            elif path.startswith("/api/messages/") and method == "DELETE":
                bump("delete")
                self.reply(200, {})
            else:
                bump("not_found")
                self.reply(404, {"error": "not found"})

        def dispatch(self, method):
            if self.path.startswith("/onvif"):
                self.proxy_onvif(method)
            elif self.path.startswith("/api/"):
                self.handle_api(method)
            else:
                self.body()
                self.reply(404)

        def do_GET(self):
            self.dispatch("GET")

        def do_POST(self):
            self.dispatch("POST")

        def do_PUT(self):
            self.dispatch("PUT")

        def do_DELETE(self):
            self.dispatch("DELETE")

        def do_PATCH(self):
            self.dispatch("PATCH")

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=18680)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin")
    ap.add_argument("--onvif-upstream", default=None, help="host:port của mock ONVIF, /onvif/* chuyển tới đó")
    ap.add_argument("--control", default=None)
    ap.add_argument("--state", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    with LOCK:
        save_state(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(args))
    server.daemon_threads = True
    print("bosch_api_mock listening on %s:%d (onvif upstream: %s)" % (args.host, args.port, args.onvif_upstream),
          flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
