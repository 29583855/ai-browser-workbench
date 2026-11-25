"""对真实受管浏览器执行登录注册/隔离/租约/文档引用验收。日期：2026-11-03。"""
import argparse
import base64
import ctypes
import hashlib
import json
import queue
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent / "fixtures"))
from web_app import FixtureServer


class ProcessExit:
    # 用途：在注入故障前持有进程句柄，避免PID复用及异步退出误判；日期：2026-11-03。
    def __init__(self, pid):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        self.kernel.OpenProcess.restype = ctypes.c_void_p
        self.kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.kernel.WaitForSingleObject.restype = ctypes.c_ulong
        self.kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        self.handle = self.kernel.OpenProcess(0x100000, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

    # 用途：有界确认同一个进程对象已经退出并释放句柄；日期：2026-11-03。
    def wait(self):
        try:
            assert self.kernel.WaitForSingleObject(self.handle, 10000) == 0, "Owned process did not exit"
        finally:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class Client:
    # 用途：启动真实CLI并在线程中有界等待协议回复；日期：2026-11-03。
    def __init__(self, executable, root, browser=None):
        arguments = [str(executable), "--root", str(root)]
        if browser:
            arguments.extend(["--browser", str(browser)])
        self.process = subprocess.Popen(arguments, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                        encoding="utf-8", bufsize=1)
        self.messages = queue.Queue()
        self.sequence = 0
        self.events = []
        self.thread = threading.Thread(target=self.read_output, daemon=True)
        self.thread.start()

    # 用途：单独读取stdout，协议等待超过预算时主测试可终止；日期：2026-11-03。
    def read_output(self):
        for line in self.process.stdout:
            self.messages.put(line)
        self.messages.put(None)

    # 用途：发送命令并校验返回ID和预期错误，不保存敏感输入；日期：2026-11-03。
    def call(self, command, arguments=None, expected_error=None):
        self.sequence += 1
        request = {"id": self.sequence, "command": command, "arguments": arguments or {}}
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        line = self.messages.get(timeout=30)
        if line is None:
            raise RuntimeError("CLI exited: " + self.process.stderr.read())
        response = json.loads(line)
        assert "secret-canary-" not in line, "A supplied secret leaked in a protocol response"
        assert response["id"] == self.sequence, response
        if expected_error:
            assert not response["ok"] and response["error"]["code"] == expected_error, response
        else:
            assert response["ok"], response
        self.events.append({"sequence": self.sequence, "command": command,
                            "outcome": response.get("error", {}).get("code", "passed")})
        return response.get("result", response)

    # 用途：关闭CLI并检查退出，异常时只终止本测试创建的进程；日期：2026-11-03。
    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                raise
        assert self.process.returncode == 0, self.process.stderr.read()


class Recorder:
    # 用途：对本测试自有端点启动真实CDP录屏，不对用户浏览器录制；日期：2026-11-03。
    def __init__(self, directory, origin, output):
        self.output = output
        self.process = subprocess.Popen(["node", str(Path(__file__).with_name("record_cdp.mjs")),
                                          str(directory), origin, str(output / "recording-frames")],
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         text=True, encoding="utf-8")
        messages = queue.Queue()
        # 用途：在线程等待准备信号，主线程保留有界超时；日期：2026-11-03。
        def read_ready():
            messages.put(self.process.stdout.readline())
        threading.Thread(target=read_ready, daemon=True).start()
        try:
            assert messages.get(timeout=15).strip() == "ready", "Screencast did not initialize"
        except BaseException:
            self.process.kill()
            self.process.wait(timeout=5)
            raise

    # 用途：停止录制并按浏览器原始时间戳编码动画录屏，非静态截图拼接；日期：2026-11-03。
    def finish(self):
        _stdout, stderr = self.process.communicate("stop\n", timeout=15)
        assert self.process.returncode == 0, stderr
        directory = self.output / "recording-frames"
        manifest = json.loads((directory / "frames.json").read_text("utf-8"))
        probe = json.loads((directory / "dom-probe.json").read_text("utf-8"))
        assert {"open", "closed"}.issubset(probe["shadowRootTypes"]), probe
        assert probe["iframeNodes"] >= 2 and probe["oopifTargets"] >= 1, probe
        frames = []
        duration = []
        for index, frame in enumerate(manifest["frames"]):
            with Image.open(directory / frame["name"]) as image:
                frames.append(image.convert("RGB"))
            following = manifest["frames"][index + 1]["timestamp"] if index + 1 < len(manifest["frames"]) else frame["timestamp"] + 0.2
            duration.append(max(10, round((following - frame["timestamp"]) * 1000)))
        recording = self.output / "registration-login.gif"
        frames[0].save(recording, save_all=True, append_images=frames[1:], duration=duration, loop=0, optimize=False)
        with Image.open(recording) as verified:
            assert verified.n_frames >= 3 and verified.size[0] > 100
        for frame in frames:
            frame.close()
        return {"source": manifest["source"], "frames": len(frames), "format": "animated GIF",
                "durationMs": sum(duration), "sha256": hashlib.sha256(recording.read_bytes()).hexdigest(),
                "domProbe": probe}


# 用途：运行真实浏览器业务及故障拒绝链，按实际执行日期生成证据；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--browser", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = output / "profiles"
    assert not root.exists(), "Use a fresh evidence directory per execution"
    server = FixtureServer()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    origin = f"http://127.0.0.1:{server.server_port}"
    client = Client(args.executable.resolve(), root, args.browser.resolve() if args.browser else None)
    report = {"date": datetime.now().astimezone().isoformat(), "executable": str(args.executable.resolve()),
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
              "origin": origin, "scenarios": [], "result": "failed"}
    recorder = None
    try:
        assert client.call("capabilities")["productionReady"] is False
        profiles = []
        for name in ["角色A中文", "角色B中文", "未登录角色C"]:
            profile = client.call("profile_create", {"name": name, "allowedOrigins": [origin]})
            profile_id = profile["id"]
            status = client.call("browser_start", {"profileId": profile_id, "headless": True})
            assert status["running"] and status["ownership"] == "private-job-object"
            lease = client.call("lease_acquire", {"profileId": profile_id, "owner": "integration", "ttlMs": 300000})
            profiles.append((profile_id, lease))
            report.setdefault("processes", []).append(status)
        profile_id, lease = profiles[0]

        # 用途：为一条页面动作附加当前实例和有效租约；日期：2026-11-03。
        def action(command, extra=None, selected=None, error=None):
            identity, permission = selected or (profile_id, lease)
            return client.call(command, {"profileId": identity, "lease": permission, **(extra or {})}, error)

        # 用途：每次重新唯一定位后执行操作，禁止盲复用旧文档引用；日期：2026-11-03。
        def locate(selector, selected=None):
            return action("page_locate", {"selector": selector}, selected)["ref"]

        # 用途：填入账号后只提交一次注册/登录，再查询业务结果；日期：2026-11-03。
        def authenticate(username, password, selected, register=True):
            action("browser_navigate", {"url": origin + "/login"}, selected)
            action("page_wait_text", {"selector": "#sw", "expected": "sw-ready", "timeoutMs": 5000}, selected)
            action("page_fill", {"ref": locate("#username", selected), "value": username}, selected)
            action("page_fill", {"ref": locate("#password", selected), "value": password}, selected)
            if register:
                action("page_click", {"ref": locate("#register", selected)}, selected)
                action("page_wait_text", {"selector": "#result", "expected": "registered", "timeoutMs": 5000}, selected)
                action("page_click", {"ref": locate("#register", selected)}, selected)
                action("page_wait_text", {"selector": "#result", "expected": "DUPLICATE_USER", "timeoutMs": 5000}, selected)
                action("page_fill", {"ref": locate("#password", selected), "value": "secret-canary-wrong-41"}, selected)
                action("page_click", {"ref": locate("#login", selected)}, selected)
                action("page_wait_text", {"selector": "#result", "expected": "INVALID_CREDENTIALS", "timeoutMs": 5000}, selected)
                action("page_wait_text", {"selector": "#identity", "expected": "anonymous", "timeoutMs": 5000}, selected)
                action("page_fill", {"ref": locate("#password", selected), "value": password}, selected)
            action("page_click", {"ref": locate("#login", selected)}, selected)
            action("page_wait_text", {"selector": "#result", "expected": "logged-in", "timeoutMs": 5000}, selected)
            action("page_wait_text", {"selector": "#identity", "expected": username, "timeoutMs": 5000}, selected)

        action("browser_navigate", {"url": origin + "/login"})
        recorder = Recorder(root / profile_id, origin, output)
        authenticate("alice-test", "secret-canary-A-17", profiles[0])
        authenticate("bob-test", "secret-canary-B-29", profiles[1])
        assert server.registration_count == 2 and len(server.users) == 2
        for selected, username in zip(profiles[:2], ["alice-test", "bob-test"]):
            expected_storage = {"local": username, "indexed": username, "cache": ["awb-" + username]}
            action("page_wait_text", {"selector": "#storage", "expected": json.dumps(expected_storage, separators=(",", ":")),
                                      "timeoutMs": 5000}, selected)
            storage = action("page_text", {"ref": locate("#storage", selected)}, selected)["text"]
            values = json.loads(storage)
            assert values == expected_storage, values
        action("browser_navigate", {"url": origin + "/login"}, profiles[2])
        action("page_wait_text", {"selector": "#identity", "expected": "anonymous", "timeoutMs": 5000}, profiles[2])
        action("page_wait_text", {"selector": "#storage", "expected": '{"local":null,"indexed":null,"cache":[]}',
                                  "timeoutMs": 5000}, profiles[2])
        storage = action("page_text", {"ref": locate("#storage", profiles[2])}, profiles[2])["text"]
        assert json.loads(storage) == {"local": None, "indexed": None, "cache": []}, storage
        report["scenarios"].append("three-profile Cookie/localStorage/IndexedDB/CacheStorage/SW isolation")

        snapshot = action("page_snapshot")
        assert any(node["name"] == "登录" for node in snapshot["nodes"]), snapshot
        (output / "snapshot.json").write_text(json.dumps(snapshot, ensure_ascii=False, indent=2), encoding="utf-8")
        action("page_locate", {"selector": ".duplicate"}, error="LOCATOR_AMBIGUOUS")
        action("page_locate", {"selector": "#absent"}, error="ELEMENT_NOT_FOUND")
        action("page_locate", {"selector": "#" + "中" * 2047}, error="ELEMENT_NOT_FOUND")
        action("page_locate", {"selector": "#" + "中" * 2048}, error="INVALID_ARGUMENTS")
        action("page_locate", {"selector": "#open-shadow button"}, error="ELEMENT_NOT_FOUND")
        action("page_locate", {"selector": "#closed-shadow button"}, error="ELEMENT_NOT_FOUND")
        action("page_locate", {"selector": "#same-frame-action"}, error="ELEMENT_NOT_FOUND")
        action("page_locate", {"selector": "#frame-action"}, error="ELEMENT_NOT_FOUND")
        action("page_locate", {"selector": "#frame-action", "frameId": "must-not-ignore"}, error="INVALID_ARGUMENTS")
        action("page_click", {"ref": locate("#disabled")}, error="ELEMENT_NOT_ACTIONABLE")
        action("page_click", {"ref": locate("#hidden")}, error="ELEMENT_NOT_ACTIONABLE")
        action("page_fill", {"ref": locate("#readonly"), "value": "must-not-write"}, error="ELEMENT_NOT_EDITABLE")
        old = locate("#username")
        action("browser_navigate", {"url": origin + "/app"})
        action("page_fill", {"ref": old, "value": "must-not-write"}, error="STALE_ELEMENT_REFERENCE")
        action("browser_navigate", {"url": "http://example.com"}, error="ORIGIN_DENIED")
        action("page_click", {"ref": locate("#filter")})
        action("page_wait_text", {"selector": "#report", "expected": "rows:1", "timeoutMs": 5000})
        report["scenarios"].append("AX snapshot, ambiguous/missing locator rejection, stale document refs and origin gate")

        screenshot = action("page_screenshot")
        png = base64.b64decode(screenshot["base64"], validate=True)
        assert png.startswith(b"\x89PNG\r\n\x1a\n")
        (output / "screenshot.png").write_bytes(png)
        report["screenshotSha256"] = hashlib.sha256(png).hexdigest()
        report["recording"] = recorder.finish()
        recorder = None
        client.call("lease_revoke", {"profileId": profile_id})
        action("page_click", {"ref": "e1"}, error="LEASE_REVOKED")
        lease = client.call("lease_acquire", {"profileId": profile_id, "owner": "new-controller", "ttlMs": 300000})
        action("page_wait_text", {"selector": "#identity", "expected": "alice-test", "timeoutMs": 5000})
        before_restart_ref = locate("#username")
        stopped = client.call("browser_stop", {"profileId": profile_id})
        assert stopped["processExitConfirmed"] and stopped["jobEmptyConfirmed"]
        client.call("browser_start", {"profileId": profile_id, "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile_id, "owner": "reopened", "ttlMs": 300000})
        action("browser_navigate", {"url": origin + "/app"})
        action("page_wait_text", {"selector": "#identity", "expected": "alice-test", "timeoutMs": 5000})
        assert locate("#username") != before_restart_ref
        action("page_fill", {"ref": before_restart_ref, "value": "must-not-write"}, error="STALE_ELEMENT_REFERENCE")
        report["scenarios"].append("revoked writer rejected, stop confirmed and persistent login after reopen")
        crashed_id, crashed_lease = profiles[2]
        hanging_ref = locate("#hang-submit", profiles[2])
        unknown = action("page_click", {"ref": hanging_ref}, profiles[2], error="ACTION_OUTCOME_UNKNOWN")
        assert unknown["error"]["outcomeUnknown"] and not unknown["error"]["automaticRetryAllowed"]
        assert server.fault_commit_count == 1, "Submission was not observed exactly once before response loss"
        report["scenarios"].append("server committed once before CDP deadline; unknown action is never automatically retried")
        crash_status = client.call("browser_status", {"profileId": crashed_id})
        crash_exit = ProcessExit(crash_status["pid"])
        subprocess.run(["taskkill", "/PID", str(crash_status["pid"]), "/T", "/F"],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        crash_exit.wait()
        assert not client.call("browser_status", {"profileId": crashed_id})["running"]
        action("page_snapshot", selected=(crashed_id, crashed_lease), error="BROWSER_NOT_RUNNING")
        client.call("browser_start", {"profileId": crashed_id, "headless": True})
        action("page_snapshot", selected=(crashed_id, crashed_lease), error="LEASE_REVOKED")
        report["scenarios"].append("owned browser crash detected without automatic business retry")
        for identity, _permission in profiles:
            client.call("browser_stop", {"profileId": identity})
        assert server.registration_count == 2 and server.login_count == 2
        report["serverFacts"] = {"registrationCount": server.registration_count, "loginCount": server.login_count,
                                  "userCount": len(server.users), "faultCommitCount": server.fault_commit_count}
        # 仅杀死本测试创建的服务；Job关闭应自动清理其自有浏览器，且索引可由新进程重开。
        survivor = client.call("browser_start", {"profileId": profile_id, "headless": True})
        survivor_exit = ProcessExit(survivor["pid"])
        prior_events = list(client.events)
        client.process.kill()
        client.process.wait(timeout=10)
        survivor_exit.wait()
        client = Client(args.executable.resolve(), root, args.browser.resolve() if args.browser else None)
        client.events = prior_events
        client.sequence = len(prior_events)
        assert len(client.call("profile_list")["profiles"]) == 3
        assert not client.call("browser_status", {"profileId": profile_id})["running"]
        client.call("browser_start", {"profileId": profile_id, "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile_id, "owner": "after-service-crash", "ttlMs": 300000})
        action("browser_navigate", {"url": origin + "/app"})
        action("page_wait_text", {"selector": "#identity", "expected": "alice-test", "timeoutMs": 5000})
        client.call("browser_stop", {"profileId": profile_id})
        report["scenarios"].append("service crash closes owned browser job, releases root lock and preserves login")
        report["result"] = "passed"
    finally:
        try:
            if recorder and recorder.process.poll() is None:
                recorder.process.kill()
                recorder.process.wait(timeout=5)
            client.close()
        finally:
            server.shutdown()
            server.server_close()
            report["commands"] = client.events
            (output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": report["result"], "commands": len(client.events), "report": str(output / "report.json")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
