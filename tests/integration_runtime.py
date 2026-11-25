"""验证UA、语言、时区的持久化及浏览器/网络真实生效值。日期：2026-11-04。"""

import argparse
import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path
from integration_browser import Client, FixtureServer


# 用途：读取真实浏览器页面返回的环境JSON，不相信仅存储的请求配置；日期：2026-11-04。
def read_json(client, base, url, selector):
    client.call("browser_navigate", {**base, "url": url})
    reference = client.call("page_locate", {**base, "selector": selector})["ref"]
    return json.loads(client.call("page_text", {**base, "ref": reference})["text"])


# 用途：双环境配置、请求标头、重启持久化与非法配置拒绝链；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "profiles"
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    client = Client(args.executable.resolve(), root, args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "effective": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        origin = f"http://127.0.0.1:{server.server_port}"
        template = {"name": "区域配置测试", "allowedOrigins": [origin]}
        for bad in [{"languages": []}, {"languages": ["en-US", "en-US"]}, {"languages": ["en_US"]},
                    {"userAgent": "ua\r\nInjected: 1"}, {"timezone": "../../ bad"}, {"unrecognized": True}]:
            client.call("profile_create", {**template, "runtimeSettings": bad}, "INVALID_ARGUMENTS")
        profiles = []
        for settings in [{"userAgent": "AWB-QA/1.0 TestAgent-A", "languages": ["zh-CN", "zh"], "timezone": "Asia/Shanghai"},
                         {"userAgent": "AWB-QA/1.0 TestAgent-B", "languages": ["en-US", "en"], "timezone": "America/New_York"}]:
            p = client.call("profile_create", {**template, "runtimeSettings": settings})
            status = client.call("browser_start", {"profileId": p["id"], "headless": True})
            assert status["runtime"]["requested"] == settings and status["runtime"]["effective"] == settings
            lease = client.call("lease_acquire", {"profileId": p["id"], "owner": "runtime", "ttlMs": 300000})
            base = {"profileId": p["id"], "lease": lease}
            actual = read_json(client, base, origin + "/runtime", "#runtime")
            assert actual == settings
            headers = read_json(client, base, origin + "/runtime-headers", "body")
            assert headers["userAgent"] == settings["userAgent"]
            assert headers["acceptLanguage"].split(",")[0] == settings["languages"][0]
            report["effective"].append({"requested": settings, "actualPage": actual, "headers": headers})
            profiles.append(p)
        for p in profiles:
            client.call("browser_stop", {"profileId": p["id"]})
        report["eventsBeforeRestart"] = client.events
        client.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        saved = client.call("profile_list")["profiles"]
        assert {p["id"] for p in saved} == {p["id"] for p in profiles}
        p = profiles[0]
        status = client.call("browser_start", {"profileId": p["id"], "headless": True})
        assert status["runtime"]["effective"] == p["runtimeSettings"]
        client.call("browser_stop", {"profileId": p["id"]})
        invalid = client.call("profile_update", {"profileId": p["id"], "expectedRevision": p["revision"],
                                                  "runtimeSettings": {"timezone": "Invalid/NoSuchZone"}})
        client.call("browser_start", {"profileId": p["id"], "headless": True}, "RUNTIME_CONFIGURATION_REJECTED")
        p = client.call("profile_update", {"profileId": p["id"], "expectedRevision": invalid["revision"], "runtimeSettings": {}})
        assert "runtimeSettings" not in p
        status = client.call("browser_start", {"profileId": p["id"], "headless": True})
        assert status["runtime"]["requested"] == {} and status["runtime"]["effective"]["userAgent"] != profiles[0]["runtimeSettings"]["userAgent"]
        client.call("browser_stop", {"profileId": p["id"]})
        report.update(result="passed", persistedAfterRestart=True, invalidTimezoneRefused=True, defaultsRestored=True)
    finally:
        try:
            client.close()
        finally:
            server.shutdown()
            server.server_close()
            report["events"] = client.events
            (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()
