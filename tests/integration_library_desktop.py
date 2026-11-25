"""操作真实原生环境库表单，核对配置、并发版本、模板、文件对话框与批次保留。日期：2026-11-05。"""

import argparse
import ctypes
import hashlib
import json
import sqlite3
import subprocess
import threading
import time
from ctypes import wintypes
from datetime import datetime
from pathlib import Path
from PIL import Image
from integration_browser import Client
from integration_desktop import USER, windows, kind, main_window, message, text, set_text

USER.MoveWindow.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.BOOL]
USER.GetDlgCtrlID.argtypes = [wintypes.HWND]


# 用途：仅截取本测试创建的窗口，保存原生布局证据供人工核对，不读取用户其他应用；日期：2026-11-05。
def capture(panel, path):
    gdi = ctypes.WinDLL("gdi32", use_last_error=True)
    gdi.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi.CreateCompatibleDC.restype = wintypes.HDC
    gdi.CreateDIBSection.argtypes = [wintypes.HDC, ctypes.c_void_p, wintypes.UINT, ctypes.POINTER(ctypes.c_void_p), wintypes.HANDLE, wintypes.DWORD]
    gdi.CreateDIBSection.restype = wintypes.HANDLE
    gdi.SelectObject.argtypes = [wintypes.HDC, wintypes.HANDLE]
    gdi.SelectObject.restype = wintypes.HANDLE
    gdi.DeleteObject.argtypes = [wintypes.HANDLE]
    gdi.DeleteDC.argtypes = [wintypes.HDC]
    USER.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    USER.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    bounds = wintypes.RECT()
    assert USER.GetWindowRect(panel, ctypes.byref(bounds))
    width, height = bounds.right - bounds.left, bounds.bottom - bounds.top
    header = (ctypes.c_long * 10)(40, width, -height, 1 | (32 << 16), 0, width * height * 4, 0, 0, 0, 0)
    pixels = ctypes.c_void_p()
    context = gdi.CreateCompatibleDC(None)
    bitmap = gdi.CreateDIBSection(context, header, 0, ctypes.byref(pixels), None, 0)
    assert context and bitmap
    previous = gdi.SelectObject(context, bitmap)
    try:
        assert USER.PrintWindow(panel, context, 2)
        Image.frombytes("RGB", (width, height), ctypes.string_at(pixels, width * height * 4), "raw", "BGRX").save(path)
    finally:
        gdi.SelectObject(context, previous)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(context)


# 用途：检查所有可见交互控件的矩形不相互遮挡，防止直接消息测试绕过鼠标不可达的问题；日期：2026-11-05。
def no_overlap(panel):
    USER.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    bounds = []
    identities = list(range(1200, 1204)) + list(range(1210, 1223)) + list(range(1240, 1253))
    for identity in identities:
        handle = USER.GetDlgItem(panel, identity)
        if handle and USER.IsWindowVisible(handle):
            rectangle = wintypes.RECT()
            assert USER.GetWindowRect(handle, ctypes.byref(rectangle))
            bounds.append((identity, rectangle))
    for index, (first, a) in enumerate(bounds):
        for second, b in bounds[index + 1:]:
            assert max(a.left, b.left) >= min(a.right, b.right) or max(a.top, b.top) >= min(a.bottom, b.bottom), ("Overlapping controls", first, second)


# 用途：按PID和窗口类发现本测试面板，不使用其他进程或用户窗口；日期：2026-11-05。
def panels(process):
    found = []
    for window in windows():
        pid = wintypes.DWORD()
        USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
        if pid.value == process.pid and kind(window) == "AWB.Library.Panel":
            found.append(window)
    return found


# 用途：打开独立管理表单并等待真实列表初始化；日期：2026-11-05。
def open_panel(process, owner):
    previous = set(panels(process))
    message(owner, 0x0111, 917)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        created = set(panels(process)) - previous
        if created:
            return created.pop()
        time.sleep(0.02)
    raise TimeoutError("Library panel did not open")


# 用途：定位固定ID控件并拒绝测试不存在的按钮；日期：2026-11-05。
def control(panel, identity):
    handle = USER.GetDlgItem(panel, identity)
    assert handle, ("Missing control", identity)
    return handle


# 用途：点击实际按钮，等待一次提交的最终回执，错误时同时验证错误码；日期：2026-11-05。
def action(panel, identity, report, error=None):
    button, output = control(panel, identity), control(panel, 1220)
    assert USER.IsWindowEnabled(button) and USER.IsWindowVisible(button), identity
    set_text(output, "")
    message(button, 0x00F5)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            answer = json.loads(text(output))
        except ValueError:
            time.sleep(0.02)
            continue
        outcome = answer.get("error", {}).get("code", "passed")
        assert outcome == (error or "passed"), answer
        assert "refreshError" not in answer, answer
        report["steps"].append({"buttonId": identity, "outcome": outcome})
        return answer.get("result", answer)
    raise TimeoutError("Library action did not complete: " + str(identity))


# 用途：选择列表中的真实资源并发出标准选择通知；日期：2026-11-05。
def select(panel, token):
    listing = control(panel, 1210)
    for index in range(message(listing, 0x018B)):
        buffer = ctypes.create_unicode_buffer(1024)
        message(listing, 0x0189, index, ctypes.addressof(buffer))
        if token in buffer.value:
            message(listing, 0x0186, index)
            message(panel, 0x0111, 1210 | (1 << 16), listing)
            return
    raise AssertionError("Resource missing: " + token)


# 用途：切换真实标签并读取新模式的资源，不复用隐藏表单字段；日期：2026-11-05。
def tab(panel, mode):
    message(control(panel, 1200 + mode), 0x00F5)


# 用途：在系统文件对话框中输入测试临时路径并确认，避免依赖鼠标坐标；日期：2026-11-05。
def file_action(process, panel, identity, path, report, owner=None):
    result = []
    errors = []
    # 用途：保持模态文件选择消息在独立测试线程等待，主线程填写系统对话框；日期：2026-11-05。
    def invoke():
        try:
            result.append(action(panel, identity, report))
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=invoke)
    worker.start()
    deadline = time.monotonic() + 8
    dialog = None
    edit = None
    while time.monotonic() < deadline and not edit:
        for candidate in windows():
            pid = wintypes.DWORD()
            USER.GetWindowThreadProcessId(candidate, ctypes.byref(pid))
            if pid.value == process.pid and kind(candidate) == "#32770":
                dialog = candidate
                candidate_edit = USER.GetDlgItem(dialog, 1152)
                if candidate_edit and kind(candidate_edit) == "Edit" and USER.IsWindowVisible(candidate_edit):
                    edit = candidate_edit
                else:
                    edits = [item for item in windows(dialog) if kind(item) == "Edit" and USER.IsWindowVisible(item)]
                    if len(edits) == 1:
                        edit = edits[0]
                break
        time.sleep(0.02)
    assert dialog, "File dialog was not displayed"
    assert edit, [(USER.GetDlgCtrlID(item), kind(item)) for item in windows(dialog)]
    if owner:
        message(owner, 0x0010)
        assert process.poll() is None and USER.IsWindowVisible(dialog), "Owner closed during a modal file operation"
    set_text(edit, str(path))
    message(dialog, 0x0111, 1)
    worker.join(35)
    assert not worker.is_alive(), "File action stayed blocked"
    if errors:
        raise errors[0]
    assert len(result) == 1
    return result[0]


# 用途：覆盖原生表单的成功路径、配置清空、冲突输入保留、生命周期、导入导出和分页保护；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "中文 环境库数据"
    binary = (args.binaries / "workbench.exe").resolve()
    cli = (args.binaries / "workbench-cli.exe").resolve()
    client = Client(cli, root, args.browser.resolve())
    profile = client.call("profile_create", {"name": "表单原环境", "allowedOrigins": ["https://example.com"]})
    operation = {"command": "browser_stop", "arguments": {"profileId": profile["id"]}}
    for index in range(35):
        client.call("profile_batch", {"requestId": f"{index + 1:032x}", "operations": [operation]})
    client.close()
    # 用途：模拟真实重启前已落盘派发、尚无回执的记录，启动后必须保留对账详情；日期：2026-11-05。
    with sqlite3.connect(root / "workbench.sqlite3") as database:
        record = {"batchId": "f" * 32, "finished": False, "state": "running", "items": [{"state": "dispatching"}],
                  "onError": "stop", "atomic": False, "automaticRetryAllowed": False, "rollbackPerformed": False}
        database.execute("INSERT INTO profile_batches(id,request_hash,record_json) VALUES(?,?,?)",
                         ("f" * 32, hashlib.sha256(b"fixture-unknown").hexdigest(), json.dumps(record)))
    process = subprocess.Popen([str(binary), "--root", str(root), "--browser", str(args.browser.resolve())])
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "steps": [], "checks": [],
              "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    try:
        owner = main_window(process)
        first = open_panel(process, owner)
        second = open_panel(process, owner)
        no_overlap(first)
        select(first, profile["id"][:8])
        select(second, profile["id"][:8])
        for identity, value in {1211: "完整配置环境", 1213: "https://example.com\r\nhttp://127.0.0.1:3000",
                                1214: "http://127.0.0.1:8888", 1215: "AWB-Library-Test/1.0",
                                1216: "Asia/Shanghai", 1217: "zh-CN, zh"}.items():
            set_text(control(first, identity), value)
        saved = action(first, 1242, report)
        assert saved["revision"] == 2 and saved["proxyServer"] == "http://127.0.0.1:8888"
        assert saved["runtimeSettings"] == {"userAgent": "AWB-Library-Test/1.0", "timezone": "Asia/Shanghai", "languages": ["zh-CN", "zh"]}
        capture(first, output / "01-环境完整配置.png")
        set_text(control(second, 1211), "冲突输入应保留")
        action(second, 1242, report, "REVISION_CONFLICT")
        assert text(control(second, 1211)) == "冲突输入应保留"
        action(second, 1243, report)
        assert text(control(second, 1211)) == "完整配置环境"
        for identity in [1214, 1215, 1216, 1217]:
            set_text(control(second, identity), "")
        saved = action(second, 1242, report)
        assert saved["revision"] == 3 and not saved.get("proxyServer") and saved.get("runtimeSettings", {}) == {}
        report["checks"].append("all-profile-fields-clear-overrides-and-two-window-conflict-preserves-input")
        # 主列表必须在异步通知后显示表单保存的名称。
        listing = next(item for item in windows(owner) if kind(item) == "ListBox")
        label = ctypes.create_unicode_buffer(1024)
        message(listing, 0x0189, 0, ctypes.addressof(label))
        assert "完整配置环境" in label.value
        USER.MoveWindow(first, 10, 10, 680, 520, True)
        assert message(first, 0x0115, 7) == 0
        assert message(first, 0x0114, 7) == 0
        capture(first, output / "02-小窗口滚动.png")
        USER.MoveWindow(first, 10, 10, 1140, 1040, True)
        report["checks"].append("main-list-refresh-and-small-window-bidirectional-scroll")
        select(first, profile["id"][:8])
        action(first, 1243, report)
        exported = file_action(process, first, 1250, output / "中文 配置导出.json", report, owner)
        assert exported["format"] == "awb-profile-config-v1"
        imported = file_action(process, first, 1251, output / "中文 配置导出.json", report)["profiles"][0]
        assert imported["id"] != profile["id"] and imported["name"] == "完整配置环境"
        assert imported["id"][:8] in json.loads(text(control(first, 1220)))["result"]["profiles"][0]["id"]
        action(first, 1245, report)
        restored = action(first, 1247, report)
        assert restored["state"] == "active"
        action(first, 1246, report)
        action(first, 1244, report, "LIBRARY_CONFIRMATION_REQUIRED")
        message(control(first, 1222), 0x00F1, 1)
        action(first, 1244, report)
        report["checks"].append("system-file-dialog-config-export-import-and-confirmed-profile-lifecycle")
        tab(first, 1)
        action(first, 1240, report)
        set_text(control(first, 1211), "原生环境组")
        message(control(first, 1218), 0x0185, 1, 0)
        group = action(first, 1241, report)
        assert group["profileIds"] == [profile["id"]]
        set_text(control(first, 1211), "原生环境组V2")
        group = action(first, 1242, report)
        assert group["revision"] == 2
        capture(first, output / "03-分组成员.png")
        no_overlap(first)
        tab(first, 2)
        action(first, 1240, report)
        set_text(control(first, 1211), "原生模板")
        set_text(control(first, 1212), "模板初始环境")
        template = action(first, 1241, report)
        set_text(control(first, 1211), "原生模板V2")
        set_text(control(first, 1216), "Asia/Shanghai")
        template = action(first, 1242, report)
        assert template["revision"] == 2 and message(control(first, 1219), 0x0146) == 2
        message(control(first, 1219), 0x014E, 1)
        template = action(first, 1248, report)
        assert template["revision"] == 3 and template["name"] == "原生模板"
        capture(first, output / "04-模板历史.png")
        no_overlap(first)
        set_text(control(first, 1212), "原生模板实例")
        instance = action(first, 1249, report)["profiles"][0]
        assert instance["name"] == "原生模板实例" and instance["id"] != profile["id"]
        message(control(first, 1222), 0x00F1, 1)
        action(first, 1244, report)
        tab(first, 1)
        message(control(first, 1222), 0x00F1, 1)
        action(first, 1244, report)
        report["checks"].append("group-membership-template-history-rollback-instance-and-resource-delete")
        tab(first, 3)
        assert message(control(first, 1210), 0x018B) == 30 and USER.IsWindowEnabled(control(first, 1252))
        select(first, "ffffffff")
        action(first, 1245, report, "LIBRARY_CONFIRMATION_REQUIRED")
        message(control(first, 1222), 0x00F1, 1)
        action(first, 1245, report, "BATCH_RECONCILIATION_REQUIRED")
        action(first, 1252, report)
        assert message(control(first, 1210), 0x018B) == 6 and not USER.IsWindowEnabled(control(first, 1252))
        # 第一条即第二页最新项，确认后压缩；同页剩余记录不会丢失。
        message(control(first, 1222), 0x00F1, 1)
        archived = action(first, 1245, report)
        assert archived["archived"] and archived["items"] == [] and archived["itemCount"] == 1
        message(control(first, 1221), 0x014E, 1)
        message(first, 0x0111, 1221 | (1 << 16), control(first, 1221))
        assert message(control(first, 1210), 0x018B) == 1
        assert json.loads(text(control(first, 1220)))["archived"]
        capture(first, output / "05-批次归档摘要.png")
        no_overlap(first)
        report["checks"].append("batch-page-navigation-archive-scope-and-unknown-outcome-retention")
        # 保存期间关闭主窗口，所有受托管面板线程必须结束后才销毁服务。
        tab(second, 0)
        select(second, profile["id"][:8])
        action(second, 1243, report)
        set_text(control(second, 1211), "关闭前异步保存")
        message(control(second, 1242), 0x00F5)
        message(owner, 0x0010)
        process.wait(timeout=20)
        assert process.returncode == 0
        client = Client(cli, root, args.browser.resolve())
        stored = client.call("profile_list", {"state": "all"})["profiles"]
        assert next(item for item in stored if item["id"] == profile["id"])["name"] == "关闭前异步保存"
        receipt = client.call("profile_batch_get", {"batchId": archived["batchId"]})
        assert receipt["archived"]
        assert client.call("profile_batch", {"requestId": archived["batchId"], "operations": [operation]})["receiptOnly"]
        assert client.call("profile_batch_get", {"batchId": "f" * 32})["state"] == "outcome-unknown"
        client.close()
        report["checks"].append("owner-close-joins-pending-save-and-restart-keeps-deduplication")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = repr(error)
        raise
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "steps": len(report["steps"])}))


if __name__ == "__main__":
    main()
