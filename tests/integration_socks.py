"""验证浏览器真实SOCKS5远端域名解析、出口及断代理拒绝。日期：2026-11-04。"""

import argparse
import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path
from integration_browser import Client, FixtureServer
from fixtures.socks_proxy import SocksServer


# 用途：使用无法由本机DNS解析的测试域名核验真实代理，不以启动参数判定通过；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    target = FixtureServer(public_host="awb-fixture.invalid")
    proxy = SocksServer(target.server_port)
    threading.Thread(target=target.serve_forever, daemon=True).start()
    threading.Thread(target=proxy.serve_forever, daemon=True).start()
    client = Client(args.executable.resolve(), output / "profiles", args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed",
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
              "scope": "Unauthenticated SOCKS5; full DNS/WebRTC/IPv6 leak matrix belongs to T091"}
    closed = False
    try:
        origin = f"http://awb-fixture.invalid:{target.server_port}"
        for bad in ["socks5://user:password@127.0.0.1:1234", "socks5://127.0.0.1:0",
                    "socks5://127.0.0.1:1234/path", "socks4://127.0.0.1:1234"]:
            client.call("profile_create", {"name": "拒绝不支持代理", "allowedOrigins": [origin], "proxyServer": bad}, "PROXY_UNSUPPORTED")
        p = client.call("profile_create", {"name": "SOCKS5域名出口", "allowedOrigins": [origin],
                                           "proxyServer": f"socks5://127.0.0.1:{proxy.server_address[1]}"})
        client.call("browser_start", {"profileId": p["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": p["id"], "owner": "socks", "ttlMs": 300000})
        base = {"profileId": p["id"], "lease": lease}
        client.call("browser_navigate", {**base, "url": origin + "/health"})
        client.call("page_wait_text", {**base, "selector": "body", "expected": '{"ready": true}', "timeoutMs": 5000})
        assert any(item["host"] == "awb-fixture.invalid" and item["addressType"] == 3 for item in proxy.forwarded)
        client.call("browser_navigate", {**base, "url": origin + "/login"})
        for selector, value in [("#username", "socks-registered-user"), ("#password", "secret-canary-socks-456")]:
            ref = client.call("page_locate", {**base, "selector": selector})["ref"]
            client.call("page_fill", {**base, "ref": ref, "value": value})
        for selector, expected in [("#register", "registered"), ("#login", "logged-in")]:
            ref = client.call("page_locate", {**base, "selector": selector})["ref"]
            client.call("page_click", {**base, "ref": ref})
            client.call("page_wait_text", {**base, "selector": "#result", "expected": expected, "timeoutMs": 5000})
        assert target.registration_count == 1 and target.login_count == 1
        proxy.disconnect()
        proxy.shutdown()
        proxy.server_close()
        closed = True
        client.call("browser_navigate", {**base, "url": origin + "/health?disconnect=1"}, "NAVIGATION_FAILED")
        client.call("browser_stop", {"profileId": p["id"]})
        report.update(result="passed", remoteDnsDomainSeen=True, noSilentDirectFallback=True, forwarded=proxy.forwarded,
                      registrationCount=target.registration_count, loginCount=target.login_count, activeTunnelsDisconnected=True)
    finally:
        try:
            client.close()
        finally:
            if not closed:
                proxy.disconnect()
                proxy.shutdown()
                proxy.server_close()
            target.shutdown()
            target.server_close()
            report["serverFacts"] = {"registrationCount": target.registration_count, "loginCount": target.login_count}
            report["commands"] = client.events
            (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()
