"""真实Console/异常/网络元数据及步骤关联验收。日期：2026-11-04。"""

import argparse
import hashlib
import json
import threading
import time
import traceback
from datetime import datetime
from pathlib import Path
from integration_browser import Client, FixtureServer
from integration_workflow import definition, start, wait, click


# 用途：分页读取有界诊断，核验序号严格递增且不会重复；日期：2026-11-04。
def read_all(client, base, epoch):
    cursor = {"epoch": epoch, "after": 0}
    events = []
    while True:
        result = client.call("browser_diagnostics_read", {**base, **cursor, "limit": 17})
        events += result["events"]
        cursor = result["cursor"]
        if not result["more"]:
            break
    sequences = [event["sequence"] for event in events]
    assert sequences == sorted(set(sequences)), "Diagnostic records duplicated or reordered"
    return events, result


# 用途：派发一次真实点击，之后仅等待文本，不重试网页动作；日期：2026-11-04。
def action(client, base, selector, expected):
    ref = client.call("page_locate", {**base, "selector": selector})["ref"]
    client.call("page_click", {**base, "ref": ref})
    client.call("page_wait_text", {**base, "selector": "#sent", "expected": expected, "timeoutMs": 5000})


# 用途：验证实际错误请求、异步上下文、分页预算、租约和实例隔离；日期：2026-11-04。
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
    client = Client(args.executable.resolve(), args.output / "profiles", args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        profile = client.call("profile_create", {"name": "诊断验收", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile["id"], "owner": "diagnostics", "ttlMs": 300000})
        base = {"profileId": profile["id"], "lease": lease}
        client.call("browser_diagnostics_start", {**base, "routePaths": ["/diag404?secret"]}, "INVALID_ARGUMENTS")
        collected = client.call("browser_diagnostics_start", {**base, "routePaths": ["/diag404", "/diag-delay", "/diag-redirect", "/diag-fail"]})
        epoch = collected["epoch"]
        assert not collected["payloadCapture"]
        client.call("browser_diagnostics_start", base, "DIAGNOSTICS_ALREADY_ACTIVE")
        client.call("browser_navigate", {**base, "url": origin + "/diagnostics?diagnostic-secret-canary"})
        action(client, base, "#failure", "sent")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            events, status = read_all(client, base, epoch)
            kinds = {event["kind"] for event in events}
            if {"console", "exception", "response", "request-failed", "request-redirect"}.issubset(kinds):
                break
            time.sleep(0.03)
        assert {"console", "exception", "response", "request-failed", "request-redirect"}.issubset(kinds), kinds
        assert any(event.get("level") == "error" and event["contentOmitted"] for event in events)
        assert any(event.get("exceptionClass") == "TypeError" and event["contentOmitted"] for event in events)
        failures = [event for event in events if event["kind"] == "response" and event["route"] == "/diag404"]
        assert failures and all(event["status"] == 404 and event["httpError"] for event in failures)
        assert any(event.get("context", {}).get("command") == "page_click" for event in failures)
        report["checks"].append("real-console-exception-http404-redirect-transport-failure-and-action-association")
        report["sampleEvents"] = events
        client.call("browser_diagnostics_read", {**base, "after": 1}, "DIAGNOSTIC_CURSOR_INVALID")
        client.call("browser_diagnostics_read", {**base, "epoch": "0" * 32}, "DIAGNOSTIC_CURSOR_INVALID")
        report["checks"].append("pagination-strict-sequence-and-invalid-cursor-rejection")
        # 故障后的DOM状态不作为新流程的前置假设，重新导航。
        client.call("browser_navigate", {**base, "url": origin + "/diagnostics"})
        flow = definition(origin, "diagnostic-late-request", [
            click("submit", "#delay", "observe", "sent", "#sent"),
            {"id": "observe", "type": "assertText", "selector": "#late", "expected": "arrived", "timeoutMs": 5000, "next": "done"},
            {"id": "done", "type": "end"}])
        flow["entry"] = "submit"
        run_id, _version = start(client, base, flow)
        client.call("workflow_control", {"runId": run_id, "operation": "pause"})
        paused = wait(client, run_id, lambda state: state["state"] == "paused")
        assert paused["state"] == "paused"
        assert client.call("browser_diagnostics_read", {**base, "epoch": epoch})["active"]
        client.call("workflow_control", {"runId": run_id, "operation": "resume"})
        result = wait(client, run_id)
        assert result["state"] == "succeeded", result
        events, status = read_all(client, base, epoch)
        late = [event for event in events if event["kind"] == "response" and event.get("route") == "/diag-delay"]
        assert late and late[-1]["status"] == 503 and late[-1]["context"]["runId"] == run_id
        assert late[-1]["context"]["stepId"] == "submit" and late[-1]["context"]["ordinal"] == 0
        assert late[-1]["correlation"] == "request-start-context-not-proven-cause"
        report["lateResponse"] = late[-1]
        report["checks"].append("late-response-keeps-originating-run-step-ordinal-with-causality-limitation")
        report["checks"].append("paused-workflow-allows-lease-checked-read-only-diagnostics")
        second = client.call("page_open", {**base, "url": origin + "/diagnostics"})["pageId"]
        action(client, base, "#foreign", "foreign")
        events, status = read_all(client, base, epoch)
        assert status["scopeFilteredEvents"] > 0
        action(client, base, "#failure", "sent")
        events, status = read_all(client, base, epoch)
        assert any(event["kind"] == "console" and event["pageId"] == second for event in events)
        assert all("localhost" not in event.get("origin", "") for event in events)
        report["checks"].append("new-managed-page-inherits-observer-and-foreign-origin-metadata-filtered")
        action(client, base, "#flood", "flooded")
        # 用途：分页期间仍可能收到已产生事件，只读等待稳定环形窗口，不重复点击或放宽512条上限；日期：2026-11-05。
        deadline = time.monotonic() + 5
        observed_windows = []
        while time.monotonic() < deadline:
            events, status = read_all(client, base, epoch)
            observed_windows.append({"observed": len(events), "sequence": status["currentSequence"], "dropped": status["droppedEvents"]})
            if len(events) == 512 and status["droppedEvents"] > 0:
                break
            time.sleep(0.03)
        report["overflowObservations"] = observed_windows
        assert len(events) == 512 and status["droppedEvents"] > 0, observed_windows
        first = client.call("browser_diagnostics_read", {**base, "epoch": epoch, "after": 0})
        assert first["gap"] and first["more"] and first["bufferLimit"] == 512
        report["checks"].append("512-record-bound-and-explicit-overflow-gap")
        action(client, base, "#request-flood", "requested")
        _events, budget = read_all(client, base, epoch)
        assert budget["pendingRequests"] <= 128 and budget["droppedPendingRequests"] > 0, budget
        report["checks"].append("128-pending-request-bound-reports-discarded-correlations")
        assert client.call("browser_reconnect", base)["replayedPageActions"] == 0
        events, status = read_all(client, base, epoch)
        assert any(event["kind"] == "connection-gap" for event in events)
        action(client, base, "#failure", "sent")
        events, status = read_all(client, base, epoch)
        assert any(event["kind"] == "console" for event in events)
        stopped = client.call("browser_diagnostics_stop", base)
        assert not stopped["active"]
        before = client.call("browser_diagnostics_read", base)["currentSequence"]
        action(client, base, "#flood", "flooded")
        assert client.call("browser_diagnostics_read", base)["currentSequence"] == before
        report["checks"].append("explicit-reconnect-gap-no-replay-stop-retains-records-without-collecting")
        restarted = client.call("browser_diagnostics_start", base)
        assert restarted["epoch"] != epoch
        client.call("browser_diagnostics_read", {**base, "epoch": epoch}, "DIAGNOSTIC_CURSOR_INVALID")
        other = client.call("profile_create", {"name": "另一个诊断实例", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": other["id"], "headless": True})
        other_lease = client.call("lease_acquire", {"profileId": other["id"], "owner": "other", "ttlMs": 300000})
        other_base = {"profileId": other["id"], "lease": other_lease}
        client.call("browser_diagnostics_read", {**other_base, "epoch": restarted["epoch"]}, "DIAGNOSTIC_CURSOR_INVALID")
        client.call("browser_stop", {"profileId": other["id"]})
        report["checks"].append("diagnostic-cursor-cannot-cross-profile-instance")
        client.call("lease_revoke", {"profileId": profile["id"]})
        for command in ["browser_diagnostics_read", "browser_diagnostics_stop", "browser_diagnostics_start"]:
            client.call(command, base, "LEASE_REVOKED")
        report["checks"].append("new-epoch-invalidates-old-cursor-and-all-diagnostic-tools-enforce-lease")
        raw = json.dumps(report, ensure_ascii=False)
        assert "diagnostic-secret-canary" not in raw and "foreign-secret-canary" not in raw and lease["token"] not in raw
        report["checks"].append("console-content-url-query-response-body-and-lease-canaries-absent")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        report["traceback"] = traceback.format_exc()
    finally:
        client.close()
        server.shutdown()
        server.server_close()
        report["commands"] = client.events
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": report["checks"], "error": report.get("error")}, ensure_ascii=False))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
