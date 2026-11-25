"""通过真实Win32按钮验证凭据管理、单步调试、功能发现及只读诊断。日期：2026-11-05。"""

import argparse
import ctypes
import hashlib
import json
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from integration_desktop import USER, click, kind, main_window, message, set_text, text, windows
from integration_visual_desktop import menu
from integration_discovery_repair import Handler
from integration_workflow import definition
from http.server import ThreadingHTTPServer


# 用途：只查找本测试进程拥有的工具面板；日期：2026-11-05。
def panel_window(process):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        for window in windows():
            pid = ctypes.c_ulong()
            USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid and kind(window) == "AWB.Automation.Panel":
                return window
        time.sleep(0.025)
    raise TimeoutError("Automation panel did not appear")


# 用途：按真实调试状态等待，避免固定延时把旧暂停当作单步结果；日期：2026-11-05。
def debug_state(panel, predicate):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        value = json.loads(text(USER.GetDlgItem(panel, 1104)))
        if predicate(value):
            return value
        assert "error" not in value, value
        time.sleep(0.025)
    raise TimeoutError("Debugger did not reach requested boundary")


# 用途：实测本地后台、分组模板、凭据CRUD和发布调试，报告不包含秘密输入；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    private = Path(tempfile.mkdtemp(prefix="awb-automation-desktop-"))
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    process = subprocess.Popen([str(args.executable.resolve()), "--root", str(private / "data"),
                                "--browser", str(args.browser.resolve())])
    report = {"date": "2026-11-05", "result": "failed", "steps": [], "checks": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    window = panel = None
    try:
        window = main_window(process)
        name, url, selector, value, output = [w for w in windows(window) if kind(w) == "Edit"]
        assert menu(window, output, 916, report)["readOnly"]
        set_text(url, origin)
        click(window, output, 101, report)
        assert menu(window, output, 941, report)["freeConcurrentLimit"] == 10
        assert menu(window, output, 940, report)["profiles"]["active"] == 1
        group = menu(window, output, 944, report)
        assert menu(window, output, 943, report)["items"] == [group]
        template = menu(window, output, 948, report)
        assert menu(window, output, 947, report)["items"] == [template]
        library_file = private / "库操作.json"
        library_file.write_bytes(json.dumps({"id": template["id"]}).encode("utf-8"))
        set_text(selector, str(library_file))
        assert menu(window, output, 956, report)["items"][0]["revision"] == 1
        library_file.write_bytes(json.dumps({"id": template["id"], "expectedRevision": 1,
                                            "sourceRevision": 1}).encode("utf-8"))
        assert menu(window, output, 957, report)["revision"] == 2
        library_file.write_bytes(json.dumps({"requestId": "a" * 32, "operations": [{"command": "browser_stop",
                                            "arguments": {"profileId": group["profileIds"][0]}}]}).encode("utf-8"))
        batch = menu(window, output, 954, report)
        assert batch["state"] == "succeeded" and menu(window, output, 958, report)["items"][0]["finished"]
        library_file.write_bytes(json.dumps({"batchId": batch["batchId"]}).encode("utf-8"))
        assert menu(window, output, 959, report)["batchId"] == batch["batchId"]
        bundle = menu(window, output, 952, report)
        imported_file = private / "配置包.json"
        imported_file.write_bytes(json.dumps(bundle, ensure_ascii=False).encode("utf-8"))
        set_text(selector, str(imported_file))
        imported = menu(window, output, 953, report)
        assert len(imported["profiles"]) == 1 and menu(window, output, 940, report)["profiles"]["active"] == 2
        report["checks"].append("desktop-policy-summary-group-template-export-import-and-list-refresh")
        message(window, 0x0111, 911)
        panel = panel_window(process)
        assert message(USER.GetDlgItem(panel, 1103), 0x00D2) != 0, "Secret field is not masked"
        set_text(USER.GetDlgItem(panel, 1102), "桌面凭据")
        secret = "desktop-private-secret-" + hashlib.sha256(str(time.time_ns()).encode()).hexdigest()
        set_text(USER.GetDlgItem(panel, 1103), secret)
        message(USER.GetDlgItem(panel, 1120), 0x00F5)
        created = json.loads(text(USER.GetDlgItem(panel, 1104)))
        assert created["revision"] == 1 and len(created["credentialId"]) == 32
        assert secret not in text(USER.GetDlgItem(panel, 1104)) and message(USER.GetDlgItem(panel, 1103), 0x000E) == 0
        set_text(USER.GetDlgItem(panel, 1103), secret + "-rotated")
        message(USER.GetDlgItem(panel, 1121), 0x00F5)
        assert json.loads(text(USER.GetDlgItem(panel, 1104)))["revision"] == 2
        message(USER.GetDlgItem(panel, 1122), 0x00F5)
        assert message(USER.GetDlgItem(panel, 1101), 0x018B) == 0
        message(panel, 0x0010)
        panel = None
        report["checks"].append("masked-secret-cleared-create-rotate-delete-real-dpapi-panel")
        click(window, output, 102, report)
        click(window, output, 104, report)
        set_text(selector, "发布")
        observed = menu(window, output, 914, report)
        assert observed["matched"] == 3 and not observed["executed"]
        report["checks"].append("desktop-real-page-function-discovery-and-doctor")
        flow = definition(origin, "桌面调试", [
            {"id": "open", "type": "navigate", "url": origin, "next": "set"},
            {"id": "set", "type": "set", "variable": "count", "value": 1, "next": "ready"},
            {"id": "ready", "type": "assertText", "selector": "#ready", "expected": "ready", "timeoutMs": 1000, "next": "done"},
            {"id": "done", "type": "end"}])
        flow["schemaVersion"] = "1.1"
        flow["variables"] = {"count": {"schema": {"type": "integer", "minimum": 0, "maximum": 10}, "initial": 0, "sensitive": False}}
        source = private / "flow.json"
        source.write_bytes(json.dumps(flow, ensure_ascii=False).encode('utf-8'))
        set_text(selector, str(source))
        click(window, output, 128, report)
        message(window, 0x0111, 913)
        set_text(value, "{}")
        click(window, output, 129, report)
        panel = panel_window(process)
        initial = debug_state(panel, lambda r: r["state"] == "paused")
        assert initial["nextStep"]["stepId"] == "open"
        set_text(USER.GetDlgItem(panel, 1105), "main/ready")
        message(USER.GetDlgItem(panel, 1127), 0x00F5)
        message(USER.GetDlgItem(panel, 1123), 0x00F5)
        second = debug_state(panel, lambda r: r["state"] == "paused" and r["nextStep"]["stepId"] == "set")
        assert len(second["steps"]) == 1
        message(USER.GetDlgItem(panel, 1122), 0x00F5)
        third = debug_state(panel, lambda r: r["state"] == "paused" and r["nextStep"]["stepId"] == "ready")
        assert len(third["steps"]) == 2 and '"count"' in json.dumps(third["debug"])
        message(USER.GetDlgItem(panel, 1122), 0x00F5)
        final = debug_state(panel, lambda r: r["state"] == "succeeded")
        assert len(final["steps"]) == 4
        message(panel, 0x0010)
        panel = None
        report["checks"].append("desktop-pause-on-start-step-set-breakpoint-redacted-variables-resume-terminal-state")
        click(window, output, 103, report)
        message(window, 0x0010)
        process.wait(timeout=10)
        assert process.returncode == 0
        report["result"] = "passed"
    finally:
        if panel:
            message(panel, 0x0010)
        if process.poll() is None:
            if window:
                message(window, 0x0010)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        server.shutdown()
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode('utf-8'))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
