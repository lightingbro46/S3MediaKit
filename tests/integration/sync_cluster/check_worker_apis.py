#!/usr/bin/env python3
"""Checks the /media/* APIs that run blocking work on the API worker pool (docs/features/worker.md, step 6).

Runs against one node started by setup_nodes.py (manager.enableAuthorize=0) and the mock API.

Usage: check_worker_apis.py [base_url] [media_server_id]
"""
import json
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:18081"
NODE_ID = sys.argv[2] if len(sys.argv) > 2 else "node-a"
BLACKHOLE_IP = "10.255.255.1"  # not routed: ONVIF connect waits for its timeout

failures = []


def call(path, params=None, timeout=120):
    url = BASE + path + ("?" + urllib.parse.urlencode(params) if params else "")
    start = time.time()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            status, body = resp.status, resp.read()
    except urllib.error.HTTPError as err:
        status, body = err.code, err.read()
    elapsed = time.time() - start
    try:
        data = json.loads(body)
    except ValueError:
        data = {"raw": body[:200].decode(errors="replace")}
    return status, data, elapsed


def check(name, ok, detail=""):
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))
    if not ok:
        failures.append(name)


# 1. A slow ONVIF discovery must not block other requests served by the same pollers.
slow = {}


def slow_discovery():
    slow["result"] = call("/media/mserver/device/discovery",
                          {"address": BLACKHOLE_IP, "port": 80, "defaultPort": 1})


worker = threading.Thread(target=slow_discovery)
worker.start()
time.sleep(0.3)
latencies = []
while worker.is_alive():
    # new connection each time, so requests land on every HTTP poller, spread over the whole discovery
    latencies.append(call("/media/mserver/healthcheck")[2])
    time.sleep(0.05)
worker.join()
status, data, elapsed = slow["result"]
check("discovery(onvif) answered with an error", status != 200 or data.get("code") != 0,
      "status=%s code=%s elapsed=%.1fs" % (status, data.get("code"), elapsed))
check("discovery(onvif) was slow enough to test blocking", elapsed > 1.0, "elapsed=%.1fs" % elapsed)
check("healthcheck stays fast during discovery", latencies and max(latencies) < 0.5,
      "requests=%d max=%.3fs" % (len(latencies), max(latencies) if latencies else -1))

# 2. RTSP discovery keeps the asynchronous probe path.
status, data, elapsed = call("/media/mserver/device/discovery",
                             {"address": "rtsp://127.0.0.1:9/none", "port": 554, "defaultPort": 1})
check("discovery(rtsp) answers", status != 200 or data.get("code") != 0,
      "status=%s code=%s elapsed=%.1fs" % (status, data.get("code"), elapsed))

# 3. Synchronous subnet scan runs on the worker and still returns the collected list.
status, data, elapsed = call("/media/mserver/device/subnetScan",
                             {"startIp": "127.0.0.1", "endIp": "127.0.0.2", "port": 9, "defaultPort": 0})
check("subnetScan returns a list", status == 200 and isinstance(data.get("data"), list),
      "status=%s data=%s elapsed=%.1fs" % (status, str(data.get("data"))[:80], elapsed))
status, data, _ = call("/media/mserver/device/subnetScan",
                       {"startIp": "bad", "endIp": "127.0.0.2", "port": 9, "defaultPort": 0})
check("subnetScan validation unchanged", status != 200, "status=%s code=%s" % (status, data.get("code")))

# 4. register: config/cert changes are applied, repeated calls are idempotent.
base = {"domain": "127.0.0.1", "ip": "127.0.0.1", "httpPort": 18090, "httpsPort": 0, "preferSSL": 0,
        # unique per run, so the first call always has something to save
        "apiSecret": "", "mediaServerDomain": "worker-test-%d.local" % int(time.time()),
        "mediaServerCert": "-----BEGIN CERTIFICATE-----\nWORKERTEST\n-----END CERTIFICATE-----\n"}
status, data, _ = call("/media/mserver/register", dict(base, mediaServerId="someone-else"))
check("register(other id) is a no-op", status == 200 and data.get("data", {}).get("changed") == 0, str(data))
status, data, _ = call("/media/mserver/register", dict(base, mediaServerId=NODE_ID))
check("register saves domain and certificate", status == 200 and data.get("data", {}).get("changed", 0) >= 1, str(data))
status, data, _ = call("/media/mserver/register", dict(base, mediaServerId=NODE_ID))
check("register again changes nothing", status == 200 and data.get("data", {}).get("changed") == 0, str(data))
status, data, _ = call("/media/mserver/register", {"mediaServerId": NODE_ID})
check("register validation unchanged", status != 200, "status=%s code=%s" % (status, data.get("code")))

print("FAILED: %s" % failures if failures else "ALL PASSED")
sys.exit(1 if failures else 0)
