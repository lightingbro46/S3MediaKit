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

# 5. Subnet scan probes IPs in parallel (searcher.max_parallel, default 8) on the discovery pool:
#    16 unrouted IPs (~20s each) finish in about two rounds, progress grows, healthcheck stays fast.
status, data, _ = call("/media/mserver/device/subnetScan/create",
                       {"startIp": "10.255.255.1", "endIp": "10.255.255.16", "port": 80, "defaultPort": 1})
key = data.get("data", {}).get("key", "")
check("subnetScan/create accepted", status == 201 and key, "status=%s data=%s" % (status, data))
if key:
    start = time.time()
    progress, finished, latencies = [], False, []
    while time.time() - start < 90:
        status, data, _ = call("/media/mserver/device/subnetScan/progress", {"key": key})
        progress.append(float(data.get("data", {}).get("progress", -1)))
        finished = bool(data.get("data", {}).get("finished"))
        if finished:
            break
        for _ in range(10):
            latencies.append(call("/media/mserver/healthcheck")[2])
            time.sleep(0.1)
    elapsed = time.time() - start
    check("subnetScan finishes in parallel rounds", finished and elapsed < 70,
          "finished=%s elapsed=%.0fs progress=%s" % (finished, elapsed, progress))
    check("subnetScan progress increases", progress == sorted(progress) and len(set(progress)) >= 2,
          "progress=%s" % progress)
    check("healthcheck stays fast during subnetScan", latencies and max(latencies) < 0.5,
          "requests=%d max=%.3fs" % (len(latencies), max(latencies) if latencies else -1))
    status, data, _ = call("/media/mserver/device/subnetScan/delete", {"key": key})
    check("subnetScan/delete", status == 200 and data.get("data", {}).get("flag") is True, str(data))

# 6. Backpressure: at most api.limit.device.discovery.maxInflight (4) discoveries at a time; the
#    rest are answered 503 CODE_SERVER_BUSY (902012) at once instead of queueing.
results = []
lock = threading.Lock()


def discovery():
    r = call("/media/mserver/device/discovery", {"address": BLACKHOLE_IP, "port": 80, "defaultPort": 1})
    with lock:
        results.append(r)


threads = [threading.Thread(target=discovery) for _ in range(6)]
for t in threads:
    t.start()
for t in threads:
    t.join()
busy = [r for r in results if r[0] == 503 and r[1].get("code") == 902012]
slow_ones = [r for r in results if r[2] > 5]
check("discovery over the limit gets 503/902012", len(busy) == 2 and all(r[2] < 1.0 for r in busy),
      "statuses=%s" % [(r[0], r[1].get("code"), round(r[2], 1)) for r in results])
check("discovery within the limit still runs", len(slow_ones) == 4, "slow=%d" % len(slow_ones))

print("FAILED: %s" % failures if failures else "ALL PASSED")
sys.exit(1 if failures else 0)
