"""P2: viewer bắt buộc overlay kết nối ở T+3·i (camera i) sau khi server lên, camera có overlay ngay từ đầu.

python3 p2_overlay_startup.py <work> <media> <out.json> [--cameras 16] [--step 3]
"""
import argparse, json, os, subprocess, sys, threading, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tests/performance/hwcodec_bench"))
import hwbench  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("work"); ap.add_argument("media"); ap.add_argument("out")
ap.add_argument("--cameras", type=int, default=16)
ap.add_argument("--step", type=float, default=3.0)
ap.add_argument("--ini", default="manager.mediaServerProjectId=e2e-project")
a = ap.parse_args()
args = argparse.Namespace(work=a.work, mode="cpu", media=a.media, out=a.out, cameras=a.cameras, port_base=19480,
                          scenarios=[], motion_steps=[], transcode_steps=[], extract_jobs=[], ini=a.ini, settle=0,
                          warmup=0, window=0)
b = hwbench.Bench(args)
b.layout()
b.online = lambda: True          # không chờ luồng online: đo đúng mốc thời gian sau khi server lên
results = []
lock = threading.Lock()


def probe(i, cam, url):
    t0 = time.time() - b.t_start
    body = os.path.join(a.work, "body-%s.bin" % cam)
    p = subprocess.run(["curl", "-s", "-o", body, "--max-time", "20", "-w", "%{http_code} %{size_download}",
                        url], capture_output=True, text=True)
    code, size = (p.stdout.split() + ["0", "0"])[:2]
    fps = ""
    if code == "200" and os.path.exists(body):
        q = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries",
                            "stream=width,height,r_frame_rate", "-of", "csv=p=0", body], capture_output=True, text=True)
        fps = q.stdout.strip().splitlines()[0] if q.stdout.strip() else "?"
    elif os.path.exists(body):
        fps = open(body, "rb").read()[:60].decode("utf-8", "replace")
    with lock:
        results.append({"cam": cam, "t": round(t0, 1), "http": int(code or 0), "bytes": int(size or 0),
                        "derived": derived.get(cam, False), "video": fps})


derived = {}


def watch_derived(stop):
    while not stop.is_set():
        try:
            for m in b.ctx.media_list():
                if ".transcode." in m.get("stream", ""):
                    derived[m["app"]] = True
        except OSError:
            pass
        time.sleep(0.5)


try:
    b.start_sources()
    b.state.update({"motion": 0, "overlay": True})
    b.start_server("p2")
    tok = b.ctx.jwt(user="bench-viewer", role=b.inv["roles"]["viewer"], exp_in=7200)
    stop = threading.Event()
    threading.Thread(target=watch_derived, args=(stop,), daemon=True).start()
    threads = []
    for i, cam in enumerate(b.cams):
        due = b.t_start + a.step * (i + 1)
        time.sleep(max(0, due - time.time()))
        th = threading.Thread(target=probe, args=(i, cam, b.ctx.url("/media/live/%s/main.live.mp4" % cam, tok)))
        th.start(); threads.append(th)
    for th in threads:
        th.join()
    time.sleep(2)
    stop.set()
    # derived có thể xuất hiện sau khi curl đã đóng: đọc lại lần cuối
    for r in results:
        r["derived"] = r["derived"] or derived.get(r["cam"], False)
    b.result["p2"] = sorted(results, key=lambda r: r["t"])
    b.result["log"] = b.stdout
finally:
    b.stop_all()
    b.gpu = type("G", (), {"stop": lambda self: None})()
    json.dump(b.result, open(a.out, "w"), indent=1)
for r in sorted(results, key=lambda r: r["t"]):
    print("%-7s T+%5.1fs http=%d bytes=%-8d derived=%s video=%s" % (r["cam"], r["t"], r["http"], r["bytes"], r["derived"], r["video"]))
