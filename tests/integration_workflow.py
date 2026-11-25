"""真实浏览器流程DSL、图与类型、20流程、控制、故障和重启不重放验收。日期：2026-11-04。"""

import argparse
import copy
import hashlib
import json
import os
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path


# 用途：生成闭合有界输入Schema，测试与产品不共享校验实现；日期：2026-11-04。
def inputs(properties=None, required=None):
    properties = properties or {}
    return {"type": "object", "properties": properties,
            "required": list(properties) if required is None else required, "additionalProperties": False}


# 用途：建立无敏感常量的流程定义，明确预算、作用域及DSL版本；日期：2026-11-04。
def definition(origin, name, steps=None, schema=None, subflows=None):
    value = {"schemaVersion": "1.0", "name": name, "version": 1, "inputSchema": schema or inputs(),
             "policy": {"allowedOrigins": [origin], "maxSteps": 128, "maxDurationMs": 60000},
             "entry": "open", "steps": steps or [
                 {"id": "open", "type": "navigate", "url": origin + "/workflow", "next": "ready"},
                 {"id": "ready", "type": "assertText", "selector": "#ready", "expected": "ready", "timeoutMs": 5000, "next": "done"},
                 {"id": "done", "type": "end"}]}
    if subflows:
        value["subflows"] = subflows
    return value


# 用途：生成每次只提交一个真实点击并要求业务断言的步骤；日期：2026-11-04。
def click(step="action", target="#increment", next_step="done", expected="ready", selector="#ready"):
    return {"id": step, "type": "click", "selector": target,
            "postcondition": {"selector": selector, "expected": expected, "timeoutMs": 5000}, "next": next_step}


# 用途：有界等待异步任务状态或特定检查点，不以固定睡眠假定成功；日期：2026-11-04。
def wait(client, run_id, predicate=None, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = client.call("workflow_status", {"runId": run_id})
        if predicate(result) if predicate else result["state"] in ("succeeded", "failed", "outcome-unknown", "cancelled", "interrupted"):
            return result
        if predicate and result["state"] in ("failed", "outcome-unknown", "cancelled", "interrupted"):
            raise AssertionError("Run ended before the requested boundary: " + json.dumps(result))
        time.sleep(0.025)
    raise AssertionError("Workflow did not reach its expected state: " + json.dumps(result))


# 用途：发布并执行实际流程，返回的检查点必须绑定发布摘要；日期：2026-11-04。
def start(client, base, flow, supplied=None):
    checked = client.call("workflow_validate", {"definition": flow, "input": supplied or {}})
    assert checked["valid"], checked
    published = client.call("workflow_publish", {"definition": flow})
    assert published["sha256"] == checked["sha256"]
    running = client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": supplied or {}})
    assert running["state"] == "queued" and running["sha256"] == published["sha256"]
    return running["runId"], published


# 用途：生成20个不同的正向流程，覆盖真实填写/选择/注册登录和各种控制图；日期：2026-11-04。
def scenarios(origin):
    end = {"id": "done", "type": "end"}
    action = {"entry": "action", "steps": [click(), end]}
    opening = [{"id": "open", "type": "navigate", "url": origin + "/workflow", "next": "ready"},
               {"id": "ready", "type": "assertText", "selector": "#ready", "expected": "ready", "timeoutMs": 5000, "next": "action"}]
    scalar = inputs({"text": {"type": "string", "maxLength": 64}, "number": {"type": "integer", "minimum": 0, "maximum": 3},
                     "flag": {"type": "boolean"}})
    supplied = {"text": "测试文字", "number": 2, "flag": True}
    cases = []
    predicates = [
        {"op": "eq", "left": 1, "right": 1}, {"op": "eq", "left": 1, "right": 2},
        {"op": "eq", "left": {"input": "/text"}, "right": "测试文字"},
        {"op": "ne", "left": {"input": "/flag"}, "right": False},
        {"op": "and", "args": [{"op": "not", "arg": {"op": "eq", "left": 2, "right": 3}}, {"op": "ge", "left": {"input": "/number"}, "right": 2}]},
        {"op": "or", "args": [{"op": "lt", "left": 3, "right": 2}, {"op": "gt", "left": 3, "right": 2}]},
        {"op": "and", "args": [{"op": "le", "left": 2, "right": 2}, {"op": "lt", "left": 1, "right": 2}]}]
    for index, predicate in enumerate(predicates):
        steps = opening + [{"id": "action", "type": "condition", "when": predicate, "then": "yes", "else": "no"},
                           click("yes"), {"id": "no", "type": "assertText", "selector": "#clicks", "expected": "0", "timeoutMs": 500, "next": "done"}, end]
        cases.append((definition(origin, "condition-" + str(index), steps, scalar), supplied, 0 if index == 1 else 1))
    for count in [0, 1, 3, {"input": "/number"}]:
        steps = opening + [{"id": "action", "type": "loop", "count": count, "maxIterations": 3, "flow": "body", "next": "done"}, end]
        cases.append((definition(origin, "loop-" + str(len(cases)), steps, scalar, {"body": action}), supplied, count if isinstance(count, int) else 2))
    nested = {"entry": "inner", "steps": [{"id": "inner", "type": "loop", "count": 2, "maxIterations": 2, "flow": "body", "next": "done"}, end]}
    steps = opening + [{"id": "action", "type": "loop", "count": 2, "maxIterations": 2, "flow": "nested", "next": "done"}, end]
    cases.append((definition(origin, "nested-loops", steps, subflows={"body": action, "nested": nested}), {}, 4))
    steps = opening + [{"id": "action", "type": "call", "flow": "body", "next": "done"}, end]
    cases.append((definition(origin, "subflow-call", steps, subflows={"body": action}), {}, 1))
    steps = opening + [{"id": "action", "type": "condition", "when": predicates[0], "then": "yes", "else": "no"},
                       click("yes", next_step="tail"), click("no", next_step="tail"),
                       {"id": "tail", "type": "assertText", "selector": "#clicks", "expected": "1", "timeoutMs": 500, "next": "done"}, end]
    cases.append((definition(origin, "diamond-join", steps), {}, 1))
    steps = opening + [{"id": "action", "type": "call", "flow": "body", "next": "again"},
                       {"id": "again", "type": "call", "flow": "body", "next": "done"}, end]
    cases.append((definition(origin, "reused-subflow", steps, subflows={"body": action}), {}, 2))
    steps = opening + [{"id": "action", "type": "select", "selector": "#choice", "values": ["b"], "next": "verify"},
                       {"id": "verify", "type": "assertText", "selector": "#selected", "expected": "b", "timeoutMs": 500, "next": "done"}, end]
    cases.append((definition(origin, "native-select", steps), {}, 0))
    steps = opening + [{"id": "action", "type": "fill", "selector": "#value", "value": {"input": "/text"}, "next": "done"}, end]
    cases.append((definition(origin, "secret-input-no-echo", steps, scalar), {**supplied, "text": "secret-canary-workflow"}, 0))
    schema = inputs({"account": inputs({"value": {"type": "string", "maxLength": 64}})})
    steps = opening + [{"id": "action", "type": "fill", "selector": "#value", "value": {"input": "/account/value"}, "next": "done"}, end]
    cases.append((definition(origin, "nested-input-pointer", steps, schema), {"account": {"value": "secret-canary-nested"}}, 0))
    schema = inputs({"user": {"type": "string", "maxLength": 64}, "password": {"type": "string", "maxLength": 64}})
    steps = [{"id": "open", "type": "navigate", "url": origin + "/register", "next": "user"},
             {"id": "user", "type": "fill", "selector": "#username", "value": {"input": "/user"}, "next": "password"},
             {"id": "password", "type": "fill", "selector": "#password", "value": {"input": "/password"}, "next": "register"},
             click("register", "#register", "login", "registered", "#result"),
             click("login", "#login", "identity", "logged-in", "#result"),
             {"id": "identity", "type": "assertText", "selector": "#identity", "expected": {"input": "/user"}, "timeoutMs": 5000, "next": "logout"},
             click("logout", "#logout", "done", "logged-out", "#result"), end]
    cases.append((definition(origin, "registration-login-logout", steps, schema), {"user": "workflow-user", "password": "secret-canary-login-password"}, None))
    cases.append((definition(origin, "sequential-read-only"), {}, 0))
    assert len(cases) == 20
    return cases


# 用途：独立构造语法、类型、能力、图和资源边界反例，确保无效定义不被发布；日期：2026-11-04。
def negative_cases(origin):
    good = definition(origin, "negative-base")
    cases = []
    # 用途：复制基线再注入一个故障，保留每项独立的预期错误码；日期：2026-11-04。
    def add(name, mutate, code="WORKFLOW_SCHEMA_INVALID"):
        value = copy.deepcopy(good)
        mutate(value)
        cases.append((name, value, code))
    add("missing-version", lambda d: d.pop("version"))
    add("unknown-root-field", lambda d: d.update(script="return true"))
    add("unsupported-version", lambda d: d.update(schemaVersion="0.1"), "WORKFLOW_VERSION_UNSUPPORTED")
    add("remote-ref", lambda d: d["inputSchema"].update({"$ref": "https://example.invalid/schema"}))
    add("schema-allOf", lambda d: d["inputSchema"].update(allOf=[]))
    add("open-input-object", lambda d: d["inputSchema"].update(additionalProperties=True))
    add("unknown-input-type", lambda d: d.update(inputSchema=inputs({"a": {"type": "number"}})))
    add("unbounded-input-string", lambda d: d.update(inputSchema=inputs({"a": {"type": "string"}})))
    add("enum-type-mismatch", lambda d: d.update(inputSchema=inputs({"a": {"type": "string", "maxLength": 5, "enum": [3]}})))
    add("enum-duplicate", lambda d: d.update(inputSchema=inputs({"a": {"type": "boolean", "enum": [True, True]}})))
    add("required-duplicate", lambda d: d.update(inputSchema=inputs({"a": {"type": "boolean"}}, ["a", "a"])))
    add("step-unsupported", lambda d: d["steps"][1].update(type="upload"), "WORKFLOW_CAPABILITY_UNSUPPORTED")
    add("step-retry-unsupported", lambda d: d["steps"][1].update(retry={"maxAttempts": 10}))
    add("navigate-outside-scope", lambda d: d["steps"][0].update(url="https://example.invalid/"), "WORKFLOW_ORIGIN_DENIED")
    add("selector-too-long", lambda d: d["steps"][1].update(selector="x" * 2049))
    add("assert-wrong-type", lambda d: d["steps"][1].update(expected=True), "WORKFLOW_TYPE_INVALID")
    add("undefined-input-reference", lambda d: d["steps"][1].update(expected={"input": "/missing"}), "WORKFLOW_TYPE_INVALID")
    add("duplicate-step", lambda d: d["steps"].append(d["steps"][1]), "WORKFLOW_GRAPH_INVALID")
    add("missing-edge-target", lambda d: d["steps"][0].update(next="missing"), "WORKFLOW_GRAPH_INVALID")
    add("implicit-cycle", lambda d: d["steps"][1].update(next="open"), "WORKFLOW_GRAPH_INVALID")
    add("unreachable-step", lambda d: d["steps"].append({"id": "unused", "type": "end"}), "WORKFLOW_GRAPH_INVALID")
    add("unused-subflow", lambda d: d.update(subflows={"unused": {"entry": "done", "steps": [{"id": "done", "type": "end"}]}}), "WORKFLOW_GRAPH_INVALID")
    add("budget-too-low", lambda d: d["policy"].update(maxSteps=1), "WORKFLOW_BUDGET")
    add("missing-click-postcondition", lambda d: d["steps"].__setitem__(1, {"id": "ready", "type": "click", "selector": "#increment", "next": "done"}))
    add("literal-fill-secret-forbidden", lambda d: d["steps"].__setitem__(1, {"id": "ready", "type": "fill", "selector": "#value", "value": "not-an-input-ref", "next": "done"}))
    add("float-integer-policy", lambda d: d["policy"].update(maxSteps=1.5))
    add("array-too-many-items", lambda d: d.update(inputSchema=inputs({"a": {"type": "array", "items": {"type": "boolean"}, "maxItems": 33}})))
    add("integer-out-of-range", lambda d: d.update(inputSchema=inputs({"a": {"type": "integer", "minimum": 3, "maximum": 2}})))
    add("optional-input-reference", lambda d: (d.update(inputSchema=inputs({"a": {"type": "string", "maxLength": 5}}, [])), d["steps"][1].update(expected={"input": "/a"})), "WORKFLOW_TYPE_INVALID")
    add("unknown-expression-key", lambda d: d["steps"][1].update(expected={"input": "/a", "js": "no"}))
    add("expression-host-eval", lambda d: d["steps"][1].update(expected={"eval": "process.env"}))
    add("enum-boundary-length", lambda d: d.update(inputSchema=inputs({"a": {"type": "string", "maxLength": 1, "enum": ["too-long"]}})))
    recursive = definition(origin, "recursive", [{"id": "open", "type": "call", "flow": "recursive", "next": "done"}, {"id": "done", "type": "end"}],
                           subflows={"recursive": {"entry": "open", "steps": [{"id": "open", "type": "call", "flow": "recursive", "next": "done"}, {"id": "done", "type": "end"}]}})
    cases.append(("recursive-subflow", recursive, "WORKFLOW_GRAPH_INVALID"))
    loop = definition(origin, "unbounded-loop", [{"id": "open", "type": "loop", "flow": "body", "count": 101, "maxIterations": 100, "next": "done"}, {"id": "done", "type": "end"}],
                      subflows={"body": {"entry": "done", "steps": [{"id": "done", "type": "end"}]}})
    cases.append(("loop-count-exceeds-bound", loop, "WORKFLOW_TYPE_INVALID"))
    return cases


# 用途：执行全部真实浏览器及失败恢复场景，并生成脱敏结构化证据；日期：2026-11-04。
def main():
    from integration_browser import Client, FixtureServer
    from integration_api import audit_lock
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    root = output / "流程环境"
    client = Client(args.executable.resolve(), root, args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [], "flows": [], "negatives": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        for name, flow, code in negative_cases(origin):
            checked = client.call("workflow_validate", {"definition": flow})
            assert not checked["valid"] and checked["errors"][0]["code"] == code, (name, checked)
            client.call("workflow_publish", {"definition": flow}, "WORKFLOW_INVALID")
            report["negatives"].append({"name": name, "code": code, "result": "passed"})
        report["checks"].append("strict-dialect-type-capability-graph-and-budget-rejections")
        profile = client.call("profile_create", {"name": "流程真实验收", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile["id"], "owner": "workflow-test", "ttlMs": 300000})
        base = {"profileId": profile["id"], "lease": lease}
        for flow, supplied, count in scenarios(origin):
            run_id, published = start(client, base, flow, supplied)
            result = wait(client, run_id)
            assert result["state"] == "succeeded" and result["checkpointDurable"] and all(step["state"] == "succeeded" for step in result["steps"]), result
            assert len(result["steps"]) <= client.call("workflow_validate", {"definition": flow})["maximumExecutedSteps"]
            if count is not None:
                client.call("page_wait_text", {**base, "selector": "#clicks", "expected": str(count), "timeoutMs": 500})
            original = client.call("workflow_get", {"workflowId": published["workflowId"]})
            changed = copy.deepcopy(flow)
            changed["name"] += "-new-draft"
            client.call("workflow_validate", {"definition": changed})
            assert client.call("workflow_get", {"workflowId": published["workflowId"]}) == original
            assert client.call("workflow_publish", {"definition": flow})["existing"]
            report["flows"].append({"name": flow["name"], "steps": len(result["steps"]), "state": result["state"], "runId": run_id})
        assert server.registration_count == 1 and server.login_count == 1
        report["checks"].append("twenty-real-flows-and-immutable-content-hashes")
        flow = definition(origin, "pause-resume")
        run_id, published = start(client, base, flow)
        wait(client, run_id, lambda r: any(s["stepId"] == "ready" and s["state"] == "executing" for s in r["steps"]))
        client.call("page_snapshot", base, "WORKFLOW_PROFILE_BUSY")
        client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": {}}, "WORKFLOW_PROFILE_BUSY")
        assert client.call("workflow_control", {"runId": run_id, "operation": "pause"})["state"] == "pausing"
        paused = wait(client, run_id, lambda r: r["state"] == "paused")
        assert len(paused["steps"]) == 2 and not paused["outcomeUnknown"]
        client.call("workflow_control", {"runId": run_id, "operation": "resume"})
        assert wait(client, run_id)["state"] == "succeeded"
        client.call("workflow_control", {"runId": run_id, "operation": "resume"}, "WORKFLOW_CONTROL_INVALID")
        report["checks"].append("asynchronous-status-pause-drain-resume-and-profile-exclusion")
        run_id, _ = start(client, base, definition(origin, "cancel-at-boundary"))
        wait(client, run_id, lambda r: len(r["steps"]) == 2)
        client.call("workflow_control", {"runId": run_id, "operation": "cancel"})
        cancelled = wait(client, run_id)
        assert cancelled["state"] == "cancelled" and len(cancelled["steps"]) == 2
        report["checks"].append("cancellation-drains-current-observation-and-blocks-next-step")
        flow = definition(origin, "revoke-before-resume")
        run_id, _ = start(client, base, flow)
        wait(client, run_id, lambda r: len(r["steps"]) == 2)
        client.call("workflow_control", {"runId": run_id, "operation": "pause"})
        wait(client, run_id, lambda r: r["state"] == "paused")
        client.call("lease_revoke", {"profileId": profile["id"]})
        client.call("workflow_control", {"runId": run_id, "operation": "resume"})
        revoked = wait(client, run_id)
        assert revoked["state"] == "failed" and revoked["errorCode"] == "LEASE_REVOKED" and len(revoked["steps"]) == 2
        base["lease"] = client.call("lease_acquire", {"profileId": profile["id"], "owner": "workflow-test-next", "ttlMs": 300000})
        report["checks"].append("resumed-run-rechecks-original-lease-even-before-control-only-end")
        invalid_input = definition(origin, "typed-input", schema=inputs({"n": {"type": "integer", "minimum": 0, "maximum": 3}}))
        pub = client.call("workflow_publish", {"definition": invalid_input})
        client.call("workflow_run", {**base, "workflowId": pub["workflowId"], "input": {"n": "3"}}, "WORKFLOW_INPUT_INVALID")
        wrong_scope = definition("https://example.invalid", "wrong-scope")
        pub = client.call("workflow_publish", {"definition": wrong_scope})
        client.call("workflow_run", {**base, "workflowId": pub["workflowId"], "input": {}}, "WORKFLOW_SCOPE_MISMATCH")
        report["checks"].append("run-input-and-normalized-origin-scope-preflight")
        schema = inputs({"label": {"type": "string", "minLength": 1, "maxLength": 2, "enum": ["中文", "ok"]},
                         "values": {"type": "array", "minItems": 1, "maxItems": 2, "items": {"type": "boolean"}}})
        bounded = definition(origin, "input-boundary-validation", schema=schema)
        for supplied in [{"label": "中文", "values": [True]}, {"label": "ok", "values": [False, True]}]:
            assert client.call("workflow_validate", {"definition": bounded, "input": supplied})["valid"]
        for supplied in [{}, {"label": "long", "values": [True]}, {"label": "no", "values": [True]},
                         {"label": "ok", "values": []}, {"label": "ok", "values": [1]},
                         {"label": "ok", "values": [True], "unknown": False}]:
            assert not client.call("workflow_validate", {"definition": bounded, "input": supplied})["valid"]
        report["checks"].append("independent-closed-input-unicode-enum-array-and-required-boundaries")
        flow = definition(origin, "read-only-failure")
        flow["steps"][1]["expected"] = "never"
        flow["steps"][1]["timeoutMs"] = 100
        run_id, _ = start(client, base, flow)
        failure = wait(client, run_id)
        assert failure["state"] == "failed" and failure["errorCode"] == "ASSERTION_TIMEOUT" and not failure["outcomeUnknown"]
        flow = definition(origin, "click-unknown")
        flow["steps"][1] = click("ready", next_step="done", expected="never", selector="#clicks")
        flow["steps"][1]["postcondition"]["timeoutMs"] = 100
        run_id, _ = start(client, base, flow)
        unknown = wait(client, run_id)
        assert unknown["state"] == "outcome-unknown" and unknown["outcomeUnknown"]
        client.call("page_wait_text", {**base, "selector": "#clicks", "expected": "1", "timeoutMs": 100})
        client.call("workflow_control", {"runId": run_id, "operation": "resume"}, "WORKFLOW_CONTROL_INVALID")
        report["checks"].append("read-failure-versus-click-postcondition-unknown-no-retry")
        flow = definition(origin, "checkpoint-write-failure")
        flow["steps"][1] = click("commit", "#slow", "after", "committed-once", "#slow-result")
        flow["steps"][0]["next"] = "commit"
        flow["steps"].insert(2, click("after"))
        before = server.fault_commit_count
        run_id, _ = start(client, base, flow)
        wait(client, run_id, lambda r: any(s["stepId"] == "commit" and s["state"] == "executing" for s in r["steps"]))
        kernel, handle = audit_lock(root / "workflows/runs" / (run_id + ".json"))
        try:
            failed = wait(client, run_id)
            assert failed["state"] == "outcome-unknown" and not failed["checkpointDurable"] and failed["errorCode"] == "WORKFLOW_STORAGE_ERROR", failed
        finally:
            kernel.CloseHandle(handle)
        assert server.fault_commit_count == before + 1 and not any(s["stepId"] == "after" for s in failed["steps"])
        client.call("page_wait_text", {**base, "selector": "#clicks", "expected": "0", "timeoutMs": 100})
        report["checks"].append("checkpoint-commit-failure-freezes-dispatch-and-preserves-prior-intent")
        client.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        recovered = client.call("workflow_status", {"runId": run_id})
        assert recovered["state"] == "outcome-unknown" and recovered["recovered"] and recovered["checkpointDurable"]
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        base["lease"] = client.call("lease_acquire", {"profileId": profile["id"], "owner": "crash-check", "ttlMs": 300000})
        flow = definition(origin, "crash-after-real-commit")
        flow["steps"][0]["next"] = "commit"
        flow["steps"][1] = click("commit", "#fault")
        before = server.fault_commit_count
        run_id, _ = start(client, base, flow)
        deadline = time.monotonic() + 10
        while server.fault_commit_count == before and time.monotonic() < deadline:
            time.sleep(0.01)
        assert server.fault_commit_count == before + 1
        client.process.kill()
        client.process.wait(timeout=5)
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        recovered = client.call("workflow_status", {"runId": run_id})
        assert recovered["state"] == "outcome-unknown" and recovered["recovered"] and server.fault_commit_count == before + 1
        client.call("workflow_control", {"runId": run_id, "operation": "resume"}, "WORKFLOW_CONTROL_INVALID")
        report["checks"].append("process-kill-after-server-commit-restarts-unknown-without-replay")
        for path in (root / "workflows").rglob("*.json"):
            raw = path.read_text("utf-8")
            assert "secret-canary-" not in raw and base["lease"]["token"] not in raw and '"input":{' not in raw
        report["checks"].append("canary-secrets-leases-and-dom-values-absent-from-persistent-checkpoints")
        client.close()
        storage_root = output / "corruption-root"
        client = Client(args.executable.resolve(), storage_root, args.browser.resolve())
        published = client.call("workflow_publish", {"definition": definition(origin, "integrity-check")})
        client.close()
        path = storage_root / "workflows/versions" / (published["workflowId"] + ".json")
        original = path.read_bytes()
        path.write_bytes(original.replace(b'integrity-check', b'integrity-broken'))
        failed = subprocess.run([str(args.executable.resolve()), "--root", str(storage_root), "--browser", str(args.browser.resolve())],
                                input="", capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert failed.returncode == 1 and "WORKFLOW_STORAGE_ERROR" in failed.stderr
        path.write_bytes(original)
        link = output / "hardlink-evidence.json"
        os.link(path, link)
        failed = subprocess.run([str(args.executable.resolve()), "--root", str(storage_root), "--browser", str(args.browser.resolve())],
                                input="", capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert failed.returncode == 1 and "WORKFLOW_STORAGE_ERROR" in failed.stderr
        link.unlink()
        report["checks"].append("corrupt-published-version-and-hardlink-record-rejected-on-reopen")
        temporary = path.with_name(path.name + "." + "a" * 32 + ".tmp")
        temporary.write_bytes(b'{"incomplete-atomic-write":')
        partial = subprocess.run([str(args.executable.resolve()), "--root", str(storage_root), "--browser", str(args.browser.resolve())],
                                 input='{"id":1,"command":"workflow_list","arguments":{}}\n', capture_output=True, text=True,
                                 encoding="utf-8", timeout=15)
        assert partial.returncode == 0 and len(json.loads(partial.stdout)["result"]["workflows"]) == 1, partial.stderr
        assert temporary.read_bytes() == b'{"incomplete-atomic-write":', "Interrupted bytes must remain evidence, never become a published version"
        report["checks"].append("interrupted-atomic-write-temp-remains-evidence-and-is-not-imported")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
    finally:
        if client.process.poll() is None:
            client.close()
        server.shutdown()
        server.server_close()
        report["commands"] = client.events
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "flows": len(report["flows"]), "negativeCases": len(report["negatives"]), "checks": len(report["checks"]), "error": report.get("error")}, ensure_ascii=False))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
