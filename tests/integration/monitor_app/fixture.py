#!/usr/bin/env python3
"""MediaServer nodes for the integration tests of the monitor desktop app (3spro-monitor).

Usage: fixture.py up|down|status [--work DIR] [--only monitor|develop]
       fixture.py ctl COMMAND ... [--work DIR]

  up      stop what runs from DIR, build the node directories, start mock_api.py (service API),
          rtsp_source.py (recording camera), fake_s3.py (object storage pool) and every node, wait
          for the healthchecks, then write DIR/fixture.json: the contract the app side reads
          (nodes, URLs, secrets, scenarios).
  down    stop everything started from DIR.
  status  print DIR/fixture.json with the live state of each process.
  ctl     change the running fixture, for the app's dynamic scenarios (faults while it watches):
            stop ID | start ID           stop (SIGINT, as an operator would) or start one node
            camera CAMERA offline|online the RTSP source drops or serves that camera's stream
            pool down|up                 stop or start fake_s3.py behind the pool of a `pool` node
            vms ID [--group N] [--failover 0|1] [--bulk N]
                                         what the VMS pushes to the node from now on: its group, its
                                         failover flag, N extra cameras on a closed port
            secret ID VALUE|initial      change api.secret of a running node (setServerConfig)
            reset                        undo all of the above
          fixture.json follows every change (secret, group, failover, bulk).

Each node plays one scenario (see NODES). Two builds take part:
  monitor  this checkout's Debug build (MONITOR_BIN_DIR, default <repo>/release/linux/Debug)
  develop  a build without /media/monitor/* (DEVELOP_BIN_DIR, default the worktree made by
           build_target.sh develop: <repo>/../S3MediaKit.worktrees/it-develop/release/linux/Debug)
Node directories use the conf/ and migration/ of the source tree their binary was built from.

The app calls the nodes through the WSL address (`url`): MediaServer lets loopback clients through
without the secret, so wrong/missing secret checks need a non-loopback client, and Windows does
not reach these ports on its own 127.0.0.1 anyway.
Ports: HTTP 18181-18189, RTSP 18581+, RTMP 18381+, service API 18190, RTSP source 18194, fake S3 18195.
Python 3, standard library only; ffmpeg for the test clip. The clock_skew node runs under
libfaketime: the system one, FAKETIME_LIB, or the Ubuntu package unpacked into
~/.cache/s3mk_monitor_app (apt-get download, nothing installed). Without it that node is left out.
"""
import argparse
import fcntl
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
FAKE_S3_PORT = 18195
# api.secret shipped in conf/config.ini: the node keeping it reports apiSecretIsDefault.
DEFAULT_SECRET = "035c73f7-bb6b-4889-a715-d9eb2d1925cc"
FAKETIME_CACHE = os.path.expanduser("~/.cache/s3mk_monitor_app")
FAKETIME_SO = "usr/lib/x86_64-linux-gnu/faketime/libfaketimeMT.so.1"

BIN_DIRS = {
    "monitor": os.environ.get("MONITOR_BIN_DIR", os.path.join(REPO, "release/linux/Debug")),
    "develop": os.environ.get("DEVELOP_BIN_DIR", os.path.join(
        os.path.dirname(REPO), "S3MediaKit.worktrees/it-develop/release/linux/Debug")),
}

# `group` is cluster.serverLocationId = VMS serverGroupId; `failover` the node takes failover cameras.
# `cameras`: "rec" records from rtsp_source.py, "dead" points at a closed port (offline, lastError).
# `ini`: extra config.ini values. `pool`: a MINIO pool on fake_s3.py is created once the node is up.
# `clockOffset`: the node runs under libfaketime with FAKETIME set to this.
# `expect`: values of /media/monitor/summary data the scenario reaches once settled; the app side
# waits for them before capturing. Keys are dotted paths (`a[].b` collects b of every item of a);
# a number means "at least", a value against a list means "contains", anything else "equals".
NODES = [
    {"id": "it-mon-ok", "build": "monitor", "scenario": "ok", "http": 18181, "group": 101,
     "failover": True, "cameras": ["rec"],
     "expect": {"overall": "ok", "ingest.online": 1, "recording.active": 1, "cluster.sync.bootstrapped": True,
                # first coverage pass done (coverageIntervalSec): /cameras has gaps_24h, longest_gap_sec
                "recording.coverage.computed_at": 1}},
    {"id": "it-mon-peer", "build": "monitor", "scenario": "peer", "http": 18182, "group": 101,
     "failover": False, "cameras": [],
     "expect": {"cluster.peers_known": 1, "cluster.sync.peers[].id": "it-mon-ok", "cluster.sync.bootstrapped": True}},
    {"id": "it-mon-warn", "build": "monitor", "scenario": "warning", "http": 18183, "group": 102,
     "failover": True, "cameras": ["dead"],
     "ini": {"globalmonitor": {"memory_warning_threshold": 1}},
     "expect": {"reasons[].metric": "ram.usage_pct", "components.ingest": "critical",
                "cluster.sync.single_node": True}},
    # MediaServer replaces the default api.secret at start (main.cpp), so the node starts with a
    # random one and gets the default back through setServerConfig, as an operator could do.
    {"id": "it-mon-default", "build": "monitor", "scenario": "default_secret", "http": 18184,
     "group": 0, "failover": True, "cameras": [], "runtimeSecret": DEFAULT_SECRET,
     "expect": {"apiSecretIsDefault": True, "components.host": "ok", "components.cluster": "ok",
                "cluster.sync.bootstrapped": True}},
    {"id": "it-mon-off", "build": "monitor", "scenario": "disabled", "http": 18185, "group": 0,
     "failover": False, "cameras": [], "ini": {"monitor": {"enable": 0}},
     "expect": {"enabled": False}},
    {"id": "it-dev", "build": "develop", "scenario": "old_build", "http": 18186, "group": 103,
     "failover": True, "cameras": ["rec"], "expect": {}},
    # One cluster, two builds: an upgrade half done. The app notes the build mismatch.
    # The monitor node also has an object storage pool (database checks every 10 s, not 60 s).
    {"id": "it-mix-mon", "build": "monitor", "scenario": "mixed", "http": 18187, "group": 104,
     "failover": True, "cameras": [], "pool": True, "ini": {"monitor": {"slowIntervalSec": 10}},
     "expect": {"cluster.peers_known": 1, "cluster.sync.bootstrapped": True,
                "storage.tier.pools[].health_status": "OK"}},
    {"id": "it-mix-dev", "build": "develop", "scenario": "mixed", "http": 18188, "group": 104,
     "failover": True, "cameras": [], "expect": {}},
    # Server clock 10 minutes ahead of the app's machine (NTP missing).
    {"id": "it-mon-clock", "build": "monitor", "scenario": "clock_skew", "http": 18189, "group": 106,
     "failover": True, "cameras": [], "clockOffset": "+10m",
     "expect": {"components.host": "ok", "cluster.sync.single_node": True}},
]
NODES_BY_ID = {n["id"]: n for n in NODES}

# Every node shares the WSL host and its virtual disk: swap, CPU and disk IO pressure depend on
# what else runs there (a build, the other nodes, the app's own build on Windows), so their
# thresholds are out of reach and only a scenario's own setting raises host or storage.
# Coverage every 60 s (minimum) instead of 900 s, so /cameras has it within the settle time.
COMMON_INI = {
    "monitor": {"swapUsedPctWarn": 95, "swapUsedPctCrit": 99, "coverageIntervalSec": 60},
    "globalmonitor": {"cpu_warning_threshold": 98, "cpu_critical_threshold": 99,
                      "disk_io_psi_full_warning_threshold": 95, "disk_io_psi_full_critical_threshold": 99,
                      "disk_io_util_warning_threshold": 99, "disk_io_util_critical_threshold": 100,
                      "disk_io_await_warning_ms": 10000, "disk_io_await_critical_ms": 20000,
                      "iowait_warning_threshold": 95, "iowait_critical_threshold": 99},
}

POOL = {"name": "it-fake-s3", "type": "MINIO", "tier": "COLD", "endpoint": "http://127.0.0.1:%d" % FAKE_S3_PORT,
        "bucket": "monitor", "base_path": "monitor", "access_key": "probe", "secret_key": "probe",
        "health_check_enabled": True}


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


def write_json(path, data):
    json.dump(data, open(path + ".tmp", "w"), indent=2)
    os.replace(path + ".tmp", path)


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


def faketime_lib():
    """libfaketimeMT: installed, FAKETIME_LIB, or the Ubuntu package unpacked into the cache."""
    cached = os.path.join(FAKETIME_CACHE, FAKETIME_SO)
    for path in (os.environ.get("FAKETIME_LIB"), "/" + FAKETIME_SO, cached):
        if path and os.path.exists(path):
            return path
    os.makedirs(FAKETIME_CACHE, exist_ok=True)
    subprocess.run(["apt-get", "download", "libfaketime"], cwd=FAKETIME_CACHE, capture_output=True)
    for f in os.listdir(FAKETIME_CACHE):
        if f.startswith("libfaketime_") and f.endswith(".deb"):
            subprocess.run(["dpkg", "-x", os.path.join(FAKETIME_CACHE, f), FAKETIME_CACHE], capture_output=True)
    return cached if os.path.exists(cached) else None


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


def camera_kinds(node):
    """The node's cameras, then its `bulk` extra ones (closed port, like "dead")."""
    return node["cameras"] + ["bulk-%03d" % i for i in range(1, node.get("bulk", 0) + 1)]


def server_entry(node, host):
    """A node as VMS lists it, at the address clients reach it on (the app's peer_list hints)."""
    p = ports(node)
    return {
        "id": node["id"], "name": node["id"], "domain": "", "ip": host,
        "httpPort": p["http"], "httpsPort": 0, "rtspPort": p["rtsp"], "rtmpPort": p["rtmp"],
        "isHttpPortAuto": True, "isHttpsPortAuto": True, "isRtspPortAuto": True, "isRtmpPortAuto": True,
        "httpPortNat": p["http"], "httpsPortNat": 0, "rtspPortNat": p["rtsp"], "rtmpPortNat": p["rtmp"],
        "clientUseSsl": False, "hasFailover": node["failover"], "serverGroupId": node["group"],
    }


def api_config(node, nodes, host):
    """What the service API (VMS) tells the node: its group's members and its cameras."""
    members = [n for n in nodes if n["group"] == node["group"]]
    devices = [camera(node, k) for k in camera_kinds(node)]
    return {
        "media_server": {"serverGroupId": node["group"], "maxConfigCameras": max(16, len(devices)), "maxStream": 64,
                         "maxStreamPerCamera": 8, "projectId": "monitor-app-it", "failover": node["failover"],
                         "unlimitedStream": True, "unlimitedStreamPerCamera": True},
        "list_media_server": [server_entry(n, host) for n in members],
        "devices": devices,
        "speakers": [],
    }


def setup(work, nodes, host):
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
        write_json(os.path.join(d, "api-config.json"), api_config(node, nodes, host))


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


def start_node(work, node):
    d = os.path.join(work, node["id"])
    bin_dir = BIN_DIRS[node["build"]]
    env = dict(os.environ, LD_LIBRARY_PATH=bin_dir)
    if node.get("clockOffset"):
        # Only the wall clock: timers and waits run on the monotonic one.
        env.update(LD_PRELOAD=faketime_lib(), FAKETIME=node["clockOffset"], FAKETIME_DONT_FAKE_MONOTONIC="1")
    spawn([os.path.join(bin_dir, "MediaServer"), "-c", os.path.join(d, "config.ini"),
           "-s", os.path.join(bin_dir, "default.pem"), "-l", "0", "-t", "4", "--affinity", "0",
           "--log-dir", os.path.join(d, "log") + "/"],
          d, os.path.join(d, "stdout.log"), os.path.join(d, "server.pid"), env)


def stop_pids(pids):
    """SIGINT (MediaServer sleeps ~10 s on shutdown), then SIGKILL after 30 s."""
    for pid, _ in pids:
        os.kill(pid, signal.SIGINT)
    deadline = time.time() + 30
    while any(alive(p) for p, _ in pids) and time.time() < deadline:
        time.sleep(1)
    for pid, pidfile in pids:
        if alive(pid):
            print("WARN: %s did not stop on SIGINT, killing" % os.path.dirname(pidfile))
            os.kill(pid, signal.SIGKILL)
        os.remove(pidfile)


def node_pid(work, node_id):
    pidfile = os.path.join(work, node_id, "server.pid")
    pid = read_pid(pidfile)
    if not alive(pid):
        if os.path.exists(pidfile):
            os.remove(pidfile)
        return None
    return pid, pidfile


HELPERS = ("mock_api", "rtsp_source", "fake_s3")


def stop_helper(work, name):
    pidfile = os.path.join(work, name + ".pid")
    pid = read_pid(pidfile)
    if alive(pid):
        os.kill(pid, signal.SIGTERM)
        while alive(pid):
            time.sleep(0.1)
    if os.path.exists(pidfile):
        os.remove(pidfile)


def start_fake_s3(work):
    spawn([sys.executable, os.path.join(SYNC, "fake_s3.py"), str(FAKE_S3_PORT)],
          work, os.path.join(work, "fake_s3.log"), os.path.join(work, "fake_s3.pid"))
    for _ in range(50):
        if port_busy(FAKE_S3_PORT):
            return
        time.sleep(0.1)


def stop(work):
    names = sorted(os.listdir(work)) if os.path.isdir(work) else []
    stop_pids([p for p in (node_pid(work, n) for n in names) if p])
    for helper in HELPERS:
        stop_helper(work, helper)


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


def wait_healthy(nodes, timeout):
    deadline = time.time() + timeout
    pending = list(nodes)
    while pending and time.time() < deadline:
        pending = [n for n in pending if not healthy(n["http"])]
        time.sleep(1)
    return pending


def api(node, path, payload):
    """POST to a node's API over loopback (setServerConfig wants the secret there too)."""
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (node["http"], path), data=json.dumps(payload).encode(),
                                 method="POST", headers={"secret": node["secret"], "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode())


def ini_secret(work, node_id):
    """api.secret as the node saved it in its config.ini (setServerConfig writes it back)."""
    section = None
    for line in open(os.path.join(work, node_id, "config.ini"), encoding="utf-8"):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            section = s[1:-1]
        elif section == "api" and s.startswith("secret="):
            return s.split("=", 1)[1].strip()
    return None


def set_secret(node, secret):
    """Change api.secret of a running node (setServerConfig, saved to its config.ini)."""
    body = api(node, "/index/api/setServerConfig", {"api.secret": secret})
    if body.get("code") != 0:
        raise RuntimeError("%s: setServerConfig answered %s" % (node["id"], body))
    node["secret"] = secret


def up(work, only):
    nodes = [dict(n) for n in NODES if only is None or n["build"] == only]
    for build in sorted({n["build"] for n in nodes}):
        if not os.path.exists(os.path.join(BIN_DIRS[build], "MediaServer")):
            sys.exit("MediaServer of build '%s' not found in %s%s" % (
                build, BIN_DIRS[build], " (run build_target.sh develop)" if build == "develop" else ""))
    if any(n.get("clockOffset") for n in nodes) and not faketime_lib():
        print("WARN: libfaketime not found (FAKETIME_LIB, apt-get download libfaketime): clock_skew left out")
        nodes = [n for n in nodes if not n.get("clockOffset")]
    stop(work)
    os.makedirs(work, exist_ok=True)
    busy = [p for n in nodes for p in ports(n).values()] + [API_PORT, RTSP_SOURCE_PORT, FAKE_S3_PORT]
    busy = [p for p in busy if port_busy(p)]
    if busy:
        sys.exit("ports already in use: %s" % ", ".join(map(str, busy)))
    host = wsl_address()
    setup(work, nodes, host)

    spawn([sys.executable, os.path.join(SYNC, "mock_api.py"), str(API_PORT), os.path.join(work, "not_qualified.txt")],
          work, os.path.join(work, "mock_api.log"), os.path.join(work, "mock_api.pid"))
    clip = os.path.join(work, "source.h264")
    if not os.path.exists(clip):
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                        "-i", "testsrc2=size=640x360:rate=25:duration=10", "-c:v", "libx264",
                        "-preset", "veryfast", "-pix_fmt", "yuv420p", "-g", "50", "-keyint_min", "50",
                        "-sc_threshold", "0", "-b:v", "400k", "-bsf:v", "h264_mp4toannexb",
                        "-f", "h264", clip, "-y"], check=True)
    # drop-spec: `ctl camera` takes a recording camera offline by listing its stream there.
    write_json(os.path.join(work, "drop_spec.json"), {"offline": []})
    spawn([sys.executable, os.path.join(STRESS, "rtsp_source.py"), clip, "--port", str(RTSP_SOURCE_PORT),
           "--fps", "25", "--stats-sec", "10", "--drop-spec", os.path.join(work, "drop_spec.json"),
           "--state", os.path.join(work, "rtsp_state.json")],
          work, os.path.join(work, "rtsp_source.log"), os.path.join(work, "rtsp_source.pid"))
    if any(n.get("pool") for n in nodes):
        start_fake_s3(work)

    for node in nodes:
        start_node(work, node)
    pending = wait_healthy(nodes, 90)
    for n in pending:
        print("WARN: %s is not healthy after 90 s (see %s/stdout.log)" % (n["id"], os.path.join(work, n["id"])))
    for n in nodes:
        if n in pending:
            continue
        if n.get("runtimeSecret"):
            set_secret(n, n["runtimeSecret"])
        if n.get("pool"):
            ret = api(n, "/media/mserver/storage/pool/create", POOL)
            if ret.get("code") != 0:
                print("WARN: %s: pool/create answered %s" % (n["id"], ret))

    builds = {b: build_ref(b) for b in sorted({n["build"] for n in nodes})}
    fixture = {
        "schema": 1,
        "createdAt": int(time.time()),
        "host": host,
        "workDir": work,
        # For `ctl` from the app side.
        "script": os.path.abspath(__file__),
        "builds": builds,
        "nodes": [{
            "id": n["id"], "scenario": n["scenario"], "build": n["build"],
            "url": "http://%s:%d" % (host, n["http"]),
            "secret": n["secret"], "group": n["group"], "failover": n["failover"],
            "cameras": ["%s-%s" % (n["id"], k) for k in n["cameras"]], "bulk": 0,
            "pool": POOL["name"] if n.get("pool") else None, "clockOffset": n.get("clockOffset"),
            "monitorApi": n["build"] != "develop", "expect": n["expect"],
        } for n in nodes],
    }
    write_json(os.path.join(work, "initial.json"), fixture)
    write_json(os.path.join(work, "fixture.json"), fixture)
    print("fixture up: %d node(s), %s" % (len(nodes) - len(pending), os.path.join(work, "fixture.json")))
    return 1 if pending else 0


def status(work):
    path = os.path.join(work, "fixture.json")
    if not os.path.exists(path):
        sys.exit("no fixture in %s (run: fixture.py up)" % work)
    fixture = json.load(open(path))
    for n in fixture["nodes"]:
        n["running"] = node_pid(work, n["id"]) is not None
    for helper in HELPERS:
        fixture[helper + "Running"] = alive(read_pid(os.path.join(work, helper + ".pid")))
    print(json.dumps(fixture, indent=2))


# ── ctl: faults while the app watches ──

class Fixture:
    def __init__(self, work):
        self.work = work
        path = os.path.join(work, "fixture.json")
        if not os.path.exists(path):
            sys.exit("no fixture in %s (run: fixture.py up)" % work)
        self.data = json.load(open(path))
        self.initial = json.load(open(os.path.join(work, "initial.json")))

    def node(self, node_id):
        for n in self.data["nodes"]:
            if n["id"] == node_id:
                return n
        sys.exit("no node %s in the fixture" % node_id)

    def full(self, n):
        """A fixture.json node with its NODES definition (ports, build, camera kinds). The secret is
        the one the node saved, in case fixture.json fell behind (an interrupted ctl)."""
        return dict(NODES_BY_ID[n["id"]], group=n["group"], failover=n["failover"], bulk=n.get("bulk", 0),
                    secret=ini_secret(self.work, n["id"]) or n["secret"])

    def save(self):
        write_json(os.path.join(self.work, "fixture.json"), self.data)

    def push_vms(self):
        """The VMS answer of every node from the current groups, flags and cameras (read on the next
        configuration hook)."""
        nodes = [self.full(n) for n in self.data["nodes"]]
        for n in nodes:
            write_json(os.path.join(self.work, n["id"], "api-config.json"), api_config(n, nodes, self.data["host"]))

    def drop(self, camera_id, offline):
        path = os.path.join(self.work, "drop_spec.json")
        spec = json.load(open(path))
        streams = set(spec.get("offline", [])) - {camera_id + "-main"}
        if offline:
            streams.add(camera_id + "-main")
        write_json(path, dict(spec, offline=sorted(streams)))


def ctl(work, argv):
    ap = argparse.ArgumentParser(prog="fixture.py ctl")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("stop", "start"):
        sub.add_parser(name).add_argument("id")
    p = sub.add_parser("camera")
    p.add_argument("camera")
    p.add_argument("state", choices=["offline", "online"])
    sub.add_parser("pool").add_argument("state", choices=["down", "up"])
    p = sub.add_parser("vms")
    p.add_argument("id")
    p.add_argument("--group", type=int)
    p.add_argument("--failover", type=int, choices=[0, 1])
    p.add_argument("--bulk", type=int)
    p = sub.add_parser("secret")
    p.add_argument("id")
    p.add_argument("value")
    sub.add_parser("reset")
    a = ap.parse_args(argv)
    # The app side runs several ctl at once: the ones that read and write fixture.json or
    # drop_spec.json take turns (stop/start touch neither, and each waits up to 30 s).
    lock = open(os.path.join(work, "ctl.lock"), "w")
    if a.cmd not in ("stop", "start"):
        fcntl.flock(lock, fcntl.LOCK_EX)
    fx = Fixture(work)

    if a.cmd == "stop":
        fx.node(a.id)
        running = node_pid(work, a.id)
        if running:
            stop_pids([running])
    elif a.cmd == "start":
        node = fx.full(fx.node(a.id))
        if not node_pid(work, a.id):
            start_node(work, node)
            if wait_healthy([node], 90):
                sys.exit("%s is not healthy after 90 s" % a.id)
    elif a.cmd == "camera":
        fx.drop(a.camera, a.state == "offline")
    elif a.cmd == "pool":
        if a.state == "down":
            stop_helper(work, "fake_s3")
        elif not alive(read_pid(os.path.join(work, "fake_s3.pid"))):
            start_fake_s3(work)
    elif a.cmd == "vms":
        n = fx.node(a.id)
        if a.group is not None:
            n["group"] = a.group
        if a.failover is not None:
            n["failover"] = bool(a.failover)
        if a.bulk is not None:
            n["bulk"] = a.bulk
        fx.push_vms()
        fx.save()
    elif a.cmd == "secret":
        n = fx.node(a.id)
        value = next(i["secret"] for i in fx.initial["nodes"] if i["id"] == a.id) if a.value == "initial" else a.value
        full = fx.full(n)
        set_secret(full, value)
        n["secret"] = value
        fx.save()
    elif a.cmd == "reset":
        stopped = [fx.full(n) for n in fx.data["nodes"] if not node_pid(work, n["id"])]
        for node in stopped:
            start_node(work, node)
        for n in wait_healthy(stopped, 90):
            print("WARN: %s is not healthy after 90 s" % n["id"])
        write_json(os.path.join(work, "drop_spec.json"), {"offline": []})
        if any(n.get("pool") for n in fx.data["nodes"]) and not alive(read_pid(os.path.join(work, "fake_s3.pid"))):
            start_fake_s3(work)
        for n, first in zip(fx.data["nodes"], fx.initial["nodes"]):
            n.update(group=first["group"], failover=first["failover"], bulk=0)
            # The node's own file, not fixture.json: an interrupted ctl may have left that behind.
            if ini_secret(work, n["id"]) != first["secret"]:
                set_secret(fx.full(n), first["secret"])
                n["secret"] = first["secret"]
        fx.push_vms()
        fx.save()
    print("ctl %s: done" % " ".join(argv))


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "ctl":
        rest, work = sys.argv[2:], "/tmp/s3mk_monitor_app"
        if "--work" in rest:
            i = rest.index("--work")
            work = rest[i + 1]
            rest = rest[:i] + rest[i + 2:]
        ctl(os.path.abspath(work), rest)
        return
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
