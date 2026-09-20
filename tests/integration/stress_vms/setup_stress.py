#!/usr/bin/env python3
"""Sinh cấu hình cho stress test VMS: N camera / 2N luồng trải trên các cohort khác nhau.

Dùng:
    setup_stress.py <repo> <work_dir> layout [--cameras 300] [--nodes 2] ...
    setup_stress.py <repo> <work_dir> phase <tên_phase>

`layout` dựng lại toàn bộ thư mục node (config.ini, api-config.json, drop/fault spec).
`phase` chỉ ghi lại api-config.json + spec để đổi điều kiện giữa chừng.

Cohort camera (xem README) quyết định hồ sơ lập lịch ghi, thời gian lưu trữ, tình
trạng nguồn stream và độ trễ ONVIF của từng camera. Tỉ lệ cohort giữ nguyên ở mọi
quy mô nên kết quả giữa các lần chạy so sánh được với nhau.
"""
import argparse
import json
import os
import shutil
import sys

RTSP_SOURCE_PORT = 18554
ONVIF_PORT = 18580
API_PORT = 18590

# Cohort: (tên, tỉ trọng, mô tả). Tỉ trọng cộng lại = 100.
COHORTS = [
    ("always", 34, "ghi 24/7, giữ 24h, nguồn ổn định"),
    ("office", 18, "chỉ ghi giờ hành chính T2-T6 08:00-18:00"),
    ("motion_night", 14, "ban ngày ghi liên tục, ban đêm chỉ ghi khi có chuyển động"),
    ("lowres_motion", 10, "RecordLowResAndMotion cả tuần, giữ ngắn"),
    ("retention_auto", 8, "keepArchivedMin/MaxFor ở chế độ Auto"),
    ("long_retention", 6, "giữ 30 ngày, dùng để kiểm tier/dung lượng"),
    ("no_record", 4, "chỉ xem trực tiếp, không ghi"),
    ("flaky", 3, "nguồn RTSP rớt định kỳ, buộc reconnect"),
    ("offline", 3, "nguồn RTSP không tồn tại (404)"),
]


def cohort_of(index, total):
    """Chia camera vào cohort theo tỉ trọng, ổn định với mọi `total`."""
    pos = (index * 100) // total
    acc = 0
    for name, weight, _ in COHORTS:
        acc += weight
        if pos < acc:
            return name
    return COHORTS[0][0]


# ------------------------------------------------------------------ lịch ghi
def schedule(days, hours, mode, fps=15, quality="M"):
    return [{"dh": "%d,%d" % (d, h), "ty": mode, "fps": fps, "q": quality}
            for d in days for h in hours]


ALL_DAYS = list(range(7))
WORK_DAYS = list(range(5))
ALL_HOURS = list(range(24))
OFFICE_HOURS = list(range(8, 18))
NIGHT_HOURS = [h for h in ALL_HOURS if h < 6 or h >= 20]
DAY_HOURS = [h for h in ALL_HOURS if 6 <= h < 20]

# RecordMode: 0=NoRecord 1=RecordLowResAndMotion 2=RecordOnlyMotion 3=RecordAlways
SCHEDULES = {
    "always": schedule(ALL_DAYS, ALL_HOURS, 3, fps=15, quality="M"),
    "office": schedule(WORK_DAYS, OFFICE_HOURS, 3, fps=20, quality="H"),
    "motion_night": (schedule(ALL_DAYS, DAY_HOURS, 3, fps=15, quality="M")
                     + schedule(ALL_DAYS, NIGHT_HOURS, 2, fps=10, quality="L")),
    "lowres_motion": schedule(ALL_DAYS, ALL_HOURS, 1, fps=8, quality="L"),
    "retention_auto": schedule(ALL_DAYS, ALL_HOURS, 3, fps=12, quality="M"),
    "long_retention": schedule(ALL_DAYS, ALL_HOURS, 3, fps=15, quality="M"),
    "no_record": [],
    "flaky": schedule(ALL_DAYS, ALL_HOURS, 3, fps=10, quality="L"),
    "offline": schedule(ALL_DAYS, ALL_HOURS, 3, fps=10, quality="L"),
}

# (enableRecording, keepMinForAuto, keepMinHours, keepMaxForAuto, keepMaxHours)
RETENTION = {
    "always": (True, False, 1, False, 24),
    "office": (True, False, 2, False, 72),
    "motion_night": (True, False, 1, False, 48),
    "lowres_motion": (True, False, 1, False, 12),
    "retention_auto": (True, True, 0, True, 0),
    "long_retention": (True, False, 24, False, 24 * 30),
    "no_record": (False, False, 0, False, 24),
    "flaky": (True, False, 1, False, 24),
    "offline": (True, False, 1, False, 24),
}


def device_ip(index):
    """127.1.x.y — tách khỏi 127.0.0.1 để không đụng dịch vụ nào của host."""
    return "127.1.%d.%d" % (1 + index // 250, 1 + index % 250)


def camera(index, total, node_ids, args):
    cohort = cohort_of(index, total)
    cam_id = "cam-%04d" % index
    owner = node_ids[index % len(node_ids)]
    enable_rec, min_auto, min_h, max_auto, max_h = RETENTION[cohort]
    ip = device_ip(index)

    # Nguồn stream: cohort offline trỏ tới path bị mock trả 404.
    suffix = "-offline" if cohort == "offline" else ""
    main_id = "%s-main" % cam_id
    sub_id = "%s-sub" % cam_id
    base = "rtsp://127.0.0.1:%d" % args.rtsp_port

    entry = {
        "id": cam_id,
        "name": "%s [%s]" % (cam_id, cohort),
        "ip": ip,
        "httpPort": args.onvif_port,
        "manufacturer": "S3MediaKit",
        "model": "StressCam",
        "username": "admin",
        "password": "stress-pass",
        "enabled": True,
        "enablePtzControl": True,
        "priMediaServerId": owner,
        "primaryStreamId": main_id,
        "primaryStreamUrl": "%s/%s%s" % (base, main_id, suffix),
        "secondaryStreamId": sub_id,
        "secondaryStreamUrl": "%s/%s%s" % (base, sub_id, suffix),
        "recordingConfig": {
            "enableRecording": enable_rec,
            "recordingSchedule": SCHEDULES[cohort],
            "keepArchivedMinForAuto": min_auto,
            "keepArchivedMinFor": min_h,
            "keepArchivedMaxForAuto": max_auto,
            "keepArchivedMaxFor": max_h,
            "motionPreRecordSec": 5,
            "motionPostRecordSec": 5,
        },
        "cameraAdvanceConfig": {
            "streamSettings": {
                "disableMainStream": False,
                "disableSubStream": False,
                "notRecordMainStream": False,
                # long_retention chỉ ghi luồng chính -> kiểm cờ chặn ghi theo từng luồng.
                # KHÔNG đặt cho lowres_motion: ở chế độ đó luồng phụ mới là luồng ghi
                # liên tục, chặn nó thì cohort không ghi được gì (StreamSink.cpp:231).
                "notRecordSubStream": cohort == "long_retention",
                "disableAudio": True,
            },
            "mediaStreaming": {"rtpTransport": "TCP", "useDefaultMediaPort": True},
            "webPage": {"useDefaultWebPort": True},
            "ptzSetting": {
                "enablePTZControl": True,
                "reversePanAxis": False,
                "reverseTiltAxis": False,
                "ptzMode": ["AUTO", "CONTINUOUS", "RELATIVE", "ABSOLUTE"][index % 4],
            },
            "onvif": {"mainStream": "AUTO", "subStream": "AUTO"},
        },
    }
    return entry, cohort


def build_cameras(total, node_ids, args):
    cams, by_cohort = [], {}
    for i in range(total):
        entry, cohort = camera(i, total, node_ids, args)
        cams.append(entry)
        by_cohort.setdefault(cohort, []).append(entry["id"])
    return cams, by_cohort


# ------------------------------------------------------------------ config.ini
def patch_ini(src, dst, overrides):
    """Áp {section: {key: value}} lên file ini, giữ nguyên comment và thứ tự."""
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


def node_ports(i):
    return {"http": 18081 + i, "rtsp": 18551 + i, "rtmp": 18351 + i}


def server_entry(node_id, ports):
    return {
        "id": node_id, "name": node_id, "domain": "", "ip": "127.0.0.1",
        "httpPort": ports["http"], "httpsPort": 0,
        "rtspPort": ports["rtsp"], "rtmpPort": ports["rtmp"],
        "isHttpPortAuto": True, "isHttpsPortAuto": True,
        "isRtspPortAuto": True, "isRtmpPortAuto": True,
        "httpPortNat": ports["http"], "httpsPortNat": 0,
        "rtspPortNat": ports["rtsp"], "rtmpPortNat": ports["rtmp"],
        "clientUseSsl": False, "hasFailover": True, "serverGroupId": 0,
    }


def api_config(node_ids, devices, args):
    return {
        "media_server": {
            "serverGroupId": 0,
            "maxConfigCameras": max(1024, args.cameras * 2),
            "maxStream": max(2048, args.cameras * 4),
            "maxStreamPerCamera": 8,
            "projectId": "stress-vms",
            "unlimitedStream": True,
            "unlimitedStreamPerCamera": True,
        },
        "list_media_server": [server_entry(n, node_ports(i)) for i, n in enumerate(node_ids)],
        "devices": devices,
    }


def write_json(path, payload):
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(payload, fh, indent=1)
    os.replace(tmp, path)


def node_overrides(node_id, d, ports, args):
    return {
        "general": {"mediaServerId": node_id},
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
        "protocol": {"mp4_save_path": d + "/record", "hls_save_path": d + "/www/live",
                     # đoạn ngắn để sinh nhiều file index trong thời gian chạy ngắn
                     "mp4_max_second": args.segment_sec},
        "record": {"fileSecond": args.segment_sec},
        "hook": {"enable": 1, "api_url": "http://127.0.0.1:%d" % args.api_port,
                 "report_interval": 10.0, "alive_interval": 5.0, "timeoutSec": 5, "retry": 0},
        "database": {"db_save_path": d + "/db", "updates_save_path": d + "/updates",
                     "mserver_updates_save_path": d + "/mserver_updates",
                     "keep_migration_sql_files": 1,
                     "enable_sync_db": 1 if args.nodes > 1 else 0,
                     "pull_interval_sec": 5, "gossip_fanout": 3},
        "manager": {"serverLocationId": 0, "apiSecret": "",
                    "certSavePath": d + "/www/certs",
                    "maxAllowedDevices": max(1024, args.cameras * 2),
                    "failoverActiveDelaySec": 15,
                    "enableAuthorize": 0},
        # Pool blocking cố định để so sánh được giữa các lần chạy; ONVIF chạy trên device().
        "blocking": {"apiThreads": args.api_threads,
                     "deviceThreads": args.device_threads,
                     "discoveryThreads": 4,
                     "ffmpegThreads": 2},
        "api": {"apiDebug": 0,
                "limit.global.maxQueueMs": 30000,
                "limit.storage.read.maxQueueMs": 30000},
        # Ngưỡng dung lượng: kịch bản storage hạ xuống để ép enforceStoragePolicy chạy.
        "storage": {"limitPercentUsage": args.limit_percent,
                    "removePercentExtra": args.remove_percent},
    }


# ------------------------------------------------------------------ các phase
def spec_paths(work):
    return (os.path.join(work, "drop_spec.json"),
            os.path.join(work, "onvif_fault_spec.json"))


def write_specs(work, cams, by_cohort, phase):
    """Ghi điều kiện lỗi cho rtsp_source.py và onvif_mock.py theo từng phase."""
    drop_path, fault_path = spec_paths(work)
    offline = list(by_cohort.get("offline", []))
    flaky = list(by_cohort.get("flaky", []))

    drop = {
        "offline": ["%s-main-offline" % c for c in offline] +
                   ["%s-sub-offline" % c for c in offline],
        "flaky": ["%s-main" % c for c in flaky] + ["%s-sub" % c for c in flaky],
        "flaky_sec": 45,
    }
    faults = {"default": {"latency_ms": 0}, "devices": {}}
    index_of = {c["id"]: i for i, c in enumerate(cams)}

    # ONVIF chậm: một nhóm nhỏ trả lời sau 8s để soi pool device() và nhánh timeout.
    slow = [c["id"] for c in cams if index_of[c["id"]] % 25 == 7]
    for cam_id in slow:
        faults["devices"][device_ip(index_of[cam_id])] = {"latency_ms": 8000}
    for cam_id in offline:
        faults["devices"][device_ip(index_of[cam_id])] = {"offline": True}

    if phase == "onvif_storm":
        # toàn hệ thống ONVIF chậm 1.5s: kiểm xem poller có bị chiếm không
        faults["default"] = {"latency_ms": 1500}
    elif phase == "source_flap":
        # một nửa số camera rớt nguồn mỗi 20s
        drop["flaky"] = ["%s-main" % c["id"] for c in cams[::2]]
        drop["flaky_sec"] = 20
    elif phase == "recover":
        drop["offline"] = []
        drop["flaky"] = []
        faults["default"] = {"latency_ms": 0}
        faults["devices"] = {}

    write_json(drop_path, drop)
    write_json(fault_path, faults)


def layout(repo, work, args):
    node_ids = ["node-%s" % chr(ord("a") + i) for i in range(args.nodes)]
    cams, by_cohort = build_cameras(args.cameras, node_ids, args)

    for i, node_id in enumerate(node_ids):
        d = os.path.join(work, node_id)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.join(d, "www"))
        os.makedirs(os.path.join(d, "record"))
        os.makedirs(os.path.join(d, "log"))
        shutil.copytree(os.path.join(repo, "migration/updates"), os.path.join(d, "updates"))
        shutil.copytree(os.path.join(repo, "migration/mserver_updates"),
                        os.path.join(d, "mserver_updates"))
        patch_ini(os.path.join(repo, "conf/config.ini"), os.path.join(d, "config.ini"),
                  node_overrides(node_id, d, node_ports(i), args))
        # Mỗi node chỉ nhận các camera mà nó sở hữu; phần còn lại đến qua sync.
        owned = [c for c in cams if c["priMediaServerId"] == node_id]
        write_json(os.path.join(d, "api-config.json"), api_config(node_ids, owned, args))

    write_specs(work, cams, by_cohort, "steady")
    write_json(os.path.join(work, "inventory.json"), {
        "cameras": args.cameras,
        "streams": args.cameras * 2,
        "nodes": node_ids,
        "by_cohort": by_cohort,
        "ports": {n: node_ports(i) for i, n in enumerate(node_ids)},
        "rtsp_port": args.rtsp_port,
        "onvif_port": args.onvif_port,
        "api_port": args.api_port,
    })
    print("layout: %d camera / %d luồng trên %d node" % (args.cameras, args.cameras * 2, args.nodes))
    for name, weight, desc in COHORTS:
        print("  %-15s %3d cam  %s" % (name, len(by_cohort.get(name, [])), desc))


def phase(repo, work, name, args):
    inv = json.load(open(os.path.join(work, "inventory.json")))
    node_ids = inv["nodes"]
    cams, by_cohort = build_cameras(inv["cameras"], node_ids, args)

    if name == "failover":
        # node-a nhả toàn bộ camera của mình; node-b phải nhận qua FAILOVER
        survivor = node_ids[-1]
        for i, node_id in enumerate(node_ids):
            owned = [] if node_id != survivor else cams
            write_json(os.path.join(work, node_id, "api-config.json"),
                       api_config(node_ids, owned, args))
    elif name == "rebalance":
        for i, node_id in enumerate(node_ids):
            owned = [c for c in cams if c["priMediaServerId"] == node_id]
            write_json(os.path.join(work, node_id, "api-config.json"),
                       api_config(node_ids, owned, args))
    elif name == "retention_drop":
        # siết thời gian lưu trữ xuống 1h cho mọi camera -> ép dọn dung lượng
        for c in cams:
            c["recordingConfig"]["keepArchivedMaxForAuto"] = False
            c["recordingConfig"]["keepArchivedMaxFor"] = 1
            c["recordingConfig"]["keepArchivedMinForAuto"] = False
            c["recordingConfig"]["keepArchivedMinFor"] = 0
        for node_id in node_ids:
            owned = [c for c in cams if c["priMediaServerId"] == node_id]
            write_json(os.path.join(work, node_id, "api-config.json"),
                       api_config(node_ids, owned, args))
    elif name == "retention_purge":
        # keepArchivedMaxFor=0 => max_value = now => ngưỡng xoá = thời điểm hiện tại,
        # nên mọi mp4 đã ghi xong đều quá hạn. Dùng để chứng minh enforceStoragePolicy
        # thực sự xoá được trong một lần chạy ngắn (1h sẽ không kịp hết hạn).
        for c in cams:
            c["recordingConfig"]["keepArchivedMaxForAuto"] = False
            c["recordingConfig"]["keepArchivedMaxFor"] = 0
            c["recordingConfig"]["keepArchivedMinForAuto"] = False
            c["recordingConfig"]["keepArchivedMinFor"] = 0
        for node_id in node_ids:
            owned = [c for c in cams if c["priMediaServerId"] == node_id]
            write_json(os.path.join(work, node_id, "api-config.json"),
                       api_config(node_ids, owned, args))
    elif name == "schedule_flip":
        # đảo hồ sơ lập lịch: cohort office chuyển sang ghi 24/7 và ngược lại
        for c in cams:
            if "[office]" in c["name"]:
                c["recordingConfig"]["recordingSchedule"] = SCHEDULES["always"]
            elif "[always]" in c["name"]:
                c["recordingConfig"]["recordingSchedule"] = SCHEDULES["office"]
        for node_id in node_ids:
            owned = [c for c in cams if c["priMediaServerId"] == node_id]
            write_json(os.path.join(work, node_id, "api-config.json"),
                       api_config(node_ids, owned, args))
    elif name == "purge":
        for node_id in node_ids:
            write_json(os.path.join(work, node_id, "api-config.json"),
                       api_config(node_ids, [], args))

    write_specs(work, cams, by_cohort, name)
    print("phase %s: đã ghi lại api-config.json + spec" % name)


def add_args(ap):
    ap.add_argument("--cameras", type=int, default=300)
    ap.add_argument("--nodes", type=int, default=2)
    ap.add_argument("--rtsp-port", type=int, default=RTSP_SOURCE_PORT)
    ap.add_argument("--onvif-port", type=int, default=ONVIF_PORT)
    ap.add_argument("--api-port", type=int, default=API_PORT)
    ap.add_argument("--segment-sec", type=int, default=60)
    ap.add_argument("--api-threads", type=int, default=4)
    ap.add_argument("--device-threads", type=int, default=8)
    ap.add_argument("--limit-percent", type=int, default=90)
    ap.add_argument("--remove-percent", type=int, default=5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo")
    ap.add_argument("work")
    ap.add_argument("command", choices=["layout", "phase"])
    ap.add_argument("name", nargs="?", default="")
    add_args(ap)
    args = ap.parse_args()

    if args.command == "layout":
        layout(args.repo, args.work, args)
    else:
        if not args.name:
            sys.exit("phase cần tên")
        phase(args.repo, args.work, args.name, args)


if __name__ == "__main__":
    main()
