"""验证真实后台失联时多浏览器停止与同地址重启恢复。日期：2026-11-05。"""
import argparse
import json
import time
from pathlib import Path
from urllib.parse import urlsplit
from integration_browser import Client
from integration_commerce import Admin, account


# 用途：强杀自有后台，验证两个真实受管浏览器均停止且槽保留，恢复后确认释放；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary_dir.resolve()
    admin = Admin(binary / "workbench-admin.exe", (args.output / "admin-data").resolve())
    client = Client(binary / "workbench-cli.exe", (args.output / "client-data").resolve(), args.browser.resolve())
    try:
        account(admin, "outage-operator", True)
        buyer, credentials = account(admin, "outage-buyer")
        client.call("account_connect", {"url": admin.url, "email": credentials["email"], "password": credentials["password"]})
        profiles = []
        for index in range(2):
            profile = client.call("profile_create", {"name": "Outage " + str(index), "allowedOrigins": [admin.url]})["id"]
            client.call("browser_start", {"profileId": profile, "headless": True})
            profiles.append(profile)
        server_root, port = admin.root, urlsplit(admin.url).port
        admin.process.kill()
        admin.process.wait(timeout=5)
        admin = None
        start = time.monotonic()
        deadline = start + 30
        while time.monotonic() < deadline:
            running = [client.call("browser_status", {"profileId": profile})["running"] for profile in profiles]
            if not any(running):
                break
            time.sleep(0.25)
        else:
            raise AssertionError("Disconnected browsers were not stopped")
        elapsed = time.monotonic() - start
        assert client.call("account_status")["trackedSlots"] == 2
        admin = Admin(binary / "workbench-admin.exe", server_root, port)
        assert len([slot for slot in admin.call("snapshot", token=buyer["token"])["slots"] if slot["state"] == "active"]) == 2
        client.call("account_connect", {"url": admin.url, "email": credentials["email"], "password": credentials["password"]})
        for profile in profiles:
            client.call("browser_stop", {"profileId": profile})
        assert client.call("account_status")["trackedSlots"] == 0
        assert all(slot["state"] == "released" for slot in admin.call("snapshot", token=buyer["token"])["slots"])
        client.call("account_disconnect")
        report = {"date": "2026-11-05", "result": "passed", "browsers": 2, "stopAfterSeconds": round(elapsed, 3),
                  "pendingSlotsPreserved": True, "sameAddressRestarted": True, "reconciledSlots": 2,
                  "clientChecks": len(client.events), "checks": ["real-two-browser-outage-stop", "no-auto-slot-release",
                      "same-port-restart-retains-admission", "same-account-reauthentication", "confirmed-stop-release"]}
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        print(json.dumps(report))
    finally:
        client.close()
        if admin:
            admin.close()


if __name__ == "__main__":
    main()
