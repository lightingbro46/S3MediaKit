#!/usr/bin/env python3
"""Minimal S3 stand-in for the health monitor check (run_cluster.sh <dir> monitor).

Answers every request with 200 and keeps PUT bodies in memory, so HeadBucket (the pool health
probe) and a put/get/delete connection test pass. Stopping the process plays "MinIO is down".

Usage: fake_s3.py <port>
"""
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

OBJECTS = {}
EMPTY_LIST = (b'<?xml version="1.0" encoding="UTF-8"?>'
              b'<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">'
              b'<Name>b</Name><KeyCount>0</KeyCount><IsTruncated>false</IsTruncated></ListBucketResult>')


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def reply(self, body=b"", ctype="application/xml"):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("ETag", '"0"')
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        body = OBJECTS.get(self.path.split("?")[0])
        self.send_response(200)
        self.send_header("Content-Length", str(len(body) if body is not None else 0))
        self.send_header("ETag", '"0"')
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in OBJECTS:
            self.reply(OBJECTS[path], "application/octet-stream")
        else:
            self.reply(EMPTY_LIST)

    def do_PUT(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        data = self.rfile.read(n) if n else b""
        OBJECTS[self.path.split("?")[0]] = data
        self.reply()

    def do_DELETE(self):
        OBJECTS.pop(self.path.split("?")[0], None)
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n:
            self.rfile.read(n)
        self.reply(b'<?xml version="1.0" encoding="UTF-8"?><DeleteResult/>')


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
