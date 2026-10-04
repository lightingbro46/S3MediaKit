#!/usr/bin/env python3
"""Dựng work dir cho kiểm thử E2E với 1 camera thật (Main + Sub) và MinIO.

Dùng:
    setup_e2e.py <repo> <work> layout
    setup_e2e.py <repo> <work> camera [--mode always] [--overlay] [--disable-sub] ...

`layout` tạo config.ini, khoá JWT, api-config.json và thư mục dữ liệu. config.ini chỉ chứa các
khoá harness cần đổi; mọi khoá khác lấy default đăng ký trong code (như bản production, nơi
config.ini được sinh từ code). Không dựa trên conf/config.ini của repo. Nguồn RTSP lấy từ
biến môi trường E2E_RTSP_MAIN / E2E_RTSP_SUB (không nhận qua tham số để URL có mật khẩu
không nằm trong shell history).

`camera` ghi lại api-config.json với cấu hình camera mới. MediaServer bản Debug đọc lại file
này ở mỗi chu kỳ `hook.report_interval` (harness đặt 10 s), nên không cần restart.
Các cờ không truyền giữ nguyên giá trị lần trước (lưu ở <work>/camera_state.json).
"""
import argparse
import base64
import datetime
import json
import os
import secrets
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "stress_vms"))
from setup_stress import server_entry, write_json  # noqa: E402

NODE_ID = "e2e-node"
PROJECT_ID = "e2e-project"
CAMERA_ID = "e2e-cam"
MAIN_ID = "main"
SUB_ID = "sub"
WATERMARK_ID = "e2e-watermark"

# Role dùng cho kịch bản overlay: admin excluded khỏi cả hai, wm-only chỉ excluded khỏi mask.
ROLE_ADMIN = "e2e-admin"
ROLE_VIEWER = "e2e-viewer"
ROLE_WM_ONLY = "e2e-wm-only"

# RecordMode: 0=NoRecord 1=RecordLowResAndMotion 2=RecordOnlyMotion 3=RecordAlways
MODES = {"none": 0, "lowres_motion": 1, "only_motion": 2, "always": 3}

DEFAULT_STATE = {
    "mode": "always",
    "schedule": "all",          # all | flip (giờ hiện tại theo mode, giờ kế tiếp NoRecord) | otherday
    "enable_recording": True,
    "overlay": False,
    "disable_main": False,
    "disable_sub": False,
    "no_record_main": False,
    "no_record_sub": False,
    "disable_audio": False,
    "motion": False,
    "rtp_transport": "TCP",
    "keep_max_hours": 0,
    "keep_min_hours": 0,
    "present": True,
}


def write_ini(dst, overrides):
    """Ghi {section: {key: value}} thành ini. MediaServer chỉ ghi đè các khoá này lên default."""
    lines = []
    for section in sorted(overrides):
        lines.append("[%s]" % section)
        lines.extend("%s=%s" % (k, v) for k, v in sorted(overrides[section].items()))
        lines.append("")
    with open(os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as fh:
        fh.write("\n".join(lines))


def ports(base):
    return {"http": base, "rtsp": base + 1, "rtmp": base + 2, "api": base + 10}


def schedule_items(state):
    mode = MODES[state["mode"]]
    if state["schedule"] == "all":
        return [{"dh": "%d,%d" % (d, h), "ty": mode, "fps": 25, "q": "H"}
                for d in range(7) for h in range(24)]
    now = datetime.datetime.now()
    today = now.weekday()            # 0 = Thứ Hai, khớp RecordPolicy::toRecordScheduleDay
    if state["schedule"] == "flip":
        # Giờ hiện tại ghi theo mode, giờ kế tiếp NoRecord: kiểm chuyển mốc giờ (REC-08).
        items = [{"dh": "%d,%d" % (today, now.hour), "ty": mode, "fps": 25, "q": "H"}]
        nxt = now + datetime.timedelta(hours=1)
        items.append({"dh": "%d,%d" % (nxt.weekday(), nxt.hour), "ty": 0, "fps": 25, "q": "H"})
        return items
    if state["schedule"] == "otherday":
        other = (today + 3) % 7
        return [{"dh": "%d,%d" % (other, h), "ty": mode, "fps": 25, "q": "H"} for h in range(24)]
    raise SystemExit("schedule không hợp lệ: %s" % state["schedule"])


def watermark_template():
    return {
        "id": WATERMARK_ID,
        "name": "E2E watermark",
        "displayMode": "REPEATED",
        "repeatEnabled": True,
        "canvas": {"width": 1280, "height": 720},
        "components": [{
            "type": "TEXT", "id": "txt_1", "name": "Text",
            "x": 400, "y": 300, "scale": 1.0, "rotation": -20.0, "opacity": 0.6,
            "repeatEnabled": True, "gapX": 360, "gapY": 220,
            "text": "E2E WATERMARK", "fontFamily": "Arial", "fontSize": 40,
            "fontWeight": 700, "color": "#ff0000", "zindex": 0,
        }],
    }


def camera_entry(env, state):
    # Thiết bị điều khiển (ONVIF/PSIA, mock hoặc camera thật). Không đặt thì camera là RTSP thuần như trước.
    ctl = {
        "ip": os.environ.get("E2E_CAMERA_IP", ""),
        "port": int(os.environ.get("E2E_CAMERA_HTTP_PORT", "80")),
        "user": os.environ.get("E2E_CAMERA_USER", ""),
        "password": os.environ.get("E2E_CAMERA_PASS", ""),
        "protocol": os.environ.get("E2E_CONTROL_PROTOCOL", "AUTO"),
        "ptz": os.environ.get("E2E_CAMERA_PTZ", "0") == "1",
    }
    entry = {
        "id": CAMERA_ID,
        "name": "E2E camera",
        "ip": ctl["ip"] or "127.0.0.1",
        "httpPort": ctl["port"],
        "manufacturer": "Generic",
        "model": "RTSP",
        "username": ctl["user"],
        "password": ctl["password"],
        "enabled": True,
        "enablePtzControl": ctl["ptz"],
        "priMediaServerId": NODE_ID,
        "primaryStreamId": MAIN_ID,
        "primaryStreamUrl": env["main"],
        "secondaryStreamId": SUB_ID,
        "secondaryStreamUrl": env["sub"],
        "recordingConfig": {
            "enableRecording": state["enable_recording"],
            "recordingSchedule": schedule_items(state),
            "keepArchivedMinForAuto": False,
            "keepArchivedMinFor": state["keep_min_hours"],
            "keepArchivedMaxForAuto": False,
            "keepArchivedMaxFor": state["keep_max_hours"],
            "motionPreRecordSec": 5,
            "motionPostRecordSec": 5,
        },
        "cameraAdvanceConfig": {
            "streamSettings": {
                "disableMainStream": state["disable_main"],
                "disableSubStream": state["disable_sub"],
                "notRecordMainStream": state["no_record_main"],
                "notRecordSubStream": state["no_record_sub"],
                "disableAudio": state["disable_audio"],
            },
            "mediaStreaming": {"rtpTransport": state["rtp_transport"], "useDefaultMediaPort": True},
            "webPage": {"useDefaultWebPort": True},
            "ptzSetting": {"enablePTZControl": ctl["ptz"]},
            "onvif": {"mainStream": "AUTO", "subStream": "AUTO"},
            "control": {"protocol": ctl["protocol"]},
        },
        "motionDetectConfig": {
            "clientEnabled": state["motion"],
            "chooseStream": "SECONDARY",
            # ROI toàn khung; định dạng mask xem manager/Motion.
            "value": "",
        },
    }
    if state["overlay"]:
        entry["watermarkTemplateId"] = WATERMARK_ID
        entry["watermarkExcludedRoleIds"] = [ROLE_ADMIN]
        entry["privacyConfig"] = {
            "enabled": True,
            "excludedRoleIds": [ROLE_ADMIN, ROLE_WM_ONLY],
            # Mảng JSON, không phải chuỗi: server tự serialize (GET_OPTION_PROPERTY_AS_STRING).
            # maskType: SOLID | BLUR | PIXELATE.
            "regions": [{
                "id": "region-1", "name": "E2E mask", "polygonMode": True, "maskType": "SOLID",
                "points": [{"x": 0.05, "y": 0.05}, {"x": 0.35, "y": 0.05},
                           {"x": 0.35, "y": 0.35}, {"x": 0.05, "y": 0.35}],
            }],
        }
    return entry


SPEAKER_ID = "e2e-speaker"


def speaker_entry():
    # Loa IP điều khiển qua ONVIF/ISAPI/PSIA (mock hoặc thiết bị thật), chỉ khi đặt E2E_SPEAKER_IP.
    # Đọc lại mỗi lần ghi cấu hình: đổi biến rồi chạy `camera` để nạp lại.
    return {
        "id": SPEAKER_ID,
        "name": "E2E speaker",
        "ip": os.environ["E2E_SPEAKER_IP"],
        "httpPort": int(os.environ.get("E2E_SPEAKER_HTTP_PORT", "80")),
        "manufacturer": os.environ.get("E2E_SPEAKER_MANUFACTURER", "Generic"),
        "model": "IP speaker",
        "username": os.environ.get("E2E_SPEAKER_USER", ""),
        "password": os.environ.get("E2E_SPEAKER_PASS", ""),
        "enabled": True,
        "priMediaServerId": NODE_ID,
        "speakerAdvanceConfig": {
            "control": {"protocol": os.environ.get("E2E_SPEAKER_PROTOCOL", "AUTO")},
            "onvif": {"enableTimeSync": os.environ.get("E2E_SPEAKER_TIME_SYNC", "1") == "1"},
        },
    }


def api_config(work, env, state):
    p = json.load(open(os.path.join(work, "inventory.json")))["ports"]
    server = server_entry(NODE_ID, {"http": p["http"], "rtsp": p["rtsp"], "rtmp": p["rtmp"]})
    return {
        "media_server": {
            "serverGroupId": 0, "maxConfigCameras": 16, "maxStream": 64,
            "maxStreamPerCamera": 8, "projectId": PROJECT_ID,
            "unlimitedStream": True, "unlimitedStreamPerCamera": True,
        },
        "list_media_server": [server],
        "watermark_configs": [watermark_template()],
        "devices": [camera_entry(env, state)] if state["present"] else [],
        "speakers": [speaker_entry()] if os.environ.get("E2E_SPEAKER_IP") else [],
    }


def read_env():
    main, sub = os.environ.get("E2E_RTSP_MAIN", ""), os.environ.get("E2E_RTSP_SUB", "")
    if not main or not sub:
        raise SystemExit("Thiếu E2E_RTSP_MAIN / E2E_RTSP_SUB")
    return {"main": main, "sub": sub}


def gen_keys(work):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    priv = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())
    pub = key.public_key().public_bytes(serialization.Encoding.PEM,
                                        serialization.PublicFormat.SubjectPublicKeyInfo)
    path = os.path.join(work, "jwt_private.pem")
    with open(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as fh:
        fh.write(priv)
    return base64.b64encode(pub).decode()


def layout(repo, work, args):
    env = read_env()
    os.makedirs(work, exist_ok=True)
    for sub in ("www", "record", "log", "db", "restore", "decrypt", "warm", "extract"):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    for name in ("updates", "mserver_updates"):
        dst = os.path.join(work, name)
        if not os.path.isdir(dst):
            import shutil
            shutil.copytree(os.path.join(repo, "migration", name), dst)

    p = ports(args.port_base)
    api_secret = secrets.token_hex(16)
    manager_secret = secrets.token_hex(16)
    public_key = gen_keys(work)
    overrides = {
        "general": {"mediaServerId": NODE_ID},
        "api": {"secret": api_secret, "apiDebug": 0, "extractRoot": work + "/extract/"},
        "http": {"port": p["http"], "sslport": 0, "rootPath": work + "/www", "keepAliveSecond": 30},
        "rtsp": {"port": p["rtsp"], "sslport": 0},
        "rtmp": {"port": p["rtmp"], "sslport": 0},
        "rtp_proxy": {"port": 0},
        "rtc": {"port": 0, "tcpPort": 0, "signalingPort": 0, "signalingSslPort": 0,
                "icePort": 0, "iceTcpPort": 0},
        "srt": {"port": 0},
        "shell": {"port": 0},
        "ssdp": {"port": 0},
        "protocol": {"mp4_save_path": work + "/record", "hls_save_path": work + "/www/live",
                     "mp4_max_second": args.segment_sec, "enable_rtmp": 1, "enable_ts": 1,
                     "enable_hls_fmp4": 1, "enable_fmp4": 1, "enable_hls": 1},
        # Template test không có overlayAssetId (SVG do VMS cấp qua watermark/assets), nên
        # render watermark bằng component. Chế độ asset là kịch bản OVL-12 riêng.
        "overlay": {"use_watermark_asset": 0, "overlay_root": work + "/www/overlay/"},
        "record": {"timefileIdleCloseSec": 60},
        "hook": {"enable": 1, "api_url": "http://127.0.0.1:%d" % p["api"],
                 "report_interval": 10.0, "alive_interval": 5.0, "timeoutSec": 5, "retry": 0},
        "database": {"db_save_path": work + "/db", "updates_save_path": work + "/updates",
                     "mserver_updates_save_path": work + "/mserver_updates",
                     "keep_migration_sql_files": 1, "enable_sync_db": 0},
        "manager": {"serverLocationId": 0, "apiSecret": manager_secret,
                    "certSavePath": work + "/www/certs", "enableAuthorize": 1,
                    "jwtPublicKey": public_key, "failoverActiveDelaySec": 15},
        "storage": {"restore_save_path": work + "/restore",
                    "decrypt_save_path": work + "/decrypt",
                    "tiering_split_granularity_seconds": 60,
                    "restore_ttl_seconds": args.restore_ttl,
                    "pool_health_check_interval_seconds": 10,
                    "auto_restore_on_record_access": 0},
    }
    write_ini(os.path.join(work, "config.ini"), overrides)

    write_json(os.path.join(work, "inventory.json"), {
        "ports": p, "node_id": NODE_ID, "project_id": PROJECT_ID, "camera_id": CAMERA_ID,
        "main_id": MAIN_ID, "sub_id": SUB_ID,
        "roles": {"admin": ROLE_ADMIN, "viewer": ROLE_VIEWER, "wm_only": ROLE_WM_ONLY},
        "segment_sec": args.segment_sec, "restore_ttl": args.restore_ttl,
    })
    # Secret dùng cho /index/api và X-Secret-Key; chỉ harness đọc, quyền 600.
    sec_path = os.path.join(work, "secrets.json")
    with open(os.open(sec_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as fh:
        json.dump({"api_secret": api_secret, "manager_secret": manager_secret}, fh)
    write_json(os.path.join(work, "deny_users.json"), [])
    state = dict(DEFAULT_STATE)
    write_json(os.path.join(work, "camera_state.json"), state)
    write_camera(work, env, state)
    print("layout xong: %s (http=%d rtsp=%d rtmp=%d mock=%d)"
          % (work, p["http"], p["rtsp"], p["rtmp"], p["api"]))


def write_camera(work, env, state):
    path = os.path.join(work, "api-config.json")
    write_json(path, api_config(work, env, state))
    os.chmod(path, 0o600)   # chứa URL RTSP có mật khẩu


def camera(repo, work, args):
    env = read_env()
    state = dict(DEFAULT_STATE)
    state.update(json.load(open(os.path.join(work, "camera_state.json"))))
    for key in DEFAULT_STATE:
        val = getattr(args, key, None)
        if val is not None:
            state[key] = val
    write_json(os.path.join(work, "camera_state.json"), state)
    write_camera(work, env, state)
    print("camera: " + json.dumps(state, sort_keys=True))


def tri(value):
    return {"1": True, "0": False, "true": True, "false": False}[value.lower()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo")
    ap.add_argument("work")
    ap.add_argument("command", choices=["layout", "camera"])
    ap.add_argument("--port-base", type=int, default=19080)
    ap.add_argument("--segment-sec", type=int, default=60)
    ap.add_argument("--restore-ttl", type=int, default=600)
    ap.add_argument("--mode", choices=sorted(MODES))
    ap.add_argument("--schedule", choices=["all", "flip", "otherday"])
    for flag in ("enable_recording", "overlay", "disable_main", "disable_sub", "no_record_main",
                 "no_record_sub", "disable_audio", "motion", "present"):
        ap.add_argument("--" + flag.replace("_", "-"), dest=flag, type=tri, metavar="0|1")
    ap.add_argument("--rtp-transport", dest="rtp_transport", choices=["TCP", "UDP", "AUTO"])
    ap.add_argument("--keep-max-hours", dest="keep_max_hours", type=int)
    ap.add_argument("--keep-min-hours", dest="keep_min_hours", type=int)
    args = ap.parse_args()
    (layout if args.command == "layout" else camera)(args.repo, args.work, args)


if __name__ == "__main__":
    main()
