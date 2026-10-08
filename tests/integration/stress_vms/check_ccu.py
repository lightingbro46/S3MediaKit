#!/usr/bin/env python3
"""Chạy và chấm test ngưỡng CCU — docs/ccu-capacity-test-design.md.

Điều khiển `viewer_load.py controller` qua HTTP, lấy mẫu MediaServer trong cửa sổ đo của từng
bậc, đối chiếu tiêu chí mục 6 của thiết kế và ghi <work>/ccu_report.{json,md}.

Kịch bản (--scenario):
  step       thang bậc tìm ngưỡng + chia đôi (mục 4.1). Mặc định.
  storm      mở đồng loạt --level viewer trong 5 s (4.2)
  churn      viewer vào/ra liên tục quanh --level trong --duration giây (4.3)
  soak       giữ --level trong --duration giây, mỗi --window giây một dòng (4.4)
  admission  giới hạn production: vượt ngưỡng, burst, theo camera, ngưỡng CPU (4.5)

Dùng:
    check_ccu.py <work> --controller http://127.0.0.1:18600 [--scenario step] [--steps 0,100,...]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_stress as cs  # noqa: E402

# Mục 6 của thiết kế. Ghi đè bằng --th ten=giatri.
# Bậc chỉ TRƯỢT vì trải nghiệm người xem và ghi hình. Ngưỡng tài nguyên (CPU, poller, RSS, NIC,
# đĩa) và log db_locked/failed_apply chỉ là CẢNH BÁO: ghi vào row["warnings"] để giải thích yếu tố
# giới hạn khi bậc trượt. Khựng tổng, trôi, tốc độ media, healthcheck max chỉ ghi số, không chấm.
THRESHOLDS = {
    "join_ok_ratio": 0.995,
    # Thời gian tới hình đầu tiên, tính cho MỌI lần mở (max, không phải p95); lần mở không có
    # frame đầu trong TTFF_TIMEOUT của viewer_load (15 s) cũng tính là vượt.
    "ttff_max_live_ms": 5000.0,     # fMP4/RTSP/HLS live
    "ttff_max_replay_ms": 10000.0,
    "replay_share_max": 0.20,       # hỗn hợp nhiều loại: replay ≤ 20% số viewer đang phát
    "long_stall_session_ratio": 0.01,
    "unexpected_close_ratio": 0.005,  # phiên bị đứt / số viewer của bậc
    "healthcheck_p95_ms": 200.0,
    "stream_online_ratio": 0.99,
    "process_cpu_pct": 80.0,        # % tổng số lõi
    "poller_load_pct": 85.0,
    "rss_ram_ratio": 0.70,
    "nic_util": 0.80,
    "record_gap_sec": 5.0,
    "record_stale_sec": 20.0,       # đoạn đang ghi phải có ghi mới trong chừng này giây
    # PSI: tỉ lệ thời gian MỌI tác vụ bị chặn chờ IO (/proc/pressure/io "full"). Ghi hình bitrate
    # thật dồn đĩa có thể làm cả máy khựng theo đợt — luồng kéo camera timeout, ghi hình hở.
    "io_full_ratio": 0.02,
    # Kernel không có PSI (CentOS 7 / 3.10): dùng tỉ lệ iowait của /proc/stat thay thế. Thô hơn
    # PSI (CPU rảnh mà có tác vụ chờ IO), nhưng đĩa bão hoà trên db3 cho 40%+ ngay bậc 0.
    "io_wait_ratio": 0.10,
    # kịch bản replay: một phiên xem lại đạt khi vào được trong ttff_max_replay_ms, xem đủ thời lượng,
    # nhận đủ media (media_sec ≥ tỉ lệ × thời lượng), không khựng > 2 s và không phải kết nối lại
    "replay_media_ratio": 0.95,
    "replay_session_ok_ratio": 0.99,
    # tự kiểm bộ sinh tải — vượt thì bậc không hợp lệ chứ không phải trượt
    "client_cpu_pct": 85.0,
    "client_loop_lag_ms": 200.0,
    # tự kiểm nguồn RTSP giả (rtsp_state.json) — nguồn tụt nhịp thì server nhận thiếu dữ liệu
    "source_cpu_pct": 70.0,
    "source_late_p99_ms": 100.0,
    "source_late_max_ms": 1000.0,   # một lần treo vài giây đủ làm server timeout luồng kéo
}
LIVE_KINDS = ("fmp4-sub", "fmp4-main", "rtsp")
# Khoá của /index/api/getStatistic dùng để phát hiện rò rỉ sau churn/soak
STRUCTURAL_OBJECTS = ("MediaSource", "MultiMediaSourceMuxer", "Socket", "TcpClient", "TcpServer",
                      "TcpSession", "UdpServer", "UdpSession")
HZ = os.sysconf("SC_CLK_TCK")
NCPU = os.cpu_count() or 1
FILE_TIME = re.compile(r"(\d{4})-(\d{2})-(\d{2})-(\d{2})-(\d{2})-(\d{2})")


def log(msg):
    print("[ccu %s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def pct(values, p):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p))]


def http_json(url, method="GET", payload=None, timeout=20):
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


# ================================================================== viewer controller
class Viewers(object):
    def __init__(self, url):
        self.url = url.rstrip("/")

    def set(self, **kw):
        return http_json(self.url + "/target", "PUT", kw)

    def mark(self):
        return http_json(self.url + "/mark", "POST", {})["mark"]

    def stats(self):
        return http_json(self.url + "/stats", timeout=60)

    def playing(self):
        st = self.stats()
        return sum(st.get("playing", {}).values()) + sum((st.get("done") or {}).values())

    def stop(self):
        try:
            self.set(stop=True)
        except (OSError, ValueError):
            pass


# ================================================================== server
class Server(object):
    def __init__(self, work, node, nic):
        self.work, self.node, self.nic = work, node, nic
        inv = cs.load_json(os.path.join(work, "inventory.json"))
        self.inv = inv
        self.port = inv["ports"][node]["http"]
        self.secret = cs.node_secret(work, node)
        self.base = "http://127.0.0.1:%d" % self.port

    def pid(self):
        try:
            return int(open(os.path.join(self.work, self.node, "server.pid")).read().strip())
        except (OSError, ValueError):
            return None

    def alive(self):
        p = self.pid()
        return bool(p) and os.path.exists("/proc/%d" % p)

    def api(self, path, **q):
        q["secret"] = self.secret
        qs = "&".join("%s=%s" % kv for kv in q.items())
        return http_json("%s%s?%s" % (self.base, path, qs))

    # ---- /proc
    def cpu_ticks(self):
        try:
            f = open("/proc/%d/stat" % self.pid()).read().rsplit(")", 1)[1].split()
            return int(f[11]) + int(f[12])
        except (OSError, TypeError, IndexError, ValueError):
            return 0

    def task_ticks(self):
        out = {}
        pid = self.pid()
        try:
            tids = os.listdir("/proc/%d/task" % pid)
        except (OSError, TypeError):
            return out
        for tid in tids:
            try:
                raw = open("/proc/%d/task/%s/stat" % (pid, tid)).read()
                comm = raw[raw.index("(") + 1:raw.rindex(")")]
                f = raw.rsplit(")", 1)[1].split()
                out[tid] = (comm, int(f[11]) + int(f[12]))
            except (OSError, ValueError, IndexError):
                continue
        return out

    def rss_kb(self):
        try:
            for line in open("/proc/%d/status" % self.pid()):
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
        except (OSError, TypeError):
            pass
        return 0

    def proc_io(self):
        """Byte tiến trình thật sự đọc/ghi xuống tầng block (không tính page cache đọc lại)."""
        out = {}
        try:
            for line in open("/proc/%d/io" % self.pid()):
                k, _, v = line.partition(":")
                out[k] = int(v)
        except (OSError, TypeError, ValueError):
            pass
        return out.get("read_bytes", 0), out.get("write_bytes", 0)

    def fd_count(self):
        try:
            return len(os.listdir("/proc/%d/fd" % self.pid()))
        except (OSError, TypeError):
            return 0

    def nic_bytes(self):
        try:
            for line in open("/proc/net/dev"):
                name, _, rest = line.partition(":")
                if name.strip() == self.nic:
                    f = rest.split()
                    return int(f[0]), int(f[8])
        except OSError:
            pass
        return 0, 0

    def nic_speed_mbps(self):
        try:
            v = int(open("/sys/class/net/%s/speed" % self.nic).read().strip())
            return v if v > 0 else None
        except (OSError, ValueError):
            return None

    # ---- API
    def media_rows(self):
        try:
            payload = self.api("/index/api/getMediaList")
        except (OSError, ValueError) as exc:
            return None, str(exc)
        return payload.get("data") or [], None

    def statistic(self):
        try:
            return self.api("/index/api/getStatistic").get("data") or {}
        except (OSError, ValueError):
            return {}

    def threads_load(self):
        try:
            return self.api("/index/api/getThreadsLoad").get("data") or []
        except (OSError, ValueError):
            return []


def psi_total_us(kind):
    """Tổng micro giây "full" của /proc/pressure/<kind>; None nếu kernel không có PSI."""
    try:
        for line in open("/proc/pressure/%s" % kind):
            if line.startswith("full"):
                return int(line.rsplit("total=", 1)[1])
    except (OSError, ValueError, IndexError):
        pass
    return None


def cpu_iowait_ticks():
    """(tổng tick, tick iowait) toàn máy từ dòng "cpu" của /proc/stat; None nếu không đọc được."""
    try:
        with open("/proc/stat") as fh:
            f = [int(x) for x in fh.readline().split()[1:]]
        # user nice system idle iowait irq softirq steal; guest đã nằm trong user
        return sum(f[:8]), f[4]
    except (OSError, ValueError, IndexError):
        return None


def mem_total_kb():
    for line in open("/proc/meminfo"):
        if line.startswith("MemTotal:"):
            return int(line.split()[1])
    return 0


class Sampler(object):
    """Lấy mẫu trong cửa sổ đo: healthcheck liên tục + getThreadsLoad mỗi 5 s."""

    def __init__(self, server):
        self.s = server
        self.stop_ev = threading.Event()
        self.health = []
        self.health_err = 0
        self.loads = defaultdict(list)
        self.source = []
        self.threads = []

    def _probe(self):
        url = self.s.base + "/media/mserver/healthcheck"
        while not self.stop_ev.is_set():
            t = time.time()
            try:
                with urllib.request.urlopen(url, timeout=5) as r:
                    r.read()
                self.health.append((time.time() - t) * 1000.0)
            except (OSError, ValueError):
                self.health_err += 1
                self.health.append(5000.0)
            self.stop_ev.wait(0.25)

    def _source(self):
        path = os.path.join(self.s.work, "rtsp_state.json")
        while not self.stop_ev.is_set():
            st = cs.load_json(path)
            if st:
                self.source.append(st)
            self.stop_ev.wait(5)

    def _loads(self):
        while not self.stop_ev.is_set():
            for i, row in enumerate(self.s.threads_load()):
                self.loads[row.get("name") or str(i)].append(row.get("load", 0))
            self.stop_ev.wait(5)

    def start(self):
        for fn in (self._probe, self._loads, self._source):
            th = threading.Thread(target=fn, daemon=True)
            th.start()
            self.threads.append(th)

    def stop(self):
        self.stop_ev.set()
        for th in self.threads:
            th.join(timeout=10)


class Perf(object):
    """`perf record` trên MediaServer trong cửa sổ đo của một bậc -> hàm nóng theo thread.

    Lấy mẫu 99 Hz nên chi phí thêm không đáng kể. Không có perf / không đủ quyền thì bỏ qua và
    ghi lý do vào báo cáo; bản build bị strip chỉ còn tên thư viện (nên đo bằng RelWithDebInfo).
    """

    def __init__(self, work, label, pid, sec):
        self.data = os.path.join(work, "perf_%s.data" % label)
        self.txt = os.path.join(work, "perf_%s.txt" % label)
        self.proc, self.error = None, None
        perf = shutil.which("perf")
        if not perf:
            self.error = "không có lệnh perf"
            return
        try:
            self.proc = subprocess.Popen(
                [perf, "record", "-F", "99", "-g", "-p", str(pid), "-o", self.data,
                 "--", "sleep", str(int(sec))],
                stdout=subprocess.DEVNULL, stderr=open(self.txt + ".log", "w"))
        except OSError as exc:
            self.error = str(exc)

    def finish(self, top=25):
        if not self.proc:
            return {"error": self.error}
        try:
            self.proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        if self.proc.returncode or not os.path.exists(self.data):
            msg = open(self.txt + ".log").read().strip().splitlines()
            return {"error": "perf record lỗi: %s" % (msg[-1] if msg else self.proc.returncode)}
        with open(self.txt, "w") as fh:
            subprocess.call(["perf", "report", "-i", self.data, "--stdio", "--no-children",
                             "-g", "none", "--sort", "comm,dso,symbol", "--percent-limit", "0.3"],
                            stdout=fh, stderr=subprocess.DEVNULL)
        rows = []
        for line in open(self.txt):
            line = line.strip()
            if not line or line.startswith("#") or "%" not in line.split()[0]:
                continue
            rows.append(" ".join(line.split()))
            if len(rows) >= top:
                break
        return {"top": rows, "file": os.path.basename(self.txt)}


def thread_group(comm):
    """Gộp "event poller 3"/"work poller 12"/"api-7" thành nhóm để thấy loại việc nào ăn CPU."""
    return re.sub(r"[\s_-]*\d+$", "", comm).strip() or comm


def scan_logs(work, node):
    """Như check_stress.scan_logs, thêm số lần luồng kéo camera bị timeout (mất RTP)."""
    out = cs.scan_logs(work, node)
    n = 0
    d = os.path.join(work, node)
    for name in os.listdir(d):
        if name.startswith("stdout"):
            try:
                with open(os.path.join(d, name), errors="replace") as fh:
                    n += sum(1 for line in fh if "receive rtp timeout" in line)
            except OSError:
                pass
    out["rtp_timeout"] = n
    return out


# ================================================================== ghi hình
def file_start(name):
    m = FILE_TIME.search(name)
    if not m:
        return None
    return time.mktime(tuple(int(x) for x in m.groups()) + (0, 0, -1))


def record_streams(work, node, cams):
    """{thư mục luồng: [(start, mtime, tên)]} cho các camera cần kiểm, sắp theo start."""
    root = os.path.join(work, node, "record")
    out = defaultdict(list)
    for dirpath, _dirs, files in os.walk(root):
        parts = os.path.relpath(dirpath, root).split(os.sep)
        if not any(p in cams for p in parts):
            continue
        for f in files:
            if not f.endswith(".mp4"):
                continue
            start = file_start(f)
            if start is None:
                continue
            try:
                mtime = os.path.getmtime(os.path.join(dirpath, f))
            except OSError:
                continue
            key = os.path.dirname(dirpath) if FILE_TIME.search(os.path.basename(dirpath) or "") else dirpath
            out[key].append((start, mtime, f))
    for v in out.values():
        v.sort()
    return out


def record_gaps(work, node, cams, t0, t1, th):
    """Khoảng hở ghi hình trong [t0, t1] của các camera ghi 24/7.

    Hở = đầu file sau - lần ghi cuối của file trước (mtime). Thêm: file mới nhất phải còn được
    ghi ở cuối cửa sổ, nếu không là ghi hình đã dừng hẳn.
    """
    streams = record_streams(work, node, cams)
    gaps, stale, examples = 0, 0, []
    for key, files in streams.items():
        for (s0, m0, f0), (s1, _m1, f1) in zip(files, files[1:]):
            if s1 < t0 or m0 > t1:
                continue
            gap = s1 - m0
            if gap > th["record_gap_sec"]:
                gaps += 1
                if len(examples) < 6:
                    examples.append("%s: %s -> %s hở %.0fs" % (os.path.relpath(key, work), f0, f1, gap))
        if files and t1 - files[-1][1] > th["record_stale_sec"]:
            stale += 1
            if len(examples) < 6:
                examples.append("%s: file cuối %s không ghi %.0fs"
                                % (os.path.relpath(key, work), files[-1][2], t1 - files[-1][1]))
    return {"streams": len(streams), "gaps": gaps, "stale": stale, "examples": examples}


# ================================================================== một bậc
class Runner(object):
    def __init__(self, args):
        self.args = args
        self.th = dict(THRESHOLDS)
        for kv in args.th or []:
            k, _, v = kv.partition("=")
            if k not in self.th:
                raise SystemExit("không có ngưỡng %s" % k)
            self.th[k] = float(v)
        self.v = Viewers(args.controller)
        self.s = Server(args.work, args.node, args.nic)
        self.always = set(self.s.inv.get("by_cohort", {}).get("always", []))
        self.baseline = None
        self.rows = []
        self.extra_md = []   # phần báo cáo riêng của kịch bản (vd bảng từng phiên replay)

    # ---- đo
    def snapshot(self):
        rx, tx = self.s.nic_bytes()
        return {"t": time.time(), "cpu": self.s.cpu_ticks(), "tasks": self.s.task_ticks(),
                "rx": rx, "tx": tx, "rss_kb": self.s.rss_kb(), "fds": self.s.fd_count(),
                "proc_io": self.s.proc_io(),
                "io_full_us": psi_total_us("io"), "mem_full_us": psi_total_us("memory"),
                "iowait": cpu_iowait_ticks()}

    def server_view(self):
        rows, err = self.s.media_rows()
        if rows is None:
            return {"error": err}
        cams = set(c for ids in self.s.inv.get("by_cohort", {}).values() for c in ids)
        streams = {}
        readers = 0
        for r in rows:
            readers += r.get("readerCount", 0) or 0
            key = (r.get("app", ""), r.get("stream", ""))
            cur = streams.setdefault(key, {"recording": False})
            cur["recording"] |= bool(r.get("isRecordingMP4"))
        live = sum(1 for (app, _s) in streams if app in cams)
        return {"streams": streams, "live_streams": live, "readers": readers}

    def ramp_to(self, level, join_rate, timeout=None):
        self.v.set(level=level, join_rate=join_rate)
        t0 = time.time()
        timeout = timeout or (level / max(1.0, join_rate) * 1.5 + 45)
        need = int(level * self.th["join_ok_ratio"])
        playing = 0
        while time.time() - t0 < timeout:
            playing = self.v.playing()
            if playing >= need and playing <= level:
                break
            time.sleep(1)
        return time.time() - t0, playing

    def measure(self, level, hold, label=None, join_rate=None, measure_join=True):
        a = self.args
        join_rate = join_rate or a.join_rate
        label = label or str(level)
        log("bậc %s: đặt %d viewer (vào %.0f/s), giữ %ds" % (label, level, join_rate, hold))
        self.v.mark()
        ramp_sec, _ = self.ramp_to(level, join_rate)
        warm = min(a.warmup, max(0, hold - 30))
        time.sleep(warm)
        ramp = self.v.stats()
        logs0 = scan_logs(a.work, a.node)
        self.v.mark()
        smp = Sampler(self.s)
        s0 = self.snapshot()
        smp.start()
        window = max(30, hold - warm)
        # hàm nóng chỉ lấy ở bậc mục tiêu: đó là bậc cần biết tài nguyên đang tiêu vào đâu
        perf = (Perf(a.work, label, self.s.pid(), min(a.perf_sec, window))
                if a.perf_sec > 0 and level == a.accept and label == str(a.accept) else None)
        t_end = time.time() + window
        while time.time() < t_end and self.s.alive():
            time.sleep(min(5, max(0.1, t_end - time.time())))
        smp.stop()
        s1 = self.snapshot()
        win = self.v.stats()
        logs1 = scan_logs(a.work, a.node)
        view = self.server_view()
        # nguồn: lấy giá trị xấu nhất trong các lần lấy mẫu của cửa sổ
        ss = smp.source or [cs.load_json(os.path.join(a.work, "rtsp_state.json"))]
        self.source_window = {
            "cpu_pct_max": max((x.get("cpu_pct_max", x.get("cpu_pct")) or 0) for x in ss),
            "tick_late_p99_ms": max((x.get("tick_late_p99_ms") or 0) for x in ss),
            "tick_late_max_ms": max((x.get("tick_late_max_ms") or 0) for x in ss),
            "kbps": ss[-1].get("kbps")}
        row = self.evaluate(level, label, ramp_sec, ramp if measure_join else None, win, s0, s1,
                            smp, view, logs0, logs1)
        if perf:
            row["perf"] = perf.finish()
        self.rows.append(row)
        self.save()
        log("  -> %s%s" % (row["verdict"], (": " + "; ".join(row["failures"][:4])) if row["failures"] else ""))
        return row

    # ---- chấm
    def evaluate(self, level, label, ramp_sec, ramp, win, s0, s1, smp, view, logs0, logs1):
        th = self.th
        fails, invalid, warns = [], [], []
        cat = []  # yếu tố giới hạn theo thứ tự phát hiện
        span = max(0.001, s1["t"] - s0["t"])
        row = {"label": label, "level": level, "at": time.strftime("%H:%M:%S", time.localtime(s0["t"])),
               "window_sec": round(span), "ramp_sec": round(ramp_sec, 1)}

        # ---- server sống / log
        new = {k: logs1.get(k, 0) - logs0.get(k, 0)
               for k in ("crash", "db_locked", "failed_apply", "rtp_timeout")}
        row["log_new"] = new
        row["ingest_timeouts"] = new["rtp_timeout"]
        if not self.s.alive() or new["crash"]:
            fails.append("MediaServer chết/crash")
            cat.append("crash")
        if new["db_locked"] or new["failed_apply"]:
            warns.append("log: db_locked=%d failed_apply=%d" % (new["db_locked"], new["failed_apply"]))

        # ---- tự kiểm client
        workers = win.get("workers") or []
        fresh = [w for w in workers if w.get("fresh") and w.get("same_mark")]
        if len(fresh) < win.get("workers_expected", 0):
            invalid.append("chỉ %d/%d worker báo số liệu" % (len(fresh), win.get("workers_expected", 0)))
        for w in fresh:
            if (w.get("cpu_pct") or 0) > th["client_cpu_pct"]:
                invalid.append("worker %s CPU %.0f%%" % (w["worker"], w["cpu_pct"]))
            if (w.get("loop_lag_p99_ms") or 0) > th["client_loop_lag_ms"]:
                invalid.append("worker %s trễ vòng lặp p99 %.0fms" % (w["worker"], w["loop_lag_p99_ms"]))
        src = self.source_window
        row["source"] = src
        if (row["source"]["cpu_pct_max"] or 0) > th["source_cpu_pct"]:
            invalid.append("nguồn RTSP CPU %.0f%%" % row["source"]["cpu_pct_max"])
        if (row["source"]["tick_late_p99_ms"] or 0) > th["source_late_p99_ms"]:
            invalid.append("nguồn RTSP tụt nhịp p99 %.0fms" % row["source"]["tick_late_p99_ms"])
        if (row["source"].get("tick_late_max_ms") or 0) > th["source_late_max_ms"]:
            invalid.append("nguồn RTSP treo %.0fms" % row["source"]["tick_late_max_ms"])
        row["client"] = {"workers": len(fresh), "cpu_max": max([w.get("cpu_pct") or 0 for w in fresh] or [0]),
                         "loop_lag_max_ms": max([w.get("loop_lag_p99_ms") or 0 for w in fresh] or [0])}

        # ---- viewer
        # phiên replay đã xem đủ --replay-watch tự kết thúc: tính như đang phát, không phải hụt
        playing = sum((win.get("playing") or {}).values()) + sum((win.get("done") or {}).values())
        row["playing"] = playing
        joins_ok = joins_fail = 0
        ttff = defaultdict(list)
        fail_reasons = defaultdict(int)
        for src in [x for x in (ramp, win) if x]:
            for k, j in (src.get("joins") or {}).items():
                joins_ok += j.get("ok", 0)
                joins_fail += j.get("fail", 0)
            for k, vals in (src.get("ttff_ms") or {}).items():
                ttff[k].extend(vals)
            for k, n in (src.get("fail_reasons") or {}).items():
                fail_reasons[k] += n
        attempts = joins_ok + joins_fail
        row["joins"] = {"ok": joins_ok, "fail": joins_fail,
                        "reasons": dict(sorted(fail_reasons.items(), key=lambda kv: -kv[1])[:6])}
        if level and playing < level * th["join_ok_ratio"]:
            fails.append("chỉ %d/%d viewer đang phát" % (playing, level))
            cat.append("viewer")
        if attempts and joins_ok / float(attempts) < th["join_ok_ratio"]:
            fails.append("vào được %d/%d lần" % (joins_ok, attempts))
            cat.append("viewer")
        row["ttff_p95_ms"] = {}
        row["ttff_max_ms"] = {}
        for k, vals in ttff.items():
            row["ttff_p95_ms"][k] = round(pct(vals, 0.95))
            row["ttff_max_ms"][k] = round(max(vals))
            limit = th["ttff_max_replay_ms"] if k == "replay" else th["ttff_max_live_ms"]
            if max(vals) > limit:
                over = sum(1 for x in vals if x > limit)
                fails.append("TTFF %s max %.0fms > %.0fms (%d lần)" % (k, max(vals), limit, over))
                cat.append("viewer")
        no_first = {k: n for k, n in fail_reasons.items() if k.endswith(":ttff_timeout")}
        if no_first:
            fails.append("không có hình đầu sau 15s: %s" % ", ".join("%s=%d" % kv for kv in sorted(no_first.items())))
            cat.append("viewer")
        # theo phiên đang mở (kể cả đang thử lại), tức đúng thành phần của plan
        active = win.get("active") or {}
        n_active = sum(active.values())
        if len(active) > 1 and n_active:
            share = active.get("replay", 0) / float(n_active)
            row["replay_share"] = round(share, 3)
            if share > th["replay_share_max"] + 1e-9:
                invalid.append("replay chiếm %.0f%% viewer > %.0f%%" % (share * 100, th["replay_share_max"] * 100))

        rows_v = win.get("viewers") or []
        by_kind = defaultdict(list)
        for r in rows_v:
            by_kind[r[0]].append(r)
        watch = sum(r[1] for r in rows_v)
        stall = sum(r[2] for r in rows_v)
        long_sess = sum(1 for r in rows_v if r[3] > 0)
        drifts = [r[4] for r in rows_v if r[1] >= 30]
        row["drift_p95_by_kind"] = {k: round(pct([r[4] for r in rs if r[1] >= 30] or [0], 0.95), 2)
                                    for k, rs in by_kind.items()}
        row["stall_pct"] = round(stall / watch * 100.0, 3) if watch else None
        row["long_stall_sessions"] = long_sess
        row["drift_p95_sec"] = round(pct(drifts, 0.95), 2) if drifts else None
        if rows_v and long_sess / float(len(rows_v)) > th["long_stall_session_ratio"]:
            fails.append("%d/%d phiên khựng > 2s" % (long_sess, len(rows_v)))
            cat.append("viewer")
        row["media_rate"] = {}
        for k, rs in by_kind.items():
            w_sum = sum(r[1] for r in rs)
            rate = sum(r[1] - r[4] for r in rs) / w_sum if w_sum else 1.0
            row["media_rate"][k] = round(rate, 3)
        closes = win.get("closes") or {}
        n_closes = sum(closes.values())
        row["unexpected_closes"] = n_closes
        if n_closes > max(level, len(rows_v)) * th["unexpected_close_ratio"]:
            top = ", ".join("%s=%d" % kv for kv in sorted(closes.items(), key=lambda kv: -kv[1])[:3])
            fails.append("%d phiên bị đứt (%s)" % (n_closes, top))
            cat.append("viewer")
        row["client_mbps"] = round(win.get("bytes", 0) * 8 / 1e6 / max(1.0, win.get("span_sec") or span), 1)
        if win.get("events"):
            row["events"] = win["events"]

        # ---- server
        cpu_pct = (s1["cpu"] - s0["cpu"]) / float(HZ) / span * 100.0
        row["cpu_pct_total"] = round(cpu_pct / NCPU, 1)
        if row["cpu_pct_total"] > th["process_cpu_pct"]:
            warns.append("CPU tiến trình %.0f%% > %.0f%%" % (row["cpu_pct_total"], th["process_cpu_pct"]))
            cat.append("cpu")
        hot = []
        for tid, (comm, ticks) in s1["tasks"].items():
            prev = s0["tasks"].get(tid)
            if prev:
                hot.append(((ticks - prev[1]) / float(HZ) / span * 100.0, comm))
        hot.sort(reverse=True)
        row["hot_threads"] = [["%s" % c, round(p, 1)] for p, c in hot[:4]]
        groups = defaultdict(lambda: [0.0, 0])
        for p, c in hot:
            g = groups[thread_group(c)]
            g[0] += p
            g[1] += 1
        # [nhóm, tổng % một lõi, số thread] — vd poller ăn 300% trên 16 thread vs 1 thread SQLite 95%
        row["thread_groups"] = [[g, round(v[0], 1), v[1]] for g, v in
                                sorted(groups.items(), key=lambda kv: -kv[1][0])[:8] if v[0] >= 0.5]
        io0, io1 = s0.get("proc_io") or (0, 0), s1.get("proc_io") or (0, 0)
        row["disk_read_mbps"] = round((io1[0] - io0[0]) / 1e6 / span, 1)
        row["disk_write_mbps"] = round((io1[1] - io0[1]) / 1e6 / span, 1)
        loads = {k: sum(v) / float(len(v)) for k, v in smp.loads.items() if v}
        row["poller_load_max"] = round(max(loads.values()), 1) if loads else None
        if loads and max(loads.values()) > th["poller_load_pct"]:
            warns.append("poller nóng %.0f%%" % max(loads.values()))
            cat.append("poller")
        hc = smp.health
        row["healthcheck_p95_ms"] = round(pct(hc, 0.95), 1) if hc else None
        row["healthcheck_max_ms"] = round(max(hc), 1) if hc else None
        if hc and pct(hc, 0.95) > th["healthcheck_p95_ms"]:
            fails.append("healthcheck p95 %.0fms > %.0fms" % (pct(hc, 0.95), th["healthcheck_p95_ms"]))
            cat.append("poller")
        row["rss_mb"] = round(s1["rss_kb"] / 1024.0)
        row["rss_delta_mb"] = round((s1["rss_kb"] - s0["rss_kb"]) / 1024.0, 1)
        row["fds"] = s1["fds"]
        if s1["rss_kb"] > mem_total_kb() * th["rss_ram_ratio"]:
            warns.append("RSS %d MB > %.0f%% RAM" % (row["rss_mb"], th["rss_ram_ratio"] * 100))
            cat.append("memory")
        for kind in ("io", "mem"):
            a0, a1 = s0.get(kind + "_full_us"), s1.get(kind + "_full_us")
            if a0 is not None and a1 is not None:
                row["%s_full_pct" % kind] = round((a1 - a0) / 1e6 / span * 100.0, 2)
        if row.get("io_full_pct") is not None and row["io_full_pct"] > th["io_full_ratio"] * 100:
            warns.append("đĩa nghẽn: mọi tác vụ chờ IO %.1f%% thời gian" % row["io_full_pct"])
            cat.append("disk")
        if row.get("io_full_pct") is None and s0.get("iowait") and s1.get("iowait"):
            total = s1["iowait"][0] - s0["iowait"][0]
            if total > 0:
                row["io_wait_pct"] = round((s1["iowait"][1] - s0["iowait"][1]) * 100.0 / total, 1)
                if row["io_wait_pct"] > th["io_wait_ratio"] * 100:
                    warns.append("đĩa nghẽn: CPU chờ IO %.1f%% thời gian (iowait, kernel không có PSI)"
                                 % row["io_wait_pct"])
                    cat.append("disk")
        tx_mbps = (s1["tx"] - s0["tx"]) * 8 / 1e6 / span
        row["nic_tx_mbps"] = round(tx_mbps, 1)
        speed = self.s.nic_speed_mbps()
        if speed and tx_mbps > speed * th["nic_util"]:
            warns.append("NIC %s %.0f/%d Mbps" % (self.s.nic, tx_mbps, speed))
            cat.append("nic")

        # ---- nền
        if "error" in view:
            fails.append("getMediaList lỗi: %s" % view["error"])
            cat.append("server")
        else:
            row["live_streams"] = view["live_streams"]
            row["server_readers"] = view["readers"]
            if self.baseline:
                base_live = self.baseline["live_streams"]
                if base_live and view["live_streams"] < base_live * th["stream_online_ratio"]:
                    fails.append("luồng nền còn %d/%d" % (view["live_streams"], base_live))
                    cat.append("recording")
                row["reader_delta"] = view["readers"] - self.baseline["readers"]
            sched = cs.check_schedules(self.args.work, self.s.inv, view["streams"])
            row["schedule_mismatch"] = sched["mismatch"]
            if sched["mismatch"]:
                fails.append("%d luồng ghi sai lịch" % sched["mismatch"])
                cat.append("recording")
        gaps = record_gaps(self.args.work, self.args.node, self.always, s0["t"], s1["t"], th)
        row["record"] = gaps
        if gaps["gaps"] or gaps["stale"]:
            fails.append("ghi hình: %d khoảng hở, %d luồng ngừng ghi" % (gaps["gaps"], gaps["stale"]))
            cat.append("recording")

        row["failures"] = fails
        row["invalid"] = invalid
        row["warnings"] = warns
        # cảnh báo tài nguyên chỉ dùng để gọi tên yếu tố giới hạn khi bậc đã trượt
        order = ["crash", "disk", "nic", "memory", "cpu", "poller", "recording", "server", "viewer"]
        row["limit"] = next((c for c in order if c in cat), None) if fails else None
        if invalid:
            row["verdict"] = "không hợp lệ (bộ sinh tải quá tải)"
        elif fails:
            row["verdict"] = "trượt"
        else:
            row["verdict"] = "đạt"
        row["ok"] = not fails and not invalid
        return row

    def take_baseline(self):
        view = self.server_view()
        if "error" in view:
            raise SystemExit("không đọc được getMediaList: %s" % view["error"])
        self.baseline = {"live_streams": view["live_streams"], "readers": view["readers"],
                         "fds": self.s.fd_count(), "rss_kb": self.s.rss_kb(),
                         "statistic": self.s.statistic()}
        log("mốc: %d luồng nền, %d reader, %d fd" % (view["live_streams"], view["readers"],
                                                   self.baseline["fds"]))

    # ---- kết quả
    def save(self, summary=None):
        out = {"scenario": self.args.scenario, "mix": self.args.mix, "dist": self.args.dist,
               "accept": self.args.accept, "ncpu": NCPU, "mem_mb": mem_total_kb() // 1024,
               "thresholds": self.th, "rows": self.rows}
        if summary:
            out["summary"] = summary
        if self.extra_md:
            out["extra_md"] = self.extra_md
        name = "ccu_report" if self.args.scenario == "step" else "ccu_%s_report" % self.args.scenario
        with open(os.path.join(self.args.work, name + ".json"), "w") as fh:
            json.dump(out, fh, indent=1, ensure_ascii=False)
        with open(os.path.join(self.args.work, name + ".md"), "w") as fh:
            fh.write(render_md(out))
        return out


def render_md(rep):
    lines = ["# CCU — %s (mix=%s, dist=%s, %d lõi, %d MB RAM)" % (
        rep["scenario"], rep["mix"], rep["dist"], rep["ncpu"], rep["mem_mb"]), ""]
    s = rep.get("summary")
    if s:
        for k, v in s.items():
            lines.append("- **%s**: %s" % (k, v))
        lines.append("")
    lines.append("| bậc | viewer | phát | client Mbps | NIC tx Mbps | CPU% | poller max | ghi đĩa MB/s | IO full % | "
                 "healthcheck p95 | TTFF max (ms) | khựng % | trôi p95 | ghi hình | kết luận |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rep["rows"]:
        ttff = ", ".join("%s %s" % kv for kv in sorted((r.get("ttff_max_ms") or r.get("ttff_p95_ms") or {}).items())) or "-"
        rec = r.get("record") or {}
        lines.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["label"], r["level"], r.get("playing"), r.get("client_mbps"), r.get("nic_tx_mbps"),
            r.get("cpu_pct_total"), r.get("poller_load_max"), r.get("disk_write_mbps"),
            r.get("io_full_pct") if r.get("io_full_pct") is not None
            else ("wa %s" % r["io_wait_pct"] if r.get("io_wait_pct") is not None else None),
            r.get("healthcheck_p95_ms"), ttff,
            r.get("stall_pct"), r.get("drift_p95_sec"),
            ("OK" if not (rec.get("gaps") or rec.get("stale")) else "%d hở/%d dừng" % (rec.get("gaps"), rec.get("stale")))
            + (" (%d timeout kéo)" % r["ingest_timeouts"] if r.get("ingest_timeouts") else ""),
            r["verdict"] + (" — " + r["limit"] if r.get("limit") else "")))
    lines.append("")
    for r in rep["rows"]:
        if r["failures"] or r["invalid"]:
            lines.append("- bậc %s: %s" % (r["label"], "; ".join(r["invalid"] + r["failures"])))
        if r.get("warnings"):
            lines.append("- bậc %s (cảnh báo, không tính trượt): %s" % (r["label"], "; ".join(r["warnings"])))
    # Nút thắt: CPU theo nhóm thread ở mọi bậc, hàm nóng ở bậc mục tiêu
    groups = [r for r in rep["rows"] if r.get("thread_groups")]
    if groups:
        lines += ["", "## CPU theo nhóm thread (% một lõi, số thread)", ""]
        for r in groups:
            lines.append("- bậc %s: %s" % (r["label"], ", ".join(
                "%s %.0f%% (%d)" % tuple(g) for g in r["thread_groups"])))
    for r in rep["rows"]:
        p = r.get("perf")
        if not p:
            continue
        lines += ["", "## Hàm nóng ở bậc %s (perf, self time)" % r["label"], ""]
        if p.get("error"):
            lines.append("- không lấy được: %s" % p["error"])
            continue
        lines.append("Chi tiết: `%s` (cột: %% mẫu, thread, thư viện, hàm)" % p["file"])
        lines += ["", "```"] + p["top"] + ["```"]
    render_extra(rep, lines)
    return "\n".join(lines) + "\n"


def render_extra(rep, lines):
    if rep.get("extra_md"):
        lines += [""] + rep["extra_md"]
    return lines


def round_down(x, step=50):
    # bậc nhỏ (máy dev) giữ nguyên, không thì 40 viewer thành "0"
    return int(x // step * step) if x >= 2 * step else int(x)


# ================================================================== kịch bản
def scenario_step(rn):
    a = rn.args
    steps = [int(x) for x in a.steps.split(",") if x.strip()]
    last_ok, first_fail, streak = None, None, 0
    stopped = ""
    for level in steps:
        hold = a.accept_hold if level == a.accept else a.hold
        row = rn.measure(level, hold)
        if level == 0 and rn.baseline is None:
            rn.take_baseline()
        if row["invalid"]:
            stopped = "dừng: bộ sinh tải quá tải ở bậc %d" % level
            break
        if row["limit"] == "crash":
            stopped = "dừng: MediaServer crash ở bậc %d" % level
            break
        if row["ok"]:
            last_ok, first_fail, streak = level, None, 0
        else:
            first_fail = first_fail if first_fail is not None else level
            streak += 1
            if streak >= 2:
                stopped = "dừng: 2 bậc trượt liên tiếp"
                break
    # chia đôi giữa bậc đạt cuối và bậc trượt đầu
    if last_ok is not None and first_fail is not None and not stopped.startswith("dừng: MediaServer"):
        lo, hi = last_ok, first_fail
        for _ in range(a.bisect):
            mid = round_down((lo + hi) / 2.0)
            if mid <= lo or mid >= hi:
                break
            row = rn.measure(mid, a.hold, label="%d*" % mid)
            if row["invalid"] or row["limit"] == "crash":
                break
            if row["ok"]:
                lo = mid
            else:
                hi = mid
        last_ok = lo
    limit_row = next((r for r in rn.rows if not r["ok"] and r["level"] > (last_ok or -1)), None)
    ceiling_hit = limit_row is not None
    accept_row = next((r for r in rn.rows if r["level"] == a.accept and r["label"] == str(a.accept)), None)
    summary = {
        "CCU đạt": ("chưa có bậc > 0 đạt" if not last_ok else
                    "%d" % round_down(last_ok) if ceiling_hit else
                    ">= %d (mọi bậc đều đạt, chưa chạm trần)" % last_ok),
        "Yếu tố giới hạn": (limit_row["limit"] or limit_row["verdict"]) if limit_row else "chưa chạm",
        "Nghiệm thu A1 (bậc %d)" % a.accept: ("ĐẠT" if accept_row and accept_row["ok"] else
                                             "KHÔNG ĐẠT" if accept_row else "chưa chạy tới"),
        "Dư so với nghiệm thu": ("%.0f%%" % ((last_ok - a.accept) * 100.0 / a.accept)) if last_ok else "-",
    }
    if accept_row:
        # mức dư ở bậc mục tiêu — đọc cùng "CPU theo nhóm thread"/"Hàm nóng" để biết tinh chỉnh gì
        groups = accept_row.get("thread_groups") or []
        summary["Tài nguyên ở bậc %d" % a.accept] = (
            "CPU %s%% tổng, poller nóng %s%%, NIC tx %s Mbps, ghi đĩa %s MB/s, RSS %s MB, nhóm nặng nhất: %s" % (
                accept_row.get("cpu_pct_total"), accept_row.get("poller_load_max"),
                accept_row.get("nic_tx_mbps"), accept_row.get("disk_write_mbps"), accept_row.get("rss_mb"),
                ", ".join("%s %.0f%%/%d thread" % tuple(g) for g in groups[:3]) or "-"))
    if stopped:
        summary["Ghi chú"] = stopped
    base_row = next((r for r in rn.rows if r["level"] == 0), None)
    if base_row and not base_row["ok"]:
        # Không viewer mà đã trượt: nền (300 camera ghi hình) hoặc máy không đạt, mọi bậc sau
        # đều trượt theo — con số CCU vô nghĩa cho tới khi sửa nền.
        summary["NỀN KHÔNG ĐẠT ở bậc 0"] = "; ".join(base_row["invalid"] + base_row["failures"])
    return summary


def judge_session(x, th, watch):
    """Lý do phiên replay không đạt (rỗng = đạt)."""
    why = []
    if x.get("ttff_ms") is None:
        why.append("không vào được (%s)" % (x.get("last_reason") or x.get("state")))
        return why
    if x["ttff_ms"] > th["ttff_max_replay_ms"]:
        why.append("TTFF %.1fs" % (x["ttff_ms"] / 1000.0))
    if x["state"] != "done":
        why.append("chưa xem đủ (%s)" % x["state"])
    if x["media_sec"] < watch * th["replay_media_ratio"]:
        why.append("media %.0f/%ds" % (x["media_sec"], watch))
    if x["long_stalls"]:
        why.append("%d lần khựng > 2s (dài nhất %.1fs)" % (x["long_stalls"], x["stall_max_sec"]))
    if x["reconnects"] or x.get("join_fails"):
        why.append("%d lần kết nối lại, %d lần mở lại lỗi (%s)" % (
            x["reconnects"], x.get("join_fails", 0), x.get("reasons") or x.get("last_reason")))
    return why


def write_sessions_csv(path, sessions):
    cols = ["slot", "kind", "cam", "stream", "back", "stamp", "ttff_ms", "watched_sec", "media_sec",
            "stall_sec", "stall_count", "stall_max_sec", "long_stalls", "reconnects", "join_fails",
            "kbps", "state", "last_reason", "reasons", "ok", "why"]
    with open(path, "w") as fh:
        fh.write(",".join(cols) + "\n")
        for x in sessions:
            fh.write(",".join('"%s"' % str(x.get(c, "")).replace('"', "'") if c in ("why", "last_reason", "reasons")
                              else str(x.get(c, "") if x.get(c) is not None else "") for c in cols) + "\n")


def scenario_replay(rn):
    """Bậc mục tiêu với đúng 20% replay (CCU_MIX=live_replay): mỗi phiên replay mở ở mốc ngẫu nhiên
    trong --replay-back, xem --replay-watch giây rồi kết thúc; chấm chất lượng TỪNG phiên."""
    a = rn.args
    watch = a.replay_watch
    if watch <= 0:
        raise SystemExit("kịch bản replay cần --replay-watch > 0")
    rn.measure(0, 60)
    rn.take_baseline()
    # cửa sổ đo phủ trọn phiên replay: mở trong lúc ramp, TTFF ≤ 10 s, xem `watch` giây
    row = rn.measure(a.level, watch + a.warmup + 30, label=str(a.level))
    deadline = time.time() + 60
    while time.time() < deadline:
        st = rn.v.stats()
        ses = st.get("sessions") or []
        if not any(x["kind"] == "replay" and x["state"] in ("playing", "opening") for x in ses):
            break
        time.sleep(3)
    ses = sorted(rn.v.stats().get("sessions") or [], key=lambda x: x["slot"])
    for x in ses:
        why = judge_session(x, rn.th, watch) if x["kind"] == "replay" else []
        if x["kind"] != "replay":
            if x.get("ttff_ms") is None:
                why.append("không vào được")
            if x.get("long_stalls"):
                why.append("%d lần khựng > 2s" % x["long_stalls"])
            if x.get("reconnects"):
                why.append("%d lần kết nối lại" % x["reconnects"])
        x["ok"] = not why
        x["why"] = "; ".join(why)
    write_sessions_csv(os.path.join(a.work, "ccu_replay_sessions.csv"), ses)

    rp = [x for x in ses if x["kind"] == "replay"]
    ok = [x for x in rp if x["ok"]]
    md = rn.extra_md
    md += ["## Chất lượng từng phiên xem lại (%d phiên, xem %d s, mốc lùi %s s)" % (len(rp), watch, a.replay_back), ""]
    # theo nhóm mốc lùi (chia khoảng --replay-back làm 3): mốc sát bản ghi mới phải chờ file đang ghi
    # đóng, mốc xa đọc file cũ hơn — so các nhóm để thấy ảnh hưởng của vị trí mốc
    try:
        lo_b, hi_b = [int(v) for v in a.replay_back.split(",")]
    except ValueError:
        lo_b, hi_b = 0, max([x.get("back") or 0 for x in rp] or [0])
    step_b = max(1, (hi_b - lo_b + 1) // 3)
    buckets = [(lo_b, lo_b + step_b - 1), (lo_b + step_b, lo_b + 2 * step_b - 1), (lo_b + 2 * step_b, 10 ** 9)]
    md += ["| mốc lùi (s) | phiên | đạt | TTFF p95/max (ms) | khựng tổng p95 (s) | khựng dài nhất (s) | kbps p50 |",
           "|---|---|---|---|---|---|---|"]
    for lo, hi in buckets:
        b = [x for x in rp if x.get("back") is not None and lo <= x["back"] <= hi]
        if not b:
            continue
        tt = [x["ttff_ms"] for x in b if x.get("ttff_ms") is not None]
        md.append("| %s | %d | %d | %s | %s | %s | %s |" % (
            "%d–%d" % (lo, hi) if hi < 10 ** 9 else ">= %d" % lo, len(b), sum(1 for x in b if x["ok"]),
            "%d / %d" % (pct(tt, 0.95), max(tt)) if tt else "-",
            round(pct([x["stall_sec"] for x in b], 0.95), 2),
            max(x["stall_max_sec"] for x in b),
            round(pct([x["kbps"] or 0 for x in b], 0.5))))
    md += ["", "| slot | camera | luồng | lùi (s) | TTFF (ms) | đã xem (s) | media (s) | khựng (lần / tổng s / max s) "
           "| kết nối lại | kbps | kết quả |", "|---|---|---|---|---|---|---|---|---|---|---|"]
    for x in rp:
        md.append("| %d | %s | %s | %s | %s | %s | %s | %d / %s / %s | %d | %s | %s |" % (
            x["slot"], x["cam"], x["stream"], x.get("back"), x.get("ttff_ms"), x["watched_sec"],
            x["media_sec"], x["stall_count"], x["stall_sec"], x["stall_max_sec"], x["reconnects"],
            x.get("kbps"), "đạt" if x["ok"] else "TRƯỢT: " + x["why"]))
    live = [x for x in ses if x["kind"] != "replay"]
    if live:
        md += ["", "## Phiên xem trực tiếp cùng lúc (theo loại)", "",
               "| loại | phiên | đạt | TTFF p95/max (ms) | phiên khựng > 2s | kết nối lại |", "|---|---|---|---|---|---|"]
        for k in sorted(set(x["kind"] for x in live)):
            b = [x for x in live if x["kind"] == k]
            tt = [x["ttff_ms"] for x in b if x.get("ttff_ms") is not None]
            md.append("| %s | %d | %d | %s | %d | %d |" % (
                k, len(b), sum(1 for x in b if x["ok"]), "%d / %d" % (pct(tt, 0.95), max(tt)) if tt else "-",
                sum(1 for x in b if x.get("long_stalls")), sum(x.get("reconnects") or 0 for x in b)))
    md += ["", "Từng phiên (cả live): `ccu_replay_sessions.csv`."]

    ratio = len(ok) / float(len(rp)) if rp else 0.0
    passed = row["ok"] and rp and ratio >= rn.th["replay_session_ok_ratio"]
    summary = {
        "Replay %d%% ở bậc %d" % (round(len(rp) * 100.0 / max(1, a.level)), a.level):
            "ĐẠT" if passed else "KHÔNG ĐẠT",
        "Phiên xem lại đạt": "%d/%d (%.1f%%, cần ≥ %.0f%%)" % (
            len(ok), len(rp), ratio * 100, rn.th["replay_session_ok_ratio"] * 100),
        "Bậc %d (live + nền ghi hình)" % a.level: row["verdict"] + (
            (": " + "; ".join(row["failures"][:3])) if row["failures"] else ""),
    }
    return summary


def scenario_storm(rn):
    a = rn.args
    rn.measure(0, 60, label="0")
    rn.take_baseline()
    log("storm: mở %d viewer trong %ds" % (a.level, a.storm_sec))
    rn.v.mark()
    rn.v.set(level=a.level, join_rate=a.level / float(a.storm_sec))
    t0 = time.time()
    all_in = None
    while time.time() - t0 < 60:
        if rn.v.playing() >= a.level:
            all_in = time.time() - t0
            break
        time.sleep(0.5)
    # TTFF của đợt mở đồng loạt phải lấy trước measure(): measure() mở cửa sổ đo mới
    burst = rn.v.stats()
    ttff = [x for vals in (burst.get("ttff_ms") or {}).values() for x in vals]
    joins = burst.get("joins") or {}
    fails = sum(j.get("fail", 0) for j in joins.values())
    th = rn.th
    slow = {}
    for k, vals in (burst.get("ttff_ms") or {}).items():
        limit = th["ttff_max_replay_ms"] if k == "replay" else th["ttff_max_live_ms"]
        if vals and max(vals) > limit:
            slow[k] = round(max(vals))
    for k, n in (burst.get("fail_reasons") or {}).items():
        if k.endswith(":ttff_timeout"):
            slow[k] = n
    row = rn.measure(a.level, 90, label="storm %d" % a.level, join_rate=a.level / float(a.storm_sec))
    ok = all_in is not None and all_in <= 30 and row["ok"] and not slow
    return {"Tất cả vào được sau": ("%.1fs" % all_in) if all_in is not None else "> 60s",
            "TTFF khi mở đồng loạt": ("p50 %.0f ms, p95 %.0f ms, max %.0f ms" % (
                pct(ttff, 0.5), pct(ttff, 0.95), max(ttff))) if ttff else "-",
            "TTFF vượt ngưỡng khi mở đồng loạt": ("%s" % slow) if slow else "không (live ≤ %.0fs, replay ≤ %.0fs)" % (
                th["ttff_max_live_ms"] / 1000, th["ttff_max_replay_ms"] / 1000),
            "Lần mở thất bại": "%d (%s)" % (fails, dict(sorted((burst.get("fail_reasons") or {}).items(),
                                                          key=lambda kv: -kv[1])[:3])),
            "Nghiệm thu A3": "ĐẠT" if ok else "KHÔNG ĐẠT"}


def settle_compare(rn, label):
    """Về 0 viewer, chờ, so fd/getStatistic với mốc."""
    rn.v.set(level=0, churn_mean_sec=0)
    time.sleep(rn.args.settle)
    fds = rn.s.fd_count()
    stat = rn.s.statistic()
    base = rn.baseline
    diffs = []
    if base["fds"] and abs(fds - base["fds"]) > max(10, base["fds"] * 0.05):
        diffs.append("fd %d -> %d" % (base["fds"], fds))
    # Chỉ đối tượng cấu trúc mới nói lên rò rỉ. Buffer*/Frame*/…Packet là gói đang bay và GOP
    # cache, dao động ±50% theo pha GOP nên so với mốc chỉ ra báo động giả.
    for k in STRUCTURAL_OBJECTS:
        v0, v1 = (base.get("statistic") or {}).get(k), stat.get(k)
        if isinstance(v0, int) and isinstance(v1, int) and abs(v1 - v0) > max(3, v0 * 0.05):
            diffs.append("%s %d -> %d" % (k, v0, v1))
    log("%s: sau %ds về 0 viewer: %s" % (label, rn.args.settle, "; ".join(diffs) or "về mốc"))
    return diffs, fds, stat


def scenario_churn(rn):
    a = rn.args
    rn.measure(0, 60, label="0")
    rn.take_baseline()
    rn.v.set(churn_mean_sec=a.churn_mean)
    t_end = time.time() + a.duration
    k = 0
    while time.time() < t_end:
        k += 1
        rn.measure(a.level, min(a.window, max(60, int(t_end - time.time()))),
                   label="churn#%d" % k, measure_join=True)
    diffs, fds, stat = settle_compare(rn, "churn")
    ok = not diffs and all(r["ok"] for r in rn.rows[1:])
    return {"Sau khi dừng churn": "; ".join(diffs) or "fd và đối tượng về mốc",
            "Nghiệm thu A5": "ĐẠT" if ok else "KHÔNG ĐẠT"}


def scenario_soak(rn):
    a = rn.args
    rn.measure(0, 60, label="0")
    rn.take_baseline()
    t_end = time.time() + a.duration
    k = 0
    while time.time() < t_end:
        k += 1
        rn.measure(a.level, min(a.window, max(60, int(t_end - time.time()))), label="soak#%d" % k,
                   measure_join=(k == 1))
    rss = [r["rss_mb"] for r in rn.rows[1:]]
    fds = [r["fds"] for r in rn.rows[1:]]
    grow = len(rss) >= 4 and all(b >= a_ for a_, b in zip(rss, rss[1:])) and rss[-1] > rss[0] * 1.1
    fd_grow = len(fds) >= 4 and all(b >= a_ for a_, b in zip(fds, fds[1:])) and fds[-1] > fds[0] * 1.05
    diffs, _fds, _stat = settle_compare(rn, "soak")
    ok = not grow and not fd_grow and all(r["ok"] for r in rn.rows[1:])
    return {"RSS theo cửa sổ (MB)": " → ".join(str(x) for x in rss),
            "fd theo cửa sổ": " → ".join(str(x) for x in fds),
            "Tăng đơn điệu": ("RSS " if grow else "") + ("fd" if fd_grow else "") or "không",
            "Sau khi về 0": "; ".join(diffs) or "về mốc",
            "Nghiệm thu A4": "ĐẠT" if ok else "KHÔNG ĐẠT"}


# ---- admission
def write_limits(work, node, **limits):
    """Ghi giới hạn vào api-config.json của node; server đọc lại mỗi hook.report_interval."""
    path = os.path.join(work, node, "api-config.json")
    cfg = cs.load_json(path)
    ms = cfg.setdefault("media_server", {})
    for k, v in limits.items():
        if v is None:
            ms.pop(k, None)
        else:
            ms[k] = v
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(cfg, fh, indent=1)
    os.replace(tmp, path)


def wait_reload(rn):
    # hook on_server_load2 chạy lại mỗi report_interval (harness đặt 10 s)
    time.sleep(rn.args.reload_wait)


def admission_count(rn, viewers, level, rate, hold=45):
    viewers.mark()
    viewers.set(level=level, join_rate=rate)
    t0 = time.time()
    peak = 0
    while time.time() - t0 < hold:
        peak = max(peak, sum(viewers.stats().get("playing", {}).values()))
        time.sleep(1)
    st = viewers.stats()
    return peak, sum(st.get("playing", {}).values()), st


def scenario_admission(rn):
    a = rn.args
    res = {}
    m = a.max_stream
    crit = int(m * 1.1)
    rn.take_baseline()

    # 1. giới hạn toàn node, mở tuần tự vượt ngưỡng
    write_limits(a.work, a.node, unlimitedStream=False, maxStream=m, streamMaxOfLicense=m,
                 unlimitedStreamPerCamera=True, maxStreamPerCamera=None, streamMaxCameraOfLicense=None,
                 thresholdConfig=None)
    wait_reload(rn)
    peak, end, st = admission_count(rn, rn.v, crit + 30, 10.0, hold=int((crit + 30) / 10.0) + 30)
    reasons = st.get("fail_reasons") or {}
    closes = sum((st.get("closes") or {}).values())
    ok1 = end <= crit and end >= crit - 2 and closes == 0
    res["1. Toàn node (maxStream=%d → critical %d)" % (m, crit)] = (
        "%s: đang phát %d, đỉnh %d, bị đứt %d, lý do từ chối %s"
        % ("ĐẠT" if ok1 else "KHÔNG ĐẠT", end, peak, closes,
           dict(sorted(reasons.items(), key=lambda kv: -kv[1])[:3])))
    rn.v.set(level=0)
    time.sleep(10)

    # 2. burst gấp đôi ngưỡng trong 1 s
    peak, end, st = admission_count(rn, rn.v, 2 * crit, 10000.0, hold=30)
    view = rn.server_view()
    res["2. Burst %d viewer / 1s" % (2 * crit)] = (
        "%s: đỉnh %d, cuối %d (ngưỡng %d, vượt %d); reader phía server %s"
        % ("ĐẠT" if peak <= crit else "VƯỢT NGƯỠNG", peak, end, crit, max(0, peak - crit),
           view.get("readers", "?") - rn.baseline["readers"] if "readers" in view else "?"))
    rn.v.set(level=0)
    time.sleep(10)

    # 3. giới hạn theo camera
    if a.single_controller:
        pc = a.max_per_camera
        pcrit = int(pc * 1.1)
        write_limits(a.work, a.node, unlimitedStream=True, streamMaxOfLicense=None, maxStream=None,
                     unlimitedStreamPerCamera=False, maxStreamPerCamera=pc, streamMaxCameraOfLicense=pc)
        wait_reload(rn)
        single = Viewers(a.single_controller)
        peak, end, st = admission_count(rn, single, pcrit + 5, 2.0, hold=int((pcrit + 5) / 2.0) + 20)
        res["3. Theo camera (maxStreamPerCamera=%d → %d)" % (pc, pcrit)] = (
            "%s: đang phát %d" % ("ĐẠT" if end == pcrit else "KHÔNG ĐẠT", end))
        single.set(level=0)
        time.sleep(5)
    else:
        res["3. Theo camera"] = "BỎ QUA (không có --single-controller)"

    # 4. ngưỡng CPU: đặt critical 1% để mọi viewer mới bị từ chối vì tài nguyên
    write_limits(a.work, a.node, unlimitedStream=True, maxStream=None, streamMaxOfLicense=None,
                 unlimitedStreamPerCamera=True, maxStreamPerCamera=None, streamMaxCameraOfLicense=None,
                 thresholdConfig={"CPU_levelLow": 1, "CPU_levelMedium": 1, "RAM_levelLow": 80,
                                  "RAM_levelMedium": 90, "STORAGE_levelLow": 80,
                                  "STORAGE_levelMedium": 90})
    wait_reload(rn)
    peak, end, st = admission_count(rn, rn.v, 10, 5.0, hold=20)
    reasons = st.get("fail_reasons") or {}
    res["4. Ngưỡng CPU (critical=1%)"] = ("%s: đang phát %d/10, lý do %s"
                                          % ("ĐẠT" if end == 0 else "KHÔNG ĐẠT", end,
                                             dict(sorted(reasons.items(), key=lambda kv: -kv[1])[:3])))
    rn.v.set(level=0)
    write_limits(a.work, a.node, thresholdConfig=None)

    # 5. A2: cấu hình production với giới hạn >= mức nghiệm thu, 400 viewer không ai bị từ chối
    write_limits(a.work, a.node, unlimitedStream=False, maxStream=a.accept, streamMaxOfLicense=a.accept,
                 unlimitedStreamPerCamera=True)
    wait_reload(rn)
    row = rn.measure(a.accept, a.hold, label="A2 %d" % a.accept)
    res["A2 (%d viewer, maxStream=%d)" % (a.accept, a.accept)] = "ĐẠT" if row["ok"] else "KHÔNG ĐẠT"
    rn.v.set(level=0)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--controller", default="http://127.0.0.1:18600")
    ap.add_argument("--single-controller", default="", help="controller có --single-camera (admission)")
    ap.add_argument("--node", default="node-a")
    ap.add_argument("--nic", default=os.environ.get("CCU_NIC", "lo"))
    ap.add_argument("--scenario", default="step", choices=("step", "storm", "churn", "soak", "admission", "replay"))
    ap.add_argument("--mix", default="vms")
    ap.add_argument("--dist", default="hot")
    ap.add_argument("--steps", default="0,100,200,300,400,500,600,800,1000,1250,1500,2000")
    ap.add_argument("--hold", type=int, default=150)
    ap.add_argument("--accept", type=int, default=400)
    ap.add_argument("--accept-hold", type=int, default=300)
    ap.add_argument("--warmup", type=int, default=30)
    ap.add_argument("--join-rate", type=float, default=20.0)
    ap.add_argument("--bisect", type=int, default=3)
    ap.add_argument("--level", type=int, default=400, help="storm/churn/soak")
    ap.add_argument("--storm-sec", type=float, default=5.0)
    ap.add_argument("--duration", type=int, default=1800, help="churn/soak")
    ap.add_argument("--window", type=int, default=300, help="churn/soak: độ dài mỗi dòng")
    ap.add_argument("--churn-mean", type=float, default=60.0)
    ap.add_argument("--settle", type=int, default=120)
    ap.add_argument("--max-stream", type=int, default=200, help="admission")
    ap.add_argument("--max-per-camera", type=int, default=8, help="admission")
    ap.add_argument("--reload-wait", type=int, default=25)
    ap.add_argument("--replay-watch", type=int, default=0, help="replay: thời lượng xem mỗi phiên (giây)")
    ap.add_argument("--replay-back", default="", help="replay: chỉ để ghi vào báo cáo")
    ap.add_argument("--perf-sec", type=int, default=0,
                    help="step: perf record bấy nhiêu giây ở bậc --accept (0 = tắt)")
    ap.add_argument("--th", action="append", help="ghi đè ngưỡng: ten=giatri")
    args = ap.parse_args()

    rn = Runner(args)
    try:
        if args.scenario == "step":
            summary = scenario_step(rn)
        elif args.scenario == "storm":
            summary = scenario_storm(rn)
        elif args.scenario == "churn":
            summary = scenario_churn(rn)
        elif args.scenario == "soak":
            summary = scenario_soak(rn)
        elif args.scenario == "replay":
            summary = scenario_replay(rn)
        else:
            summary = scenario_admission(rn)
    finally:
        try:
            rn.v.set(level=0, churn_mean_sec=0)
        except (OSError, ValueError):
            pass
    rep = rn.save(summary)
    print(render_md(rep))
    verdicts = " ".join(str(v) for k, v in summary.items()
                        if k.startswith("Nghiệm thu") or k[:1].isdigit() or k.startswith("A") or k.startswith("Replay"))
    sys.exit(1 if "KHÔNG ĐẠT" in verdicts or "VƯỢT" in verdicts else 0)


if __name__ == "__main__":
    main()
