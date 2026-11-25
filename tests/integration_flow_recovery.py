"""通过真实窗口、专用剪贴板、计时器和强制退出验证草稿恢复。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path

from integration_desktop import USER, kind, main_window, message, set_text, text, windows
from integration_visual_desktop import editor_window

USER.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
USER.PostMessageW.restype = ctypes.c_int
USER.GetDlgCtrlID.argtypes = [ctypes.c_void_p]
USER.IsWindow.argtypes = [ctypes.c_void_p]
USER.OpenClipboard.argtypes = [ctypes.c_void_p]
USER.CloseClipboard.argtypes = []


# 用途：等待本测试进程的系统模态框，避免操作其他程序；日期：2026-11-04。
def modal(process):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        for window in windows():
            pid = ctypes.c_ulong()
            USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid and kind(window) == "#32770":
                return window
        time.sleep(0.02)
    raise TimeoutError("Expected workbench-owned modal dialog")


# 用途：在真实文件选择器中选择恢复文件，不绕过恢复按钮；日期：2026-11-04。
def recover(process, editor, path, owner):
    assert USER.PostMessageW(editor, 0x0111, 826, 0)
    dialog = modal(process)
    deadline = time.monotonic() + 8
    edits = []
    while not edits and time.monotonic() < deadline:
        edits = [window for window in windows(dialog) if kind(window) == "Edit" and USER.IsWindowVisible(window)]
        if not edits:
            time.sleep(0.02)
    assert edits, "Recovery file dialog has no file-name edit"
    message(owner, 0x0010)
    assert USER.IsWindow(owner) and process.poll() is None, "Closing owner destroyed an active file dialog"
    filename = next((window for window in edits if USER.GetDlgCtrlID(window) in [1148, 1152]), edits[-1])
    set_text(filename, str(path))
    assert USER.PostMessageW(USER.GetDlgItem(dialog, 1), 0x00F5, 0, 0)
    deadline = time.monotonic() + 8
    output = USER.GetDlgItem(editor, 804)
    while time.monotonic() < deadline:
        try:
            answer = json.loads(text(output))
            assert answer.get("recovered") and answer.get("saveAsRequired"), answer
            return
        except ValueError:
            time.sleep(0.02)
    raise TimeoutError("Recovery file was not loaded through dialog")


# 用途：等待真实五秒计时器保存指定节点数量的模型；日期：2026-11-04。
def snapshot(directory, count, exclude=()):
    deadline = time.monotonic() + 9
    while time.monotonic() < deadline:
        for path in directory.glob("*.awbrecovery"):
            if path in exclude:
                continue
            data = json.loads(path.read_text(encoding="utf-8"))
            if len(data["definition"]["steps"]) == count:
                return path, data
        time.sleep(0.04)
    raise TimeoutError("Autosave timer did not preserve expected draft")


# 用途：异常退出后从保留文件恢复草稿，并验证显式关闭会清除本窗口恢复文件；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    private = tempfile.TemporaryDirectory(prefix="awb-flow-recovery-")
    root = Path(private.name)
    process = None
    window = None
    report = {"date": "2026-11-04", "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        process = subprocess.Popen([str(args.executable.resolve()), "--root", str(root)])
        window = main_window(process)
        message(window, 0x0111, 901)
        editor = editor_window(process)
        canvas = USER.GetDlgItem(editor, 801)
        packed = 60 | (60 << 16)
        message(canvas, 0x0201, 1, packed)
        message(canvas, 0x0202, 0, packed)
        message(USER.GetDlgItem(editor, 827), 0x00F5)
        message(USER.GetDlgItem(editor, 828), 0x00F5)
        path, copied = snapshot(root / "flow-drafts", 2)
        ids = [node["id"] for node in copied["definition"]["steps"]]
        assert len(set(ids)) == 2 and "done" in ids
        report["checks"].append("native-node-clipboard-paste-uses-distinct-identities-and-real-timer-autosave")
        # 用途：由外部进程实际占用剪贴板，忙碌拒绝必须保留节点；释放后才可剪切；日期：2026-11-05。
        assert USER.OpenClipboard(None), "Cannot hold clipboard for busy-case fixture"
        try:
            message(USER.GetDlgItem(editor, 829), 0x00F5)
            denied = json.loads(text(USER.GetDlgItem(editor, 804)))
            assert denied["error"]["code"] == "FLOW_CLIPBOARD_BUSY", denied
        finally:
            assert USER.CloseClipboard()
        properties = json.loads(text(USER.GetDlgItem(editor, 803)))
        assert properties["id"] in ids, "Busy cut lost selected node"
        report["checks"].append("clipboard-exclusive-contention-refuses-cut-without-removing-selected-node")
        message(USER.GetDlgItem(editor, 829), 0x00F5)
        cut_path, cut = snapshot(root / "flow-drafts", 1)
        assert cut_path == path and cut["definition"]["steps"][0]["id"] == "done"
        message(USER.GetDlgItem(editor, 815), 0x00F5)
        path, copied = snapshot(root / "flow-drafts", 2)
        report["checks"].append("cut-deletes-only-after-clipboard-success-and-undo-restores-nodes")
        process.kill()
        process.wait(timeout=10)
        assert json.loads(path.read_text(encoding="utf-8")) == copied
        report["checks"].append("abrupt-process-exit-retains-last-complete-atomic-snapshot")
        process = subprocess.Popen([str(args.executable.resolve()), "--root", str(root)])
        window = main_window(process)
        message(window, 0x0111, 901)
        editor = editor_window(process)
        recover(process, editor, path, window)
        report["checks"].append("owner-close-is-blocked-while-editor-file-dialog-is-active")
        restored_path, restored = snapshot(root / "flow-drafts", 2, [path])
        if restored != copied:
            report["recoveryExpected"] = copied
            report["recoveryActual"] = restored
        assert restored == copied, "Recovered model/layout differs; comparison retained in report"
        report["checks"].append("restarted-workbench-restores-model-and-layout-through-native-file-dialog")
        assert USER.PostMessageW(editor, 0x0010, 0, 0)
        confirmation = modal(process)
        assert USER.PostMessageW(USER.GetDlgItem(confirmation, 1), 0x00F5, 0, 0)
        deadline = time.monotonic() + 5
        while restored_path.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not restored_path.exists() and path.exists()
        report["checks"].append("confirmed-editor-close-removes-only-its-own-recovery-file")
        report["result"] = "passed"
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            if process and process.poll() is None:
                for dialog in windows():
                    pid = ctypes.c_ulong()
                    USER.GetWindowThreadProcessId(dialog, ctypes.byref(pid))
                    if pid.value == process.pid and kind(dialog) == "#32770":
                        USER.PostMessageW(USER.GetDlgItem(dialog, 2), 0x00F5, 0, 0)
                        time.sleep(0.1)
                if window:
                    message(window, 0x0010)
                process.wait(timeout=15)
                assert process.returncode == 0
            assert root.resolve().name.startswith("awb-flow-recovery-")
            private.cleanup()
        except BaseException as error:
            report["result"] = "failed"
            report["cleanupError"] = str(error)
            if process and process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            raise
        finally:
            (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
