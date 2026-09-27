#!/usr/bin/env python3
"""Kiểm tự động các kịch bản "A" trong docs/e2e-test-plan.md.

Dùng:
    e2e_check.py <work> <nhóm> [<nhóm> ...] [tuỳ chọn]

Nhóm: smoke live auth overlay record replay auto-profile bookmark extract proxy
      tier-setup tier-verify encrypt-setup encrypt-verify all-basic

Kết quả từng kịch bản in ra màn hình và gộp vào <work>/report.json (theo ID, lần chạy sau
ghi đè lần trước). Ảnh/khung hình làm bằng chứng cho phần kiểm mắt ghi vào <work>/evidence.

Không in secret, token hay URL RTSP. MinIO lấy từ biến môi trường E2E_S3_*.
"""
import argparse
import base64
import glob
import hashlib
import http.client
import json
import os
import re
import socket
import subprocess
import time
import urllib.parse

# ---------------------------------------------------------------- hạ tầng


class Ctx:
    def __init__(self, work, args):
        self.work = work
        self.args = args
        self.inv = json.load(open(os.path.join(work, "inventory.json")))
        self.sec = json.load(open(os.path.join(work, "secrets.json")))
        self.port = self.inv["ports"]
        self.cam = self.inv["camera_id"]
        self.main = self.inv["main_id"]
        self.sub = self.inv["sub_id"]
        self.host = "127.0.0.1"
        self.results = []
        self.evidence = os.path.join(work, "evidence")
        os.makedirs(self.evidence, exist_ok=True)
        self._key = None

    # ---- kết quả
    def record(self, sid, ok, detail=""):
        status = {True: "PASS", False: "FAIL", None: "SKIP"}[ok]
        detail = redact(str(detail))
        self.results.append({"id": sid, "status": status, "detail": detail, "t": int(time.time())})
        print("%-8s %-5s %s" % (sid, status, detail), flush=True)
        return ok

    def save(self):
        path = os.path.join(self.work, "report.json")
        old = {}
        if os.path.exists(path):
            old = {r["id"]: r for r in json.load(open(path))}
        for r in self.results:
            old[r["id"]] = r
        with open(path, "w") as fh:
            json.dump(sorted(old.values(), key=lambda r: r["id"]), fh, indent=1)
        n = {}
        for r in self.results:
            n[r["status"]] = n.get(r["status"], 0) + 1
        print("Tổng lần chạy này: %s  -> %s" % (json.dumps(n), path))

    # ---- JWT
    def jwt(self, user="e2e-user", role=None, perms=None, level=1, project=None,
            overlay=None, exp_in=3600, bad_sig=False):
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import padding

        if self._key is None:
            self._key = serialization.load_pem_private_key(
                open(os.path.join(self.work, "jwt_private.pem"), "rb").read(), password=None)
        now = int(time.time())
        payload = {
            "sub": user, "user_id": user, "iat": now, "exp": now + exp_in,
            "project_id": project or self.inv["project_id"], "level": level,
            "role_code": role or self.inv["roles"]["viewer"], "role_name": role or "viewer",
            "session_id": "e2e-" + user,
            "permissions": ",".join(perms if perms is not None else
                                    ["1002", "1003", "1004", "200101", "200102",
                                     "150301", "150302", "40102"]),
        }
        if overlay is not None:
            payload["overlay"] = overlay

        def b64(obj):
            raw = json.dumps(obj, separators=(",", ":")).encode()
            return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()

        signing = "%s.%s" % (b64({"alg": "RS256", "typ": "JWT"}), b64(payload))
        sig = self._key.sign(signing.encode(), padding.PKCS1v15(), hashes.SHA256())
        if bad_sig:
            sig = bytes(reversed(sig))
        return signing + "." + base64.urlsafe_b64encode(sig).rstrip(b"=").decode()

    def admin(self):
        return self.jwt(user="e2e-admin", role=self.inv["roles"]["admin"])

    # ---- HTTP
    def http(self, method, path, params=None, body=None, token=None, headers=None,
             timeout=15, raw=False, max_bytes=None):
        """Trả (status, headers dict, body). body là JSON nếu parse được, trừ khi raw=True."""
        if params:
            path += ("&" if "?" in path else "?") + urllib.parse.urlencode(params)
        hdr = dict(headers or {})
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            hdr["Content-Type"] = "application/json"
        if token:
            hdr["Authorization"] = "Bearer " + token
        conn = http.client.HTTPConnection(self.host, self.port["http"], timeout=timeout)
        try:
            conn.request(method, path, body=data, headers=hdr)
            resp = conn.getresponse()
            payload = resp.read(max_bytes) if max_bytes else resp.read()
            h = {k.lower(): v for k, v in resp.getheaders()}
            if raw:
                return resp.status, h, payload
            try:
                return resp.status, h, json.loads(payload.decode("utf-8", "replace") or "null")
            except ValueError:
                return resp.status, h, payload
        finally:
            conn.close()

    def index_api(self, name, **params):
        params["secret"] = self.sec["api_secret"]
        return self.http("GET", "/index/api/" + name, params)[2]

    def api(self, path, params=None, body=None, token=None, method=None):
        token = token or self.admin()
        return self.http(method or ("POST" if body is not None else "GET"), path,
                         params=params, body=body, token=token)

    def url(self, path, token=None, scheme="http", **query):
        if token:
            query["token"] = token
        q = ("?" + urllib.parse.urlencode(query)) if query else ""
        port = {"http": self.port["http"], "ws": self.port["http"],
                "rtsp": self.port["rtsp"], "rtmp": self.port["rtmp"]}[scheme]
        return "%s://%s:%d%s%s" % (scheme, self.host, port, path, q)

    # ---- trạng thái luồng
    def media_list(self):
        res = self.index_api("getMediaList")
        return res.get("data") or [] if isinstance(res, dict) else []

    def stream_info(self):
        """{stream_id: {width,height,codec,recording}} của camera theo schema rtsp."""
        out = {}
        for m in self.media_list():
            if m.get("app") != self.cam or m.get("schema") != "rtsp":
                continue
            info = {"recording": bool(m.get("isRecordingMP4")), "readers": m.get("totalReaderCount")}
            for t in m.get("tracks") or []:
                if t.get("codec_type") == 0:
                    info.update(width=t.get("width"), height=t.get("height"),
                                codec=t.get("codec_id_name"), fps=t.get("fps"))
                elif t.get("codec_type") == 1:
                    info["audio"] = t.get("codec_id_name")
            out[m.get("stream")] = info
        return out

    def resolutions(self):
        path = os.path.join(self.work, "streams.json")
        if os.path.exists(path):
            return json.load(open(path))
        return {}


def redact(text):
    """Che JWT, secret trong query và user:pass trong URL trước khi in/ghi báo cáo."""
    text = re.sub(r"eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]*", "<jwt>", text)
    text = re.sub(r"(token|secret|access_key|secret_key)=[^&\s'\"]+", r"\1=***", text)
    return re.sub(r"(rtsps?|rtmp|https?)://[^/@\s'\"]+@", r"\1://***@", text)


# ---------------------------------------------------------------- ffmpeg helpers


def ffprobe(url, timeout=20, headers=None):
    """Trả dict {width,height,codec,audio} của luồng video đầu tiên, hoặc {'error': ...}."""
    cmd = ["ffprobe", "-v", "error", "-rw_timeout", str(timeout * 1000000),
           "-show_entries", "stream=codec_type,codec_name,width,height",
           "-of", "json"]
    if headers:
        cmd += ["-headers", headers]
    if url.startswith("rtsp://"):
        cmd += ["-rtsp_transport", "tcp"]
    cmd.append(url)
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=timeout + 5, text=True)
    except subprocess.TimeoutExpired:
        return {"error": "timeout"}
    if out.returncode != 0:
        last = (out.stderr.strip().splitlines() or ["rc=%d" % out.returncode])[-1]
        if last.startswith(url):
            last = last[len(url):].lstrip(": ")
        return {"error": last[:200]}
    res = {}
    for s in json.loads(out.stdout or "{}").get("streams", []):
        if s.get("codec_type") == "video" and "width" not in res:
            res.update(width=s.get("width"), height=s.get("height"), codec=s.get("codec_name"))
        elif s.get("codec_type") == "audio":
            res["audio"] = s.get("codec_name")
    return res if "width" in res else {"error": "không có track video"}


def pace(url, media_sec, timeout=None):
    """Giải mã media_sec giây rồi trả (wall_sec, số dòng lỗi decode, stderr rút gọn)."""
    timeout = timeout or media_sec * 6 + 30
    cmd = ["ffmpeg", "-hide_banner", "-nostats", "-loglevel", "error",
           "-rw_timeout", "20000000"]
    if url.startswith("rtsp://"):
        cmd += ["-rtsp_transport", "tcp"]
    cmd += ["-i", url, "-t", str(media_sec), "-map", "0:v:0", "-f", "null", "-"]
    t0 = time.time()
    try:
        out = subprocess.run(cmd, capture_output=True, timeout=timeout, text=True)
    except subprocess.TimeoutExpired:
        return time.time() - t0, -1, "timeout"
    errors = [l for l in out.stderr.splitlines() if l.strip()]
    return time.time() - t0, (len(errors) if out.returncode == 0 else -1), " | ".join(errors[-3:])[:300]


def grab_frame(url, dst, seek=0):
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-rw_timeout", "20000000"]
    if url.startswith("rtsp://"):
        cmd += ["-rtsp_transport", "tcp"]
    cmd += ["-i", url]
    if seek:
        cmd += ["-ss", str(seek)]
    cmd += ["-frames:v", "1", dst]
    try:
        return subprocess.run(cmd, capture_output=True, timeout=60).returncode == 0
    except subprocess.TimeoutExpired:
        return False


def file_start_time(path):
    """Epoch bắt đầu của file ghi, lấy từ tên `YYYY-MM-DD-HH-MM-SS-<n>.mp4[.enc]` (giờ local)."""
    m = re.match(r"(\d{4})-(\d{2})-(\d{2})-(\d{2})-(\d{2})-(\d{2})-", os.path.basename(path))
    if not m:
        return None
    return int(time.mktime(tuple(int(x) for x in m.groups()) + (0, 0, -1)))


def same_res(probe, ref):
    return ref and probe.get("width") == ref.get("width") and probe.get("height") == ref.get("height")


def ws_fmp4(ctx, path, token, seconds=6):
    """Client WebSocket tối giản: trả danh sách box top-level đã thấy (ftyp/moov/moof...)."""
    key = base64.b64encode(os.urandom(16)).decode()
    q = "?" + urllib.parse.urlencode({"token": token})
    s = socket.create_connection((ctx.host, ctx.port["http"]), timeout=10)
    s.sendall(("GET %s%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
               % (path, q, ctx.host, key)).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = s.recv(4096)
        if not chunk:
            break
        head += chunk
    status = head.split(b"\r\n", 1)[0].decode(errors="replace")
    if " 101 " not in status:
        s.close()
        return status, []
    buf = head.split(b"\r\n\r\n", 1)[1]
    payload = b""
    end = time.time() + seconds
    s.settimeout(2)
    while time.time() < end and len(payload) < 8 * 1024 * 1024:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 2:
            ln = buf[1] & 0x7F
            off = 2
            if ln == 126:
                if len(buf) < 4:
                    break
                ln = int.from_bytes(buf[2:4], "big"); off = 4
            elif ln == 127:
                if len(buf) < 10:
                    break
                ln = int.from_bytes(buf[2:10], "big"); off = 10
            if len(buf) < off + ln:
                break
            payload += buf[off:off + ln]
            buf = buf[off + ln:]
    s.close()
    boxes, i = [], 0
    while i + 8 <= len(payload) and len(boxes) < 64:
        size = int.from_bytes(payload[i:i + 4], "big")
        boxes.append(payload[i + 4:i + 8].decode("latin1"))
        if size < 8:
            break
        i += size
    return status, boxes


# ---------------------------------------------------------------- nhóm kịch bản


def g_smoke(ctx):
    st, _, body = ctx.http("GET", "/media/mserver/healthcheck")
    ctx.record("SMK-01", st == 200, "healthcheck HTTP %s" % st)
    info = {}
    for _ in range(20):
        info = ctx.stream_info()
        if ctx.main in info and ctx.sub in info and info[ctx.main].get("width"):
            break
        time.sleep(1)
    ok = ctx.main in info and ctx.sub in info
    detail = ", ".join("%s=%sx%s %s %sfps audio=%s" % (k, v.get("width"), v.get("height"),
                                                      v.get("codec"), v.get("fps"), v.get("audio"))
                       for k, v in sorted(info.items()))
    ctx.record("SMK-02", ok, detail or "không thấy luồng camera trong getMediaList")
    if ok:
        with open(os.path.join(ctx.work, "streams.json"), "w") as fh:
            json.dump(info, fh, indent=1)
        if info[ctx.main].get("width") == info[ctx.sub].get("width"):
            print("  CẢNH BÁO: Main và Sub cùng độ phân giải — không phân biệt được profile khi replay")
    slow = []
    for name in ("version", "getServerConfig", "getThreadsLoad", "getWorkThreadsLoad", "getStatistic"):
        t0 = time.time()
        res = ctx.index_api(name)
        dt = time.time() - t0
        if not isinstance(res, dict) or res.get("code") != 0 or dt > 2:
            slow.append("%s code=%s %.1fs" % (name, isinstance(res, dict) and res.get("code"), dt))
    st, _, res = ctx.api("/media/mserver/systemStatistic")
    if st != 200 or not isinstance(res, dict) or res.get("code") != 0:
        slow.append("systemStatistic HTTP %s" % st)
    ctx.record("SMK-03", not slow, "; ".join(slow) or "5 API /index + systemStatistic OK")
    lat = []
    for _ in range(20):
        t0 = time.time()
        ctx.http("GET", "/media/mserver/healthcheck", timeout=5)
        lat.append((time.time() - t0) * 1000)
    lat.sort()
    ctx.record("SMK-04", lat[18] <= 1000, "healthcheck p95=%.0fms max=%.0fms" % (lat[18], lat[-1]))


def g_live(ctx):
    res = ctx.resolutions()
    tok = ctx.admin()
    cam, main, sub = ctx.cam, ctx.main, ctx.sub

    def check(sid, url, ref, label):
        p = ffprobe(url)
        ok = "error" not in p and same_res(p, ref)
        return ctx.record(sid, ok, "%s -> %s" % (label, p))

    ok_main = check("LIV-01", ctx.url("/%s/%s" % (cam, main), tok, "rtsp"), res.get(main), "rtsp main")
    check("LIV-01b", ctx.url("/%s/%s" % (cam, sub), tok, "rtsp"), res.get(sub), "rtsp sub")
    check("LIV-02", ctx.url("/%s/%s" % (cam, main), tok, "rtmp"), res.get(main), "rtmp main")
    check("LIV-03", ctx.url("/media/live/%s/%s.live.flv" % (cam, main), tok), res.get(main), "http-flv")
    check("LIV-04", ctx.url("/media/live/%s/%s.live.ts" % (cam, main), tok), res.get(main), "http-ts")
    check("LIV-05", ctx.url("/media/live/%s/%s.live.mp4" % (cam, main), tok), res.get(main), "http-fmp4")
    status, boxes = ws_fmp4(ctx, "/media/live/%s/%s.live.mp4" % (cam, main), tok)
    ok = boxes[:2] == ["ftyp", "moov"] and "moof" in boxes
    ctx.record("LIV-06", ok, "%s boxes=%s" % (status, boxes[:6]))

    # HLS: không session_id -> 302 kèm session_id
    st, h, _ = ctx.http("GET", "/media/live/%s/%s/hls.m3u8" % (cam, main), {"token": tok}, raw=True)
    loc = h.get("location", "")
    ok = st == 302 and "session_id=" in loc
    detail = "HTTP %s location có session_id=%s" % (st, "session_id=" in loc)
    if ok:
        p = ffprobe(ctx.url("/media/live/%s/%s/hls.m3u8" % (cam, main), tok, session_id="e2e-liv07"))
        ok = "error" not in p and same_res(p, res.get(main))
        detail += "; probe=%s" % p
    ctx.record("LIV-07", ok, detail)

    st, _, body = ctx.http("GET", "/media/live/%s/%s/hls.fmp4.m3u8" % (cam, main),
                           {"token": tok, "session_id": "e2e-liv08"}, raw=True)
    has_map = b"#EXT-X-MAP" in body if isinstance(body, bytes) else False
    p = ffprobe(ctx.url("/media/live/%s/%s/hls.fmp4.m3u8" % (cam, main), tok, session_id="e2e-liv08"))
    ctx.record("LIV-08", st == 200 and has_map and "error" not in p,
               "HTTP %s EXT-X-MAP=%s probe=%s" % (st, has_map, p))

    st, _, body = ctx.http("GET", "/media/live/%s/hls.master.m3u8" % cam,
                           {"token": tok, "session_id": "e2e-liv09"}, raw=True)
    text = body.decode(errors="replace") if isinstance(body, bytes) else ""
    ok = st == 200 and main in text and sub in text and text.count("session_id=e2e-liv09") >= 2
    ctx.record("LIV-09", ok, "HTTP %s, master liệt kê main=%s sub=%s" % (st, main in text, sub in text))

    for sid, q, ref in (("LIV-10a", {"quality": "hi"}, main), ("LIV-10b", {"quality": "lo"}, sub),
                        ("LIV-10c", {"quality": "auto", "prefered": "hi"}, main)):
        check(sid, ctx.url("/media/live/%s.live2.mp4" % cam, tok, **q), res.get(ref), "live2 %s" % q)

    st, _, _ = ctx.http("GET", "/media/live/%s/%s/hls.m3u8" % (cam, main),
                        {"token": tok, "session_id": "x" * 129}, raw=True)
    st2, _, _ = ctx.http("GET", "/media/live/%s/%s/hls.m3u8" % (cam, main),
                         {"token": tok, "session_id": "bad id<>"}, raw=True)
    ctx.record("LIV-13", st == 400 and st2 == 400, "129 ký tự -> %s, ký tự lạ -> %s" % (st, st2))

    res_snap = ctx.http("GET", "/index/api/getSnap", {
        "secret": ctx.sec["api_secret"], "url": ctx.url("/%s/%s" % (cam, main), tok, "rtsp"),
        "timeout_sec": 10, "expire_sec": 1}, raw=True, timeout=30)
    jpeg = res_snap[2][:3] == b"\xff\xd8\xff"
    if jpeg:
        open(os.path.join(ctx.evidence, "LIV-16_snap.jpg"), "wb").write(res_snap[2])
    ctx.record("LIV-16", jpeg, "HTTP %s JPEG=%s (ảnh: evidence/LIV-16_snap.jpg)" % (res_snap[0], jpeg))

    if ok_main:
        wall, errs, msg = pace(ctx.url("/media/live/%s/%s.live.mp4" % (cam, main), tok), 20)
        ctx.record("LIV-P1", errs == 0 and 15 <= wall <= 30,
                   "20 s live fMP4 mất %.1fs, lỗi decode=%s %s" % (wall, errs, msg))


def g_auth(ctx):
    cam, main = ctx.cam, ctx.main
    path = "/media/live/%s/%s.live.mp4" % (cam, main)

    def denied(token=None, header=None):
        params = {"token": token} if token else None
        st, _, body = ctx.http("GET", path, params, headers=header, raw=True, max_bytes=4096, timeout=10)
        return st, st != 200 or len(body) == 0

    st, ok = denied()
    ctx.record("AUTH-01", ok, "không token -> HTTP %s" % st)
    st1, ok1 = denied(ctx.jwt(bad_sig=True))
    st2, ok2 = denied(ctx.jwt(exp_in=-900))
    ctx.record("AUTH-02", ok1 and ok2, "chữ ký sai -> %s, hết hạn -> %s" % (st1, st2))
    st, ok = denied(ctx.jwt(project="other-project"))
    ctx.record("AUTH-03", ok, "project khác -> HTTP %s" % st)

    deny = os.path.join(ctx.work, "deny_users.json")
    json.dump(["e2e-denied"], open(deny, "w"))
    try:
        tok = ctx.jwt(user="e2e-denied")
        st, ok_http = denied(tok)
        rt = ffprobe(ctx.url("/%s/%s" % (cam, main), tok, "rtsp"), timeout=10)
        rm = ffprobe(ctx.url("/%s/%s" % (cam, main), tok, "rtmp"), timeout=10)
        ok = ok_http and "error" in rt and "error" in rm
        ctx.record("AUTH-04", ok, "on_play từ chối: http=%s rtsp=%s rtmp=%s"
                   % (st, rt.get("error", "PHÁT ĐƯỢC"), rm.get("error", "PHÁT ĐƯỢC")))
    finally:
        json.dump([], open(deny, "w"))

    st, _, body = ctx.http("GET", path, headers={"Authorization": "Bearer " + ctx.admin()},
                           raw=True, max_bytes=4096, timeout=10)
    ctx.record("AUTH-05", st == 200 and body[4:8] == b"ftyp", "header Bearer -> HTTP %s" % st)

    no_read = ctx.jwt(user="e2e-noread", perms=["1002", "1003"])
    st, _, res = ctx.api("/media/mserver/storage/pool/list", token=no_read)
    st2, _, res2 = ctx.api("/media/mserver/storage/policy/clone",
                           {"id": "policy-system-default", "name": "x"}, token=no_read, method="POST")
    c1 = isinstance(res, dict) and res.get("code")
    c2 = isinstance(res2, dict) and res2.get("code")
    ctx.record("AUTH-07", c1 == 901006 and c2 == 901007, "thiếu 150301 -> %s, thiếu 150302 -> %s" % (c1, c2))

    no_pb = ctx.jwt(user="e2e-nopb", perms=["1002", "150301"])
    st, _, res = ctx.api("/media/esc/recordedTimePeriod",
                         {"cameraId": cam, "startTime": 0, "endTime": 0, "periodType": 2, "detail": 0},
                         token=no_pb)
    c = isinstance(res, dict) and res.get("code")
    ctx.record("AUTH-08", c == 901004, "timeline thiếu 1003 -> code %s" % c)

    res = ctx.http("GET", "/index/api/getMediaList", {"secret": "wrong"})[2]
    c = isinstance(res, dict) and res.get("code")
    ctx.record("AUTH-09", c not in (0, None), "/index/api sai secret -> code %s" % c)


def g_overlay(ctx):
    """Cần camera nạp với --overlay 1."""
    cam, main = ctx.cam, ctx.main
    roles = ctx.inv["roles"]
    viewer = ctx.jwt(user="e2e-viewer", role=roles["viewer"])
    admin = ctx.jwt(user="e2e-admin", role=roles["admin"])
    wm_only = ctx.jwt(user="e2e-wmonly", role=roles["wm_only"])
    no_overlay = ctx.jwt(user="e2e-nooverlay", role=roles["viewer"], overlay=False)
    live = "/media/live/%s/%s.live.mp4" % (cam, main)

    def transcodes():
        return sorted({m["stream"] for m in ctx.media_list()
                       if m.get("app") == cam and ".transcode." in m.get("stream", "")})

    before = transcodes()
    for name, tok in (("viewer", viewer), ("admin", admin), ("wmonly", wm_only), ("nooverlay", no_overlay)):
        grab_frame(ctx.url(live, tok), os.path.join(ctx.evidence, "OVL_live_%s.jpg" % name))
    after = transcodes()
    ctx.record("OVL-01", len(after) > 0,
               "derived trước=%s sau=%s; xem evidence/OVL_live_viewer.jpg (phải có watermark + mask)"
               % (before, after))

    st, h, _ = ctx.http("GET", "/media/live/%s/%s/hls.m3u8" % (cam, main),
                        {"token": viewer, "session_id": "e2e-ovl02"}, raw=True)
    loc = h.get("location", "")
    ok = st == 302 and ".transcode." in loc
    ctx.record("OVL-02", ok, "HLS viewer -> HTTP %s, redirect sang derived=%s" % (st, ".transcode." in loc))
    if ok:
        target = urllib.parse.urlsplit(loc)
        q = dict(urllib.parse.parse_qsl(target.query))
        q["token"] = admin
        st3, _, _ = ctx.http("GET", target.path, q, raw=True)
        ctx.record("OVL-03", st3 == 403, "playlist derived của viewer gọi bằng JWT admin -> %s" % st3)
    else:
        ctx.record("OVL-03", None, "cần OVL-02 đạt")

    st, h, _ = ctx.http("GET", "/media/live/%s/%s/hls.m3u8" % (cam, main),
                        {"token": admin, "session_id": "e2e-ovl04"}, raw=True)
    loc = h.get("location", "")
    ctx.record("OVL-04", st in (200, 302) and ".transcode." not in loc,
               "HLS admin -> HTTP %s, derived=%s (mong đợi source gốc); ảnh evidence/OVL_live_admin.jpg"
               % (st, ".transcode." in loc))
    st, h, _ = ctx.http("GET", "/media/live/%s/%s/hls.m3u8" % (cam, main),
                        {"token": no_overlay, "session_id": "e2e-ovl06"}, raw=True)
    loc = h.get("location", "")
    ctx.record("OVL-06", st in (200, 302) and ".transcode." not in loc,
               "JWT overlay=false -> HTTP %s, derived=%s" % (st, ".transcode." in loc))

    rt = ffprobe(ctx.url("/%s/%s" % (cam, main), viewer, "rtsp"))
    rm = ffprobe(ctx.url("/%s/%s" % (cam, main), viewer, "rtmp"))
    ctx.record("OVL-07", "error" not in rt and "error" not in rm,
               "viewer RTSP=%s RTMP=%s (source gốc, không 403)" % (rt, rm))

    stamp = pick_stamp(ctx, back=120)
    if stamp:
        url = ctx.url("/media/record/%s/vod/%d.live2.mp4" % (cam, stamp), viewer, quality="auto", prefered="hi")
        ok = grab_frame(url, os.path.join(ctx.evidence, "OVL-08_replay_viewer.jpg"))
        ctx.record("OVL-08", ok, "replay auto viewer; xem evidence/OVL-08_replay_viewer.jpg")
        url = ctx.url("/media/record/%s/vod/%d/hls.m3u8" % (cam, stamp), viewer,
                      quality="auto", prefered="hi", session_id="e2e-ovl09")
        ok = grab_frame(url, os.path.join(ctx.evidence, "OVL-09_replay_hls_viewer.jpg"))
        ctx.record("OVL-09", ok, "replay HLS viewer; xem evidence/OVL-09_replay_hls_viewer.jpg")
    else:
        ctx.record("OVL-08", None, "chưa có bản ghi")


def g_record(ctx):
    """Đối chiếu trạng thái ghi với kỳ vọng --expect-main/--expect-sub (1/0)."""
    exp = {ctx.main: ctx.args.expect_main, ctx.sub: ctx.args.expect_sub}
    info = ctx.stream_info()
    since = time.time() - ctx.args.window * 60
    sid = ctx.args.scenario or "REC"
    problems, notes = [], []
    for stream, want in exp.items():
        if want is None:
            continue
        state = info.get(stream, {}).get("recording")
        files = recent_files(ctx, stream, since)
        notes.append("%s rec=%s file_mới=%d" % (stream, state, len(files)))
        if bool(state) != bool(want):
            problems.append("%s isRecordingMP4=%s mong đợi %s" % (stream, state, bool(want)))
        if want and not files:
            problems.append("%s không có file mới trong %d phút" % (stream, ctx.args.window))
        if not want and files:
            problems.append("%s có %d file mới dù không được ghi" % (stream, len(files)))
        bad = [f for f in files[:-1] if not f.endswith(".enc") and "error" in ffprobe(f, timeout=10)]
        if bad:
            problems.append("%s: %d file đóng không đọc được: %s" % (stream, len(bad), bad[:2]))
    ctx.record(sid, not problems, "; ".join(problems or notes))


def recent_files(ctx, stream, since):
    roots = [os.path.join(ctx.work, "record")] + glob.glob(os.path.join(ctx.work, "pool-*"))
    out = []
    for root in roots:
        for f in glob.glob(os.path.join(root, "**", ctx.cam, stream, "**", "*.mp4*"), recursive=True):
            if os.path.getmtime(f) >= since and not f.endswith(".tmp"):
                out.append(f)
    return sorted(out, key=os.path.getmtime)


def timeline(ctx, start=0, end=0, stream=None):
    """Danh sách {stream,start,end} theo từng stream (periodType=1, detail=1).

    periodType=0 trả timeline đã gộp Main/Sub (mỗi khoảng chỉ mang một streamId), không dùng
    được để biết một stream có dữ liệu ở đâu. docs/api-timeline-thumbnail.md còn mô tả dạng
    {start,end} cũ.
    """
    st, _, res = ctx.api("/media/esc/recordedTimePeriod", {
        "cameraId": ctx.cam, "startTime": start, "endTime": end, "periodType": 1, "detail": 1})
    if not isinstance(res, dict) or res.get("code") != 0:
        return None
    out = []
    for s_ in (res.get("data") or {}).get("streams") or []:
        for p in s_.get("periods") or []:
            b = int(p.get("startTime", 0))
            item = {"stream": s_.get("streamId"), "start": b, "end": b + int(p.get("duration", 0)),
                    "tier": p.get("tier")}
            if stream is None or item["stream"] == stream:
                out.append(item)
    return out


def pick_stamp(ctx, back=300, need=60, stream=None):
    """Stamp mới nhất có ≥ `need` giây dữ liệu phía sau, cách hiện tại ít nhất `back` giây."""
    now = time.time()
    best = None
    for item in timeline(ctx, stream=stream or ctx.main) or []:
        e = min(item["end"], int(now - back))
        if e - item["start"] >= need:
            best = max(best or 0, e - need)
    return best


def g_replay(ctx):
    cam, main, sub = ctx.cam, ctx.main, ctx.sub
    res = ctx.resolutions()
    tok = ctx.admin()
    tl = timeline(ctx)
    streams = sorted({t["stream"] for t in tl or []})
    total = {st_: sum(t["end"] - t["start"] for t in tl if t["stream"] == st_) for st_ in streams}
    ctx.record("RPL-01", bool(tl), "%d block, stream=%s, tổng giây=%s" % (len(tl or []), streams, total))
    stamp = pick_stamp(ctx, back=ctx.args.back, need=45)
    if not stamp:
        ctx.record("RPL-02", None, "chưa có khoảng ghi ≥ 45 s cách hiện tại ≥ %d s" % ctx.args.back)
        return
    print("  stamp=%d (%s)" % (stamp, time.strftime("%H:%M:%S", time.localtime(stamp))))

    def play(sid, path, ref, media_sec=20, **q):
        url = ctx.url(path, tok, **q)
        p = ffprobe(url)
        if "error" in p or not same_res(p, res.get(ref)):
            return ctx.record(sid, False, "%s probe=%s" % (path, p))
        wall, errs, msg = pace(url, media_sec)
        ok = errs == 0 and media_sec * 0.85 <= wall <= media_sec * 1.4
        return ctx.record(sid, ok, "%dx%d, %d s media mất %.1fs, lỗi decode=%s %s"
                          % (p["width"], p["height"], media_sec, wall, errs, msg))

    play("RPL-02", "/media/record/%s/%s/vod/%d.live.mp4" % (cam, main, stamp), main)
    play("RPL-03", "/media/record/%s/%s/vod/%d.live.mp4" % (cam, sub, stamp), sub)
    grab_frame(ctx.url("/media/record/%s/%s/vod/%d.live.mp4" % (cam, main, stamp), tok),
               os.path.join(ctx.evidence, "RPL-04_first_frame_%d.jpg" % stamp))
    ctx.record("RPL-04", None, "kiểm tay: OSD trong evidence/RPL-04_first_frame_%d.jpg phải ≈ %s (≤ 1 GOP)"
               % (stamp, time.strftime("%H:%M:%S", time.localtime(stamp))))
    # RPL-22: request đầu tiên của replay gộp, stamp chưa mở lần nào (source chưa có sẵn).
    cold = stamp - 7
    st1, t1 = first_request(ctx, "/media/record/%s/vod/%d.live2.mp4" % (cam, cold),
                            {"token": tok, "quality": "auto", "prefered": "hi"})
    st2, t2 = first_request(ctx, "/media/record/%s/vod/%d/hls.m3u8" % (cam, cold - 3),
                            {"token": tok, "quality": "auto", "prefered": "hi", "session_id": "e2e-rpl22"})
    ctx.record("RPL-22", st1 == 200 and st2 in (200, 302) and max(t1, t2) <= 3,
               "request đầu: live2 HTTP %s sau %.2fs, HLS HTTP %s sau %.2fs (yêu cầu ≤ 3 s)" % (st1, t1, st2, t2))
    play("RPL-05a", "/media/record/%s/vod/%d.live2.mp4" % (cam, stamp), main, 10, quality="hi")
    play("RPL-05b", "/media/record/%s/vod/%d.live2.mp4" % (cam, stamp), sub, 10, quality="lo")
    play("RPL-06", "/media/record/%s/vod/%d.live2.mp4" % (cam, stamp), main, 10,
         quality="auto", prefered="hi")
    far = int(time.time()) - 30 * 86400
    st, _, _ = ctx.http("GET", "/media/record/%s/%s/vod/%d.live.mp4" % (cam, main, far),
                        {"token": tok}, raw=True, max_bytes=4096)
    ctx.record("RPL-11", st == 404, "stamp ngoài timeline -> HTTP %s" % st)

    status, boxes = ws_fmp4(ctx, "/media/record/%s/%s/vod/%d.live.mp4" % (cam, main, stamp), tok)
    ctx.record("RPL-12", boxes[:2] == ["ftyp", "moov"] and "moof" in boxes,
               "%s boxes=%s" % (status, boxes[:6]))

    for sid, name in (("RPL-13", "hls.m3u8"), ("RPL-14", "hls.fmp4.m3u8")):
        path = "/media/record/%s/vod/%d/%s" % (cam, stamp, name)
        st, h, _ = ctx.http("GET", path, {"token": tok, "quality": "auto", "prefered": "hi",
                                          "session_id": "e2e-" + sid.lower()}, raw=True)
        loc = h.get("location", "")
        leak = ".mp4" in urllib.parse.unquote(loc) and "/record/" in loc
        p = ffprobe(ctx.url(path, tok, quality="auto", prefered="hi", session_id="e2e-" + sid.lower()))
        ctx.record(sid, st in (200, 302) and "error" not in p and not leak,
                   "HTTP %s, lộ đường dẫn mp4=%s, probe=%s" % (st, leak, p))

    # RPL-15: đổi tốc độ khi đang phát
    url = ctx.url("/media/record/%s/%s/vod/%d.live.mp4" % (cam, main, stamp), tok)
    proc = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", url, "-t", "20",
                             "-map", "0:v:0", "-f", "null", "-"], stderr=subprocess.PIPE)
    time.sleep(2)
    # findPlaybackStream parse bằng MediaInfo: chỉ nhận URL tuyệt đối (path trần trả 906001).
    play_url = "http://%s:%d/media/record/%s/%s/vod/%d.live.mp4" % (ctx.host, ctx.port["http"], cam, main, stamp)
    st, _, res_speed = ctx.api("/media/mserver/playback/speed", body={"url": play_url, "speed": 2})
    t0 = time.time()
    proc.wait(timeout=120)
    wall = time.time() - t0 + 2
    code = isinstance(res_speed, dict) and res_speed.get("code")
    ctx.record("RPL-15", code == 0 and wall < 16,
               "speed=2 code=%s; 20 s media mất %.1fs (mong đợi ~11 s)" % (code, wall))

    # RPL-15b: như RPL-15 với replay gộp; url của API phải kèm đúng quality/prefered/duration đang phát.
    path2 = "/media/record/%s/vod/%d.live2.mp4" % (cam, stamp)
    proc = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error",
                             "-i", ctx.url(path2, tok, quality="auto", prefered="hi"), "-t", "20",
                             "-map", "0:v:0", "-f", "null", "-"], stderr=subprocess.PIPE)
    time.sleep(2)
    play_url = "http://%s:%d%s?quality=auto&prefered=hi" % (ctx.host, ctx.port["http"], path2)
    st, _, res_speed = ctx.api("/media/mserver/playback/speed", body={"url": play_url, "speed": 2})
    t0 = time.time()
    proc.wait(timeout=120)
    wall = time.time() - t0 + 2
    code = isinstance(res_speed, dict) and res_speed.get("code")
    ctx.record("RPL-15b", code == 0 and wall < 16,
               "live2 auto speed=2 code=%s; 20 s media mất %.1fs (mong đợi ~11 s)" % (code, wall))

    recent = int(time.time()) - 90
    wall, errs, msg = pace(ctx.url("/media/record/%s/%s/vod/%d.live.mp4" % (cam, main, recent), tok),
                           600, timeout=240)
    ctx.record("RPL-17", 0 <= errs and wall < 200,
               "replay stamp=now-90s kết thúc sau %.0fs (không chờ bản ghi mới), lỗi=%s %s"
               % (wall, errs, msg))

    t0 = time.time()
    st, h, img = ctx.http("GET", "/media/esc/recordedThumnail",
                          {"cameraId": cam, "streamId": main, "pos": stamp}, token=tok, raw=True, timeout=30)
    d1 = time.time() - t0
    t0 = time.time()
    st2, _, img2 = ctx.http("GET", "/media/esc/recordedThumnail",
                            {"cameraId": cam, "streamId": main, "pos": stamp}, token=tok, raw=True, timeout=30)
    d2 = time.time() - t0
    ok = img[:3] == b"\xff\xd8\xff" and img2[:3] == b"\xff\xd8\xff"
    if ok:
        open(os.path.join(ctx.evidence, "RPL-19_thumb_%d.jpg" % stamp), "wb").write(img)
    ctx.record("RPL-19", ok, "thumbnail HTTP %s/%s, lần 1 %.2fs lần 2 %.2fs (cache)" % (st, st2, d1, d2))


def first_request(ctx, path, params, timeout=30):
    """Một request duy nhất (không gọi lại): trả (HTTP status, giây tới header)."""
    t0 = time.time()
    st, _, _ = ctx.http("GET", path, params, raw=True, max_bytes=1024, timeout=timeout)
    return st, time.time() - t0


def resolution_changes(url, media_sec):
    """Giải mã media_sec giây, trả [(pts, 'WxH')] mỗi khi độ phân giải đổi, số dòng lỗi, giây đồng hồ."""
    cmd = ["ffmpeg", "-hide_banner", "-nostats", "-rw_timeout", "20000000", "-i", url,
           "-t", str(media_sec), "-map", "0:v:0", "-vf", "showinfo", "-f", "null", "-"]
    t0 = time.time()
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=media_sec * 3 + 60)
    except subprocess.TimeoutExpired:
        return [], -1, time.time() - t0
    seq = []
    for line in out.stderr.splitlines():
        m = re.search(r"pts_time:([\d.]+).* s:(\d+)x(\d+)", line)
        if m:
            res = "%sx%s" % (m.group(2), m.group(3))
            if not seq or seq[-1][1] != res:
                seq.append((round(float(m.group(1)), 2), res))
    # Server đóng kết nối khi hết `duration` (ffmpeg báo "Stream ends prematurely" rồi
    # "Input/output error" mỗi lần thử đọc lại): đó là kết thúc luồng, không phải lỗi decode.
    errs = len([l for l in out.stderr.splitlines()
                if " error" in l.lower() and "input/output error" not in l.lower()])
    return seq, errs, time.time() - t0


def g_auto_profile(ctx):
    """Replay quality=auto trên dữ liệu RecordLowResAndMotion (camera --mode lowres_motion --motion 1).

    Chọn stamp trong đoạn chỉ có Sub, ngay trước một đoạn Main (sự kiện motion): luồng phải bắt
    đầu bằng Sub rồi chuyển sang Main đúng thời điểm (RPL-06 bản đầy đủ).
    """
    res = ctx.resolutions()
    main_res, sub_res = res.get(ctx.main) or {}, res.get(ctx.sub) or {}
    main_wh = "%sx%s" % (main_res.get("width"), main_res.get("height"))
    sub_wh = "%sx%s" % (sub_res.get("width"), sub_res.get("height"))
    if main_wh == sub_wh:
        ctx.record("APF-01", None, "Main và Sub cùng độ phân giải, không phân biệt được")
        return
    now = int(time.time())
    since = ctx.args.since or now - 1800
    tl = timeline(ctx) or []

    def blocks(stream):
        return sorted((max(t["start"], since), t["end"]) for t in tl
                      if t["stream"] == stream and t["end"] > since)

    mb, sb = blocks(ctx.main), blocks(ctx.sub)
    span = now - since
    sub_total, main_total = sum(b - a for a, b in sb), sum(b - a for a, b in mb)
    ctx.record("APF-01", sub_total >= span * 0.8 and 0 < main_total < sub_total,
               "từ %s: Sub %ds/%ds (%d block), Main %ds (%d block motion)"
               % (time.strftime("%H:%M:%S", time.localtime(since)), sub_total, span, len(sb), main_total, len(mb)))

    lead = 20
    cand = None
    for i, (a, b) in enumerate(mb):
        prev_end = mb[i - 1][1] if i else since
        s_ = a - lead
        if s_ >= prev_end + 2 and b - a >= 10 and b < now - 30 and any(x <= s_ and a <= y for x, y in sb):
            cand = (s_, a, b)
    if not cand:
        ctx.record("APF-02", None, "chưa có đoạn Sub-only ≥ %d s ngay trước một đoạn Main ≥ 10 s" % lead)
        return
    stamp, m_start, m_end = cand
    print("  stamp=%s, Main %s dài %ds" % (time.strftime("%H:%M:%S", time.localtime(stamp)),
                                         time.strftime("%H:%M:%S", time.localtime(m_start)), m_end - m_start))
    # overlay=false: phát source replay gốc, tách lỗi replay khỏi overlay transcode (kiểm ở APF-06).
    raw_tok = ctx.jwt(user="e2e-apf", role=ctx.inv["roles"]["admin"], overlay=False)
    path = "/media/record/%s/vod/%d.live2.mp4" % (ctx.cam, stamp)
    q = {"quality": "auto", "prefered": "hi"}
    st, ttfb = first_request(ctx, path, dict(q, token=raw_tok))
    ctx.record("APF-05", st == 200 and ttfb <= 3, "request đầu HTTP %s sau %.2fs (yêu cầu 200, ≤ 3 s)" % (st, ttfb))

    # Dừng trước khi hết đoạn Main để chỉ có một lần chuyển Sub→Main. Replay bắt đầu ở keyframe
    # đầu tiên từ stamp, nên khung đầu cách stamp ≤ 1 GOP.
    media = lead + max(4, min(10, m_end - m_start - 2))
    # `duration` thuộc id của source replay: giá trị riêng cho mỗi phép đo để không nhập vào source
    # mà request trước đã mở (người xem sau sẽ vào giữa luồng, không từ stamp).
    seq, errs, wall = resolution_changes(ctx.url(path, raw_tok, duration=media + 1, **q), media)
    ok = (len(seq) == 2 and seq[0][1] == sub_wh and seq[0][0] <= 2.5 and seq[1][1] == main_wh
          and abs(seq[1][0] - (m_start - stamp)) <= 3)
    ctx.record("APF-02", ok, "(pts, độ phân giải)=%s; kỳ vọng [(≤2.5, %s), (~%d, %s)]; %ds media mất %.1fs"
               % (seq, sub_wh, m_start - stamp, main_wh, media, wall))
    # Player đọc fMP4 tuần tự (ffmpeg, VLC) không dùng init segment mới khi đổi track, nên cần
    # SPS/PPS trong luồng ở IDR đầu tiên sau lần chuyển. Trình duyệt (MSE) dùng init segment mới.
    ctx.record("APF-07", errs == 0, "lỗi decode quanh lần chuyển Sub→Main (fMP4 đọc tuần tự): %s dòng" % errs)

    st, _ = first_request(ctx, path, {"token": raw_tok, "quality": "hi"})
    st_lo, _ = first_request(ctx, path, {"token": raw_tok, "quality": "lo"})
    p_lo = ffprobe(ctx.url(path, raw_tok, quality="lo")) if st_lo == 200 else {}
    ctx.record("APF-03", st == 404 and st_lo == 200 and same_res(p_lo, sub_res),
               "quality=hi -> %s (chỉ có Sub nên phải 404); quality=lo -> %s %s" % (st, st_lo, p_lo))

    hls = "/media/record/%s/vod/%d/hls.m3u8" % (ctx.cam, stamp)
    hq = dict(q, session_id="e2e-apf04")
    st, ttfb = first_request(ctx, hls, dict(hq, token=raw_tok))
    hls_seq, hls_errs, _ = (resolution_changes(ctx.url(hls, raw_tok, duration=media + 2, **dict(hq, session_id="e2e-apf04b")), media)
                            if st in (200, 302) else ([], -1, 0))
    ctx.record("APF-04", st in (200, 302) and ttfb <= 3 and len(hls_seq) == 2 and hls_errs == 0,
               "HLS auto HTTP %s sau %.2fs; (pts, độ phân giải)=%s, lỗi decode=%s" % (st, ttfb, hls_seq, hls_errs))

    # Cùng stamp, role viewer có overlay: đầu ra đi qua derived transcode (P3 của fix plan).
    view_tok = ctx.jwt(user="e2e-apf-viewer")
    seq, errs, wall = resolution_changes(ctx.url(path, view_tok, duration=media + 3, **q), media)
    # Derived transcode giữ độ phân giải lớn nhất của timeline (Main) suốt luồng, đoạn Sub được upscale.
    ctx.record("APF-06", len(seq) == 1 and seq[0][1] == main_wh and errs == 0,
               "overlay viewer: (pts, độ phân giải)=%s, lỗi=%s; kỳ vọng chỉ %s — xem tay chất lượng đoạn Sub"
               % (seq, errs, main_wh))


def g_bookmark(ctx):
    tok = ctx.admin()
    stamp = pick_stamp(ctx, back=120, need=30)
    if not stamp:
        ctx.record("BKM-01", None, "chưa có bản ghi")
        return
    # Tên có dấu '-' và ':' để kiểm cả tìm kiếm full-text (FTS5 coi chúng là toán tử).
    name = "e2e-bkm:%d" % int(time.time())
    steps = []

    def ok(r):
        return isinstance(r, dict) and r.get("code") == 0

    def search(keyword):
        # sort chỉ nhận asc|desc (BookmarkIndexImp::search), page bắt đầu từ 0.
        _, _, r = ctx.api("/media/esc/bookmark/search", {
            "camera_id": ctx.cam, "start_time": stamp - 60, "end_time": stamp + 60,
            "page": 0, "size": 50, "sort": "desc", "search": keyword}, token=tok)
        items = (r.get("data") or []) if isinstance(r, dict) else []
        return r, [b for b in items if isinstance(b, dict)]

    _, _, r = ctx.api("/media/esc/bookmark/create", body={
        "name": name, "camera_id": ctx.cam, "start_time": stamp, "duration": 20,
        "description": "e2e", "tags": "e2e,incident"}, token=tok)
    steps.append(("create", r))
    r, items = search("")
    steps.append(("search", r))
    bid = next((b.get("id") for b in items if b.get("name") == name), None)
    for sid, keyword in (("BKM-03a", name), ("BKM-03b", "incident")):
        r, found = search(keyword)
        ctx.record(sid, ok(r) and any(b.get("id") == bid for b in found),
                   "search=%r -> code=%s msg=%s, %d kết quả"
                   % (keyword, (r or {}).get("code"), str((r or {}).get("msg"))[:120], len(found)))
    if bid:
        _, _, r = ctx.api("/media/esc/bookmark/detail", {"ids": bid}, token=tok); steps.append(("detail", r))
        _, _, r = ctx.api("/media/esc/bookmark/update", body={
            "id": bid, "camera_id": ctx.cam, "start_time": stamp, "duration": 25, "name": name + "-u"},
            token=tok); steps.append(("update", r))
        _, _, r = ctx.api("/media/esc/bookmark/recent",
                          {"camera_id": ctx.cam, "size": 10, "sort": "desc"}, token=tok); steps.append(("recent", r))
        _, _, r = ctx.api("/media/esc/bookmark/mostUsedTags", {"size": 10}, token=tok); steps.append(("tags", r))
        st, _, img = ctx.http("GET", "/media/esc/bookmark/recordThumbnail", {"id": bid}, token=tok,
                              raw=True, timeout=30)
        ctx.record("BKM-02", img[:3] == b"\xff\xd8\xff", "recordThumbnail HTTP %s" % st)
        _, _, r = ctx.api("/media/esc/bookmark/delete", body={"id": bid}, token=tok); steps.append(("delete", r))
    bad = ["%s(%s)" % (n, (r or {}).get("code") if isinstance(r, dict) else r) for n, r in steps if not ok(r)]
    ctx.record("BKM-01", bid is not None and not bad, "id=%s, bước lỗi=%s" % (bid, bad))


def g_extract(ctx):
    tok = ctx.admin()
    stamp = pick_stamp(ctx, back=120, need=70)
    if not stamp:
        ctx.record("EXT-01", None, "chưa có khoảng ghi ≥ 70 s")
        return
    start, end = stamp, stamp + 60
    _, _, pv = ctx.api("/media/esc/extractArchived/previews",
                       {"cameraId": ctx.cam, "startTime": start, "endTime": end}, token=tok)
    for sid, ext in (("EXT-01", "mp4"), ("EXT-03a", "mkv"), ("EXT-03b", "avi")):
        fname = "e2e_%d.%s" % (start, ext)
        body = {"cameraId": ctx.cam, "streamId": ctx.main, "startTime": start, "endTime": end,
                "filename": fname, "description": "e2e"}
        _, _, r = ctx.api("/media/esc/extractArchived/create", body=body, token=tok)
        key = isinstance(r, dict) and (r.get("data") or {}).get("key")
        if not key:
            ctx.record(sid, False, "create -> %s" % r)
            continue
        if sid == "EXT-01":
            _, _, r2 = ctx.api("/media/esc/extractArchived/create", body=body, token=tok)
            ctx.record("EXT-02", (r2.get("data") or {}).get("key") == key, "create lặp lại cùng key")
        ready, last = False, None
        for _ in range(120):
            _, _, last = ctx.api("/media/esc/extractArchived/progress", {"key": key}, token=tok)
            if not isinstance(last, dict) or last.get("code") != 0:
                break
            if (last.get("data") or {}).get("ready"):
                ready = True
                break
            time.sleep(2)
        if not ready:
            ctx.record(sid, False, "progress -> %s" % last)
            continue
        st, _, blob = ctx.http("GET", "/media/esc/extractArchived/download", {"key": key}, token=tok,
                               raw=True, timeout=120)
        out = os.path.join(ctx.evidence, fname)
        open(out, "wb").write(blob)
        dur = probe_duration(out)
        p = ffprobe(out)
        ok = st == 200 and dur is not None and abs(dur - 60) <= 5 and same_res(p, ctx.resolutions().get(ctx.main))
        ctx.record(sid, ok, "previews=%s; tải %d byte, dài %.1fs, %s" % (
            (pv or {}).get("data"), len(blob), dur or -1, p))
        ctx.api("/media/esc/extractArchived/delete", {"key": key}, token=tok)


def probe_duration(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of",
                          "default=nw=1:nk=1", path], capture_output=True, text=True)
    try:
        return float(out.stdout.strip())
    except ValueError:
        return None


def g_proxy(ctx):
    src = os.environ.get("E2E_RTSP_SUB", "")
    if not src:
        ctx.record("PRX-01", None, "cần E2E_RTSP_SUB")
    else:
        r = ctx.index_api("addStreamProxy", vhost="__defaultVhost__", app="proxy", stream="e2e",
                          url=src, rtp_type=0, retry_count=1)
        key = isinstance(r, dict) and (r.get("data") or {}).get("key")
        time.sleep(5)
        online = any(m.get("app") == "proxy" and m.get("stream") == "e2e" for m in ctx.media_list())
        r2 = ctx.index_api("delStreamProxy", key=key or "")
        time.sleep(2)
        gone = not any(m.get("app") == "proxy" for m in ctx.media_list())
        ctx.record("PRX-01", bool(key) and online and gone,
                   "add code=%s online=%s del code=%s gone=%s" % (r.get("code"), online, r2.get("code"), gone))

    # PRX-02: publish RTMP vào server bằng ffmpeg (testsrc), phát lại qua HTTP-FLV
    tok = ctx.admin()
    push = subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i",
                             "testsrc=size=640x360:rate=25", "-t", "40", "-c:v", "libx264", "-g", "50",
                             "-preset", "veryfast", "-f", "flv",
                             ctx.url("/push/e2e", None, "rtmp") + "?" + urllib.parse.urlencode({"token": tok})])
    time.sleep(8)
    p = ffprobe(ctx.url("/media/live/push/e2e.live.flv", tok))
    push.terminate()
    ctx.record("PRX-02", "error" not in p and p.get("width") == 640,
               "publish RTMP -> phát HTTP-FLV %s (publish có thể bị chặn nếu on_publish yêu cầu quyền)" % p)


# ---------------------------------------------------------------- tier + mã hoá


def s3_env():
    keys = ["E2E_S3_ENDPOINT", "E2E_S3_BUCKET", "E2E_S3_ACCESS_KEY", "E2E_S3_SECRET_KEY"]
    missing = [k for k in keys if not os.environ.get(k)]
    return None if missing else {k: os.environ[k] for k in keys}


def tier_state(ctx):
    path = os.path.join(ctx.work, "tier_state.json")
    return json.load(open(path)) if os.path.exists(path) else {}


def save_tier_state(ctx, st):
    json.dump(st, open(os.path.join(ctx.work, "tier_state.json"), "w"), indent=1)


def code_of(res):
    return res.get("code") if isinstance(res, dict) else None


def g_tier_setup(ctx):
    s3 = s3_env()
    if not s3:
        ctx.record("TIR-01", None, "thiếu E2E_S3_* (endpoint/bucket/access/secret)")
        return
    tok = ctx.admin()
    _, _, opts = ctx.api("/media/mserver/storage/pool/options", token=tok)
    ctx.record("TIR-01", code_of(opts) == 0, "options=%s" % json.dumps((opts or {}).get("data"))[:300])
    prefix = "e2e-%d" % int(time.time())
    minio = {"name": "E2E MinIO", "type": "MINIO", "tier": "COLD", "endpoint": s3["E2E_S3_ENDPOINT"],
             "bucket": s3["E2E_S3_BUCKET"], "base_path": prefix, "access_key": s3["E2E_S3_ACCESS_KEY"],
             "secret_key": s3["E2E_S3_SECRET_KEY"], "enabled": True, "health_check_enabled": True,
             "high_watermark_percent": 85, "critical_watermark_percent": 90}
    _, _, ok_conn = ctx.api("/media/mserver/storage/pool/testConnection", body=minio, token=tok)
    bad = dict(minio, secret_key="wrong-secret")
    _, _, bad_conn = ctx.api("/media/mserver/storage/pool/testConnection", body=bad, token=tok)
    d = (ok_conn or {}).get("data") or {}
    ctx.record("TIR-02", code_of(ok_conn) == 0 and d.get("can_read") and d.get("can_write")
               and code_of(bad_conn) == 914005,
               "đúng: code=%s read=%s write=%s; sai secret: code=%s"
               % (code_of(ok_conn), d.get("can_read"), d.get("can_write"), code_of(bad_conn)))
    _, _, created = ctx.api("/media/mserver/storage/pool/create", body=minio, token=tok)
    cold_id = ((created or {}).get("data") or {}).get("id")
    _, _, det = ctx.api("/media/mserver/storage/pool/detail", {"id": cold_id or ""}, token=tok)
    dd = (det or {}).get("data") or {}
    masked = dd.get("secret_key", "").strip("*") == "" and dd.get("access_key", "").strip("*") == ""
    ctx.record("TIR-03", bool(cold_id) and masked,
               "pool=%s credential mask=%s capacity=%s" % (cold_id, masked, dd.get("capacity_status")))

    warm_path = os.environ.get("E2E_NAS_PATH") or os.path.join(ctx.work, "warm")
    warm_type = "NAS" if os.environ.get("E2E_NAS_PATH") else "LOCAL_DISK"
    _, _, wr = ctx.api("/media/mserver/storage/pool/create", body={
        "name": "E2E warm", "type": warm_type, "tier": "WARM", "mount_path": warm_path,
        "enabled": True, "health_check_enabled": True,
        "high_watermark_percent": 85, "critical_watermark_percent": 90}, token=tok)
    warm_id = ((wr or {}).get("data") or {}).get("id")

    three = ctx.args.three_tier
    tiers = [{"tier": "HOT", "enabled": True, "pool_id": "pool-default-hot", "retain_until_days": 0,
              "data_mode": "FULL_VIDEO", "overflow_action": "MOVE_TO_NEXT_TIER",
              "high_watermark_percent": 85, "critical_watermark_percent": 90, "priority": "NORMAL"},
             {"tier": "WARM", "enabled": bool(three), "pool_id": warm_id or "", "retain_until_days": 1 if three else 0,
              "data_mode": "FULL_VIDEO", "overflow_action": "MOVE_TO_NEXT_TIER",
              "high_watermark_percent": 85, "critical_watermark_percent": 90, "priority": "NORMAL"},
             {"tier": "COLD", "enabled": True, "pool_id": cold_id or "", "retain_until_days": 7,
              "data_mode": "FULL_VIDEO", "overflow_action": "DELETE_OLDEST",
              "high_watermark_percent": 85, "critical_watermark_percent": 90, "priority": "NORMAL"}]
    if not three:
        tiers = [tiers[0], tiers[2]]
    policy = {"name": "E2E %s" % prefix, "description": "e2e", "enabled": True,
              "total_retention_days": 7, "allow_camera_override": True, "protect_event_video": True,
              "tiers": tiers,
              "delete_policy": {"delete_after_days": 7, "delete_mode": "DELETE_AUTOMATICALLY",
                                "skip_protected_video": True, "skip_evidence_video": True,
                                "require_approval_before_delete": False},
              "advanced_rules": {"enable_early_move_when_pool_high": False,
                                 "prefer_move_no_event_video_first": True,
                                 "prefer_keep_event_video_longer": True,
                                 "min_segment_age_minutes_before_move": 1}}
    _, _, pr = ctx.api("/media/mserver/storage/policy/create", body=policy, token=tok)
    policy_id = ((pr or {}).get("data") or {}).get("id")
    _, _, asg = ctx.api("/media/mserver/storage/policy/assignCamera",
                        {"camera_id": ctx.cam, "policy_id": policy_id or "", "override_reason": "e2e"},
                        token=tok, method="POST")
    _, _, eff = ctx.api("/media/mserver/storage/camera/effectivePolicy", {"camera_id": ctx.cam}, token=tok)
    ed = (eff or {}).get("data") or {}
    ctx.record("TIR-04", bool(policy_id) and code_of(asg) == 0 and ed.get("policy_id") == policy_id,
               "policy=%s (%s tầng) assign=%s effective=%s source=%s"
               % (policy_id, 3 if three else 2, code_of(asg), ed.get("policy_id"), ed.get("policy_source")))
    save_tier_state(ctx, {"cold_pool": cold_id, "warm_pool": warm_id, "policy": policy_id,
                          "prefix": prefix, "setup_at": int(time.time()), "three_tier": bool(three)})
    g_tier_validation(ctx, tok, cold_id)
    print("  Tiering cycle 300 s: chờ ≥ 10 phút rồi chạy `tier-verify`.")


def g_tier_validation(ctx, tok, cold_id):
    base = {"name": "e2e-invalid", "total_retention_days": 7,
            "delete_policy": {"delete_after_days": 7, "delete_mode": "DELETE_AUTOMATICALLY"}}

    def tier(t, days, enabled=True, pool="pool-default-hot", action="MOVE_TO_NEXT_TIER", hi=85, crit=90):
        return {"tier": t, "enabled": enabled, "pool_id": pool, "retain_until_days": days,
                "overflow_action": action, "high_watermark_percent": hi, "critical_watermark_percent": crit,
                "data_mode": "FULL_VIDEO", "priority": "NORMAL"}

    cases = {
        "HOT disabled": dict(base, tiers=[tier("HOT", 1, enabled=False), tier("COLD", 3, pool=cold_id)]),
        "retain giảm dần": dict(base, tiers=[tier("HOT", 5), tier("COLD", 3, pool=cold_id)]),
        "high>=critical": dict(base, tiers=[tier("HOT", 1, hi=90, crit=90), tier("COLD", 3, pool=cold_id)]),
        "delete<retain": dict(base, tiers=[tier("HOT", 1), tier("COLD", 30, pool=cold_id)]),
    }
    accepted = []
    for name, body in cases.items():
        _, _, r = ctx.api("/media/mserver/storage/policy/create", body=body, token=tok)
        if code_of(r) == 0:
            accepted.append(name)
            pid = (r.get("data") or {}).get("id")
            ctx.api("/media/mserver/storage/policy/delete", {"id": pid}, token=tok, method="POST")
    ctx.record("TIR-14", not accepted, "policy sai vẫn được tạo: %s" % accepted if accepted else "4 policy sai đều bị từ chối")
    _, _, r1 = ctx.api("/media/mserver/storage/pool/delete", {"id": cold_id or ""}, token=tok, method="POST")
    _, _, r2 = ctx.api("/media/mserver/storage/pool/delete", {"id": "pool-default-hot"}, token=tok, method="POST")
    _, _, r3 = ctx.api("/media/mserver/storage/policy/delete", {"id": "policy-system-default"}, token=tok, method="POST")
    ctx.record("TIR-15", code_of(r1) == 914003 and code_of(r2) == 914029 and code_of(r3) == 914030,
               "xoá pool đang dùng=%s, default HOT=%s, policy default=%s"
               % (code_of(r1), code_of(r2), code_of(r3)))


def g_tier_verify(ctx):
    st = tier_state(ctx)
    if not st:
        ctx.record("TIR-05", None, "chưa chạy tier-setup")
        return
    tok = ctx.admin()
    now = int(time.time())
    _, _, jobs = ctx.api("/media/mserver/storage/tieringJob/list",
                         {"camera_id": ctx.cam, "page": 0, "size": 100}, token=tok)
    items = ((jobs or {}).get("data") or {}).get("items") or []
    counts = {}
    for j in items:
        counts[j.get("status")] = counts.get(j.get("status"), 0) + 1
    _, _, tlr = ctx.api("/media/mserver/storage/camera/timeline",
                        {"camera_id": ctx.cam, "start_time": st["setup_at"] - 3600, "end_time": now}, token=tok)
    ranges = ((tlr or {}).get("data") or {}).get("ranges") or []
    cold = [r for r in ranges if r.get("tier") == "COLD" and r.get("status") == "AVAILABLE"]
    ctx.record("TIR-05", counts.get("DONE", 0) > 0 and bool(cold),
               "job=%s; range COLD=%d/%d (%.0f phút sau setup)"
               % (counts, len(cold), len(ranges), (now - st["setup_at"]) / 60.0))
    if not cold:
        return
    rng = cold[0]
    s, e = int(rng["start"]), int(rng["end"])
    probe_s = s + 5
    probe_e = min(e, probe_s + 30)
    _, _, rs = ctx.api("/media/mserver/storage/playback/resolve",
                       {"camera_id": ctx.cam, "start_time": probe_s, "end_time": probe_e, "stream_id": ctx.main},
                       token=tok, method="POST")
    rsd = (rs or {}).get("data") or {}
    ctx.record("TIR-06", code_of(rs) == 914009 and rsd.get("status") in ("RESTORE_REQUIRED", "RESTORING"),
               "resolve -> code=%s status=%s" % (code_of(rs), rsd.get("status")))

    params = {"camera_id": ctx.cam, "start_time": probe_s, "end_time": probe_e, "stream_id": ctx.main,
              "reason": "e2e"}
    _, _, rj = ctx.api("/media/mserver/storage/restoreJob/create", params, token=tok, method="POST")
    job_id = ((rj or {}).get("data") or {}).get("job_id")
    _, _, rj2 = ctx.api("/media/mserver/storage/restoreJob/create", params, token=tok, method="POST")
    d2 = (rj2 or {}).get("data") or {}
    ctx.record("TIR-09", d2.get("job_id") == job_id and d2.get("job_created") is False,
               "lặp lại -> job_id giống=%s job_created=%s" % (d2.get("job_id") == job_id, d2.get("job_created")))
    detail = {}
    for _ in range(90):
        _, _, dj = ctx.api("/media/mserver/storage/restoreJob/detail", {"job_id": job_id or ""}, token=tok)
        detail = (dj or {}).get("data") or {}
        if detail.get("playback_status") in ("READY", "FAILED", "CANCELLED", "EXPIRED"):
            break
        time.sleep(2)
    ready = detail.get("playback_status") == "READY"
    detail_txt = "job=%s playback_status=%s restored=%s/%s" % (
        job_id, detail.get("playback_status"), detail.get("restored_segment_count"),
        detail.get("total_segment_count"))
    if ready:
        url = ctx.url("/media/record/%s/%s/vod/%d.live.mp4" % (ctx.cam, ctx.main, probe_s), tok)
        p = ffprobe(url)
        wall, errs, msg = pace(url, 15)
        ok = "error" not in p and errs == 0
        detail_txt += "; replay %s, 15 s mất %.1fs lỗi=%s %s" % (p, wall, errs, msg)
    else:
        ok = False
    ctx.record("TIR-07", ok, detail_txt)
    json.dump({"restore_job": job_id, "range": [probe_s, probe_e], "at": int(time.time())},
              open(os.path.join(ctx.work, "restore_state.json"), "w"))


def g_encrypt_setup(ctx):
    tok = ctx.admin()
    mode = ctx.args.encrypt_mode
    root = os.path.join(ctx.work, "pool-enc-%d" % mode)
    os.makedirs(root, exist_ok=True)
    password = os.environ.get("E2E_ENCRYPT_PASSWORD") or hashlib.sha256(os.urandom(16)).hexdigest()[:20]
    pw_path = os.path.join(ctx.work, "encrypt_password")
    with open(os.open(pw_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as fh:
        fh.write(password)
    _, _, r = ctx.api("/media/mserver/storage/pool/create", body={
        "name": "E2E HOT enc%d" % mode, "type": "LOCAL_DISK", "tier": "HOT", "mount_path": root,
        "enabled": True, "health_check_enabled": True, "high_watermark_percent": 85,
        "critical_watermark_percent": 90, "encryptMode": mode, "encryptPassword": password}, token=tok)
    pool_id = ((r or {}).get("data") or {}).get("id")
    enc = ((r or {}).get("data") or {}).get("encryption")
    st = tier_state(ctx)
    tiers = [{"tier": "HOT", "enabled": True, "pool_id": pool_id or "", "retain_until_days": 0,
              "data_mode": "FULL_VIDEO",
              "overflow_action": "MOVE_TO_NEXT_TIER" if st.get("cold_pool") else "DELETE_OLDEST",
              "high_watermark_percent": 85, "critical_watermark_percent": 90, "priority": "NORMAL"}]
    if st.get("cold_pool"):
        tiers.append({"tier": "COLD", "enabled": True, "pool_id": st["cold_pool"], "retain_until_days": 7,
                      "data_mode": "FULL_VIDEO", "overflow_action": "DELETE_OLDEST",
                      "high_watermark_percent": 85, "critical_watermark_percent": 90, "priority": "NORMAL"})
    else:
        tiers[0]["retain_until_days"] = 7
    _, _, pr = ctx.api("/media/mserver/storage/policy/create", body={
        "name": "E2E enc%d %d" % (mode, int(time.time())), "enabled": True, "total_retention_days": 7,
        "allow_camera_override": True, "tiers": tiers,
        "delete_policy": {"delete_after_days": 7, "delete_mode": "DELETE_AUTOMATICALLY",
                          "skip_protected_video": True, "require_approval_before_delete": False},
        "advanced_rules": {"min_segment_age_minutes_before_move": 1}}, token=tok)
    policy_id = ((pr or {}).get("data") or {}).get("id")
    _, _, asg = ctx.api("/media/mserver/storage/policy/assignCamera",
                        {"camera_id": ctx.cam, "policy_id": policy_id or ""}, token=tok, method="POST")
    sid = "ENC-01" if mode == 1 else "ENC-02"
    ctx.record(sid + "-setup", bool(pool_id) and bool(policy_id) and code_of(asg) == 0,
               "pool=%s encryption=%s policy=%s assign=%s (mật khẩu trong %s)"
               % (pool_id, enc, policy_id, code_of(asg), pw_path))
    json.dump({"pool": pool_id, "policy": policy_id, "mode": mode, "root": root, "at": int(time.time())},
              open(os.path.join(ctx.work, "encrypt_state.json"), "w"))
    print("  Chờ ≥ 2 segment (%d s) rồi chạy `encrypt-verify`." % (2 * ctx.inv["segment_sec"]))


def g_encrypt_verify(ctx):
    path = os.path.join(ctx.work, "encrypt_state.json")
    if not os.path.exists(path):
        ctx.record("ENC-01", None, "chưa chạy encrypt-setup")
        return
    st = json.load(open(path))
    mode = st["mode"]
    files = sorted((f for f in glob.glob(os.path.join(st["root"], "**", "*.mp4*"), recursive=True)
                    if os.path.getmtime(f) >= st["at"]), key=os.path.getmtime)
    closed = files[:-2] if len(files) > 2 else []
    problems = []
    plain = [f for f in closed if not f.endswith(".enc")]
    if plain:
        problems.append("%d file plaintext sau khi bật mã hoá" % len(plain))
    for f in [x for x in closed if x.endswith(".enc")][:4]:
        with open(f, "rb") as fh:
            head = fh.read(16)
            fh.seek(-64, os.SEEK_END)
            tail = fh.read()
        if b"SENC" not in tail:
            problems.append("%s thiếu magic SENC" % os.path.basename(f))
        has_ftyp = head[4:8] == b"ftyp"
        if mode == 1 and not has_ftyp:
            problems.append("Light nhưng mất ftyp: %s" % os.path.basename(f))
        if mode == 2 and has_ftyp:
            problems.append("Strong nhưng còn ftyp: %s" % os.path.basename(f))
    sid = "ENC-01" if mode == 1 else "ENC-02"
    ctx.record(sid, bool(closed) and not problems,
               "%d file đã đóng; %s" % (len(closed), "; ".join(problems) or "đúng định dạng"))
    enc_files = [x for x in closed if x.endswith(".enc")]
    if mode == 2 and enc_files:
        p = ffprobe(enc_files[0], timeout=10)
        ctx.record("ENC-03", "error" in p, "ffprobe file Strong -> %s" % p)

    tok = ctx.admin()
    res = ctx.resolutions()
    # Stamp nằm trong file mã hoá dài nhất: file đầu tiên sau khi bật mã hoá có thể chỉ dài vài
    # giây, nên suy từ mtime của nó dễ rơi ra trước dữ liệu.
    stamp, longest = None, 0
    for f in enc_files:
        begin = file_start_time(f)
        if begin and os.path.getmtime(f) - begin > longest:
            longest = os.path.getmtime(f) - begin
            stamp = begin + 5
    if stamp and longest < 20:
        ctx.record("ENC-05a", None, "chưa có file mã hoá dài ≥ 20 s")
        stamp = None
    if stamp:
        urls = {
            "ENC-05a": ("/media/record/%s/%s/vod/%d.live.mp4" % (ctx.cam, ctx.main, stamp), {}),
            "ENC-05b": ("/media/record/%s/vod/%d.live2.mp4" % (ctx.cam, stamp), {"quality": "auto", "prefered": "hi"}),
            "ENC-05c": ("/media/record/%s/vod/%d/hls.m3u8" % (ctx.cam, stamp),
                        {"quality": "auto", "prefered": "hi", "session_id": "e2e-enc05"}),
        }
        for sid2, (p_, q) in urls.items():
            p = ffprobe(ctx.url(p_, tok, **q))
            ctx.record(sid2, "error" not in p and same_res(p, res.get(ctx.main)), "%s -> %s" % (p_, p))
        st2, _, img = ctx.http("GET", "/media/esc/recordedThumnail",
                               {"cameraId": ctx.cam, "streamId": ctx.main, "pos": stamp + 5},
                               token=tok, raw=True, timeout=30)
        ctx.record("ENC-13a", img[:3] == b"\xff\xd8\xff", "thumbnail trên bản ghi mã hoá HTTP %s" % st2)
    live = ffprobe(ctx.url("/media/live/%s/%s.live.mp4" % (ctx.cam, ctx.main), tok))
    ctx.record("ENC-04", "error" not in live, "live khi bật mã hoá -> %s" % live)


GROUPS = {
    "smoke": g_smoke, "live": g_live, "auth": g_auth, "overlay": g_overlay, "record": g_record,
    "replay": g_replay, "bookmark": g_bookmark, "extract": g_extract, "proxy": g_proxy,
    "auto-profile": g_auto_profile, "tier-setup": g_tier_setup, "tier-verify": g_tier_verify,
    "encrypt-setup": g_encrypt_setup, "encrypt-verify": g_encrypt_verify,
}
BASIC = ["smoke", "live", "auth", "replay", "bookmark", "extract"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("work")
    ap.add_argument("groups", nargs="+", choices=sorted(GROUPS) + ["all-basic"])
    ap.add_argument("--expect-main", type=int, choices=[0, 1], help="record: Main phải đang ghi?")
    ap.add_argument("--expect-sub", type=int, choices=[0, 1], help="record: Sub phải đang ghi?")
    ap.add_argument("--window", type=int, default=3, help="record: số phút gần nhất xét file mới")
    ap.add_argument("--scenario", help="record: ID ghi vào báo cáo, ví dụ REC-04")
    ap.add_argument("--back", type=int, default=300, help="replay: stamp cách hiện tại tối thiểu (s)")
    ap.add_argument("--three-tier", action="store_true", help="tier-setup: HOT→WARM→COLD")
    ap.add_argument("--encrypt-mode", type=int, choices=[1, 2], default=1)
    ap.add_argument("--since", type=int, help="auto-profile: epoch bắt đầu dữ liệu lowres_motion")
    args = ap.parse_args()
    ctx = Ctx(os.path.abspath(args.work), args)
    groups = []
    for g in args.groups:
        groups += BASIC if g == "all-basic" else [g]
    try:
        for g in groups:
            print("== %s" % g, flush=True)
            try:
                GROUPS[g](ctx)
            except Exception as ex:  # noqa: BLE001 - một nhóm lỗi không được chặn các nhóm khác
                ctx.record(g.upper() + "-ERR", False, "%s: %s" % (type(ex).__name__, ex))
    finally:
        ctx.save()


if __name__ == "__main__":
    main()
