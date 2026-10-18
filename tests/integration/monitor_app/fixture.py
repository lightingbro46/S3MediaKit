#!/usr/bin/env python3
"""MediaServer nodes for the integration tests of the monitor desktop app (3spro-monitor).

Usage: fixture.py up|down|status [--work DIR] [--only monitor|develop]

  up      stop what runs from DIR, build the node directories, start mock_api.py (service API),
          rtsp_source.py (recording camera) and every node, wait for the healthchecks, then write
          DIR/fixture.json: the contract the app side reads (nodes, URLs, secrets, scenarios).
  down    stop everything started from DIR.
  status  print DIR/fixture.json with the live state of each process.

Each node plays one scenario (see NODES). Two builds take part:
  monitor  this checkout's Debug build (MONITOR_BIN_DIR, default <repo>/release/linux/Debug)
  develop  a build without /media/monitor/* (DEVELOP_BIN_DIR, default the worktree made by
           build_target.sh develop: <repo>/../S3MediaKit.worktrees/it-develop/release/linux/Debug)
Node directories use the conf/ and migration/ of the source tree their binary was built from.

The app calls the nodes through the WSL address (`url`): MediaServer lets loopback clients through
without the secret, so wrong/missing secret checks need a non-loopback client, and Windows does
not reach these ports on its own 127.0.0.1 anyway.
Ports: HTTP 18181-18186, RTSP 18581+, RTMP 18381+, service API 18190, RTSP source 18194.
Python 3, standard library only; ffmpeg for the test clip.
"""
import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "../../.."))
SYNC = os.path.join(REPO, "tests/integration/sync_cluster")
STRESS = os.path.join(REPO, "tests/integration/stress_vms")
API_PORT = 18190
RTSP_SOURCE_PORT = 18194
# api.secret shipped in conf/config.ini: the node keeping it reports apiSecretIsDefault.
DEFAULT_SECRET = "035c73f7-bb6b-4889-a715-d9eb2d1925cc"

BIN_DIRS = {
    "monitor": os.environ.get("MONITOR_BIN_DIR", os.path.join(REPO, "release/linux/Debug")),
    "develop": os.environ.get("DEVELOP_BIN_DIR", os.path.join(
        os.path.dirname(REPO), "S3MediaKit.worktrees/it-develop/release/linux/Debug")),
}

# `group` is cluster.serverLocationId = VMS serverGroupId; `failover` the node takes failover cameras.
# `cameras`: "rec" records from rtsp_source.py, "dead" points at a closed port (offline, lastError).
# `ini`: extra config.ini values. `expect`: values of /media/monitor/summary data the scenario
# reaches once settled; the app side waits for them before capturing. Keys are dotted paths
# (`a[].b` collects b of every item of a); a number means "at least", a value against a list
# means "contains", anything else "equals".
NODES = [
    {"id": "it-mon-ok", "build": "monitor", "scenario": "ok", "http": 18181, "group": 101,
     "failover": True, "cameras": ["rec"],
     "expect": {"overall": "ok", "ingest.online": 1, "recording.active": 1,
                # first coverage pass done (coverageIntervalSec): /cameras has gaps_24h, longest_gap_sec
                "recording.coverage.computed_at": 1}},
    {"id": "it-mon-peer", "build": "monitor", "scenario": "peer", "http": 18182, "group": 101,
     "failover": False, "cameras": [],
     "expect": {"cluster.peers_known": 1, "cluster.sync.peers[].id": "it-mon-ok"}},
    {"id": "it-mon-warn", "build": "monitor", "scenario": "warning", "http": 18183, "group": 102,
     "failover": True, "cameras": ["dead"],
     "ini": {"globalmonitor": {"memory_warning_threshold": 1}},
     "expect": {"reasons[].metric": "ram.usage_pct", "components.ingest": "critical"}},
    # MediaServer replaces the default api.secret at start (main.cpp), so the node starts with a
    # random one and gets the default back through setServerConfig, as an operator could do.
    {"id": "it-mon-default", "build": "monitor", "scenario": "default_secret", "http": 18184,
     "group": 0, "failover": True, "cameras": [], "runtimeSecret": DEFAULT_SECRET,
     "expect": {"apiSecretIsDefault": True, "components.host": "ok", "components.cluster": "ok"}},
    {"id": "it-mon-off", "build": "monitor", "scenario": "disabled", "http": 18185, "group": 0,
     "failover": False, "cameras": [], "ini": {"monitor": {"enable": 0}},
     "expect": {"enabled": False}},
    {"id": "it-dev", "build": "develop", "scenario": "old_build", "http": 18186, "group": 103,
     "failover": True, "cameras": ["rec"], "expect": {}},
]

# Every node shares the WSL host: swap and CPU depend on what else runs there (a build, the other
# nodes), so their thresholds are out of reach and only a scenario's own setting raises host.
# Coverage every 60 s (minimum) instead of 900 s, so /cameras has it within the settle time.
COMMON_INI = {
    "monitor": {"swapUsedPctWarn": 95, "swapUsedPctCrit": 99, "coverageIntervalSec": 60},
    "globalmonitor": {"cpu_warning_threshold": 98, "cpu_critical_threshold": 99},
}


def ports(node):
    i = node["http"] - 18181
    return {"http": node["http"], "rtsp": 18581 + i, "rtmp": 18381 + i}


def patch_ini(src, dst, overrides):
    """Apply {section: {key: value}} to an ini file, keeping comments and order (as setup_nodes.py)."""
    lines = open(src, encoding="utf-8").read().splitlines()
    out, section, seen = [], None, set()

    def flush(sec):
        for k, v in overrides.get(sec, {}).items():
            if (sec, k) not in seen:
                out.append("%s=%s" % (k, v))
                seen.add((sec, k))

    for line in lines:
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            flush(section)
            section = s[1:-1]
            out.append(line)
            continue
        if section and "=" in s and not s.startswith("#"):
            key = s.split("=", 1)[0].strip()
            if key in overrides.get(section, {}):
                out.append("%s=%s" % (key, overrides[section][key]))
                seen.add((section, key))
                continue
        out.append(line)
    flush(section)
    for sec, kv in overrides.items():
        missing = [(k, v) for k, v in kv.items() if (sec, k) not in seen]
        if missing:
            out.append("[%s]" % sec)
            out.extend("%s=%s" % (k, v) for k, v in missing)
    open(dst, "w", encoding="utf-8").write("\n".join(out) + "\n")


def source_tree(build):
    """Checkout the binary of `build` was built from (conf/, migration/, BUILD_REF)."""
    return os.path.abspath(os.path.join(BIN_DIRS[build], "../../.."))


def build_ref(build):
    """{ref, commit, dirty} of the source tree of a build."""
    tree = source_tree(build)
    ref_file = os.path.join(BIN_DIRS[build], "BUILD_REF")
    if os.path.exists(ref_file):
        commit, _, ref = open(ref_file).read().strip().partition(" ")
    else:
        git = lambda *a: subprocess.run(["git", "-C", tree, *a], capture_output=True, text=True).stdout.strip()
        commit, ref = git("rev-parse", "HEAD"), git("rev-parse", "--abbrev-ref", "HEAD")
    dirty = subprocess.run(["git", "-C", tree, "status", "--porcelain", "--untracked-files=no", "--ignore-submodules"],
                           capture_output=True, text=True).stdout.strip() != ""
    binary = os.path.join(BIN_DIRS[build], "MediaServer")
    return {"ref": ref, "commit": commit, "dirty": dirty,
            "binaryTime": int(os.path.getmtime(binary)) if os.path.exists(binary) else None}


def camera(node, kind):
    cam_id = "%s-%s" % (node["id"], kind)
    url = ("rtsp://127.0.0.1:%d/%s-main" % (RTSP_SOURCE_PORT, cam_id) if kind == "rec"
           else "rtsp://127.0.0.1:9/%s-main" % cam_id)
    return {
        "id": cam_id, "name": cam_id, "ip": "127.0.0.1", "httpPort": 80,
        "manufacturer": "Generic", "model": "RTSP", "username": "", "password": "",
        "enabled": True, "priMediaServerId": node["id"],
        "primaryStreamId": cam_id + "-main", "primaryStreamUrl": url,
        "recordingConfig": {
            "enableRecording": kind == "rec",
            "recordingSchedule": [{"dh": "%d,%d" % (d, h), "ty": 3, "fps": 25, "q": "H"}
                                  for d in range(7) for h in range(24)],
            "keepArchivedMinForAuto": False, "keepArchivedMinFor": 0,
            "keepArchivedMaxForAuto": False, "keepArchivedMaxFor": 24,
        },
    }


def server_entry(node):
    p = ports(node)
    return {
        "id": node["id"], "name": node["id"], "domain": "", "ip": "127.0.0.1",
        "httpPort": p["http"], "httpsPort": 0, "rtspPort": p["rtsp"], "rtmpPort": p["rtmp"],
        "isHttpPortAuto": True, "isHttpsPortAuto": True, "isRtspPortAuto": True, "isRtmpPortAuto": True,
        "httpPortNat": p["http"], "httpsPortNat": 0, "rtspPortNat": p["rtsp"], "rtmpPortNat": p["rtmp"],
        "clientUseSsl": False, "hasFailover": node["failover"], "serverGroupId": node["group"],
    }


def api_config(node, nodes):
    """What the service API (VMS) tells the node: its group's members and its cameras."""
    members = [n for n in nodes if n["group"] == node["group"]]
    return {
        "media_server": {"serverGroupId": node["group"], "maxConfigCameras": 16, "maxStream": 64,
                         "maxStreamPerCamera": 8, "projectId": "monitor-app-it", "failover": node["failover"],
                         "unlimitedStream": True, "unlimitedStreamPerCamera": True},
        "list_media_server": [server_entry(n) for n in members],
        "devices": [camera(node, k) for k in node["cameras"]],
        "speakers": [],
    }


def setup(work, nodes):
    for node in nodes:
        d = os.path.join(work, node["id"])
        tree = source_tree(node["build"])
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "www"))
        shutil.copytree(os.path.join(tree, "migration/updates"), os.path.join(d, "updates"))
        shutil.copytree(os.path.join(tree, "migration/mserver_updates"), os.path.join(d, "mserver_updates"))
        p = ports(node)
        node.setdefault("secret", str(uuid.uuid4()))
        overrides = {
            "general": {"mediaServerId": node["id"]},
            "api": {"secret": node["secret"]},
            "http": {"port": p["http"], "sslport": 0, "rootPath": d + "/www", "allow_cross_domains": 1},
            "rtsp": {"port": p["rtsp"], "sslport": 0},
            "rtmp": {"port": p["rtmp"], "sslport": 0},
            "rtp_proxy": {"port": 0},
            "rtc": {"port": 0, "tcpPort": 0, "signalingPort": 0, "signalingSslPort": 0,
                    "icePort": 0, "iceTcpPort": 0},
            "srt": {"port": 0},
            "shell": {"port": 0},
            "ssdp": {"port": 0},
            "protocol": {"mp4_save_path": d + "/www", "hls_save_path": d + "/www/live"},
            "hook": {"enable": 1, "api_url": "http://127.0.0.1:%d" % API_PORT,
                     "report_interval": 10.0, "alive_interval": 5.0, "timeoutSec": 3, "retry": 0,
                     "configReapplySec": 0},
            "database": {"db_save_path": d + "/db", "updates_save_path": d + "/updates",
                         "mserver_updates_save_path": d + "/mserver_updates",
                         "keep_migration_sql_files": 1, "enable_sync_db": 1,
                         "pull_interval_sec": 5, "gossip_fanout": 3},
            "manager": {"apiSecret": "", "certSavePath": d + "/www/certs"},
            # enableFailover as VMS pushes it: a config reload (setServerConfig) goes back to the ini
            # value until the next push, which would flip the node's flag for a while.
            "cluster": {"serverLocationId": node["group"], "enableFailover": int(node["failover"])},
            "auth": {"enableAuthorize": 0},
        }
        for extra in (COMMON_INI, node.get("ini", {})):
            for sec, kv in extra.items():
                overrides.setdefault(sec, {}).update(kv)
        patch_ini(os.path.join(tree, "conf/config.ini"), os.path.join(d, "config.ini"), overrides)
        json.dump(api_config(node, nodes), open(os.path.join(d, "api-config.json"), "w"), indent=2)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


def read_pid(path):
    try:
        return int(open(path).read().strip())
    except (OSError, ValueError):
        return None


def spawn(cmd, cwd, log, pidfile, env=None):
    out = open(log, "ab")
    p = subprocess.Popen(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, env=env,
                         start_new_session=True)
    open(pidfile, "w").write(str(p.pid))
    return p.pid


def stop(work):
    """SIGINT every MediaServer (it sleeps ~10 s on shutdown), then SIGKILL after 30 s; kill helpers."""
    pids = []
    for name in sorted(os.listdir(work)) if os.path.isdir(work) else []:
        pidfile = os.path.join(work, name, "server.pid")
        pid = read_pid(pidfile)
        if alive(pid):
            os.kill(pid, signal.SIGINT)
            pids.append((pid, pidfile))
        elif os.path.exists(pidfile):
            os.remove(pidfile)
    deadline = time.time() + 30
    while any(alive(p) for p, _ in pids) and time.time() < deadline:
        time.sleep(1)
    for pid, pidfile in pids:
        if alive(pid):
            print("WARN: %s did not stop on SIGINT, killing" % os.path.dirname(pidfile))
            os.kill(pid, signal.SIGKILL)
        os.remove(pidfile)
    for helper in ("mock_api", "rtsp_source"):
        pidfile = os.path.join(work, helper + ".pid")
        pid = read_pid(pidfile)
        if alive(pid):
            os.kill(pid, signal.SIGTERM)
        if os.path.exists(pidfile):
            os.remove(pidfile)


def port_busy(port):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


def wsl_address():
    """Address of eth0, the one Windows reaches WSL on (NAT mode)."""
    out = subprocess.run(["hostname", "-I"], capture_output=True, text=True).stdout.split()
    return out[0] if out else "127.0.0.1"


def healthy(port):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/media/mserver/healthcheck" % port, timeout=2):
            return True
    except Exception:
        return False


def set_secret(node, secret):
    """Change api.secret of a running node (setServerConfig, saved to its config.ini)."""
    req = urllib.request.Request("http://127.0.0.1:%d/index/api/setServerConfig" % node["http"],
                                 data=json.dumps({"api.secret": secret}).encode(), method="POST",
                                 headers={"secret": node["secret"], "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = json.loads(r.read().decode())
    if body.get("code") != 0:
        raise RuntimeError("%s: setServerConfig answered %s" % (node["id"], body))
    node["secret"] = secret


def up(work, only):
    nodes = [dict(n) for n in NODES if only is None or n["build"] == only]
    for build in sorted({n["build"] for n in nodes}):
        if not os.path.exists(os.path.join(BIN_DIRS[build], "MediaServer")):
            sys.exit("MediaServer of build '%s' not found in %s%s" % (
                build, BIN_DIRS[build], " (run build_target.sh develop)" if build == "develop" else ""))
    stop(work)
    os.makedirs(work, exist_ok=True)
    busy = [p for n in nodes for p in ports(n).values()] + [API_PORT, RTSP_SOURCE_PORT]
    busy = [p for p in busy if port_busy(p)]
    if busy:
        sys.exit("ports already in use: %s" % ", ".join(map(str, busy)))
    setup(work, nodes)

    spawn([sys.executable, os.path.join(SYNC, "mock_api.py"), str(API_PORT), os.path.join(work, "not_qualified.txt")],
          work, os.path.join(work, "mock_api.log"), os.path.join(work, "mock_api.pid"))
    clip = os.path.join(work, "source.h264")
    if not os.path.exists(clip):
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", "testsrc2=size=640x360:rate=25:duration=10", "-c:v", "libx264",
                        "-preset", "veryfast", "-pix_fmt", "yuv420p", "-g", "50", "-keyint_min", "50",
                        "-sc_threshold", "0", "-b:v", "400k", "-bsf:v", "h264_mp4toannexb",
                        "-f", "h264", clip, "-y"], check=True)
    # drop-spec: P2/P3 scenarios take a recording camera offline by listing its stream there.
    drop = os.path.join(work, "drop_spec.json")
    json.dump({"offline": []}, open(drop, "w"))
    spawn([sys.executable, os.path.join(STRESS, "rtsp_source.py"), clip, "--port", str(RTSP_SOURCE_PORT),
           "--fps", "25", "--stats-sec", "10", "--drop-spec", drop, "--state", os.path.join(work, "rtsp_state.json")],
          work, os.path.join(work, "rtsp_source.log"), os.path.join(work, "rtsp_source.pid"))

    for node in nodes:
        d = os.path.join(work, node["id"])
        bin_dir = BIN_DIRS[node["build"]]
        env = dict(os.environ, LD_LIBRARY_PATH=bin_dir)
        spawn([os.path.join(bin_dir, "MediaServer"), "-c", os.path.join(d, "config.ini"),
               "-s", os.path.join(bin_dir, "default.pem"), "-l", "0", "-t", "4", "--affinity", "0",
               "--log-dir", os.path.join(d, "log") + "/"],
              d, os.path.join(d, "stdout.log"), os.path.join(d, "server.pid"), env)

    deadline = time.time() + 90
    pending = list(nodes)
    while pending and time.time() < deadline:
        pending = [n for n in pending if not healthy(n["http"])]
        time.sleep(1)
    for n in pending:
        print("WARN: %s is not healthy after 90 s (see %s/stdout.log)" % (n["id"], os.path.join(work, n["id"])))
    for n in nodes:
        if n.get("runtimeSecret") and n not in pending:
            set_secret(n, n["runtimeSecret"])

    host = wsl_address()
    builds = {b: build_ref(b) for b in sorted({n["build"] for n in nodes})}
    fixture = {
        "schema": 1,
        "createdAt": int(time.time()),
        "host": host,
        "workDir": work,
        "builds": builds,
        "nodes": [{
            "id": n["id"], "scenario": n["scenario"], "build": n["build"],
            "url": "http://%s:%d" % (host, n["http"]),
            "secret": n["secret"], "group": n["group"], "failover": n["failover"],
            "cameras": ["%s-%s" % (n["id"], k) for k in n["cameras"]],
            "monitorApi": n["build"] != "develop", "expect": n["expect"],
        } for n in nodes],
    }
    path = os.path.join(work, "fixture.json")
    json.dump(fixture, open(path + ".tmp", "w"), indent=2)
    os.replace(path + ".tmp", path)
    print("fixture up: %d node(s), %s" % (len(nodes) - len(pending), path))
    return 1 if pending else 0


def status(work):
    path = os.path.join(work, "fixture.json")
    if not os.path.exists(path):
        sys.exit("no fixture in %s (run: fixture.py up)" % work)
    fixture = json.load(open(path))
    for n in fixture["nodes"]:
        n["running"] = alive(read_pid(os.path.join(work, n["id"], "server.pid")))
    for helper in ("mock_api", "rtsp_source"):
        fixture[helper + "Running"] = alive(read_pid(os.path.join(work, helper + ".pid")))
    print(json.dumps(fixture, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["up", "down", "status"])
    ap.add_argument("--work", default="/tmp/s3mk_monitor_app")
    ap.add_argument("--only", choices=["monitor", "develop"])
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    if a.action == "up":
        sys.exit(up(work, a.only))
    if a.action == "down":
        stop(work)
        print("fixture down: %s" % work)
    else:
        status(work)


if __name__ == "__main__":
    main()
