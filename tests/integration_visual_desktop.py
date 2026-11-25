"""验证真实桌面录制、画布配置、发布和回放闭环。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import json
import subprocess
import tempfile
import threading
import time
from pathlib import Path

from integration_desktop import (FixtureServer, USER, click, kind, main_window,
                                 message, set_text, text, wait_text, windows)


# 用途：等待同一工作台进程拥有的可视化编辑器；日期：2026-11-04。
def editor_window(process):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        for window in windows():
            pid = ctypes.c_ulong()
            USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid and kind(window) == "AwbFlowEditor":
                return window
        time.sleep(0.03)
    raise TimeoutError("Flow editor did not appear")


# 用途：发送菜单命令并检查实际异步服务结果；日期：2026-11-04。
def menu(window, output, command, report):
    set_text(output, "")
    message(window, 0x0111, command)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        try:
            answer = json.loads(text(output))
            assert answer.get("ok"), answer
            report["steps"].append({"menuId": command, "outcome": "passed"})
            return answer["result"]
        except ValueError:
            time.sleep(0.02)
    raise TimeoutError("Menu command timed out: " + str(command))


# 用途：按真实环境ID选中列表项，不假定同时间创建时的排序；日期：2026-11-04。
def select_profile(profiles, identity):
    matches = []
    for index in range(message(profiles, 0x018B)):
        label = ctypes.create_unicode_buffer(1024)
        message(profiles, 0x0189, index, ctypes.addressof(label))
        if "[" + identity[:8] + "]" in label.value:
            matches.append(index)
    assert len(matches) == 1, "Profile identity is absent or ambiguous in desktop list"
    message(profiles, 0x0186, matches[0])


# 用途：用真实画布命中并编辑对应节点，不修改服务数据库；日期：2026-11-04。
def configure(editor, point, changes):
    canvas = USER.GetDlgItem(editor, 801)
    USER.GetDpiForWindow.argtypes = [ctypes.c_void_p]
    dpi = USER.GetDpiForWindow(canvas)
    packed = round(point[0] * dpi / 96) | (round(point[1] * dpi / 96) << 16)
    message(canvas, 0x0201, 1, packed)
    message(canvas, 0x0202, 0, packed)
    properties = USER.GetDlgItem(editor, 803)
    step = json.loads(text(properties))
    step.update(changes)
    set_text(properties, json.dumps(step, ensure_ascii=False))
    message(USER.GetDlgItem(editor, 819), 0x00F5)
    assert json.loads(text(properties)) == step


# 用途：从菜单录制到桌面运行验证一次真实提交和Profile绑定；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    private = tempfile.TemporaryDirectory(prefix="awb-visual-desktop-")
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    process = subprocess.Popen([str(args.executable.resolve()), "--root", private.name,
                                "--browser", str(args.browser.resolve())])
    report = {"date": "2026-11-04", "result": "failed", "checks": [], "steps": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    window = output = profiles = None
    started = False
    try:
        window = main_window(process)
        name, url, selector, value, output = [item for item in windows(window) if kind(item) == "Edit"]
        profiles = next(item for item in windows(window) if kind(item) == "ListBox")
        origin = f"http://127.0.0.1:{server.server_port}"
        set_text(url, origin + "/recording")
        identities = []
        for label in ["录制回放环境", "切换验证环境"]:
            set_text(name, label)
            identities.append(click(window, output, 101, report)["result"]["id"])
        select_profile(profiles, identities[0])
        click(window, output, 102, report)
        started = True
        click(window, output, 104, report)
        menu(window, output, 902, report)
        set_text(selector, "#user")
        set_text(value, "桌面录制输入")
        click(window, output, 108, report)
        set_text(selector, "#submit")
        click(window, output, 109, report)
        wait_text(window, output, selector, "#submits", "1", report)
        stopped = menu(window, output, 906, report)
        assert [step["type"] for step in stopped["draft"]["steps"]] == ["navigate", "fill", "click", "end"]
        assert "桌面录制输入" not in json.dumps(stopped, ensure_ascii=False)
        report["checks"].append("desktop-record-stop-produces-parameterized-draft-with-one-submit")
        message(window, 0x0111, 907)
        editor = editor_window(process)
        configure(editor, (60, 60), {"url": origin + "/recording"})
        configure(editor, (520, 60), {"postcondition": {"selector": "#status", "expected": "submitted", "timeoutMs": 5000}})
        message(USER.GetDlgItem(editor, 821), 0x00F5)
        assert json.loads(text(USER.GetDlgItem(editor, 804)))["valid"]
        report["checks"].append("recorded-draft-opens-on-canvas-and-shared-validation-accepts-configuration")
        select_profile(profiles, identities[1])
        message(USER.GetDlgItem(editor, 822), 0x00F5)
        published = json.loads(text(output))
        assert published["ok"] and published["command"] == "workflow_publish"
        assert published["profileId"] == identities[0]
        assert json.loads(text(USER.GetDlgItem(editor, 804)))["workflowId"] == published["result"]["workflowId"]
        report["checks"].append("canvas-publication-binds-original-profile-even-after-selection-changes")
        select_profile(profiles, identities[0])
        set_text(value, json.dumps({"input_1": "桌面参数化回放"}, ensure_ascii=False))
        click(window, output, 129, report)
        assert message(value, 0x000E) == 0
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            status = click(window, output, 130, report)["result"]
            if status["state"] == "succeeded":
                break
            assert status["state"] in ["running", "queued"], status
            time.sleep(0.05)
        else:
            raise TimeoutError("Published workflow did not finish")
        wait_text(window, output, selector, "#submits", "1", report)
        report["checks"].append("main-window-runs-canvas-publication-with-cleared-input-and-one-submit")
        report["result"] = "passed"
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            if started and process.poll() is None:
                select_profile(profiles, identities[0])
                stopped = click(window, output, 103, report)["result"]
                assert stopped.get("jobEmptyConfirmed") or stopped.get("alreadyStopped")
            if process.poll() is None and window:
                message(window, 0x0010)
            process.wait(timeout=20)
            assert process.returncode == 0
            private.cleanup()
        except BaseException as error:
            report["result"] = "failed"
            report["cleanupError"] = str(error)
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            raise
        finally:
            (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
            server.shutdown()
            server.server_close()
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "steps": len(report["steps"])}))


if __name__ == "__main__":
    main()
