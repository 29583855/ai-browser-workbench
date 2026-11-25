"""真实浏览器验证长目录中Cookie/LS/IDB/CacheStorage及重开持久性。日期：2026-11-04。"""

import argparse
import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path

from integration_browser import Client, FixtureServer


# 用途：对长路径执行真实注册登录和重开，禁止业务失败后补提交；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [], "paths": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    client = None
    try:
        for number, target in enumerate([160, 200, 240]):
            prefix = str(output / ("中文 空格存储" + str(number)))
            root = Path(prefix + "x" * max(0, target - 33 - len(prefix)))
            client = Client(args.executable.resolve(), root, args.browser.resolve())
            profile = client.call("profile_create", {"name": "长路径真实存储", "allowedOrigins": [origin]})
            identity = profile["id"]
            profile_length = len(str(root / identity))
            assert profile_length >= target, (profile_length, target)
            client.call("browser_start", {"profileId": identity, "headless": True})
            lease = client.call("lease_acquire", {"profileId": identity, "owner": "storage-paths", "ttlMs": 300000})
            base = {"profileId": identity, "lease": lease}

            # 用途：所有动作复用真实租约和服务校验，不直连产品CDP；日期：2026-11-04。
            def action(command, extra=None):
                return client.call(command, {**base, **(extra or {})})

            # 用途：每次重新定位控件再派发，禁止导航后复用旧引用；日期：2026-11-04。
            def element(command, selector, extra=None):
                reference = action("page_locate", {"selector": selector})["ref"]
                return action(command, {"ref": reference, **(extra or {})})

            # 用途：只读等待业务及存储完成，不重试注册/登录；日期：2026-11-04。
            def wait(selector, expected):
                action("page_wait_text", {"selector": selector, "expected": expected, "timeoutMs": 5000})

            username = "storage-path-user-" + str(number)
            password = "secret-canary-storage-paths-" + str(number)
            action("browser_navigate", {"url": origin + "/login"})
            wait("#sw", "sw-ready")
            element("page_fill", "#username", {"value": username})
            element("page_fill", "#password", {"value": password})
            element("page_click", "#register")
            wait("#result", "registered")
            element("page_click", "#login")
            wait("#result", "logged-in")
            wait("#identity", username)
            storage = json.loads(element("page_text", "#storage")["text"])
            assert storage == {"local": username, "indexed": username, "cache": ["awb-" + username]}, storage
            action("browser_navigate", {"url": origin + "/login"})
            wait("#identity", username)
            client.call("browser_stop", {"profileId": identity})
            client.close()
            client = Client(args.executable.resolve(), root, args.browser.resolve())
            client.call("browser_start", {"profileId": identity, "headless": True})
            base["lease"] = client.call("lease_acquire", {"profileId": identity, "owner": "storage-paths-reopen", "ttlMs": 300000})
            action("browser_navigate", {"url": origin + "/login"})
            wait("#sw", "sw-ready")
            wait("#identity", username)
            expected = json.dumps(storage, separators=(",", ":"))
            wait("#storage", expected)
            client.call("browser_stop", {"profileId": identity})
            client.close()
            client = None
            report["paths"].append({"profileUtf16Units": profile_length, "containsUnicodeAndSpace": True,
                                    "cookieLocalIndexedCacheAndWorkerVerified": True, "restartVerified": True})
            report["checks"].append("real-storage-register-login-navigate-reopen-path-" + str(target))
        assert server.registration_count == 3 and server.login_count == 3
        report["serverFacts"] = {"registrationCount": server.registration_count, "loginCount": server.login_count}
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        if client:
            try:
                report["failureObservation"] = {selector: element("page_text", selector)["text"]
                    for selector in ["#result", "#identity", "#storage", "#fixture-diagnostic", "#sw"]}
            except Exception:
                report["failureObservationUnavailable"] = True
    finally:
        if client:
            client.close()
        server.shutdown()
        server.server_close()
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "error": report.get("error")}))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
