"""验证受管进程崩溃恢复、持久归属和外部进程不误杀。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import subprocess
import sys
from ctypes import wintypes
from datetime import datetime
from pathlib import Path
from integration_browser import Client, ProcessExit


# 用途：读取本测试创建的外部进程身份，独立于产品恢复代码构造边界；日期：2026-11-04。
def identity(process):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    handle = kernel.OpenProcess(0x1000, False, process.pid)
    assert handle
    try:
        created, exited, system, user = (wintypes.FILETIME() for _ in range(4))
        assert kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(system), ctypes.byref(user))
        size = wintypes.DWORD(32768)
        image = ctypes.create_unicode_buffer(size.value)
        assert kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(size))
        return {"schemaVersion": 1, "pid": process.pid, "processCreatedFileTime": (created.dwHighDateTime << 32) | created.dwLowDateTime,
                "executable": image.value, "ownership": "private-job-object"}
    finally:
        kernel.CloseHandle(handle)


# 用途：真实终止本测试服务验证Job清理，并核对伪造/过期记录不触发外部终止；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "profiles"
    client = Client(args.executable.resolve(), root, args.browser.resolve())
    external = None
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        p = client.call("profile_create", {"name": "恢复测试", "allowedOrigins": ["https://example.com"]})
        status = client.call("browser_start", {"profileId": p["id"], "headless": True})
        record = root / p["id"] / "awb-process-owner.json"
        saved = json.loads(record.read_text("utf-8"))
        assert saved["pid"] == status["pid"] and saved["processCreatedFileTime"] == status["processCreatedFileTime"]
        assert client.call("browser_status", {"profileId": p["id"]})["recovery"]["state"] == "owned-by-current-service"
        browser_exit = ProcessExit(status["pid"])
        report["eventsBeforeCrash"] = client.events
        client.process.kill()
        client.process.wait(timeout=10)
        browser_exit.wait()
        client.process.stdin.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        recovery = client.call("browser_status", {"profileId": p["id"]})["recovery"]
        assert recovery["state"] in ["recorded-process-exited", "stale-process-identity"] and not recovery["blocked"]
        restarted = client.call("browser_start", {"profileId": p["id"], "headless": True})
        assert restarted["processCreatedFileTime"] != saved["processCreatedFileTime"]
        client.call("browser_stop", {"profileId": p["id"]})
        report["checks"].append("service-crash-job-cleanup-persistent-identity-and-restart")
        client.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        external = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"], creationflags=0x08000000)
        external_record = identity(external)
        record.write_bytes((json.dumps(external_record) + "\n").encode("utf-8"))
        state = client.call("browser_status", {"profileId": p["id"]})["recovery"]
        assert state["state"] == "live-process-unowned" and state["automaticKillAllowed"] is False
        client.call("browser_start", {"profileId": p["id"]}, "PROCESS_OWNERSHIP_UNCONFIRMED")
        client.call("browser_stop", {"profileId": p["id"]}, "PROCESS_OWNERSHIP_UNCONFIRMED")
        client.call("profile_trash", {"profileId": p["id"], "expectedRevision": p["revision"]}, "PROCESS_OWNERSHIP_UNCONFIRMED")
        assert external.poll() is None
        report["checks"].append("live-external-process-blocked-no-automatic-kill-or-delete")
        external_record["processCreatedFileTime"] -= 1
        record.write_bytes((json.dumps(external_record) + "\n").encode("utf-8"))
        state = client.call("browser_status", {"profileId": p["id"]})["recovery"]
        assert state["state"] == "stale-process-identity" and not state["blocked"]
        client.call("browser_start", {"profileId": p["id"], "headless": True})
        client.call("browser_stop", {"profileId": p["id"]})
        assert external.poll() is None
        report["checks"].append("pid-reuse-identity-mismatch-never-kills-new-process")
        client.close()
        record.write_bytes(b"{corrupt-record")
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        assert client.call("browser_status", {"profileId": p["id"]})["recovery"]["state"] == "invalid-record"
        client.call("browser_start", {"profileId": p["id"]}, "PROCESS_OWNERSHIP_UNCONFIRMED")
        assert external.poll() is None
        report["checks"].append("corrupt-record-visible-and-refuses-automatic-start")
        report["result"] = "passed"
    finally:
        try:
            client.close()
        finally:
            if external and external.poll() is None:
                external.terminate()
                external.wait(timeout=5)
            report["events"] = client.events
            (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()
