"""验证文件、结构化断言及网页数组到循环的完整流程执行路径。日期：2026-11-05。"""

import argparse
import copy
import hashlib
import json
import threading
import time
from pathlib import Path
from fixtures.web_app import FixtureServer
from integration_browser import Client
from integration_workflow import definition, inputs, start, wait


# 用途：定义可发布的文件及列表流程，网页数据实际传入逐项填写和点击；日期：2026-11-05。
def file_flow(origin):
    value = definition(origin, "文件与网页列表完整流程", [
        {"id": "open", "type": "navigate", "url": origin + "/extraction", "next": "configure"},
        {"id": "configure", "type": "downloadConfig", "directory": {"input": "/downloadDirectory"}, "next": "upload"},
        {"id": "upload", "type": "upload", "selector": "#upload", "path": {"input": "/uploadPath"}, "next": "uploaded"},
        {"id": "uploaded", "type": "assertText", "selector": "#uploaded", "expected": "fixture.txt", "timeoutMs": 1000, "next": "download"},
        {"id": "download", "type": "click", "selector": "#download",
         "postcondition": {"selector": "#title", "expected": "中文😀报告", "timeoutMs": 1000}, "next": "downloaded"},
        {"id": "read", "type": "readTexts", "selector": ".duplicate", "variable": "labels", "maxItems": 2, "next": "each"},
        {"id": "each", "type": "foreach", "items": {"var": "labels"}, "item": "label", "maxIterations": 2, "flow": "body", "next": "count"},
        {"id": "count", "type": "assert", "assertion": {"type": "elementCount", "selector": ".duplicate", "expected": 2}, "next": "checked"},
        {"id": "checked", "type": "assert", "assertion": {"type": "elementState", "selector": "#agree", "state": "checked"}, "next": "verify"},
        {"id": "verify", "type": "assertText", "selector": "#clicks", "expected": "2", "timeoutMs": 1000, "next": "echo"},
        {"id": "echo", "type": "assert", "assertion": {"type": "text", "selector": "#echo", "expected": "B"}, "next": "done"},
        {"id": "downloaded", "type": "downloadWait", "fileName": "fixture.csv", "timeoutMs": 10000, "next": "read"},
        {"id": "done", "type": "end"}],
        inputs({"uploadPath": {"type": "string", "maxLength": 32768},
                "downloadDirectory": {"type": "string", "maxLength": 32768}}),
        {"body": {"entry": "fill", "steps": [
            {"id": "fill", "type": "fill", "selector": "#field", "value": {"var": "label"}, "next": "click"},
            {"id": "click", "type": "click", "selector": "#increment",
             "postcondition": {"selector": "#title", "expected": "中文😀报告", "timeoutMs": 1000}, "next": "done"},
            {"id": "done", "type": "end"}]}})
    value["schemaVersion"] = "1.1"
    value["variables"] = {
        "labels": {"schema": {"type": "array", "items": {"type": "string", "maxLength": 4096}, "maxItems": 2},
                   "initial": {"literal": [], "schema": {"type": "array", "items": {"type": "string", "maxLength": 4096}, "maxItems": 0}}, "sensitive": False},
        "label": {"schema": {"type": "string", "maxLength": 4096}, "initial": "", "sensitive": False}}
    return value


# 用途：构建独立只读流程以验证确定性失败、错误分支及后续动作不执行；日期：2026-11-05。
def observation_flow(origin, node, variable=None):
    value = definition(origin, "只读错误分支", [
        {"id": "open", "type": "navigate", "url": origin + "/extraction", "next": "observe"},
        {"id": "observe", "next": "done", **node}, {"id": "done", "type": "end"}])
    value["schemaVersion"] = "1.1"
    if variable:
        value["variables"] = {"labels": {"schema": {"type": "array", "items": {"type": "string", "maxLength": 4096},
                                                   "maxItems": 2}, "initial": {"literal": [], "schema": {"type": "array", "items": {"type": "string", "maxLength": 4096}, "maxItems": 0}}, "sensitive": False}}
    return value


# 用途：验证完整发布执行、严格预检、真实文件结果、读失败分支和租约吊销；日期：2026-11-05。
def main():
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
    client = Client(args.executable.resolve(), output / "profiles", args.browser.resolve())
    report = {"date": "2026-11-05", "result": "failed", "checks": [], "runs": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        flow = file_flow(origin)
        negatives = []
        for index, field, value in [(1, "directory", "C:/literal"), (2, "path", 42), (5, "maxItems", 33),
                                     (5, "maxItems", 2**63), (5, "variable", "missing"),
                                     (7, "assertion", {"type": "elementCount", "selector": "p", "expected": -1}),
                                     (7, "assertion", {"type": "urlContains", "expected": "x", "ignored": True})]:
            sample = copy.deepcopy(flow)
            sample["steps"][index][field] = value
            negatives.append(sample)
        sample = copy.deepcopy(flow)
        sample["variables"]["labels"]["schema"]["maxItems"] = 1
        negatives.append(sample)
        sample = copy.deepcopy(flow)
        sample["schemaVersion"] = "1.0"
        sample.pop("variables")
        negatives.append(sample)
        for sample in negatives:
            checked = client.call("workflow_validate", {"definition": sample})
            assert not checked["valid"], checked
            client.call("workflow_publish", {"definition": sample}, "WORKFLOW_INVALID")
        report["checks"].append("nine-preflight-negatives-rejected-before-browser-start")
        profile = client.call("profile_create", {"name": "流程文件与数组", "allowedOrigins": [origin]})["id"]
        client.call("browser_start", {"profileId": profile, "headless": True})
        base = {"profileId": profile, "lease": client.call("lease_acquire", {"profileId": profile, "owner": "file-flow", "ttlMs": 300000})}
        upload = output / "fixture.txt"
        upload.write_bytes("真实上传文件".encode("utf-8"))
        supplied = {"uploadPath": str(upload), "downloadDirectory": str(output / "下载目录")}
        run_id, _published = start(client, base, flow, supplied)
        result = wait(client, run_id)
        assert result["state"] == "succeeded", result
        report["runs"].append(result)
        classes = {item["type"]: item["replayClass"] for item in result["steps"]}
        assert classes["upload"] == "page-effect-no-replay" and classes["downloadConfig"] == "local-effect-no-replay"
        assert classes["readTexts"] == classes["assert"] == "read-only"
        downloaded = output / "下载目录/fixture.csv"
        deadline = time.monotonic() + 5
        while not downloaded.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert downloaded.read_text("utf-8").startswith("fixture,value")
        report["checks"].append("real-upload-download-array-foreach-two-clicks-and-final-B")
        for selector, maximum, code in [(".duplicate", 1, "EXTRACTION_CONSTRAINT_FAILED"),
                                         ("body", 2, "EXTRACTION_SOURCE_DENIED")]:
            observed = observation_flow(origin, {"type": "readTexts", "selector": selector,
                                                "maxItems": maximum, "variable": "labels"}, True)
            run_id, _ = start(client, base, observed)
            result = wait(client, run_id)
            assert result["state"] == "failed" and result["errorCode"] == code, result
            assert not any(step["stepId"] == "done" for step in result["steps"])
            observed["name"] += "-handled"
            observed["steps"][1]["onError"] = {"codes": [code], "next": "done"}
            run_id, _ = start(client, base, observed)
            result = wait(client, run_id)
            assert result["state"] == "succeeded" and result["steps"][1]["state"] == "handled-error", result
        report["checks"].append("array-overflow-and-sensitive-source-stop-or-explicit-observation-route")
        failed_assert = observation_flow(origin, {"type": "assert", "assertion": {"type": "urlContains", "expected": "missing"}})
        run_id, _ = start(client, base, failed_assert)
        result = wait(client, run_id)
        assert result["state"] == "failed" and result["errorCode"] == "ASSERTION_FAILED", result
        report["checks"].append("structured-assertion-failure-is-deterministic-and-not-retried")
        published = client.call("workflow_publish", {"definition": flow})
        queued = client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": supplied,
                                               "debug": {"pauseOnStart": True}})
        wait(client, queued["runId"], lambda r: r["state"] == "paused")
        client.call("lease_revoke", {"profileId": profile})
        client.call("workflow_control", {"runId": queued["runId"], "operation": "resume"})
        result = wait(client, queued["runId"])
        assert result["state"] == "failed" and result["errorCode"] == "LEASE_REVOKED" and not result["steps"], result
        client.call("browser_stop", {"profileId": profile})
        report["checks"].append("revoked-lease-prevents-any-first-dispatch")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            client.close()
        finally:
            server.shutdown()
            server.server_close()
            report["commands"] = client.events
            (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "commands": len(client.events)}))


if __name__ == "__main__":
    main()
