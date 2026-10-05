#!/usr/bin/env python3
"""Chạy thật checklist của video config rule trên MediaServer + onvif_mock.py.

Được run_stress.sh gọi trong kịch bản `videorule` (1 node). Mỗi mục của checklist
(tests/integration/stress_vms/README.md, kịch bản `videorule`) chạy trên một camera riêng, song
song với nhau, nên cả lượt mất khoảng một thời hạn rule cộng vài chu kỳ đồng bộ profile.

    --part rule : áp rule, hết hạn, gia hạn, rút mạng lúc hết hạn, onvifSetVideoConfigs bị hoãn
    --part auth : node đã restart với auth.enableAuthorize=1 -> kiểm X-Secret-Key

Dùng: check_video_rule.py <work_dir> --part rule|auth [--duration 120]
Thoát 0 khi mọi mục đạt; in từng mục PASS/FAIL và ghi <work>/video_rule_<part>.json.
"""
import argparse
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from setup_stress import VIDEO_RULE_SECRET, device_ip  # noqa: E402

BASELINE = {"encoding": "H264", "width": 1920, "height": 1080, "fps": 25, "bitrate": 4096}
RULE = {"width": 1280, "height": 720, "fps": 15, "bitrate": 1024}
USER = {"encoding": "H264", "width": 704, "height": 576, "fps": 10, "bitrate": 768}
# Một chu kỳ đồng bộ profile là ~60 s; chừa thêm cho lần thử lại và độ trễ webhook.
CONFIRM_SEC = 150


class Ctx(object):
    def __init__(self, work, duration):
        inv = json.load(open(os.path.join(work, "inventory.json")))
        self.base = "http://127.0.0.1:%d" % inv["ports"]["node-a"]["http"]
        self.onvif = "http://127.0.0.1:%d" % inv["onvif_port"]
        self.api = "http://127.0.0.1:%d" % inv["api_port"]
        self.fault_spec = os.path.join(work, "onvif_fault_spec.json")
        self.duration = duration
        self.lock = threading.Lock()
        self.results = []

    def record(self, name, ok, detail):
        with self.lock:
            self.results.append({"check": name, "ok": bool(ok), "detail": detail})
            print("  %s %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else " -> %s" % detail),
                  flush=True)
        return ok


def http(method, url, body=None, headers=None, timeout=15):
    data = None
    hdrs = dict(headers or {})
    if body is not None:
        data = json.dumps(body).encode()
        hdrs.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        status = exc.code
    try:
        return status, json.loads(raw) if raw else None
    except ValueError:
        return status, raw.decode("utf-8", "replace")


def apply_rule(ctx, log_id, device_id, config, headers=None):
    cam = dict(config, deviceId=device_id, applyDuration=ctx.duration)
    return http("POST", ctx.base + "/media/mserver/device/onvifApplyVideoConfigRule",
                {"logId": log_id, "cameras": [cam]},
                headers if headers is not None else {"X-Secret-Key": VIDEO_RULE_SECRET})


def rule_result(ctx, log_id, headers=None):
    return http("GET", ctx.base + "/media/mserver/device/onvifGetVideoConfigRuleResult?logId="
                + urllib.parse.quote(log_id),
                headers=headers if headers is not None else {"X-Secret-Key": VIDEO_RULE_SECRET})


def hooks(ctx, log_id, device_id):
    _, recorded = http("GET", ctx.api + "/_mock/hooks")
    return [h for h in recorded or []
            if isinstance(h.get("body"), dict) and h["body"].get("logId") == log_id
            and h["body"].get("deviceId") == device_id]


def encoder(ctx, device_id):
    _, state = http("GET", ctx.onvif + "/_mock/encoders")
    return ((state or {}).get(device_ip(int(device_id.split("-")[1]))) or {}).get("Profile_1")


def matches(cfg, want):
    return cfg is not None and all(cfg.get(k) == v for k, v in want.items())


def wait_until(fn, timeout, step=2.0):
    deadline = time.time() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.time() >= deadline:
            return value
        time.sleep(step)


def set_fault(ctx, device_id, offline):
    spec = json.load(open(ctx.fault_spec))
    devices = spec.setdefault("devices", {})
    ip = device_ip(int(device_id.split("-")[1]))
    if offline:
        devices.setdefault(ip, {})["offline"] = True
    else:
        devices.get(ip, {}).pop("offline", None)
    tmp = ctx.fault_spec + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(spec, fh, indent=1)
    os.replace(tmp, ctx.fault_spec)


def accept(ctx, name, log_id, device_id, config=RULE):
    status, body = apply_rule(ctx, log_id, device_id, config)
    results = ((body or {}).get("data") or {}).get("results") or [] if isinstance(body, dict) else []
    ok = status == 200 and body.get("code") == 0 and len(results) == 1 and results[0].get("code") == 0
    ctx.record("%s: %s được nhận" % (name, log_id), ok, "HTTP %s %s" % (status, body))
    return ok


def confirmed(ctx, name, log_id, device_id):
    """Webhook SUCCESS đúng một lần, kèm Idempotency-Key, và API tra cứu cũng ra SUCCESS."""
    got = wait_until(lambda: hooks(ctx, log_id, device_id), CONFIRM_SEC)
    ok = len(got) == 1 and got[0]["body"].get("status") == "SUCCESS" and \
        got[0]["body"].get("errorCode") is None and \
        got[0].get("idempotency_key") == "%s|%s" % (log_id, device_id)
    ctx.record("%s: webhook SUCCESS cho %s" % (name, log_id), ok, got)
    _, body = rule_result(ctx, log_id)
    rows = ((body or {}).get("data") or {}).get("results") or []
    ctx.record("%s: tra cứu %s ra SUCCESS" % (name, log_id),
               len(rows) == 1 and rows[0].get("status") == "SUCCESS", body)
    return ok


# ------------------------------------------------------------------ các mục checklist
def case_apply_and_expire(ctx, cam):
    name = "áp rule rồi hết hạn"
    started = time.time()
    if not accept(ctx, name, "vr-apply", cam):
        return
    confirmed(ctx, name, "vr-apply", cam)
    ctx.record("%s: camera mang cấu hình rule" % name, matches(encoder(ctx, cam), RULE), encoder(ctx, cam))
    remaining = started + ctx.duration - time.time()
    if remaining > 5:
        time.sleep(remaining - 5)
    ctx.record("%s: chưa hết hạn thì chưa rollback" % name, matches(encoder(ctx, cam), RULE),
               encoder(ctx, cam))
    back = wait_until(lambda: matches(encoder(ctx, cam), BASELINE), CONFIRM_SEC + 5)
    ctx.record("%s: hết hạn thì về baseline" % name, back, encoder(ctx, cam))


def case_extend(ctx, cam):
    name = "gửi lại cùng cấu hình là gia hạn"
    started = time.time()
    if not accept(ctx, name, "vr-extend-1", cam):
        return
    confirmed(ctx, name, "vr-extend-1", cam)
    # Gửi lại giữa chừng: hạn mới = lúc gửi lại + duration, muộn hơn hạn cũ.
    wait = started + ctx.duration / 2.0 - time.time()
    if wait > 0:
        time.sleep(wait)
    resent = time.time()
    sets_before = (encoder(ctx, cam) or {}).get("sets")
    if not accept(ctx, name, "vr-extend-2", cam):
        return
    confirmed(ctx, name, "vr-extend-2", cam)
    # Qua hạn cũ mà camera vẫn phải giữ cấu hình rule. Hạn mới là started + 1.5 * duration,
    # nên điểm kiểm phải nằm giữa hai hạn, cách cả hai một khoảng an toàn.
    wait = started + ctx.duration + min(20, ctx.duration / 4.0) - time.time()
    if wait > 0:
        time.sleep(wait)
    now_cfg = encoder(ctx, cam)
    ctx.record("%s: qua hạn cũ vẫn giữ cấu hình rule" % name, matches(now_cfg, RULE), now_cfg)
    ctx.record("%s: gia hạn không đẩy lại cấu hình" % name,
               now_cfg is not None and now_cfg.get("sets") == sets_before,
               "sets trước=%s sau=%s" % (sets_before, (now_cfg or {}).get("sets")))
    wait = resent + ctx.duration - 5 - time.time()
    if wait > 0:
        time.sleep(wait)
    back = wait_until(lambda: matches(encoder(ctx, cam), BASELINE), CONFIRM_SEC + 5)
    ctx.record("%s: hết hạn mới thì về baseline" % name, back, encoder(ctx, cam))


def case_offline_at_expiry(ctx, cam):
    name = "rút mạng lúc hết hạn"
    started = time.time()
    if not accept(ctx, name, "vr-offline", cam):
        return
    confirmed(ctx, name, "vr-offline", cam)
    wait = started + ctx.duration - 20 - time.time()
    if wait > 0:
        time.sleep(wait)
    set_fault(ctx, cam, True)
    try:
        # Giữ mất mạng qua hạn thêm hai chu kỳ đồng bộ.
        time.sleep(max(0.0, started + ctx.duration + 120 - time.time()))
        ctx.record("%s: lúc mất mạng camera còn cấu hình rule" % name, matches(encoder(ctx, cam), RULE),
                   encoder(ctx, cam))
    finally:
        set_fault(ctx, cam, False)
    back = wait_until(lambda: matches(encoder(ctx, cam), BASELINE), 240)
    ctx.record("%s: cắm lại thì baseline được đẩy lại" % name, back, encoder(ctx, cam))


def case_deferred_user_set(ctx, cam):
    name = "onvifSetVideoConfigs khi rule đang chạy"
    started = time.time()
    if not accept(ctx, name, "vr-deferred", cam):
        return
    confirmed(ctx, name, "vr-deferred", cam)
    sets_before = (encoder(ctx, cam) or {}).get("sets")
    query = dict(USER, deviceId=cam, token="Profile_1")
    status, body = http("GET", ctx.base + "/media/mserver/device/onvifSetVideoConfigs?"
                        + urllib.parse.urlencode(query), timeout=60)
    data = (body or {}).get("data") or {} if isinstance(body, dict) else {}
    ctx.record("%s: trả deferred=true kèm deferredUntil" % name,
               status == 200 and body.get("code") == 0 and data.get("deferred") is True and
               int(data.get("deferredUntil") or 0) >= int(started * 1000), "HTTP %s %s" % (status, body))
    # Một chu kỳ đồng bộ sau đó camera vẫn giữ rule và không bị set lại.
    time.sleep(min(70, max(0.0, started + ctx.duration - 10 - time.time())))
    now_cfg = encoder(ctx, cam)
    ctx.record("%s: camera vẫn giữ cấu hình rule, không bị set lại" % name,
               matches(now_cfg, RULE) and now_cfg.get("sets") == sets_before,
               "%s (sets trước=%s)" % (now_cfg, sets_before))
    wait = started + ctx.duration - 5 - time.time()
    if wait > 0:
        time.sleep(wait)
    back = wait_until(lambda: matches(encoder(ctx, cam), USER), CONFIRM_SEC + 5)
    ctx.record("%s: hết hạn thì về cấu hình người dùng vừa set" % name, back, encoder(ctx, cam))


def part_rule(ctx):
    cases = [(case_apply_and_expire, "cam-0000"), (case_extend, "cam-0001"),
             (case_offline_at_expiry, "cam-0002"), (case_deferred_user_set, "cam-0003")]
    for _, cam in cases:
        ctx.record("%s: baseline ban đầu" % cam, wait_until(lambda: matches(encoder(ctx, cam), BASELINE), 60),
                   encoder(ctx, cam))
    threads = []
    for fn, cam in cases:
        def run(fn=fn, cam=cam):
            try:
                fn(ctx, cam)
            except Exception as exc:  # một mục hỏng không được kéo các mục khác
                ctx.record("%s: lỗi kịch bản" % fn.__name__, False, repr(exc))
        t = threading.Thread(target=run)
        t.start()
        threads.append(t)
    for t in threads:
        t.join()


def part_auth(ctx):
    for label, headers in (("thiếu X-Secret-Key", {}), ("sai X-Secret-Key", {"X-Secret-Key": "wrong"})):
        status, body = rule_result(ctx, "vr-apply", headers=headers)
        ctx.record("tra cứu với %s -> 401" % label,
                   status == 401 and isinstance(body, dict) and body.get("code") == 901001,
                   "HTTP %s %s" % (status, body))
        status, body = apply_rule(ctx, "vr-auth", "cam-0004", RULE, headers=headers)
        ctx.record("áp rule với %s -> 401" % label,
                   status == 401 and isinstance(body, dict) and body.get("code") == 901001,
                   "HTTP %s %s" % (status, body))
    # Secret đúng: kết quả của lượt trước vẫn còn sau restart.
    status, body = rule_result(ctx, "vr-apply")
    rows = ((body or {}).get("data") or {}).get("results") or [] if isinstance(body, dict) else []
    ctx.record("tra cứu với secret đúng -> 200, kết quả còn sau restart",
               status == 200 and len(rows) == 1 and rows[0].get("status") == "SUCCESS",
               "HTTP %s %s" % (status, body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("work")
    ap.add_argument("--part", choices=["rule", "auth"], required=True)
    ap.add_argument("--duration", type=int, default=120)
    args = ap.parse_args()
    ctx = Ctx(args.work, args.duration)
    print("video config rule, phần %s (applyDuration=%ds)" % (args.part, args.duration), flush=True)
    (part_rule if args.part == "rule" else part_auth)(ctx)
    failed = [r for r in ctx.results if not r["ok"]]
    with open(os.path.join(args.work, "video_rule_%s.json" % args.part), "w") as fh:
        json.dump(ctx.results, fh, indent=1, ensure_ascii=False)
    print("video config rule, phần %s: %d/%d đạt" % (args.part, len(ctx.results) - len(failed),
                                                     len(ctx.results)), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
