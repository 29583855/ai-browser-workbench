"""验证便携包CLI/MCP/HTTP新增录制及拾取工具的同一业务闭环。日期：2026-11-04。"""

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from integration_browser import Client, FixtureServer
from integration_api import Api, start


class Mcp(Client):
    # 用途：在继承的有界队列上完成真实MCP握手和就绪通知；日期：2026-11-04。
    def __init__(self, executable, root, browser):
        super().__init__(executable, root, browser)
        self.rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                "clientInfo": {"name": "visual-contract-test", "version": "1"}})
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.process.stdin.flush()
        tools = self.rpc("tools/list", {})["tools"]
        assert {t["name"]: t["inputSchema"] for t in tools} == {t["name"]: t["inputSchema"] for t in json.loads((Path(__file__).resolve().parents[1] / "docs/contracts/tools-v1.json").read_text("utf-8"))["tools"]}
        self.registry = tools

    # 用途：发送真实JSON-RPC请求并检查ID及协议错误，不输出业务输入；日期：2026-11-04。
    def rpc(self, method, params):
        self.sequence += 1
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.sequence,
                                             "method": method, "params": params}, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        line = self.messages.get(timeout=30)
        assert line and "secret-canary-" not in line
        answer = json.loads(line)
        assert answer["id"] == self.sequence and "error" not in answer, "JSON-RPC contract mismatch"
        return answer["result"]

    # 用途：检查工具错误标志与共享业务错误码，证据只记录工具名和结果；日期：2026-11-04。
    def call(self, command, arguments=None, expected_error=None):
        result = self.rpc("tools/call", {"name": command, "arguments": arguments or {}})
        answer = json.loads(result["content"][0]["text"])
        if expected_error:
            assert result["isError"] and answer["error"]["code"] == expected_error
        else:
            assert not result["isError"]
        self.events.append({"command": command, "outcome": expected_error or "passed"})
        return answer


class Http:
    # 用途：启动独立数据根的真实鉴权API服务，不在报告中保存令牌；日期：2026-11-04。
    def __init__(self, executable, root, browser):
        self.process, config = start(executable, browser, root)
        self.report = {"requests": [], "rawRejections": []}
        self.api = Api(config, self.report)
        self.registry = self.api.call("GET", "/v1/tools")["tools"]
        assert {t["name"]: t["inputSchema"] for t in self.registry} == {t["name"]: t["inputSchema"] for t in json.loads((Path(__file__).resolve().parents[1] / "docs/contracts/tools-v1.json").read_text("utf-8"))["tools"]}

    # 用途：把共享测试请求送到实际HTTP工具路由，检查业务错误的HTTP状态；日期：2026-11-04。
    def call(self, command, arguments=None, expected_error=None):
        return self.api.call("POST", "/v1/tools/" + command, arguments or {}, error=expected_error,
                             status=400 if expected_error == "INVALID_ARGUMENTS" else 422 if expected_error else 200)

    # 用途：通过受鉴权shutdown正常退出，避免临时浏览器目录占用；日期：2026-11-04。
    def close(self):
        self.api.call("POST", "/v1/shutdown", {})
        assert self.process.wait(timeout=15) == 0


# 用途：依次调用三个实际通道，核对拾取不提交、参数化录制、停止和租约错误一致；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    package = args.package.resolve()
    assert (package / "portable.mode").is_file(), "Expected a complete extracted portable package"
    browser = package / "browser/chrome-win64/chrome.exe"
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    report = {"date": "2026-11-04", "result": "failed", "transports": [], "checks": []}
    client = None
    try:
        for transport in ["CLI", "MCP", "HTTP"]:
            with tempfile.TemporaryDirectory(prefix="awb-contract-") as temporary:
                root = Path(temporary)
                executable = package / ("workbench-mcp.exe" if transport == "MCP" else "workbench-cli.exe")
                client = Mcp(executable, root, browser) if transport == "MCP" else Http(executable, root, browser) if transport == "HTTP" else Client(executable, root, browser)
                profile = None
                record = {"transport": transport, "binarySha256": hashlib.sha256(executable.read_bytes()).hexdigest(), "checks": []}
                report["transports"].append(record)
                try:
                    tools = client.registry if transport != "CLI" else client.call("capabilities")
                    record["checks"].append("shared-registry-and-capability-discovery")
                    profile = client.call("profile_create", {"name": transport + "录制契约", "allowedOrigins": [origin]})["id"]
                    client.call("browser_start", {"profileId": profile, "headless": True})
                    lease = client.call("lease_acquire", {"profileId": profile, "owner": "contract-test", "ttlMs": 300000})
                    base = {"profileId": profile, "lease": lease}
                    client.call("browser_navigate", {**base, "url": origin + "/recording"})
                    client.call("page_pick_start", base)
                    assert client.call("page_pick_read", base)["state"] == "picking"
                    gesture = subprocess.run(["node", str(Path(__file__).with_name("recording_gestures.mjs")), str(root / profile), "picker", origin], capture_output=True, timeout=25)
                    assert gesture.returncode == 0
                    picked = client.call("page_pick_read", base)
                    assert picked["state"] == "picked" and picked["selector"] == "#submit"
                    client.call("page_pick_stop", {"profileId": profile})
                    submits = client.call("page_locate", {**base, "selector": "#submits"})["ref"]
                    assert client.call("page_text", {**base, "ref": submits})["text"] == "0"
                    record["checks"].append("all-picker-tools-no-business-submit")
                    client.call("recorder_start", base)
                    client.call("recorder_control", {**base, "operation": "pause"})
                    client.call("recorder_control", {**base, "operation": "resume"})
                    ref = client.call("page_locate", {**base, "selector": "#password"})["ref"]
                    client.call("page_fill", {**base, "ref": ref, "value": "secret-canary-transport"})
                    ref = client.call("page_locate", {**base, "selector": "#submit"})["ref"]
                    client.call("page_click", {**base, "ref": ref})
                    assert "secret-canary-" not in json.dumps(client.call("recorder_read", base))
                    stopped = client.call("recorder_control", {**base, "operation": "stop"})
                    assert stopped["complete"] and [event["kind"] for event in stopped["events"]] == ["document", "fill", "click"]
                    assert "secret-canary-" not in json.dumps(stopped)
                    record["checks"].append("all-recorder-tools-parameterized-secret-free-draft")
                    client.call("recorder_start", {**base, "unregisteredField": True}, "INVALID_ARGUMENTS")
                    client.call("recorder_start", base)
                    client.call("lease_revoke", {"profileId": profile})
                    client.call("recorder_read", base, "LEASE_REVOKED")
                    lease = client.call("lease_acquire", {"profileId": profile, "owner": "new-controller", "ttlMs": 300000})
                    client.call("recorder_control", {"profileId": profile, "lease": lease, "operation": "stop"})
                    record["checks"].append("same-argument-and-revoked-lease-rejections")
                    record["result"] = "passed"
                finally:
                    if profile:
                        stopped = client.call("browser_stop", {"profileId": profile})
                        assert stopped.get("jobEmptyConfirmed") or stopped.get("alreadyStopped")
                    if transport == "HTTP":
                        record["requests"] = client.report["requests"]
                    else:
                        record["calls"] = client.events
                    client.close()
                    client = None
        report["checks"] = [item["transport"] + ":" + check for item in report["transports"] for check in item["checks"]]
        report["result"] = "passed"
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        if client and client.process.poll() is None:
            client.process.kill()
            client.process.wait(timeout=10)
        server.shutdown()
        server.server_close()
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
