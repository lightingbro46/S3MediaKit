#!/usr/bin/env python3
"""Create two isolated MediaServer node directories (node-a, node-b) for a local sync test.

Usage: setup_nodes.py <repo_root> <cluster_dir> [phase2..phase4 | fo1..fo3]
"""
import json
import os
import shutil
import sys

REPO, CLUSTER = sys.argv[1], sys.argv[2]
API_PORT = 18090

NODES = {
    "node-a": {"http": 18081, "rtsp": 18551, "rtmp": 18351},
    "node-b": {"http": 18082, "rtsp": 18552, "rtmp": 18352},
}


def patch_ini(src, dst, overrides):
    """Apply {section: {key: value}} to an ini file, keeping comments and order."""
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


def server_entry(node_id, ports):
    return {
        "id": node_id, "name": node_id, "domain": "", "ip": "127.0.0.1",
        "httpPort": ports["http"], "httpsPort": 0, "rtspPort": ports["rtsp"], "rtmpPort": ports["rtmp"],
        "isHttpPortAuto": True, "isHttpsPortAuto": True, "isRtspPortAuto": True, "isRtmpPortAuto": True,
        "httpPortNat": ports["http"], "httpsPortNat": 0, "rtspPortNat": ports["rtsp"], "rtmpPortNat": ports["rtmp"],
        "clientUseSsl": False, "hasFailover": True, "serverGroupId": 0,
    }


def camera(enabled, keep_max_hours=24):
    # Unreachable RTSP source: the stream fails fast, which is fine for DB sync testing.
    return {
        "id": "cam-sync-1", "name": "cam-sync-1", "ip": "", "httpPort": 80,
        "manufacturer": "test", "model": "test", "username": "", "password": "",
        "enabled": enabled, "priMediaServerId": "node-a",
        "primaryStreamId": "cam-sync-1-main", "primaryStreamUrl": "rtsp://127.0.0.1:9/none",
        "recordingConfig": {"enableRecording": False, "keepArchivedMaxFor": keep_max_hours,
                            "keepArchivedMaxForAuto": False},
    }


FO_CAMERA = "cam-fo-1"
FO_RTSP_PORT = 18554


def failover_camera(keep_max_hours):
    """Recording camera for the failover scenario, fed by rtsp_source.py on FO_RTSP_PORT.

    node-a is the primary server: node-b runs it in failover mode (enableFailover is derived
    from priMediaServerId != mediaServerId). Recording is always on, every hour of the week.
    """
    return {
        "id": FO_CAMERA, "name": FO_CAMERA, "ip": "127.0.0.1", "httpPort": 80,
        "manufacturer": "Generic", "model": "RTSP", "username": "", "password": "",
        "enabled": True, "priMediaServerId": "node-a",
        "primaryStreamId": FO_CAMERA + "-main",
        "primaryStreamUrl": "rtsp://127.0.0.1:%d/%s-main" % (FO_RTSP_PORT, FO_CAMERA),
        "recordingConfig": {
            "enableRecording": True,
            "recordingSchedule": [{"dh": "%d,%d" % (d, h), "ty": 3, "fps": 25, "q": "H"}
                                  for d in range(7) for h in range(24)],
            "keepArchivedMinForAuto": False, "keepArchivedMinFor": 0,
            "keepArchivedMaxForAuto": False, "keepArchivedMaxFor": keep_max_hours,
        },
    }


def api_config(devices):
    return {
        "media_server": {"serverGroupId": 0, "maxConfigCameras": 16, "maxStream": 64,
                         "maxStreamPerCamera": 8, "projectId": "sync-test",
                         "unlimitedStream": True, "unlimitedStreamPerCamera": True},
        "list_media_server": [server_entry(n, p) for n, p in NODES.items()],
        "devices": devices,
    }


def write_api_config(node_id, devices):
    path = os.path.join(CLUSTER, node_id, "api-config.json")
    tmp = path + ".tmp"
    json.dump(api_config(devices), open(tmp, "w"), indent=2)
    os.replace(tmp, path)


def main():
    for node_id, ports in NODES.items():
        d = os.path.join(CLUSTER, node_id)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "www"))
        shutil.copytree(os.path.join(REPO, "migration/updates"), os.path.join(d, "updates"))
        shutil.copytree(os.path.join(REPO, "migration/mserver_updates"), os.path.join(d, "mserver_updates"))
        overrides = {
            "general": {"mediaServerId": node_id},
            # keepAliveSecond: check_worker_apis.py drives requests that deliberately take far
            # longer than the 30s default (pool/testConnection against a blackhole address waits out
            # the AWS SDK setup plus its connect timeout). The server closes an HTTP session that
            # sends nothing for keepAliveSecond, so at the default the client loses the connection
            # before the answer and the measurement collapses.
            "http": {"port": ports["http"], "sslport": 0, "rootPath": d + "/www",
                     "keepAliveSecond": 180},
            "rtsp": {"port": ports["rtsp"], "sslport": 0},
            "rtmp": {"port": ports["rtmp"], "sslport": 0},
            "rtp_proxy": {"port": 0},
            "rtc": {"port": 0, "tcpPort": 0, "signalingPort": 0, "signalingSslPort": 0,
                    "icePort": 0, "iceTcpPort": 0},
            "srt": {"port": 0},
            "shell": {"port": 0},
            "ssdp": {"port": 0},
            "protocol": {"mp4_save_path": d + "/www", "hls_save_path": d + "/www/live"},
            "hook": {"enable": 1, "api_url": "http://127.0.0.1:%d" % API_PORT,
                     "report_interval": 10.0, "alive_interval": 5.0, "timeoutSec": 3, "retry": 0},
            "database": {"db_save_path": d + "/db", "updates_save_path": d + "/updates",
                         "mserver_updates_save_path": d + "/mserver_updates",
                         "keep_migration_sql_files": 1, "enable_sync_db": 1,
                         "pull_interval_sec": 5, "gossip_fanout": 3},
            # failoverActiveDelaySec: how long an inactive camera is kept after its release (default 1800s).
            # The failover scenario keeps the production default: a node that just dropped a camera
            # must still hold it when the primary node takes it back one config reload later.
            "manager": {"apiSecret": "", "certSavePath": d + "/www/certs"},
            "cluster": {"serverLocationId": 0,
                        "failoverActiveDelaySec": int(os.environ.get("FAILOVER_ACTIVE_DELAY_SEC", "5"))},
            # device/config APIs are exercised without JWT (check_worker_apis.py)
            "auth": {"enableAuthorize": 0},
            # check_worker_apis.py blocks every API worker on purpose (pool/testConnection against
            # a blackhole address) to measure the pollers and the storage.read cap. It needs a
            # known pool size, instead of the default max(2, nproc/2) which differs per machine.
            "blocking": {"apiThreads": 4},
            # ... and queue deadlines longer than that blockage, so the in-flight cap is the only
            # reason a request of the burst can be answered 503.
            "api": {"limit.global.maxQueueMs": 120000, "limit.storage.read.maxQueueMs": 120000},
        }
        patch_ini(os.path.join(REPO, "conf/config.ini"), os.path.join(d, "config.ini"), overrides)

    # Phase 1: camera owned by node-a only.
    write_api_config("node-a", [camera(True)])
    write_api_config("node-b", [])


def failover_phase(name):
    """Service API decisions of the failover scenario (run_cluster.sh <dir> failover)."""
    keep = int(os.environ.get("FO_KEEP_HOURS", "1"))
    if name == "fo1":
        # Camera on its primary server.
        write_api_config("node-a", [failover_camera(keep)])
        write_api_config("node-b", [])
    elif name == "fo2":
        # node-a is offline: the service hands the camera to node-b (FAILOVER assignment).
        write_api_config("node-b", [failover_camera(keep)])
    elif name == "fo3":
        # node-a is back: the camera returns to it and node-b is told to drop it.
        write_api_config("node-a", [failover_camera(keep)])
        write_api_config("node-b", [])


def phase(n):
    if n == 2:
        # Move camera: node-a drops it (delCamera → release), node-b takes it over (FAILOVER).
        write_api_config("node-a", [])
        write_api_config("node-b", [camera(True)])
    elif n == 3:
        # Config change on the current owner must reach the other node.
        write_api_config("node-b", [camera(True, keep_max_hours=48)])
    elif n == 4:
        # Camera deleted from the cluster: after the failover delay both nodes drop the ESC resource.
        write_api_config("node-a", [])
        write_api_config("node-b", [])


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[3].startswith("fo"):
        failover_phase(sys.argv[3])
    elif len(sys.argv) > 3 and sys.argv[3].startswith("phase"):
        phase(int(sys.argv[3][5:]))
    else:
        main()
