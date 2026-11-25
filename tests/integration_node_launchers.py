"""验证便携包节点脚本在中文空格路径及无关工作目录下正常注册和停止。日期：2026-11-05。"""

import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path

from integration_commerce import Admin, account
from integration_package import OwnedDesktop


# 用途：使用系统Windows PowerShell运行包内脚本，不依赖当前工作目录；日期：2026-11-05。
def invoke(package, name, config):
    # 用途：日志使用普通文件，防止后台节点继承管道句柄使communicate等待其整个生命周期；日期：2026-11-05。
    log_path = config.parent / (name + "." + str(time.monotonic_ns()) + ".log")
    with log_path.open("wb") as stream:
        result = subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(package / name),
                "-Config",
                str(config),
            ],
            cwd=os.environ["SystemRoot"],
            stdin=subprocess.DEVNULL,
            stdout=stream,
            stderr=stream,
            timeout=30,
        )
    result.stdout = log_path.read_bytes()
    return result


# 用途：运行真实启动/审批/心跳/停止及配置拒绝路径，仅输出脱敏断言；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package, output = args.package.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    admin = Admin(package / "workbench-admin.exe", output / "admin")
    owned = None
    try:
        owner, _ = account(admin, "node-launcher", bootstrap=True)
        project = admin.call(
            "project.save", {"name": "节点脚本测试", "allowedOrigins": [admin.url]}, owner["token"]
        )
        invitation = admin.call(
            "node.enrollment.create", {"projectIds": [project["id"]], "capacity": 1}, owner["token"]
        )
        private = output / "中文 空格配置"
        private.mkdir()
        config = private / "node.json"
        value = {
            "root": str(private / "private"),
            "browser": str(package / "browser/chrome-win64/chrome.exe"),
            "url": admin.url,
            "name": "脚本启动节点",
            "headless": True,
            "enrollmentToken": invitation["enrollmentToken"],
        }
        config.write_bytes(json.dumps(value, ensure_ascii=False).encode("utf-8"))
        started = invoke(package, "Start-Node.ps1", config)
        assert started.returncode == 0, "Start-Node.ps1 failed"
        match = re.search(rb"Node process started: (\d+)", started.stdout)
        assert match, "Start script did not identify its process"
        owned = OwnedDesktop(int(match[1]))
        node = None
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            snapshot = admin.call("scheduler.snapshot", token=owner["token"])
            if snapshot["nodes"]:
                node = snapshot["nodes"][0]
                break
            assert owned.poll() is None, "Node exited before enrollment"
            time.sleep(0.15)
        assert node and node["state"] == "pending", "Enrollment was not pending approval"
        admin.call("node.state", {"id": node["id"], "state": "active"}, owner["token"])
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            snapshot = admin.call("scheduler.snapshot", token=owner["token"])
            if snapshot["nodes"][0].get("online"):
                break
            time.sleep(0.15)
        assert snapshot["nodes"][0]["online"], "Approved node did not heartbeat"
        stopped = invoke(package, "Stop-Node.ps1", config)
        assert stopped.returncode == 0, "Stop-Node.ps1 failed"
        owned.wait()
        invalid = private / "invalid.json"
        invalid.write_bytes(json.dumps({**value, "root": "relative-root"}).encode("utf-8"))
        rejected = invoke(package, "Start-Node.ps1", invalid)
        assert rejected.returncode != 0, "Relative private root was accepted"
        report = {
            "date": "2026-11-05",
            "result": "passed",
            "checks": [
                "windows-powershell-5.1-unrelated-cwd-chinese-space-config",
                "start-script-real-node-enrollment-and-approval",
                "authenticated-heartbeat-after-approval",
                "stop-script-graceful-exit",
                "relative-private-root-rejected",
            ],
            "physicalSecondMachineTested": False,
        }
        (output / "report.json").write_bytes(
            (json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        )
        print(json.dumps({"result": "passed", "checks": len(report["checks"])}))
    finally:
        if owned:
            owned.close()
        admin.close()


if __name__ == "__main__":
    main()
