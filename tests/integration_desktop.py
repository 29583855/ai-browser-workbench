"""通过真实Win32控件运行环境管理及登录注册，不模拟服务返回。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import subprocess
import sys
import threading
import time
from ctypes import wintypes
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "fixtures"))
from web_app import FixtureServer
from integration_workflow import definition
from integration_extraction import extraction_spec, batch_spec

USER = ctypes.WinDLL("user32", use_last_error=True)
CALLBACK = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
USER.EnumWindows.argtypes = [CALLBACK, wintypes.LPARAM]
USER.EnumChildWindows.argtypes = [wintypes.HWND, CALLBACK, wintypes.LPARAM]
USER.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
USER.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
USER.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
USER.GetDlgItem.restype = wintypes.HWND
USER.IsWindowVisible.argtypes = [wintypes.HWND]
USER.IsWindowVisible.restype = wintypes.BOOL
USER.IsWindowEnabled.argtypes = [wintypes.HWND]
USER.IsWindowEnabled.restype = wintypes.BOOL
USER.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                                    wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
USER.SendMessageTimeoutW.restype = ctypes.c_size_t


# 用途：有界发送系统控件消息，避免测试因窗口无响应而无限阻塞；日期：2026-11-04。
def message(window, code, wparam=0, lparam=0):
    result = ctypes.c_size_t()
    ctypes.set_last_error(0)
    if not USER.SendMessageTimeoutW(window, code, wparam, lparam, 2, 5000, ctypes.byref(result)):
        raise RuntimeError("Window message failed: " + str(code) + "; Windows error " + str(ctypes.get_last_error()))
    return result.value


# 用途：枚举窗口或子控件并立即释放回调引用；日期：2026-11-04。
def windows(parent=None):
    found = []
    # 用途：收集系统回调中的真实窗口句柄；日期：2026-11-04。
    def collect(window, _parameter):
        found.append(window)
        return True
    callback = CALLBACK(collect)
    if parent:
        USER.EnumChildWindows(parent, callback, 0)
    else:
        USER.EnumWindows(callback, 0)
    return found


# 用途：读取控件类别以发现实际窗口布局，不依赖屏幕坐标；日期：2026-11-04。
def kind(window):
    value = ctypes.create_unicode_buffer(128)
    USER.GetClassNameW(window, value, len(value))
    return value.value


# 用途：按PID等待本测试创建的主窗口，禁止连接到用户其他窗口；日期：2026-11-04。
def main_window(process):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        for window in windows():
            pid = wintypes.DWORD()
            USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid and kind(window) == "AIBrowserWorkbench":
                children = windows(window)
                if sum(kind(child) == "Edit" for child in children) == 5 and any(kind(child) == "ListBox" for child in children):
                    return window
        assert process.poll() is None, "Desktop exited before creating its window"
        time.sleep(0.05)
    raise TimeoutError("Main window did not appear")


# 用途：读取跨进程标准控件文本，处理Unicode与多行JSON；日期：2026-11-04。
def text(window):
    length = message(window, 0x000E)
    buffer = ctypes.create_unicode_buffer(length + 1)
    message(window, 0x000D, len(buffer), ctypes.addressof(buffer))
    return buffer.value


# 用途：向真实编辑框填写内容，测试不记录填写值；日期：2026-11-04。
def set_text(window, value):
    buffer = ctypes.create_unicode_buffer(value)
    message(window, 0x000C, 0, ctypes.addressof(buffer))


# 用途：点击真实按钮并等待异步业务JSON，而非只确认按钮存在；日期：2026-11-04。
def click(window, output, button, report, error=None):
    groups = {501: {105, 107, 108, 109, 110, 123, 124, 125, 126},
              502: {117, 118, 119, 120, 121, 122, 134, 135, 136},
              503: {127, 128, 129, 130, 131, 132, 133, 137, 138},
              504: {112, 113, 114, 115, 116}}
    for group, buttons in groups.items():
        if button in buttons and not USER.IsWindowVisible(USER.GetDlgItem(window, button)):
            message(USER.GetDlgItem(window, group), 0x00F5)
            report.setdefault("groupSwitches", []).append(group)
    assert USER.IsWindowVisible(USER.GetDlgItem(window, button)), "Requested action is not visible in its group"
    # 用途：等待实际按钮恢复可用，禁止向忙碌的禁用控件发送无效点击；日期：2026-11-04。
    ready_deadline = time.monotonic() + 10
    while not USER.IsWindowEnabled(USER.GetDlgItem(window, button)):
        if time.monotonic() >= ready_deadline:
            raise TimeoutError("Desktop action stayed disabled: " + str(button))
        time.sleep(0.01)
    set_text(output, "")
    message(USER.GetDlgItem(window, button), 0x00F5)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            answer = json.loads(text(output))
        except ValueError:
            time.sleep(0.03)
            continue
        actual = answer.get("error", {}).get("code", "passed")
        report["steps"].append({"buttonId": button, "outcome": actual})
        if error:
            assert not answer.get("ok") and actual == error, (button, actual)
        else:
            assert answer.get("ok") is True, (button, actual)
        return answer
    raise TimeoutError("Desktop command timed out: " + str(button))


# 用途：反复执行只读文本查询等待后端结果，不重复提交注册或登录；日期：2026-11-04。
def wait_text(window, output, selector, selector_value, expected, report):
    set_text(selector, selector_value)
    deadline = time.monotonic() + 8
    observed = ""
    while time.monotonic() < deadline:
        observed = click(window, output, 110, report)["result"]["text"]
        if observed == expected:
            return
        time.sleep(0.05)
    raise AssertionError("Expected desktop observation: " + expected + "; observed: " + observed)


# 用途：验证新环境选择、隔离登录、按钮动作、撤销和重开持久性；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    process = subprocess.Popen([str(args.executable.resolve()), "--root", str(args.output.resolve() / "desktop-data"),
                                "--browser", str(args.browser.resolve())])
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "steps": [], "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        window = main_window(process)
        edits = [item for item in windows(window) if kind(item) == "Edit"]
        profiles = [item for item in windows(window) if kind(item) == "ListBox"][0]
        assert len(edits) == 5, "Unexpected desktop input layout"
        name, url, selector, value, output = edits
        set_text(url, f"http://127.0.0.1:{server.server_port}/login")
        created = []
        for index in range(8):
            set_text(name, "桌面回归环境" + str(index + 1))
            answer = click(window, output, 101, report)
            identity = answer["result"]["id"]
            selected = message(profiles, 0x0188)
            label = ctypes.create_unicode_buffer(1024)
            message(profiles, 0x0189, selected, ctypes.addressof(label))
            assert "[" + identity[:8] + "]" in label.value, "Newly created profile is not selected; next operation targets another profile"
            created.append(identity)
        report["checks"].append("eight-new-profiles-select-the-created-profile")
        message(profiles, 0x0186, 0)
        click(window, output, 102, report)
        assert click(window, output, 134, report)["result"]["active"]
        click(window, output, 104, report)
        for selector_value, field_value in [("#username", "desktop-review-user"), ("#password", "desktop-fixture-password")]:
            set_text(selector, selector_value)
            set_text(value, field_value)
            click(window, output, 108, report)
            assert message(value, 0x000E) == 0, "Desktop did not clear the supplied value"
        set_text(selector, "#register"); click(window, output, 109, report)
        wait_text(window, output, selector, "#result", "registered", report)
        set_text(selector, "#login"); click(window, output, 109, report)
        wait_text(window, output, selector, "#identity", "desktop-review-user", report)
        set_text(selector, "#username")
        inspected = click(window, output, 123, report)["result"]
        assert inspected["highlighted"] and not inspected["executed"] and inspected["fillReady"]
        preview = click(window, output, 125, report)["result"]
        assert preview["readyNow"] and not preview["executed"]
        assert click(window, output, 124, report)["result"]["cleared"]
        set_text(url, f"http://127.0.0.1:{server.server_port}/inspect")
        click(window, output, 104, report)
        set_text(selector, "#single"); set_text(value, "cn")
        assert click(window, output, 126, report)["result"]["selectedCount"] == 1
        assert message(value, 0x000E) == 0
        wait_text(window, output, selector, "#single-value", "cn", report)
        set_text(url, f"http://127.0.0.1:{server.server_port}/login")
        click(window, output, 104, report)
        report["checks"].append("desktop-inspection-highlight-clear-preview-and-native-select")
        set_text(url, f"http://127.0.0.1:{server.server_port}/extraction")
        click(window, output, 104, report)
        for button, spec in [(137, extraction_spec()), (138, batch_spec())]:
            imported = args.output.resolve() / ("中文 操作" + str(button) + ".json")
            imported.write_bytes((json.dumps(spec, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
            set_text(selector, str(imported))
            answer = click(window, output, button, report)["result"]
            assert answer.get("valid") is True if button == 137 else answer["state"] == "succeeded"
        interrupted = {"actions": [
            {"type": "assertText", "selector": "#echo", "expected": "never-matches", "timeoutMs": 3000},
            {"type": "fill", "selector": "#field", "value": "must-not-be-written"}], "onError": "continue"}
        imported = args.output.resolve() / "批次撤销.json"
        imported.write_bytes((json.dumps(interrupted, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        set_text(selector, str(imported))
        message(USER.GetDlgItem(window, 138), 0x00F5)
        time.sleep(0.3)
        message(USER.GetDlgItem(window, 106), 0x00F5)
        # 撤销后异步批次结果可能立即覆盖提示，应验证最终停止及后续写入未发生。
        deadline = time.monotonic() + 10
        cancelled = None
        while time.monotonic() < deadline:
            try:
                current = json.loads(text(output))
                if current.get("command") == "page_batch":
                    cancelled = current["result"]
                    break
            except ValueError:
                pass
            time.sleep(0.03)
        assert cancelled and cancelled["stopped"] and cancelled["notRun"] == 1
        assert cancelled["items"][0]["error"]["code"] == "LEASE_REVOKED"
        click(window, output, 111, report)
        wait_text(window, output, selector, "#echo", "公开批次数据", report)
        report["checks"].append("desktop-revoke-during-batch-read-prevents-following-write")
        set_text(url, f"http://127.0.0.1:{server.server_port}/login")
        click(window, output, 104, report)
        report["checks"].append("desktop-schema-extraction-and-verified-action-batch-from-json-file")
        first_page = click(window, output, 117, report)["result"]["activePageId"]
        set_text(url, f"http://127.0.0.1:{server.server_port}/runtime")
        extra_page = click(window, output, 118, report)["result"]["pageId"]
        listed = click(window, output, 117, report)["result"]["pages"]
        page_selector = [item for item in windows(window) if kind(item) == "ComboBox"][0]
        index = next(i for i, page in enumerate(listed) if page["pageId"] == first_page)
        message(page_selector, 0x014E, index)
        assert click(window, output, 119, report)["result"]["pageId"] == first_page
        wait_text(window, output, selector, "#identity", "desktop-review-user", report)
        index = next(i for i, page in enumerate(listed) if page["pageId"] == extra_page)
        message(page_selector, 0x014E, index)
        assert click(window, output, 120, report)["result"]["pageId"] == extra_page
        assert click(window, output, 121, report)["result"]["bufferLimit"] == 256
        assert click(window, output, 122, report)["result"]["replayedPageActions"] == 0
        set_text(url, f"http://127.0.0.1:{server.server_port}/login")
        report["checks"].append("desktop-pages-open-select-switch-close-events-and-explicit-reconnect")
        flow = definition(f"http://127.0.0.1:{server.server_port}", "desktop-workflow")
        flow_file = args.output.resolve() / "中文 流程定义.json"
        flow_file.write_bytes((json.dumps(flow, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        set_text(selector, str(flow_file))
        assert click(window, output, 127, report)["result"]["valid"]
        published = click(window, output, 128, report)["result"]
        set_text(value, "{}")
        run = click(window, output, 129, report)["result"]
        assert run["workflowId"] == published["workflowId"] and message(value, 0x000E) == 0
        assert click(window, output, 131, report)["result"]["state"] == "pausing"
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            observed = click(window, output, 130, report)["result"]
            if observed["state"] == "paused":
                break
            time.sleep(0.05)
        assert observed["state"] == "paused"
        click(window, output, 132, report)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            observed = click(window, output, 130, report)["result"]
            if observed["state"] == "succeeded":
                break
            time.sleep(0.05)
        assert observed["state"] == "succeeded" and len(observed["steps"]) == 3
        set_text(value, "{}")
        click(window, output, 129, report)
        click(window, output, 133, report)
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            observed = click(window, output, 130, report)["result"]
            if observed["state"] == "cancelled":
                break
            time.sleep(0.05)
        assert observed["state"] == "cancelled"
        set_text(url, f"http://127.0.0.1:{server.server_port}/login")
        click(window, output, 104, report)
        report["checks"].append("desktop-workflow-file-validate-publish-run-status-pause-resume-cancel")
        diagnostic = click(window, output, 135, report)["result"]
        assert diagnostic["events"] and not diagnostic["payloadCapture"]
        assert not click(window, output, 136, report)["result"]["active"]
        report["checks"].append("desktop-diagnostics-start-read-stop-with-real-network-records")
        click(window, output, 105, report)
        set_text(selector, ".duplicate"); click(window, output, 107, report, "LOCATOR_AMBIGUOUS")
        # 撤销按钮返回直接业务对象，没有ok封装；仍核对真实撤销状态。
        message(USER.GetDlgItem(window, 106), 0x00F5)
        assert json.loads(text(output))["revoked"] is True
        set_text(selector, "#identity"); click(window, output, 110, report, "LEASE_REQUIRED")
        click(window, output, 111, report)
        wait_text(window, output, selector, "#identity", "desktop-review-user", report)
        click(window, output, 103, report)
        report["checks"].append("register-login-fill-clear-snapshot-ambiguity-revoke-reacquire-stop")
        message(profiles, 0x0186, 1)
        click(window, output, 102, report); click(window, output, 104, report)
        wait_text(window, output, selector, "#identity", "anonymous", report)
        click(window, output, 103, report)
        message(profiles, 0x0186, 0)
        click(window, output, 102, report); click(window, output, 104, report)
        wait_text(window, output, selector, "#identity", "desktop-review-user", report)
        click(window, output, 103, report)
        report["checks"].append("second-profile-isolated-first-profile-login-persists-after-restart")
        set_text(name, "桌面生命周期名称")
        updated = click(window, output, 112, report)["result"]
        archived = click(window, output, 113, report)["result"]
        assert archived["id"] == updated["id"] and archived["state"] == "archived"
        click(window, output, 102, report, "PROFILE_STATE_INVALID")
        trashed = click(window, output, 114, report)["result"]
        restored = click(window, output, 115, report)["result"]
        assert trashed["state"] == "trashed" and restored["id"] == updated["id"] and restored["state"] == "active"
        click(window, output, 102, report); click(window, output, 104, report)
        wait_text(window, output, selector, "#identity", "desktop-review-user", report)
        click(window, output, 103, report)
        report["checks"].append("desktop-update-archive-trash-restore-preserves-login-and-id")
        set_text(name, "仅删除本测试临时环境")
        disposable = click(window, output, 101, report)["result"]["id"]
        click(window, output, 114, report)
        confirmations = []
        report["purgeConfirmation"] = confirmations
        # 用途：只确认本测试PID创建的永久删除对话框，核验完整环境ID后点击是；日期：2026-11-04。
        def confirm_purge():
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                for dialog in windows():
                    pid = wintypes.DWORD()
                    USER.GetWindowThreadProcessId(dialog, ctypes.byref(pid))
                    if pid.value == process.pid and kind(dialog) == "#32770":
                        labels = " ".join(text(child) for child in windows(dialog) if kind(child) == "Static")
                        if disposable not in labels:
                            confirmations.append("wrong-profile")
                            message(dialog, 0x0111, 7)
                            return
                        confirmations.append(disposable)
                        message(dialog, 0x0111, 6)
                        return
                time.sleep(0.02)
        confirmer = threading.Thread(target=confirm_purge, daemon=True)
        confirmer.start()
        purged = click(window, output, 116, report)["result"]
        confirmer.join(timeout=5)
        assert confirmations == [disposable] and purged["purged"] and purged["id"] == disposable
        assert message(profiles, 0x018B) == 8
        assert not (args.output / "desktop-data" / disposable).exists()
        report["checks"].append("desktop-permanent-delete-confirmation-and-list-refresh")
        assert server.registration_count == 1 and server.login_count == 1
        report["serverFacts"] = {"registrationCount": server.registration_count, "loginCount": server.login_count}
        message(window, 0x0010)
        assert process.wait(timeout=10) == 0
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        # 仅保留固定夹具状态及脱敏诊断，不读取输入、口令或Cookie；日期：2026-11-04。
        try:
            report["failureObservation"] = {}
            for selector_value in ["#result", "#identity", "#storage", "#fixture-diagnostic"]:
                set_text(selector, selector_value)
                report["failureObservation"][selector_value] = click(window, output, 110, report)["result"]["text"]
            report["failureDiagnostics"] = click(window, output, 135, report)["result"]
        except Exception as observation_error:
            report["failureObservationError"] = str(observation_error)
    finally:
        report["serverFacts"] = {"registrationCount": server.registration_count, "loginCount": server.login_count}
        report["identityObservations"] = server.identity_observations
        if process.poll() is None:
            process.kill(); process.wait(timeout=10)
        server.shutdown(); server.server_close()
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "steps": len(report["steps"]), "checks": report["checks"], "error": report.get("error")}))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
