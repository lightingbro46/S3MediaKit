#!/usr/bin/env python3
"""Mock trang thông tin thiết bị SUNAPI của camera Hanwha Vision (Wisenet), xác thực HTTP Digest.

Chỉ phục vụ plugin camera Hanwha (ext-plugin/Hanwha) nhận hãng:
    GET /stw-cgi/system.cgi?msubmenu=deviceinfo&action=view
trả các dòng "Key=Value" (hoặc JSON với --json). Đường khác trả 404, như camera thật với API của hãng khác.

    sunapi_mock.py [--port 18690] [--user admin --password admin123] [--json]

Test: SUNAPI_MOCK_ADDR=127.0.0.1:18690 unit_vendor_stage --gtest_filter='VendorCameraMockTest.*'
"""
import argparse
import hashlib
import json
import os
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REALM = "iPOLiS"
DEVICE_INFO_PATH = "/stw-cgi/system.cgi?msubmenu=deviceinfo&action=view"
DEVICE_INFO = [
    ("Model", "XNV-8080R"),
    ("SerialNumber", "ZC7S70GR300017A"),
    ("FirmwareVersion", "2.21.02_20230712"),
    ("BuildDate", "2023-07-12"),
    ("ConnectedMACAddress", "00:16:6C:D1:00:01"),
    ("DeviceType", "NWC"),
]


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18690)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin123")
    ap.add_argument("--json", action="store_true", help="trả JSON thay cho Key=Value")
    args = ap.parse_args()

    class Handler(BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="text/plain", headers=()):
            data = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            for k, v in headers:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(data)

        def _authorized(self):
            auth = self.headers.get("Authorization", "")
            if not auth.startswith("Digest "):
                return False
            p = dict(re.findall(r'(\w+)="?([^",]*)"?', auth[7:]))
            if p.get("username") != args.user:
                return False
            ha1 = md5("%s:%s:%s" % (args.user, REALM, args.password))
            ha2 = md5("GET:%s" % p.get("uri", ""))
            if p.get("qop"):
                want = md5(":".join([ha1, p.get("nonce", ""), p.get("nc", ""), p.get("cnonce", ""), p["qop"], ha2]))
            else:
                want = md5("%s:%s:%s" % (ha1, p.get("nonce", ""), ha2))
            return p.get("response") == want and p.get("uri") == self.path

        def do_GET(self):
            if self.path != DEVICE_INFO_PATH:
                return self._send(404, "Not Found")
            if not self._authorized():
                nonce = os.urandom(8).hex()
                challenge = 'Digest realm="%s", qop="auth", nonce="%s"' % (REALM, nonce)
                return self._send(401, "Unauthorized", headers=[("WWW-Authenticate", challenge)])
            if args.json:
                return self._send(200, json.dumps(dict(DEVICE_INFO)), "application/json")
            return self._send(200, "".join("%s=%s\r\n" % kv for kv in DEVICE_INFO))

        def log_message(self, fmt, *a):
            pass

    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
