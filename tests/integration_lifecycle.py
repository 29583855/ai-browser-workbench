"""验证真实环境生命周期、浏览器保护和文件系统删除边界。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import os
import subprocess
from datetime import datetime
from pathlib import Path

from integration_browser import Client


# 用途：按实际返回版本构造修改请求，避免测试误用固定版本；日期：2026-11-04。
def edit(client, command, profile, error=None, **extra):
    return client.call(command, {"profileId": profile["id"], "expectedRevision": profile["revision"], **extra}, error)


# 用途：确认删除范围、重启恢复和运行中保护并保留全部失败证据；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "中文 空格环境"
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    client = Client(args.executable.resolve(), root, args.browser.resolve())
    try:
        p = client.call("profile_create", {"name": "环境甲", "allowedOrigins": ["https://example.com"]})
        assert p["revision"] == 1 and p["state"] == "active"
        original = p.copy()
        directory = root / p["id"]
        sentinel = directory / "持久登录测试.txt"
        sentinel.write_text("identity-sentinel", encoding="utf-8")
        p = edit(client, "profile_update", p, name="改名环境", proxyServer="http://127.0.0.1:9999")
        assert p["name"] == "改名环境" and p["revision"] == 2
        edit(client, "profile_update", original, "REVISION_CONFLICT", name="旧版本")
        edit(client, "profile_update", p, "INVALID_ARGUMENTS", unexpected="ignored")
        edit(client, "profile_update", p, "INVALID_ARGUMENTS")
        edit(client, "profile_update", p, "ORIGIN_DENIED", allowedOrigins=["file:///C:/Windows"])
        p = edit(client, "profile_update", p, proxyServer="")
        assert "proxyServer" not in p
        report["checks"].append("revision-cas-validation-and-proxy-clear")
        client.call("browser_start", {"profileId": p["id"], "headless": True})
        for command in ["profile_update", "profile_archive", "profile_trash", "profile_restore", "profile_purge"]:
            extra = {"name": "拒绝"} if command == "profile_update" else {}
            if command == "profile_purge":
                extra["confirmProfileId"] = p["id"]
            edit(client, command, p, "PROFILE_RUNNING", **extra)
        client.call("browser_stop", {"profileId": p["id"]})
        report["checks"].append("all-five-mutations-refused-while-real-browser-running")
        p = edit(client, "profile_archive", p)
        assert p["state"] == "archived" and sentinel.read_text(encoding="utf-8") == "identity-sentinel"
        assert not client.call("profile_list")["profiles"]
        assert client.call("profile_list", {"state": "archived"})["profiles"][0]["id"] == p["id"]
        client.call("browser_start", {"profileId": p["id"]}, "PROFILE_STATE_INVALID")
        client.call("lease_acquire", {"profileId": p["id"], "owner": "test", "ttlMs": 1000}, "PROFILE_STATE_INVALID")
        p = edit(client, "profile_restore", p)
        p = edit(client, "profile_trash", p)
        edit(client, "profile_update", p, "PROFILE_STATE_INVALID", name="回收中拒绝")
        report["checks"].append("archive-recycle-restore-preserves-identity-and-files")
        report["eventsBeforeRestart"] = client.events
        client.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        recovered = client.call("profile_list", {"state": "trashed"})["profiles"][0]
        assert recovered == p
        p = edit(client, "profile_restore", recovered)
        assert sentinel.read_text(encoding="utf-8") == "identity-sentinel"
        p = edit(client, "profile_trash", p)
        edit(client, "profile_purge", p, "PROFILE_CONFIRMATION_REQUIRED", confirmProfileId="0" * 32)
        client.call("profile_purge", {"profileId": "../outside", "expectedRevision": 1,
                                     "confirmProfileId": "../outside"}, "PROFILE_NOT_FOUND")
        report["checks"].append("restart-state-confirmation-and-traversal-refusal")
        outside = output / "external-preserve"
        outside.mkdir()
        external = outside / "keep.txt"
        external.write_bytes(b"outside-data-must-survive")
        junction = directory / "escape-junction"
        made = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(junction), str(outside)],
                              capture_output=True, timeout=10)
        assert made.returncode == 0, made.stderr.decode(errors="replace")
        edit(client, "profile_purge", p, "PATH_REPARSE_POINT", confirmProfileId=p["id"])
        assert external.read_bytes() == b"outside-data-must-survive" and sentinel.exists()
        assert junction.is_junction()
        junction.rmdir()
        p = client.call("profile_list", {"state": "purging"})["profiles"][0]
        hardlink = directory / "escape-hardlink"
        os.link(external, hardlink)
        edit(client, "profile_purge", p, "PATH_INVALID", confirmProfileId=p["id"])
        assert external.read_bytes() == b"outside-data-must-survive"
        hardlink.unlink()
        report["checks"].append("junction-and-hardlink-refused-external-data-intact")
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                      ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.CreateFileW(str(sentinel), 0x80000000, 1, None, 3, 0, None)
        assert handle not in (None, ctypes.c_void_p(-1).value)
        try:
            edit(client, "profile_purge", p, "PROFILE_DIRECTORY_BUSY", confirmProfileId=p["id"])
        finally:
            kernel.CloseHandle(handle)
        report["eventsBeforePurgeRestart"] = client.events
        client.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        p = client.call("profile_list", {"state": "purging"})["profiles"][0]
        long_directory = directory
        for index in range(6):
            long_directory /= "长目录" + str(index) + "x" * 50
        long_directory.mkdir(parents=True)
        (long_directory / "深层数据.txt").write_bytes(b"long-path-data")
        edit(client, "profile_purge", p, confirmProfileId=p["id"])
        assert not directory.exists() and external.read_bytes() == b"outside-data-must-survive"
        assert not client.call("profile_list", {"state": "all"})["profiles"]
        client.call("browser_status", {"profileId": p["id"]}, "PROFILE_NOT_FOUND")
        report["checks"].append("busy-purge-restart-retry-long-path-delete-and-no-external-damage")
        client.call("profile_list", {"state": "invalid"}, "INVALID_ARGUMENTS")
        report["result"] = "passed"
    finally:
        report["events"] = client.events
        try:
            client.close()
        finally:
            (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()
