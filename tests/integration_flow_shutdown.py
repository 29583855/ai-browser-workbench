"""验证主工作台退出时完整保留草稿，并在恢复写入失败时阻止数据丢失。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import subprocess
import tempfile
import time
from pathlib import Path

from integration_desktop import USER, kind, main_window, message, text, windows
from integration_flow_recovery import snapshot
from integration_visual_desktop import editor_window

KERNEL = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                             ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
KERNEL.CreateFileW.restype = ctypes.c_void_p
KERNEL.CloseHandle.argtypes = [ctypes.c_void_p]


# 用途：通过真实节点库插入结束节点，读取已应用属性中的稳定节点ID；日期：2026-11-04。
def insert_node(editor):
    palette = USER.GetDlgItem(editor, 802)
    message(palette, 0x0186, 8)
    message(editor, 0x0111, 802 | (2 << 16), palette)
    return json.loads(text(USER.GetDlgItem(editor, 803)))["id"]


# 用途：在独立数据根复现退出保存、配额已满和文件被独占三类边界；日期：2026-11-04。
def scenario(executable, mode):
    with tempfile.TemporaryDirectory(prefix="awb-flow-shutdown-") as private:
        root = Path(private)
        directory = root / "flow-drafts"
        process = subprocess.Popen([str(executable), "--root", str(root)])
        lock = None
        try:
            owner = main_window(process)
            message(owner, 0x0111, 901)
            editor = editor_window(process)
            if mode in ["quota", "multiple-quota"]:
                for index in range(49 if mode == "multiple-quota" else 50):
                    (directory / f"retained-{index}.awbrecovery").write_bytes(b"{}\n")
            identity = insert_node(editor)
            second_identity = None
            before = None
            path = None
            if mode in ["multiple", "multiple-quota"]:
                message(owner, 0x0111, 901)
                editors = []
                for window in windows():
                    pid = ctypes.c_ulong()
                    USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
                    if pid.value == process.pid and kind(window) == "AwbFlowEditor":
                        editors.append(window)
                assert len(editors) == 2
                second_identity = insert_node(next(window for window in editors if window != editor))
            elif mode in ["locked", "deleted"]:
                path, before = snapshot(directory, 2)
                if mode == "deleted":
                    path.unlink()
                else:
                    lock = KERNEL.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
                    assert lock != ctypes.c_void_p(-1).value, ctypes.get_last_error()
                    identity = insert_node(editor)
            started = time.monotonic()
            message(owner, 0x0010)
            if mode in ["quota", "multiple-quota", "locked"]:
                assert process.poll() is None and USER.IsWindow(owner), "Workbench exited although recovery could not be written"
                main_output = [item for item in windows(owner) if kind(item) == "Edit"][-1]
                error = json.loads(text(main_output))
                code = error.get("error", {}).get("code", "")
                assert code == ("WINDOWS_ERROR" if mode == "locked" else "FLOW_RECOVERY_FULL"), error
                if mode in ["quota", "multiple-quota"]:
                    assert len(list(directory.glob("*.awbrecovery"))) == 50
                    stale = list(directory.glob("retained-*.awbrecovery"))
                    assert len(stale) == (49 if second_identity else 50)
                    assert all(item.read_bytes() == b"{}\n" for item in stale)
                    if second_identity:
                        assert all(USER.IsWindow(window) for window in editors), "Partial preflight closed another editor"
                    (directory / "retained-0.awbrecovery").unlink()
                else:
                    KERNEL.CloseHandle(lock)
                    lock = None
                    assert json.loads(path.read_text("utf-8")) == before, "Failed replacement damaged the previous snapshot"
                message(owner, 0x0010)
            assert process.wait(timeout=15) == 0
            saved = [json.loads(item.read_text("utf-8")) for item in directory.glob("*.awbrecovery")]
            node_sets = [{node["id"] for node in item.get("definition", {}).get("steps", [])} for item in saved]
            assert any(identity in ids and "done" in ids for ids in node_sets), "Latest applied edit was lost on normal exit"
            if second_identity:
                assert sum(bool(ids) for ids in node_sets) == 2 and any(second_identity in ids for ids in node_sets)
            if mode == "immediate":
                assert time.monotonic() - started < 5, "Case did not exercise close before the next autosave interval"
            return {"mode": mode, "result": "passed", "snapshots": len(saved),
                    "failureBlocked": mode in ["quota", "multiple-quota", "locked"]}
        finally:
            if lock is not None:
                KERNEL.CloseHandle(lock)
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)


# 用途：独立记录每个失败场景并绑定真实二进制哈希，失败不覆盖成通过；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"date": "2026-11-04", "result": "failed", "checks": [], "cases": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    for mode in ["immediate", "multiple", "quota", "multiple-quota", "locked", "deleted"]:
        try:
            case = scenario(args.executable.resolve(), mode)
            report["checks"].append(mode)
        except Exception as error:
            case = {"mode": mode, "result": "failed", "error": str(error), "errorType": type(error).__name__}
        report["cases"].append(case)
    if len(report["checks"]) == 6:
        report["result"] = "passed"
    (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps(report, ensure_ascii=False))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
