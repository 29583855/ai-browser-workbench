"""验收完整ZIP部署、桌面与自检并行、启动脚本及校验失败路径。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import os
import queue
import subprocess
import threading
import urllib.request
import zipfile
from datetime import datetime
from pathlib import Path

from integration_desktop import main_window, message
from integration_api import Api


class OwnedDesktop:
    # 用途：持有本次启动桌面的真实进程句柄，防止PID复用导致误判；日期：2026-11-04。
    def __init__(self, pid):
        self.pid = pid
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        self.kernel.TerminateProcess.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.handle = self.kernel.OpenProcess(0x101001, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

    # 用途：查询同一个进程对象的真实退出状态；日期：2026-11-04。
    def poll(self):
        if self.kernel.WaitForSingleObject(self.handle, 0) == 0x102:
            return None
        code = ctypes.c_ulong()
        if not self.kernel.GetExitCodeProcess(self.handle, ctypes.byref(code)):
            raise ctypes.WinError(ctypes.get_last_error())
        return code.value

    # 用途：有界等待正常退出并核对退出码；日期：2026-11-04。
    def wait(self):
        assert self.kernel.WaitForSingleObject(self.handle, 10000) == 0, "Desktop shutdown timed out"
        assert self.poll() == 0, "Desktop did not exit cleanly"

    # 用途：测试异常时仅清理本测试持有的进程对象并释放句柄；日期：2026-11-04。
    def close(self):
        if self.poll() is None:
            self.kernel.TerminateProcess(self.handle, 1)
            self.kernel.WaitForSingleObject(self.handle, 10000)
        self.kernel.CloseHandle(self.handle)


# 用途：按块计算部署制品哈希，避免读取大浏览器文件到内存；日期：2026-11-04。
def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


# 用途：从无关工作目录运行系统PowerShell，并保存原始日志；日期：2026-11-04。
def self_test(package, output, name):
    result = subprocess.run(["powershell.exe", "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(package / "Self-Test.ps1")],
                            cwd=os.environ["SystemRoot"], capture_output=True, timeout=90)
    (output / (name + ".log")).write_bytes(result.stdout + result.stderr)
    reports = sorted((package / "test-results").glob("*/report.json"), key=lambda item: item.stat().st_mtime_ns)
    report = json.loads(reports[-1].read_text("utf-8"))
    (output / (name + ".json")).write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return result.returncode, report


# 用途：启动真实CMD入口并验证仅输出规定数量的JSON协议回复；日期：2026-11-04。
def launcher(package, name, lines, expected_replies):
    result = subprocess.run('"' + str(package / name) + '"', shell=True, input=("\n".join(lines) + "\n").encode("utf-8"),
                            cwd=os.environ["SystemRoot"], capture_output=True, timeout=30)
    assert result.returncode == 0, (name, result.stderr)
    replies = [json.loads(line) for line in result.stdout.decode("utf-8").splitlines()]
    assert len(replies) == expected_replies, name
    return replies


# 用途：从无关目录启动API脚本，验证包内浏览器发现/轮换令牌端点并正常退出；日期：2026-11-04。
def api_launcher(package):
    process = subprocess.Popen('"' + str(package / "Start-API.cmd") + '"', shell=True,
                               cwd=os.environ["SystemRoot"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, encoding="utf-8")
    ready = queue.Queue()
    owned = None
    # 用途：在有界队列读取准备通知，避免启动脚本失败时无限等待；日期：2026-11-04。
    def read_ready():
        ready.put(process.stdout.readline())
    threading.Thread(target=read_ready, daemon=True).start()
    try:
        notice = json.loads(ready.get(timeout=15))
        assert notice["ready"]
        config = json.loads((package / "data/api/local-api.json").read_text("utf-8"))
        owned = OwnedDesktop(config["pid"])
        api = Api(config, {"requests": [], "rawRejections": []})
        assert api.call("GET", "/v1/health")["localRestApi"]
        api.call("POST", "/v1/shutdown", {})
        assert process.wait(timeout=15) == 0
        owned.wait()
        assert not (package / "data/api/local-api.json").exists()
    finally:
        if owned:
            owned.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


# 用途：在部署副本从无关目录启动后台脚本，核对匿名状态与私有停止入口；日期：2026-11-05。
def admin_launcher(package):
    # Windows后台子进程可能继承管道句柄；使用日志文件使验收等待启动器本身退出。
    with (package / "data/admin-launcher-test.log").open("wb") as log:
        started = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                  str(package / "Start-Admin.ps1"), "-Port", "0", "-NoBrowser"],
                                 cwd=os.environ["SystemRoot"], stdout=log, stderr=log, timeout=25)
    assert started.returncode == 0, (package / "data/admin-launcher-test.log").read_bytes()
    config = json.loads((package / "data/admin/admin-endpoint.json").read_text("utf-8"))
    owned = OwnedDesktop(config["pid"])
    try:
        request = urllib.request.Request(config["url"] + "/api/status", data=b"{}",
                                         headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(request, timeout=10) as response:
            status = json.load(response)
        assert status["ok"] and not status["result"]["initialized"]
        stopped = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                  str(package / "Stop-Admin.ps1")], cwd=os.environ["SystemRoot"],
                                 capture_output=True, timeout=25)
        assert stopped.returncode == 0, stopped.stdout + stopped.stderr
        owned.wait()
    finally:
        owned.close()


# 用途：解压新包并核对并行自检、用户数据不变、损坏拒绝和源码再构建；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vs-root", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [], "secondCleanMachineTested": False}
    process = None
    try:
        checksum = json.loads(args.archive.with_suffix(".zip.sha256.json").read_text("utf-8"))
        assert digest(args.archive) == checksum["sha256"] and args.archive.stat().st_size == checksum["bytes"]
        report["archiveSha256"] = checksum["sha256"]
        parent = args.output.resolve() / "中文 空格部署"
        with zipfile.ZipFile(args.archive) as source:
            source.extractall(parent)
        package = parent / args.archive.stem
        report["copiedFolder"] = str(package)
        manifest = json.loads((package / "package-manifest.json").read_text("utf-8"))
        report["version"] = manifest["version"]
        report["manifestFiles"] = len(manifest["files"])
        assert not list((package / "data").iterdir()) and not (package / "test-results").exists()
        for entry in manifest["files"]:
            assert digest(package / entry["path"]) == entry["sha256"], entry["path"]
        report["checks"].append("archive-and-every-file-verified-pristine-data")
        # 真实启动桌面，观察实际窗口而非使用--smoke-test替代正常启动。
        command = '"' + str(package / "Start-Workbench.cmd") + '"'
        started = subprocess.run(command, shell=True, cwd=os.environ["SystemRoot"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
        assert started.returncode == 0
        # 通过有界进程枚举取得启动脚本创建的唯一工作台PID。
        query = subprocess.run(["powershell.exe", "-NoProfile", "-Command", "$p=Get-Process workbench | Where-Object { $_.Path -eq '" + str(package / "workbench.exe").replace("'", "''") + "' }; $p.Id"],
                               capture_output=True, text=True, timeout=15)
        pid = int(query.stdout.strip())
        process = OwnedDesktop(pid)
        window = main_window(process)
        database = package / "data/desktop/workbench.sqlite3"
        before = digest(database)
        code, detail = self_test(package, args.output, "self-test-with-desktop-open")
        assert code == 0 and detail["result"] == "passed", detail.get("errors")
        assert digest(database) == before, "Self-test changed the open desktop catalog"
        report["checks"].append("self-test-passes-with-desktop-open-and-keeps-user-catalog-unchanged")
        message(window, 0x0010)
        process.wait()
        report["checks"].append("normal-gui-launcher-and-close")
        cli = launcher(package, "Start-CLI.cmd", ['{"id":1,"command":"capabilities","arguments":{}}'], 1)
        assert cli[0]["result"]["version"] == manifest["version"]
        mcp = launcher(package, "Start-MCP.cmd", [
            '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"deploy-test","version":"1"}}}',
            '{"jsonrpc":"2.0","method":"notifications/initialized"}',
            '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'], 2)
        assert mcp[0]["result"]["serverInfo"]["version"] == manifest["version"]
        names = {tool["name"] for tool in mcp[1]["result"]["tools"]}
        assert {"profile_update", "profile_trash", "profile_purge", "page_fill", "page_list", "page_open",
                "page_switch", "page_close", "browser_events", "browser_reconnect", "page_inspect", "page_preview",
                "page_clear_highlight", "page_select", "recorder_start", "recorder_read", "recorder_control",
                "page_pick_start", "page_pick_read", "page_pick_stop", "account_connect",
                "account_disconnect", "account_status", "account_action"}.issubset(names)
        contract = json.loads((package / "source/docs/contracts/tools-v1.json").read_text("utf-8"))
        assert {t["name"]: t["inputSchema"] for t in mcp[1]["result"]["tools"]} == {t["name"]: t["inputSchema"] for t in contract["tools"]}
        report["checks"].append("cli-mcp-launchers-no-extra-stdout-and-consistent-version")
        api_launcher(package)
        report["checks"].append("api-launcher-from-unrelated-cwd-authentication-and-shutdown")
        admin_launcher(package)
        # 短暂持有旧端点的非删除共享读取，验证重启只重试元数据发布，不重放业务动作。
        held = (package / "data/admin/admin-endpoint.json").open("rb")
        release = threading.Timer(1.5, held.close)
        release.start()
        try:
            admin_launcher(package)
        finally:
            release.join(timeout=2)
            held.close()
        report["checks"].append("admin-start-restart-stop-launchers-with-endpoint-read-contention")
        script = package / "Self-Test.cmd"
        result = subprocess.run('"' + str(script) + '"', shell=True, input=b"\n", cwd=os.environ["SystemRoot"], capture_output=True, timeout=90)
        (args.output / "self-test-cmd.log").write_bytes(result.stdout + result.stderr)
        assert result.returncode == 0
        report["checks"].append("double-click-self-test-cmd-full-hash-check")
        altered = package / "使用说明.md"
        original = altered.read_bytes()
        try:
            altered.write_bytes(original + b"\nchanged-for-negative-test\n")
            code, detail = self_test(package, args.output, "reject-tampered-document")
            assert code == 1 and detail["result"] == "failed" and any("checksum mismatch" in error for error in detail["errors"])
        finally:
            altered.write_bytes(original)
        report["checks"].append("tampered-package-rejected")
        dll = package / "MSVCP140.dll"
        saved = dll.with_suffix(".dll.test-backup")
        try:
            dll.rename(saved)
            code, detail = self_test(package, args.output, "reject-missing-runtime")
            assert code == 1 and detail["result"] == "failed"
        finally:
            saved.rename(dll)
        report["checks"].append("missing-crt-rejected-before-starting-executables")
        if args.vs_root:
            result = subprocess.run(subprocess.list2cmdline([str(package / "source/Build-VS2019.cmd"), "-Configuration", "Release", "-VsRoot", str(args.vs_root)]),
                                    shell=True, input=b"\n", cwd=os.environ["SystemRoot"], capture_output=True, timeout=180)
            (args.output / "copied-source-build.log").write_bytes(result.stdout + result.stderr)
            assert result.returncode == 0
            report["checks"].append("copied-source-vs2019-release-build-and-tests")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
    finally:
        if process:
            process.close()
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": report["checks"], "error": report.get("error")}))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
