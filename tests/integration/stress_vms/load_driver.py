#!/usr/bin/env python3
"""Sinh tải REST lên MediaServer trong lúc stress test và đo độ trễ.

Ba nhóm tải chạy song song:
  ptz      — điều khiển ONVIF: ptz_control, goto_preset, get_presets, set_preset
  storage  — truy vấn lưu trữ: timeline, summary, dashboard, effectivePolicy
  probe    — /media/mserver/healthcheck liên tục; đây là tín hiệu cho biết poller
             có bị công việc blocking chiếm hay không (phải luôn ở mức ms)

Nếu probe tăng vọt trong khi ptz/storage vẫn chạy thì có công việc blocking đang
nằm nhầm trên EventPoller — xem docs/threading-blocking-work.md.

Dùng:
    load_driver.py --base http://127.0.0.1:18081 --inventory work/inventory.json \
                   --duration 300 --state work/load_state.json
"""
import argparse
import asyncio
import json
import os
import random
import sys
import time
from collections import defaultdict

DIRECTIONS = ["UP", "DOWN", "LEFT", "RIGHT", "ZOOM_IN", "ZOOM_OUT", "STOP"]


class Stats(object):
    def __init__(self):
        self.lat = defaultdict(list)
        self.codes = defaultdict(lambda: defaultdict(int))
        self.errors = defaultdict(int)

    def add(self, group, seconds, code):
        self.lat[group].append(seconds * 1000.0)
        self.codes[group][str(code)] += 1

    def summary(self):
        out = {}
        for group, samples in self.lat.items():
            if not samples:
                continue
            s = sorted(samples)
            out[group] = {
                "count": len(s),
                "p50_ms": round(s[len(s) // 2], 1),
                "p95_ms": round(s[min(len(s) - 1, int(len(s) * 0.95))], 1),
                "p99_ms": round(s[min(len(s) - 1, int(len(s) * 0.99))], 1),
                "max_ms": round(s[-1], 1),
                "codes": dict(self.codes[group]),
            }
        out["transport_errors"] = dict(self.errors)
        return out


class Client(object):
    """HTTP/1.1 keep-alive tối giản; tự mở lại kết nối khi bị đóng."""

    def __init__(self, host, port, timeout):
        self.host, self.port, self.timeout = host, port, timeout
        self.reader = self.writer = None

    async def connect(self):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), self.timeout)

    async def get(self, path):
        if self.writer is None:
            await self.connect()
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: keep-alive\r\n\r\n"
               % (path, self.host, self.port))
        self.writer.write(req.encode())
        await self.writer.drain()
        status, body = await self._read_response()
        return status, body

    async def _read_response(self):
        line = await asyncio.wait_for(self.reader.readline(), self.timeout)
        if not line:
            raise ConnectionError("server đóng kết nối")
        status = int(line.split()[1])
        length, chunked = 0, False
        while True:
            hl = await asyncio.wait_for(self.reader.readline(), self.timeout)
            if not hl or hl in (b"\r\n", b"\n"):
                break
            low = hl.lower()
            if low.startswith(b"content-length:"):
                length = int(low.split(b":")[1])
            elif low.startswith(b"transfer-encoding:") and b"chunked" in low:
                chunked = True
        if chunked:
            chunks = []
            while True:
                size_line = await asyncio.wait_for(self.reader.readline(), self.timeout)
                size = int(size_line.strip().split(b";")[0] or b"0", 16)
                if size == 0:
                    await self.reader.readline()
                    break
                chunks.append(await asyncio.wait_for(self.reader.readexactly(size), self.timeout))
                await self.reader.readline()
            return status, b"".join(chunks)
        body = await asyncio.wait_for(self.reader.readexactly(length), self.timeout) if length else b""
        return status, body

    def close(self):
        if self.writer is not None:
            try:
                self.writer.close()
            except OSError:
                pass
            self.reader = self.writer = None


def api_code(body):
    try:
        return json.loads(body).get("code", "nojson")
    except (ValueError, AttributeError):
        return "nojson"


async def worker(group, paths_fn, args, stats, deadline, pace):
    host, port = args.host, args.port
    client = Client(host, port, args.timeout)
    while time.time() < deadline:
        path = paths_fn()
        started = time.time()
        try:
            status, body = await client.get(path)
            code = api_code(body) if status == 200 else "http%d" % status
            stats.add(group, time.time() - started, code)
        except (ConnectionError, OSError, asyncio.IncompleteReadError,
                asyncio.TimeoutError, ValueError, IndexError) as exc:
            stats.errors["%s:%s" % (group, type(exc).__name__)] += 1
            client.close()
            await asyncio.sleep(0.5)
            continue
        if pace:
            await asyncio.sleep(pace)
    client.close()


def build_path_factories(inv, args):
    cams = []
    for ids in inv["by_cohort"].values():
        cams.extend(ids)
    cams.sort()
    if not cams:
        sys.exit("inventory không có camera nào")

    # Chỉ gọi tới camera mà node này sở hữu. Node khác trả 404 CODE_DEVICE_NOT_FOUND,
    # đo trên đó thì chỉ đo đường lỗi chứ không phải tải thật.
    # Quy tắc chia sở hữu phải khớp setup_stress.py: node_ids[index % len(node_ids)].
    nodes = inv.get("nodes") or []
    if args.owner and nodes:
        try:
            pos = nodes.index(args.owner)
        except ValueError:
            pos = 0
        owned = [c for c in cams if int(c.rsplit("-", 1)[1]) % len(nodes) == pos]
        if owned:
            cams = owned
    print("[load] %d camera thuộc %s" % (len(cams), args.owner or "tất cả"), flush=True)

    def ptz():
        cam = random.choice(cams)
        roll = random.random()
        if roll < 0.55:
            return ("/media/mserver/device/ptz_control?deviceId=%s&direct=%s&speed=%d"
                    % (cam, random.choice(DIRECTIONS), random.randint(1, 8)))
        if roll < 0.75:
            return "/media/mserver/device/ptz_control/get_presets?deviceId=%s" % cam
        if roll < 0.92:
            return ("/media/mserver/device/ptz_control/goto_preset"
                    "?deviceId=%s&presetToken=Preset%d&isUserPreset=0"
                    % (cam, random.randint(1, 2)))
        return ("/media/mserver/device/ptz_control/set_preset"
                "?deviceId=%s&presetToken=Preset%d&presetName=stress&isUserPreset=0"
                % (cam, random.randint(3, 6)))

    now = int(time.time())
    # Nhóm /media/mserver/storage/* chỉ được đăng ký khi build có ENABLE_TIER_STORAGE
    # (server/WebApiStorage.cpp:44). Với bản tier-off thì chỉ còn các API thiết bị.
    # Nhóm storage dùng tên tham số snake_case (camera_id/start_time/end_time), khác với
    # nhóm device dùng `id`; sai tên thì chỉ nhận 400 chứ không chạm vào xử lý thật.
    storage_paths = [
        lambda c: ("/media/mserver/storage/camera/timeline?camera_id=%s&start_time=%d&end_time=%d"
                   % (c, now - 86400, now)),
        lambda c: "/media/mserver/storage/camera/summary?camera_id=%s" % c,
        lambda c: "/media/mserver/storage/dashboard/summary",
    ]
    device_paths = [
        lambda c: "/media/mserver/device/statistics?id=%s" % c,
    ]
    pool = storage_paths + device_paths if args.storage_api else device_paths

    def storage():
        return random.choice(pool)(random.choice(cams))

    def probe():
        return "/media/mserver/healthcheck"

    return {"ptz": ptz, "storage": storage, "probe": probe}


async def reporter(stats, args, deadline):
    while time.time() < deadline:
        await asyncio.sleep(args.stats_sec)
        summary = stats.summary()
        parts = []
        for group in ("probe", "ptz", "storage"):
            g = summary.get(group)
            if g:
                parts.append("%s n=%d p50=%.0f p95=%.0f max=%.0f"
                             % (group, g["count"], g["p50_ms"], g["p95_ms"], g["max_ms"]))
        print("[load] " + " | ".join(parts), flush=True)
        if args.state:
            tmp = args.state + ".tmp"
            with open(tmp, "w") as fh:
                json.dump(summary, fh, indent=1)
            os.replace(tmp, args.state)


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="vd http://127.0.0.1:18081")
    ap.add_argument("--inventory", required=True)
    ap.add_argument("--duration", type=float, default=300)
    ap.add_argument("--ptz-workers", type=int, default=8)
    ap.add_argument("--storage-workers", type=int, default=6)
    ap.add_argument("--probe-workers", type=int, default=2)
    ap.add_argument("--ptz-pace", type=float, default=0.15)
    ap.add_argument("--storage-pace", type=float, default=0.2)
    ap.add_argument("--probe-pace", type=float, default=0.25)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--stats-sec", type=float, default=15)
    ap.add_argument("--state", default=None)
    ap.add_argument("--owner", default="", help="chỉ gọi camera do node này sở hữu")
    args = ap.parse_args()

    without_scheme = args.base.split("://", 1)[-1]
    args.host, _, port = without_scheme.partition(":")
    args.port = int(port or 80)

    inv = json.load(open(args.inventory))

    # Dò một lần xem build này có nhóm API storage không, để không đếm nhầm 404 là lỗi.
    args.storage_api = False
    probe_client = Client(args.host, args.port, args.timeout)
    try:
        status, _ = await probe_client.get("/media/mserver/storage/dashboard/summary")
        args.storage_api = status != 404
    except (ConnectionError, OSError, asyncio.TimeoutError, asyncio.IncompleteReadError,
            ValueError, IndexError):
        pass
    probe_client.close()
    print("[load] nhóm API storage: %s"
          % ("có" if args.storage_api else "không (build tier-off)"), flush=True)

    factories = build_path_factories(inv, args)
    stats = Stats()
    deadline = time.time() + args.duration

    tasks = [asyncio.ensure_future(reporter(stats, args, deadline))]
    for group, count, pace in (("ptz", args.ptz_workers, args.ptz_pace),
                               ("storage", args.storage_workers, args.storage_pace),
                               ("probe", args.probe_workers, args.probe_pace)):
        for _ in range(count):
            tasks.append(asyncio.ensure_future(
                worker(group, factories[group], args, stats, deadline, pace)))
    await asyncio.gather(*tasks, return_exceptions=True)

    summary = stats.summary()
    print(json.dumps(summary, indent=1), flush=True)
    if args.state:
        with open(args.state, "w") as fh:
            json.dump(summary, fh, indent=1)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
