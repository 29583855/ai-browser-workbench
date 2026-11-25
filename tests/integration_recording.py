"""验证真实受管浏览器录制、敏感输入、原生IME/Enter与新环境回放。日期：2026-11-04。"""

import argparse
import hashlib
import json
import subprocess
import threading
import tempfile
import time
from pathlib import Path

from integration_browser import Client, FixtureServer


# 用途：在独立数据根执行录制闭环并保留实际失败及证据，不重复业务来掩盖错误；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    private = tempfile.TemporaryDirectory(prefix="awb-recording-")
    private_root = Path(private.name)
    client = Client(args.executable.resolve(), private_root, args.browser.resolve())
    report = {"date": "2026-11-04", "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
              "scope": "Main-document preview; cross-frame/Shadow/physical desktop IME not accepted"}
    base = {}
    created_profiles = []

    # 用途：所有页面调用复用同一Profile及租约，不存储输入内容；日期：2026-11-04。
    def call(command, extra=None, error=None):
        return client.call(command, dict(base, **(extra or {})), error)

    # 用途：创建独立环境并导航到固定测试夹具；日期：2026-11-04。
    def profile(name):
        identity = client.call("profile_create", {"name": name, "allowedOrigins": [origin]})["id"]
        created_profiles.append(identity)
        client.call("browser_start", {"profileId": identity, "headless": True})
        lease = client.call("lease_acquire", {"profileId": identity, "owner": "recording-test", "ttlMs": 300000})
        base.clear()
        base.update(profileId=identity, lease=lease)
        call("browser_navigate", {"url": origin + "/recording"})
        return identity

    # 用途：每次动作前重新定位当前文档，避免把旧ref当稳定定位；日期：2026-11-04。
    def action(command, selector, extra=None):
        reference = call("page_locate", {"selector": selector})["ref"]
        return call(command, dict(extra or {}, ref=reference))

    # 用途：读取实际DOM计数，作为工具返回之外的独立业务依据；日期：2026-11-04。
    def text(selector):
        return action("page_text", selector)["text"]

    # 用途：通过测试专用CDP连接发送IME与原生Enter，不开放产品宿主执行接口；日期：2026-11-04。
    def gesture(mode):
        directory = private_root / base["profileId"]
        result = subprocess.run(["node", str(Path(__file__).with_name("recording_gestures.mjs")),
                                 str(directory), mode, origin], capture_output=True, text=True, timeout=25)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "passed"

    # 用途：等待已启动流程终态，只读轮询不重复发起运行；日期：2026-11-04。
    def wait_run(identity):
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            result = client.call("workflow_status", {"runId": identity})
            if result["state"] in ["succeeded", "failed", "outcome-unknown", "interrupted", "cancelled"]:
                return result
            time.sleep(0.02)
        raise TimeoutError("Recorded workflow did not complete")

    try:
        identity = profile("录制源环境")
        call("page_pick_start")
        assert call("page_pick_read")["state"] == "picking"
        gesture("picker")
        picked = call("page_pick_read")
        assert picked["state"] == "picked" and picked["selector"] == "#submit", picked
        assert text("#submits") == "0"
        client.call("page_pick_stop", {"profileId": identity})
        assert text("#submits") == "0"
        action("page_click", "#submit")
        assert text("#submits") == "1"
        report["checks"].append("native-picker-intercepts-submit-click-without-replay-and-returns-unique-selector")
        call("page_pick_start")
        client.call("lease_revoke", {"profileId": identity})
        client.call("page_pick_stop", {"profileId": identity})
        base["lease"] = client.call("lease_acquire", {"profileId": identity, "owner": "after-pick", "ttlMs": 300000})
        report["checks"].append("picker-stop-remains-available-after-lease-revocation")
        call("browser_navigate", {"url": origin + "/recording"})
        call("recorder_start")
        call("recorder_start", error="RECORDING_ACTIVE")
        call("page_open", {"url": origin + "/recording"}, "RECORDING_PAGE_LOCKED")
        action("page_fill", "#user", {"value": "输入区间一"})
        action("page_fill", "#user", {"value": "中文最终帐号"})
        action("page_fill", "#password", {"value": "secret-canary-recording"})
        action("page_select", "#region", {"values": ["us"]})
        action("page_click", "#submit")
        assert text("#submits") == "1", "submit-count=" + text("#submits")
        stopped = call("recorder_control", {"operation": "stop"})
        assert stopped["complete"] and stopped["pageActionsDispatched"] == 0
        kinds = [item["kind"] for item in stopped["events"]]
        assert kinds == ["document", "fill", "fill", "select", "click"], kinds
        encoded = json.dumps(stopped, ensure_ascii=False)
        assert "中文最终帐号" not in encoded and "secret-canary-" not in encoded and '"us"' not in encoded
        assert sum(item["sensitive"] for item in stopped["events"]) == 1
        report["checks"].append("input-interval-merge-secret-no-values-select-and-one-submit")
        draft = stopped["draft"]
        assert client.call("workflow_validate", {"definition": draft})["valid"] is False
        for step in draft["steps"]:
            if step["type"] == "navigate":
                step["url"] = origin + "/recording"
            if step["type"] == "click":
                step["postcondition"] = {"selector": "#status", "expected": "submitted", "timeoutMs": 5000}
        validation = client.call("workflow_validate", {"definition": draft})
        assert validation["valid"], validation
        published = client.call("workflow_publish", {"definition": draft})
        report["checks"].append("missing-postcondition-blocks-publish-and-configured-draft-validates")
        profile("全新回放环境")
        run = client.call("workflow_run", dict(base, workflowId=published["workflowId"], input={
            "input_1": "回放帐号", "input_2": "secret-canary-replay", "input_3": "us"}))
        status = wait_run(run["runId"])
        assert status["state"] == "succeeded", status
        assert text("#submits") == "1" and text("#status") == "submitted"
        report["checks"].append("recorded-dsl-replays-on-new-profile-with-one-submit")
        call("recorder_start")
        client.call("workflow_run", dict(base, workflowId=published["workflowId"], input={}), "RECORDING_ACTIVE")
        initial = call("recorder_control", {"operation": "pause"})
        action("page_click", "#plain")
        assert len(call("recorder_read")["events"]) == len(initial["events"])
        call("recorder_control", {"operation": "resume"})
        action("page_click", "#plain")
        resumed = call("recorder_read")
        assert len(resumed["events"]) == len(initial["events"]) + 1 and text("#clicks") == "2"
        stopped = call("recorder_control", {"operation": "stop"})
        action("page_click", "#plain")
        assert call("recorder_read")["events"] == stopped["events"] and text("#clicks") == "3"
        report["checks"].append("pause-resume-stop-detaches-and-replay-is-mutually-exclusive")
        call("browser_navigate", {"url": origin + "/recording"})
        call("recorder_start")
        gesture("composition")
        action("page_fill", "#password", {"value": "secret-canary-ime"})
        gesture("enter")
        assert text("#submits") == "1", "submit-count=" + text("#submits")
        ime = call("recorder_control", {"operation": "stop"})
        assert [item["kind"] for item in ime["events"]] == ["document", "fill", "fill", "click"]
        assert "中文帐号" not in json.dumps(ime, ensure_ascii=False)
        report["checks"].append("real-chromium-ime-composition-and-enter-click-submit-dedup")
        call("recorder_start")
        action("page_click", "#next")
        call("page_wait_text", {"selector": "#status", "expected": "ready", "timeoutMs": 5000})
        action("page_click", "#plain")
        navigation = call("recorder_control", {"operation": "stop"})
        assert len({item["document"] for item in navigation["events"]}) == 2
        assert sum(item["kind"] == "click" for item in navigation["events"]) == 2
        assert any(item["kind"] == "cross-document-review-required" for item in navigation["requiredConfiguration"])
        report["checks"].append("new-document-preinstalled-listener-and-explicit-navigation-review")
        call("recorder_start")
        client.call("lease_revoke", {"profileId": base["profileId"]})
        call("recorder_read", error="LEASE_REVOKED")
        base["lease"] = client.call("lease_acquire", {"profileId": base["profileId"], "owner": "new-owner", "ttlMs": 300000})
        call("recorder_control", {"operation": "stop"})
        report["checks"].append("recorder-uses-shared-lease-revocation-gate")
        call("browser_navigate", {"url": origin + "/recording"})
        call("recorder_start")
        gesture("overflow")
        overflow = call("recorder_control", {"operation": "stop"})
        assert overflow["complete"] is False and len(overflow["events"]) <= 1000
        assert overflow["draft"] is None and "event-budget-exceeded" in overflow["captureIssues"]
        assert text("#clicks") == "1100"
        report["checks"].append("1000-event-overflow-is-incomplete-without-extra-clicks")
        call("browser_navigate", {"url": origin + "/login"})
        call("recorder_start")
        unsupported = call("recorder_control", {"operation": "stop"})
        assert unsupported["complete"] is False
        report["checks"].append("frame-page-explicitly-incomplete-in-main-document-preview")
        report["result"] = "passed"
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            for identity in created_profiles:
                stopped = client.call("browser_stop", {"profileId": identity})
                assert stopped.get("jobEmptyConfirmed") or stopped.get("alreadyStopped")
            client.close()
            private.cleanup()
        except BaseException as error:
            report["result"] = "failed"
            report["cleanupError"] = str(error)
            raise
        finally:
            report["calls"] = client.events
            report["commandCount"] = len(client.events)
            (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
            server.shutdown()
            server.server_close()
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "commands": report["commandCount"]}))


if __name__ == "__main__":
    main()
