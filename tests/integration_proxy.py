"""验证Chromium真实HTTP代理出口和断代理不静默直连。日期：2026-11-03。"""

import argparse
import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path
from integration_browser import Client, FixtureServer
from fixtures.http_proxy import ProxyServer


# 用途：验证真实代理出口和断线拒绝，并记录实际执行时间；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = args.output.resolve() / "proxy-profiles"
    assert not root.exists(), "Use a fresh evidence directory"
    target = FixtureServer()
    proxy = ProxyServer(target.server_port)
    target_worker = threading.Thread(target=target.serve_forever, daemon=True)
    proxy_worker = threading.Thread(target=proxy.serve_forever, daemon=True)
    target_worker.start()
    proxy_worker.start()
    client = Client(args.executable.resolve(), root, args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "scope": "unauthenticated HTTP proxy only",
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    proxy_closed = False
    try:
        origin = f"http://127.0.0.1:{target.server_port}"
        client.call("profile_create", {"name": "禁止假支持HTTPS", "allowedOrigins": [origin],
                                        "proxyServer": "https://127.0.0.1:1234"}, "PROXY_UNSUPPORTED")
        profile = client.call("profile_create", {"name": "HTTP代理出口", "allowedOrigins": [origin],
                                                 "proxyServer": f"http://127.0.0.1:{proxy.server_port}"})
        profile_id = profile["id"]
        client.call("browser_start", {"profileId": profile_id, "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile_id, "owner": "proxy-test", "ttlMs": 300000})
        base = {"profileId": profile_id, "lease": lease}
        client.call("browser_navigate", {**base, "url": origin + "/health"})
        client.call("page_wait_text", {**base, "selector": "body", "expected": '{"ready": true}', "timeoutMs": 5000})
        assert {"path": "/health", "status": 200} in proxy.forwarded, "Browser bypassed proxy"
        proxy.shutdown()
        proxy.server_close()
        proxy_closed = True
        client.call("browser_navigate", {**base, "url": origin + "/health?after-proxy-stop=1"}, "NAVIGATION_FAILED")
        stopped = client.call("browser_stop", {"profileId": profile_id})
        assert stopped["jobEmptyConfirmed"]
        report.update({"result": "passed", "forwarded": proxy.forwarded,
                       "proxyFailure": "NAVIGATION_FAILED", "noSilentDirectFallback": True})
    finally:
        try:
            client.close()
        finally:
            if not proxy_closed:
                proxy.shutdown()
                proxy.server_close()
            target.shutdown()
            target.server_close()
            report["commands"] = client.events
            (args.output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": report["result"], "report": str(args.output / "report.json")}))


if __name__ == "__main__":
    main()
