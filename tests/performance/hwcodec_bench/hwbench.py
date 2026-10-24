#!/usr/bin/env python3
"""Đo hiệu năng GPU so với CPU cho các tính năng dùng hwcodec (thiết kế: README.md cùng thư mục).

Dùng:
    hwbench.py media <dir>                          # dựng clip nguồn (1080p/360p H.264, 60 s H.264/H.265)
    hwbench.py raw <dir> --out raw.json             # L0: năng lực thô của ffmpeg đi kèm (CPU vs VAAPI)
    hwbench.py live <work> --mode cpu|gpu --media <dir> --out live-<mode>.json
                [--cameras 16] [--scenarios motion-sub,motion-main,transcode,extract]
    hwbench.py tier --mode cpu|gpu --media <dir> --out tier-<mode>.json
    hwbench.py report <json...>                     # in bảng markdown

`live` dựng work dir (dùng lại setup_e2e.py), chạy nguồn RTSP giả, mock API và MediaServer với
hwcodec.enable=0 (cpu) hoặc 1 (gpu), rồi đo từng bậc tải. GPU trên WSL2 qua Mesa d3d12 cần
`modprobe vgem` và LIBVA_DRIVER_NAME=d3d12 (harness tự đặt biến này khi --mode gpu).
"""
import argparse
import glob
import itertools
import json
import os
import re
import resource
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tests", "integration", "e2e_real"))
BIN_DIR = os.environ.get("MEDIASERVER_DIR", os.path.join(REPO, "release", "linux", "Debug"))
FF_DIR = os.path.join(REPO, "release", "linux", "3rdpart", "ffmpeg")
FFMPEG = os.path.join(FF_DIR, "bin", "ffmpeg")
FFPROBE = os.path.join(FF_DIR, "bin", "ffprobe")
RENDER_NODE = "/dev/dri/renderD128"
CLK = os.sysconf("SC_CLK_TCK")
NCPU = os.cpu_count() or 1


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)


def gpu_env(mode):
    env = dict(os.environ)
    if mode == "gpu":
        env.setdefault("LIBVA_DRIVER_NAME", "d3d12")
    else:
        env.pop("LIBVA_DRIVER_NAME", None)
    return env


# ---------------------------------------------------------------- lấy mẫu


class GpuSampler:
    """Chạy gpu_sample.ps1 qua powershell.exe, giữ (epoch, {engine: %})."""

    def __init__(self):
        self.samples = []
        self.proc = None
        ps = "powershell.exe"
        script = subprocess.run(["wslpath", "-w", os.path.join(HERE, "gpu_sample.ps1")],
                                capture_output=True, text=True).stdout.strip()
        try:
            self.proc = subprocess.Popen([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script],
                                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        except OSError:
            log("không chạy được powershell.exe: bỏ số liệu GPU")
            return
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            parts = line.split()
            if not parts or not parts[0].isdigit():
                continue
            vals = {}
            for kv in parts[1:]:
                k, _, v = kv.partition("=")
                try:
                    vals[k] = float(v.replace(",", "."))
                except ValueError:
                    pass
            self.samples.append((int(parts[0]), vals))

    def window(self, t0, t1):
        rows = [v for t, v in self.samples if t0 <= t <= t1 + 1]
        if not rows:
            return {}
        keys = sorted({k for r in rows for k in r})
        return {k: round(sum(r.get(k, 0.0) for r in rows) / len(rows), 1) for k in keys
                if k in ("3d", "videodecode", "videoprocessing", "videoencode", "compute", "copy")}

    def stop(self):
        if self.proc:
            self.proc.terminate()


def proc_times(pid):
    """(utime+stime, cutime+cstime, rss MB) của pid, giây."""
    try:
        data = open("/proc/%d/stat" % pid).read()
    except OSError:
        return None
    f = data[data.rfind(")") + 2:].split()
    return ((int(f[11]) + int(f[12])) / CLK, (int(f[13]) + int(f[14])) / CLK,
            int(f[21]) * os.sysconf("SC_PAGE_SIZE") / 2 ** 20)


def sys_busy():
    f = [int(x) for x in open("/proc/stat").readline().split()[1:]]
    idle = f[3] + f[4]
    return sum(f) - idle, sum(f)


class ChildWatch:
    """Cộng CPU của mọi tiến trình con (ffmpeg extract) của pid, kể cả con đã thoát giữa hai lần quét."""

    def __init__(self, pid):
        self.pid = pid
        self.seen = {}
        self.stop_flag = False
        self.t = threading.Thread(target=self._loop, daemon=True)
        self.t.start()

    def _loop(self):
        while not self.stop_flag:
            for stat in glob.glob("/proc/[0-9]*/stat"):
                try:
                    data = open(stat).read()
                except OSError:
                    continue
                f = data[data.rfind(")") + 2:].split()
                if int(f[1]) == self.pid:
                    pid = int(stat.split("/")[2])
                    self.seen[pid] = (int(f[11]) + int(f[12])) / CLK
            time.sleep(0.3)

    def total(self):
        self.stop_flag = True
        self.t.join()
        return round(sum(self.seen.values()), 2)


# ---------------------------------------------------------------- media


def cmd_media(args):
    d = args.dir
    os.makedirs(d, exist_ok=True)
    # Như MEDIA_PROFILE=realistic của run_stress.sh: nhiễu theo thời gian để bitrate thật, GOP 2 s.
    specs = [("main_1080p.h264", "1920x1080", "4000k", 10, ["-bsf:v", "h264_mp4toannexb", "-f", "h264"], "libx264"),
             ("sub_360p.h264", "640x360", "512k", 10, ["-bsf:v", "h264_mp4toannexb", "-f", "h264"], "libx264"),
             ("main_1080p_60s.mp4", "1920x1080", "4000k", 60, [], "libx264"),
             ("main_1080p_60s_hevc.mp4", "1920x1080", "3000k", 60, [], "libx265")]
    for name, size, rate, dur, extra, enc in specs:
        dst = os.path.join(d, name)
        if os.path.exists(dst):
            continue
        log("dựng " + name)
        # Bản ffmpeg đi kèm không có lavfi (--disable-avdevice): dựng bằng ffmpeg hệ thống.
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
               "-i", "testsrc2=size=%s:rate=25:duration=%d" % (size, dur), "-vf", "noise=alls=12:allf=t",
               # Không B-frame như camera IP; rtsp_source.py đánh timestamp RTP theo thứ tự giải mã.
               "-c:v", enc, "-preset", "veryfast", "-pix_fmt", "yuv420p", "-bf", "0", "-g", "50", "-keyint_min", "50",
               "-b:v", rate, "-maxrate", rate, "-bufsize", rate, "-an"]
        cmd += ["-x265-params", "log-level=error:keyint=50:min-keyint=50:scenecut=0"] if enc == "libx265" \
            else ["-profile:v", "main", "-sc_threshold", "0"]
        subprocess.run(cmd + extra + [dst, "-y"], check=True)


# ---------------------------------------------------------------- L0: ffmpeg thô


def run_parallel(cmds, env, gpu):
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.time()
    procs = [subprocess.Popen(c, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) for c in cmds]
    errs = [p.communicate()[1].decode(errors="replace") for p in procs]
    t1 = time.time()
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (r1.ru_utime - r0.ru_utime) + (r1.ru_stime - r0.ru_stime)
    ok = all(p.returncode == 0 for p in procs)
    return {"ok": ok, "wall_s": round(t1 - t0, 2), "cpu_s": round(cpu, 2),
            "err": "" if ok else errs[0][-300:], "gpu": gpu.window(int(t0) + 1, int(t1) - 1)}


def cmd_raw(args):
    d = args.dir
    h264, hevc = os.path.join(d, "main_1080p_60s.mp4"), os.path.join(d, "main_1080p_60s_hevc.mp4")
    frames = 1500
    yuv = "/dev/shm/hwbench_1080p.yuv"
    raw_frames = 150
    base = [FFMPEG, "-hide_banner", "-loglevel", "error", "-nostdin"]
    # Như FFmpegDecoder: CPU threads=auto; GPU threads=1, tải frame về RAM rồi đổi sang YUV420P.
    dec = {
        "cpu": lambda src: base + ["-threads", "0", "-i", src, "-vf", "format=yuv420p", "-f", "null", "-"],
        "gpu": lambda src: base + ["-threads", "1", "-hwaccel", "vaapi", "-hwaccel_device", RENDER_NODE,
                                   "-i", src, "-vf", "format=yuv420p", "-f", "null", "-"],
    }
    raw_in = ["-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "25", "-stream_loop", "3", "-i", yuv]
    # Như FFmpegEncoder: libx264 veryfast zerolatency main, không B-frame; GPU h264_vaapi upload NV12.
    enc = {
        "cpu": base + raw_in + ["-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
                                "-profile:v", "main", "-bf", "0", "-b:v", "4M", "-f", "null", "-"],
        "gpu": base + ["-vaapi_device", RENDER_NODE] + raw_in +
               ["-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-bf", "0", "-b:v", "4M", "-f", "null", "-"],
    }
    if not os.path.exists(yuv):
        subprocess.run(base + ["-i", h264, "-frames:v", str(raw_frames), "-f", "rawvideo", "-pix_fmt", "yuv420p",
                               yuv, "-y"], check=True)
    gpu = GpuSampler()
    time.sleep(6)
    idle = time.time()
    time.sleep(8)
    out = {"kind": "raw", "machine": machine(), "idle_gpu": gpu.window(int(idle), int(time.time())), "cases": []}
    for kind, src, nframes in (("decode-h264-1080p", h264, frames), ("decode-hevc-1080p", hevc, frames),
                               ("encode-h264-1080p", None, raw_frames * 4)):
        for mode in ("cpu", "gpu"):
            for k in args.parallel:
                cmd = dec[mode](src) if src else enc[mode]
                log("L0 %s %s x%d" % (kind, mode, k))
                r = run_parallel([cmd] * k, gpu_env(mode), gpu)
                r.update({"case": kind, "mode": mode, "parallel": k,
                          "fps_total": round(nframes * k / r["wall_s"], 1),
                          "cpu_ms_per_frame": round(r["cpu_s"] * 1000 / (nframes * k), 2),
                          "cpu_cores": round(r["cpu_s"] / r["wall_s"], 2)})
                out["cases"].append(r)
                log("   -> %s" % json.dumps({x: r[x] for x in ("ok", "fps_total", "cpu_cores", "gpu")}))
                time.sleep(3)
    gpu.stop()
    os.remove(yuv)
    json.dump(out, open(args.out, "w"), indent=1)


def machine():
    cpu = re.search(r"model name\s*:\s*(.*)", open("/proc/cpuinfo").read())
    mem = int(open("/proc/meminfo").readline().split()[1]) // 1024
    ver = subprocess.run([FFMPEG, "-version"], capture_output=True, text=True).stdout.split("\n")[0]
    vainfo = subprocess.run(["vainfo", "--display", "drm", "--device", RENDER_NODE], capture_output=True, text=True,
                            env=gpu_env("gpu"))
    drv = re.search(r"Driver version: (.*)", vainfo.stdout + vainfo.stderr)
    return {"cpu": cpu.group(1) if cpu else "?", "vcpu": NCPU, "mem_mb": mem, "ffmpeg": ver,
            "va_driver": drv.group(1) if drv else "không có", "kernel": os.uname().release}


# ---------------------------------------------------------------- L1: MediaServer


class Bench:
    def __init__(self, args):
        self.args = args
        self.work = os.path.abspath(args.work)
        self.mode = args.mode
        self.procs = {}
        self.result = {"kind": "live", "mode": args.mode, "machine": machine(), "cameras": args.cameras,
                       "ini": args.ini, "steps": []}

    # ---- dựng
    def layout(self):
        import setup_e2e
        if os.path.exists(self.work):
            raise SystemExit("work dir đã có: %s (xoá hoặc chọn thư mục khác)" % self.work)
        os.environ["E2E_RTSP_MAIN"] = os.environ["E2E_RTSP_SUB"] = "rtsp://placeholder/"
        setup_e2e.layout(REPO, self.work, argparse.Namespace(port_base=self.args.port_base, segment_sec=60,
                                                             restore_ttl=600))
        with open(os.path.join(self.work, "config.ini"), "a") as fh:
            fh.write("\n[hwcodec]\nenable=%d\n" % (1 if self.mode == "gpu" else 0))
            # --ini "hwcodec.maxDecodeSessions=2;motion.intervalMS=200": mỗi khoá một section riêng.
            for kv in filter(None, self.args.ini.split(";")):
                key, _, value = kv.partition("=")
                section, _, name = key.partition(".")
                fh.write("[%s]\n%s=%s\n" % (section, name, value))
        self.inv = json.load(open(os.path.join(self.work, "inventory.json")))
        self.rtsp_port = self.args.port_base + 20
        self.cams = ["cam-%02d" % i for i in range(self.args.cameras)]
        self.state = {"motion": 0, "motion_stream": "SECONDARY", "overlay": False}

    def write_cameras(self):
        import setup_e2e
        env = {"main": "", "sub": ""}
        base_state = dict(setup_e2e.DEFAULT_STATE)
        base_state["overlay"] = self.state["overlay"]
        cfg = setup_e2e.api_config(self.work, env, base_state)
        cfg["media_server"].update({"maxConfigCameras": 256, "maxStream": 1024})
        devices = []
        for i, cam in enumerate(self.cams):
            st = dict(base_state)
            st["motion"] = i < self.state["motion"]
            # Chỉ camera đầu ghi (cho kịch bản extract); camera khác không ghi để tách chi phí ghi đĩa.
            st["enable_recording"] = i == 0
            e = setup_e2e.camera_entry({"main": "rtsp://127.0.0.1:%d/%s-main" % (self.rtsp_port, cam),
                                        "sub": "rtsp://127.0.0.1:%d/%s-sub" % (self.rtsp_port, cam)}, st)
            e["id"], e["name"] = cam, "bench " + cam
            e["motionDetectConfig"]["chooseStream"] = self.state["motion_stream"]
            devices.append(e)
        cfg["devices"] = devices
        path = os.path.join(self.work, "api-config.json")
        json.dump(cfg, open(path, "w"), indent=1)

    def start_sources(self):
        m = self.args.media
        self.spawn("rtsp", [sys.executable, os.path.join(REPO, "tests/integration/stress_vms/rtsp_source.py"),
                            os.path.join(m, "main_1080p.h264"), "--sub-media", os.path.join(m, "sub_360p.h264"),
                            "--port", str(self.rtsp_port), "--fps", "25", "--procs", "2"], cwd=self.work)
        self.spawn("mock", [sys.executable, os.path.join(REPO, "tests/integration/e2e_real/mock_api.py"),
                            self.work, str(self.inv["ports"]["api"])])
        from e2e_check import Ctx
        self.ctx = Ctx(self.work, argparse.Namespace())

    def start_server(self, label):
        """MediaServer mới cho mỗi bậc: đổi motion/overlay giữa chừng không dựng lại luồng camera đang chạy."""
        self.write_cameras()
        # Camera lưu trong SQLite và record/<cam>/info.txt (StatisticRecorder) được nạp trước api-config.json:
        # xoá để cấu hình bậc trước (motion, overlay) không chạy thêm ~20 s đầu bậc sau. Giữ file ghi của camera 0.
        import shutil
        shutil.rmtree(os.path.join(self.work, "db"), ignore_errors=True)
        os.makedirs(os.path.join(self.work, "db"))
        for info in glob.glob(os.path.join(self.work, "record", "*", "info.txt")):
            os.remove(info)
        env = gpu_env(self.mode)
        env["LD_LIBRARY_PATH"] = BIN_DIR
        self.stdout = os.path.join(self.work, "stdout-%s.log" % label)
        # Không -t: số poller mặc định = số lõi, như production.
        self.spawn("server", [os.path.join(BIN_DIR, "MediaServer"), "-c", os.path.join(self.work, "config.ini"),
                              "-s", os.path.join(BIN_DIR, "default.pem"), "-l", "2",
                              "--log-dir", os.path.join(self.work, "log") + "/"], env=env, cwd=self.work,
                   out=self.stdout)
        self.pid = self.procs["server"].pid
        self.t_start = time.time()
        for _ in range(60):
            try:
                if self.ctx.http("GET", "/media/mserver/healthcheck", timeout=2)[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(1)
        else:
            raise SystemExit("MediaServer không lên, xem %s" % self.stdout)
        return self.online()

    def spawn(self, name, cmd, env=None, cwd=None, out=None):
        fh = open(out or os.path.join(self.work, name + ".log"), "w")
        self.procs[name] = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT,
                                            start_new_session=True)

    def alive(self):
        return self.procs["server"].poll() is None

    def stop_server(self):
        p = self.procs.get("server")
        if p and p.poll() is None:
            p.send_signal(signal.SIGINT)
            try:
                p.wait(40)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        crash = self.log_count("## signal:")
        if crash:
            self.result.setdefault("crashes", []).append(os.path.basename(self.stdout))
        if not self.result.get("probe_log"):
            self.result["probe_log"] = [re.sub(r".*\| ", "", l.strip())[:160] for l in open(self.stdout, errors="replace")
                                        if "GPU backend" in l][:6]
        time.sleep(3)
        return crash

    def stop_all(self):
        self.stop_server()
        for p in self.procs.values():
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)

    # ---- trạng thái
    def stat(self):
        try:
            r = self.ctx.index_api("getStatistic")
        except OSError:
            r = None
        d = (r or {}).get("data") or {} if isinstance(r, dict) else {}
        return {k: d.get(k) for k in ("VideoDecoder", "VideoEncoder", "HwVideoDecoder", "HwVideoEncoder",
                                      "HwCodecFallback", "HwCodecBackends", "HwCodecLoad")}

    def wait(self, pred, what, timeout=90):
        t = time.time()
        while time.time() - t < timeout and self.alive():
            s = self.stat()
            if pred(s):
                return s
            time.sleep(2)
        log("  hết hạn chờ %s: %s" % (what, self.stat() if self.alive() else "server đã chết"))
        return None

    def online(self):
        want = {(c, s) for c in self.cams for s in ("main", "sub")}
        have = set()
        for _ in range(90):
            have = {(m["app"], m["stream"]) for m in self.ctx.media_list() if m.get("schema") == "rtsp"}
            if want <= have:
                return True
            time.sleep(2)
        log("  chỉ %d/%d luồng online" % (len(want & have), len(want)))
        return False

    def log_count(self, pattern):
        try:
            return open(self.stdout, errors="replace").read().count(pattern)
        except OSError:
            return 0

    def drops(self):
        return self.log_count("decode thread start drop frame") + self.log_count("encoder thread task is too more")

    def measure(self, sec, viewers=()):
        t0 = time.time()
        a, b = proc_times(self.pid), sys_busy()
        drops0 = self.drops()
        f0 = [v.frames for v in viewers]
        end = t0 + sec
        while time.time() < end and self.alive():
            time.sleep(1)
        t1 = time.time()
        a1, b1 = proc_times(self.pid), sys_busy()
        dt = t1 - t0
        if not a1:
            return {"crashed": True, "window_s": round(dt, 1), "stat": self.stat()}
        res = {"window_s": round(dt, 1),
               "server_cpu_pct": round((a1[0] - a[0]) / dt * 100, 1),
               "server_rss_mb": round(a1[2]),
               "system_cpu_pct": round((b1[0] - b[0]) / max(1, b1[1] - b[1]) * 100, 1),
               "drop_events": self.drops() - drops0,
               "restart_churn": self.log_count("already existed:hls://"),
               "gpu": self.gpu.window(int(t0) + 1, int(t1) - 1), "stat": self.stat()}
        if viewers:
            fps = [(v.frames - f) / dt for v, f in zip(viewers, f0)]
            res["viewer_fps_min"] = round(min(fps), 1)
            res["viewer_fps_avg"] = round(sum(fps) / len(fps), 1)
        return res

    def step(self, scenario, n, viewers=(), extra=None):
        # ~30 s đầu sau khi lên, camera nạp cấu hình rồi khởi động lại luồng (play lỗi "media source already
        # existed: hls" tới khi nguồn HLS cũ hết hạn): không đo trong khoảng đó.
        t = max(time.time() + self.args.settle, self.t_start + self.args.warmup)
        while time.time() < t and self.alive():
            time.sleep(1)
        r = self.measure(self.args.window, viewers) if self.alive() else {"crashed": True, "stat": {}}
        r.update({"scenario": scenario, "n": n})
        r.update(extra or {})
        self.result["steps"].append(r)
        if r.get("crashed"):
            log("  %s n=%d: MediaServer CHẾT trong lúc đo (%s)" % (scenario, n, os.path.basename(self.stdout)))
        else:
            st = r["stat"]
            log("  %s n=%d cpu=%s%% gpu=%s dec=%s/%s enc=%s/%s drop=%d%s" % (
                scenario, n, r["server_cpu_pct"], fmt_gpu(r["gpu"]), st["HwVideoDecoder"], st["VideoDecoder"],
                st["HwVideoEncoder"], st["VideoEncoder"], r["drop_events"],
                " fps=%s/%s" % (r.get("viewer_fps_min"), r.get("viewer_fps_avg")) if viewers else ""))
        self.save()

    def save(self):
        json.dump(self.result, open(self.args.out, "w"), indent=1)

    # ---- kịch bản
    def sc_idle(self):
        self.state.update({"motion": 0, "overlay": False})
        log("idle: %d camera, không giải mã" % len(self.cams))
        self.start_server("idle")
        self.step("idle", 0)
        self.stop_server()

    def sc_motion(self, stream):
        name = "motion-" + ("main" if stream == "PRIMARY" else "sub")
        for n in self.args.motion_steps:
            n = min(n, len(self.cams))
            self.state.update({"motion_stream": stream, "overlay": False, "motion": n})
            log("%s: motion trên %d camera" % (name, n))
            self.start_server("%s-%d" % (name, n))
            self.wait(lambda s: (s["VideoDecoder"] or 0) >= n, "%d decoder" % n)
            self.step(name, n)
            self.stop_server()

    def sc_transcode(self):
        for n in self.args.transcode_steps:
            n = min(n, len(self.cams))
            self.state.update({"motion": 0, "overlay": True})
            log("transcode overlay: %d viewer (mỗi camera một luồng derived)" % n)
            self.start_server("transcode-%d" % n)
            tok = self.ctx.jwt(user="bench-viewer", role=self.inv["roles"]["viewer"], exp_in=7200)
            urls = [self.ctx.url("/media/live/%s/main.live.mp4" % cam, tok) for cam in self.cams[:n]]
            viewers = []
            # Viewer vào trước khi policy overlay của camera nạp xong thì nhận luồng gốc: mở lại tới khi đủ derived.
            for attempt in range(4):
                for v in viewers:
                    v.stop()
                viewers = [Viewer(u) for u in urls]
                if self.wait(lambda s: (s["VideoEncoder"] or 0) >= n, "%d encoder (lần %d)" % (n, attempt + 1),
                             timeout=30) or not self.alive():
                    break
            derived = list({m["stream"] + m["app"]: m for m in self.ctx.media_list()
                            if ".transcode." in m.get("stream", "")}.values()) if self.alive() else []
            tracks = [t for m in derived[:1] for t in m.get("tracks", []) if t.get("codec_type") == 0]
            out = {"derived": len(derived),
                   "output": {k: tracks[0].get(k) for k in ("codec_id_name", "width", "height", "fps")}
                   if tracks else {}}
            self.step("transcode-overlay", n, viewers, out)
            for v in viewers:
                v.stop()
            self.stop_server()

    def sc_extract(self):
        self.state.update({"motion": 0, "overlay": True})
        self.start_server("extract")
        # Job i lấy [now-200-61i, now-140-61i]: cần đủ bản ghi liên tục từ lúc server lên.
        left = self.t_start + 215 + 61 * max(self.args.extract_jobs) - time.time()
        log("extract: chờ %d s cho đủ bản ghi camera 0" % max(0, left))
        time.sleep(max(0, left))
        cam = self.cams[0]
        tok = self.ctx.jwt(user="bench-viewer", role=self.inv["roles"]["viewer"], exp_in=7200)
        now = int(time.time())
        for jobs in self.args.extract_jobs:
            # Mỗi job một khoảng 60 s khác nhau (cùng khoảng thì server gộp cùng key).
            spans = [(now - 200 - 61 * i, now - 140 - 61 * i) for i in range(jobs)]
            watch = ChildWatch(self.pid)
            a = proc_times(self.pid)
            gpu_lines = self.log_count("overlay re-encode on GPU backend")
            t0 = time.time()
            keys, walls = [], []
            for s, e in spans:
                body = {"cameraId": cam, "streamId": "main", "startTime": s, "endTime": e,
                        "filename": "bench_%d_%d.mp4" % (jobs, s), "description": "hwbench"}
                r = self.ctx.api("/media/esc/extractArchived/create", body=body, token=tok)[2]
                key = isinstance(r, dict) and (r.get("data") or {}).get("key")
                if not key:
                    log("  extract create lỗi: %s" % r)
                    watch.total()
                    self.stop_server()
                    return
                keys.append((key, time.time()))
            pending = dict(keys)
            while pending and time.time() - t0 < 600 and self.alive():
                for key in list(pending):
                    r = self.ctx.api("/media/esc/extractArchived/progress", {"key": key}, token=tok)[2]
                    if isinstance(r, dict) and (r.get("data") or {}).get("ready"):
                        walls.append(time.time() - pending.pop(key))
                time.sleep(0.5)
            t1 = time.time()
            a1 = proc_times(self.pid)
            child = watch.total()
            res = {"scenario": "extract-overlay", "n": jobs, "clip_s": 60, "ok": not pending,
                   "job_wall_s": [round(w, 1) for w in walls], "batch_wall_s": round(t1 - t0, 1),
                   "child_cpu_s": child, "server_cpu_s": round(a1[0] - a[0], 2) if a1 else None,
                   "gpu_jobs": self.log_count("overlay re-encode on GPU backend") - gpu_lines,
                   "gpu_retry": self.log_count("retrying with libx264"),
                   "gpu": self.gpu.window(int(t0) + 1, int(t1) - 1), "stat": self.stat()}
            for key, _ in keys:
                st, _, blob = self.ctx.http("GET", "/media/esc/extractArchived/download", {"key": key}, token=tok,
                                            raw=True, timeout=120)
                if st == 200 and "out_probe" not in res:
                    path = os.path.join(self.work, "extract_sample_%s_%d.mp4" % (self.mode, jobs))
                    open(path, "wb").write(blob)
                    res["out_probe"] = ffprobe_v(path)
                self.ctx.api("/media/esc/extractArchived/delete", {"key": key}, token=tok)
            self.result["steps"].append(res)
            log("  extract x%d: wall=%s child_cpu=%ss gpu_jobs=%d gpu=%s ra=%s" % (
                jobs, res["job_wall_s"], child, res["gpu_jobs"], fmt_gpu(res["gpu"]), res.get("out_probe")))
            self.save()
            time.sleep(5)
        self.stop_server()

    def run(self):
        self.layout()
        self.gpu = GpuSampler()
        try:
            self.start_sources()
            sc = self.args.scenarios
            self.sc_idle()
            if "motion-sub" in sc:
                self.sc_motion("SECONDARY")
            if "motion-main" in sc:
                self.sc_motion("PRIMARY")
            if "transcode" in sc:
                self.sc_transcode()
            if "extract" in sc:
                self.sc_extract()
        finally:
            self.stop_all()
            self.gpu.stop()
            self.save()


class Viewer:
    """ffmpeg kéo một luồng HTTP, không giải mã (-c copy), đếm frame nhận được qua -progress."""

    def __init__(self, url):
        self.frames = 0
        self.p = subprocess.Popen([FFMPEG, "-hide_banner", "-loglevel", "error", "-nostats", "-rw_timeout",
                                   "30000000", "-i", url, "-map", "0:v", "-c", "copy", "-f", "null", "-",
                                   "-progress", "pipe:1"],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.p.stdout:
            if line.startswith("frame="):
                try:
                    self.frames = int(line.split("=", 1)[1])
                except ValueError:
                    pass

    def stop(self):
        self.p.terminate()


def ffprobe_v(path):
    out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=codec_name,width,height,avg_frame_rate,bit_rate:format=duration", "-of", "json",
                          path], capture_output=True, text=True)
    try:
        j = json.loads(out.stdout)
        s = j["streams"][0]
        return {"codec": s.get("codec_name"), "size": "%sx%s" % (s.get("width"), s.get("height")),
                "fps": s.get("avg_frame_rate"), "duration": j.get("format", {}).get("duration")}
    except (ValueError, KeyError, IndexError):
        return {}


def cmd_live(args):
    Bench(args).run()


# ---------------------------------------------------------------- transcode chuyển tầng


def cmd_tier(args):
    exe = os.path.join(BIN_DIR, "integration_tier_transcode_ffmpeg")
    out = {"kind": "tier", "mode": args.mode, "machine": machine(), "runs": []}
    gpu = GpuSampler()
    time.sleep(6)
    sources = (("h264", "main_1080p_60s.mp4"), ("hevc", "main_1080p_60s_hevc.mp4"))
    for (codec_name, src), target, profile in itertools.product(sources, args.targets, args.profiles):
        env = gpu_env(args.mode)
        env.update({"TIER_BENCH_SOURCE": os.path.join(args.media, src),
                    "TIER_BENCH_HWACCEL": "1" if args.mode == "gpu" else "0", "TIER_BENCH_CODEC": target,
                    "TIER_BENCH_PROFILE": profile, "TIER_BENCH_REPS": str(args.reps),
                    "TIER_BENCH_FFMPEG": FFMPEG, "TIER_BENCH_FFPROBE": FFPROBE,
                    "LD_LIBRARY_PATH": BIN_DIR})
        log("tier %s nguồn %s -> %s %s" % (args.mode, codec_name, target, profile))
        t0 = time.time()
        proc = subprocess.Popen([exe, "--gtest_filter=*BenchmarkFromEnv*"], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        levels = set()
        threading.Thread(target=watch_levels, args=(proc, levels), daemon=True).start()
        stdout = proc.communicate()[0]
        p = argparse.Namespace(returncode=proc.returncode, stdout=stdout)
        t1 = time.time()
        runs = [json.loads(l.split(" ", 1)[1]) for l in p.stdout.splitlines() if l.startswith("TIER_BENCH ")]
        for r in runs:
            r.update({"source": codec_name, "profile": profile})
        out["runs"].append({"source": codec_name, "target": target, "profile": profile, "ok": p.returncode == 0,
                            "runs": runs, "levels": sorted(set(levels)), "gpu": gpu.window(int(t0) + 2, int(t1) - 1),
                            "tail": "" if p.returncode == 0 else p.stdout[-600:]})
        log("   -> %s" % [(r["encoder"], round(r["wall_s"], 1), round(r["child_cpu_s"], 1)) for r in runs])
        json.dump(out, open(args.out, "w"), indent=1)
        time.sleep(3)
    gpu.stop()


def watch_levels(proc, levels):
    """Mức L1/L2/L3 của transformer, đọc từ argv của tiến trình ffmpeg con (transformer không log khi đạt)."""
    while proc.poll() is None:
        for path in glob.glob("/proc/[0-9]*/cmdline"):
            try:
                argv = open(path, "rb").read().split(b"\0")
            except OSError:
                continue
            if not argv or not argv[0].endswith(b"/ffmpeg") or b"-progress" in argv:
                continue
            if b"-i" not in argv or b"bench.mp4" not in b" ".join(argv) and b"main_1080p_60s" not in b" ".join(argv):
                continue
            joined = b" ".join(argv)
            levels.add("L1" if b"-hwaccel" in argv else "L2" if b"hwupload" in joined or b"_nvenc" in joined
                       or b"_qsv" in joined else "L3")
        time.sleep(0.3)


# ---------------------------------------------------------------- báo cáo


def cmd_report(args):
    for path in args.files:
        j = json.load(open(path))
        print("\n## %s (%s)\n" % (os.path.basename(path), j.get("mode", "")))
        if j["kind"] == "raw":
            print("| Ca | Chế độ | Song song | fps tổng | lõi CPU | ms CPU/frame | GPU |")
            print("|---|---|---|---|---|---|---|")
            for c in j["cases"]:
                print("| %s | %s | %d | %s | %s | %s | %s |" % (c["case"], c["mode"], c["parallel"], c["fps_total"],
                                                              c["cpu_cores"], c["cpu_ms_per_frame"],
                                                              fmt_gpu(c["gpu"]) if c["ok"] else "LỖI " + c["err"][:80]))
        elif j["kind"] == "live":
            print("| Kịch bản | N | CPU server % | RSS MB | GPU | HW dec/dec | HW enc/enc | drop | fps viewer |")
            print("|---|---|---|---|---|---|---|---|---|")
            for s in j["steps"]:
                if s["scenario"] == "extract-overlay":
                    continue
                if s.get("crashed"):
                    print("| %s | %d | CRASH | | | | | | |" % (s["scenario"], s["n"]))
                    continue
                st = s["stat"]
                print("| %s | %d | %s | %s | %s | %s/%s | %s/%s | %s | %s |" % (
                    s["scenario"], s["n"], s["server_cpu_pct"], s["server_rss_mb"], fmt_gpu(s["gpu"]),
                    st["HwVideoDecoder"], st["VideoDecoder"], st["HwVideoEncoder"], st["VideoEncoder"],
                    s["drop_events"], "%s/%s" % (s.get("viewer_fps_min"), s.get("viewer_fps_avg"))
                    if "viewer_fps_min" in s else "-"))
            for s in j["steps"]:
                if s["scenario"] == "extract-overlay":
                    print("\nextract x%d: wall %s, CPU ffmpeg %ss, job GPU %d, GPU %s, ra %s" % (
                        s["n"], s["job_wall_s"], s["child_cpu_s"], s["gpu_jobs"], fmt_gpu(s["gpu"]),
                        s.get("out_probe")))
        elif j["kind"] == "tier":
            print("| Nguồn | Đích | Profile | Mức | encoder | wall s | CPU s | GPU |")
            print("|---|---|---|---|---|---|---|---|")
            for g in j["runs"]:
                for r in g["runs"]:
                    print("| %s | %s | %s | %s | %s | %.1f | %.1f | %s |" % (
                        g["source"], g.get("target", "H264"), g["profile"], ",".join(g["levels"]), r["encoder"],
                        r["wall_s"], r["child_cpu_s"], fmt_gpu(g["gpu"])))


def fmt_gpu(g):
    return " ".join("%s=%s" % (k, v) for k, v in sorted(g.items()) if v) or "0"


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("media")
    p.add_argument("dir")
    p = sp.add_parser("raw")
    p.add_argument("dir")
    p.add_argument("--out", required=True)
    p.add_argument("--parallel", type=lambda s: [int(x) for x in s.split(",")], default=[1, 4])
    p = sp.add_parser("live")
    p.add_argument("work")
    p.add_argument("--mode", choices=("cpu", "gpu"), required=True)
    p.add_argument("--media", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--cameras", type=int, default=16)
    p.add_argument("--port-base", type=int, default=19480)
    p.add_argument("--scenarios", type=lambda s: s.split(","), default=["motion-sub", "motion-main", "transcode",
                                                                         "extract"])
    p.add_argument("--motion-steps", type=lambda s: [int(x) for x in s.split(",")], default=[1, 4, 8, 16])
    p.add_argument("--transcode-steps", type=lambda s: [int(x) for x in s.split(",")], default=[1, 2, 4, 8])
    p.add_argument("--extract-jobs", type=lambda s: [int(x) for x in s.split(",")], default=[1, 3])
    p.add_argument("--ini", default="", help='ghi đè config.ini, vd "hwcodec.maxDecodeSessions=2"')
    p.add_argument("--settle", type=int, default=15)
    p.add_argument("--warmup", type=int, default=45, help="không đo trước giây này kể từ lúc server lên")
    p.add_argument("--window", type=int, default=40)
    p = sp.add_parser("tier")
    p.add_argument("--mode", choices=("cpu", "gpu"), required=True)
    p.add_argument("--media", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--reps", type=int, default=3)
    p.add_argument("--profiles", type=lambda s: s.split(","), default=["1280x720:15:1500", "640x360:10:500"])
    p.add_argument("--targets", type=lambda s: s.split(","), default=["H264", "H265"])
    p = sp.add_parser("report")
    p.add_argument("files", nargs="+")
    args = ap.parse_args()
    {"media": cmd_media, "raw": cmd_raw, "live": cmd_live, "tier": cmd_tier, "report": cmd_report}[args.cmd](args)


if __name__ == "__main__":
    main()
