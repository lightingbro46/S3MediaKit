#!/usr/bin/env python3
"""Minimal stand-in for the external service API used by MediaServer hooks.

Every hook gets HTTP 200 with a small non-empty JSON body. In Debug builds the
MediaServer reads the real configuration from ./api-config.json once the
configuration hook succeeds, so the body content does not matter.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def _reply(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        body = json.dumps({"code": 0, "ok": 1}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = _reply
    do_POST = _reply
    do_PUT = _reply

    def log_message(self, fmt, *args):
        sys.stdout.write("%s %s\n" % (self.command, self.path))
        sys.stdout.flush()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18090
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
