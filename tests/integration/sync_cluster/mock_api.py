#!/usr/bin/env python3
"""Minimal stand-in for the external service API used by MediaServer hooks.

Every hook gets HTTP 200 with a small non-empty JSON body. In Debug builds the
MediaServer reads the real configuration from ./api-config.json once the
configuration hook succeeds, so the body content does not matter.

Optional second argument: a file listing media server ids (one per line) the service
no longer knows. Their requests get the service API's HTTP 412 "not qualified" answer.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


NOT_QUALIFIED_FILE = None


def not_qualified_ids():
    if not NOT_QUALIFIED_FILE:
        return set()
    try:
        return {line.strip() for line in open(NOT_QUALIFIED_FILE) if line.strip()}
    except OSError:
        return set()


class Handler(BaseHTTPRequestHandler):
    def _reply(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            server_id = json.loads(raw.decode("utf-8", "replace")).get("mediaServerId", "")
        except (ValueError, AttributeError):
            server_id = ""
        status, body = 200, {"code": 0, "ok": 1}
        if server_id and server_id in not_qualified_ids():
            status = 412
            body = {"status": 412, "code": 689501, "message": "No recording server is qualified", "traceId": None}
        body = json.dumps(body).encode()
        self.send_response(status)
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
    NOT_QUALIFIED_FILE = sys.argv[2] if len(sys.argv) > 2 else None
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
