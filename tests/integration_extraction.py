"""验证真实页面Schema提取、整批预检、部分成功及未知结果停批。日期：2026-11-04。"""

import argparse
import copy
import hashlib
import json
import os
import threading
import time
from datetime import datetime
from pathlib import Path
from fixtures.web_app import FixtureServer


# 用途：为CLI、HTTP、官方MCP及桌面提供相同的公开数据提取定义；日期：2026-11-04。
def extraction_spec():
    return {"schema": {"$schema": "http://json-schema.org/draft-07/schema#", "type": "object",
                       "properties": {"title": {"type": "string", "minLength": 5, "maxLength": 5},
                                      "amount": {"type": "number", "minimum": 10, "maximum": 15},
                                      "truth": {"type": "boolean", "enum": [True]},
                                      "count": {"type": "integer", "enum": [2]},
                                      "checked": {"type": "boolean"}, "hint": {"type": "string"}},
                       "required": ["title", "amount", "truth", "count", "checked", "hint"],
                       "additionalProperties": False},
            "bindings": {"title": {"selector": "#title", "source": "text"},
                         "amount": {"selector": "#amount", "source": "text"},
                         "truth": {"selector": "#truth", "source": "text"},
                         "count": {"selector": ".duplicate", "source": "count"},
                         "checked": {"selector": "#agree", "source": "checked"},
                         "hint": {"selector": "#title", "source": "attribute", "attribute": "title"}}}


# 用途：构造必须由业务文本确认的一次点击，避免只检查已发送；日期：2026-11-04。
def click_action(expected):
    return {"type": "click", "selector": "#increment",
            "postcondition": {"selector": "#clicks", "expected": str(expected), "timeoutMs": 500}}


# 用途：构造有填充、选择、业务点击及只读断言的完整批次；日期：2026-11-04。
def batch_spec():
    return {"actions": [{"type": "fill", "selector": "#field", "value": "公开批次数据"},
                        {"type": "assertText", "selector": "#echo", "expected": "公开批次数据", "timeoutMs": 500},
                        {"type": "select", "selector": "#choice", "values": ["b"]},
                        {"type": "assertText", "selector": "#selected", "expected": "b", "timeoutMs": 500},
                        click_action(1)], "onError": "stop"}


# 用途：执行正反数据读取、数组边界和副作用计数，保留失败证据；日期：2026-11-05。
def main():
    from integration_browser import Client

    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    client = Client(args.executable.resolve(), args.output.resolve() / "profiles", args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        origin = f"http://127.0.0.1:{server.server_port}"
        profile = client.call("profile_create", {"name": "结构化提取与批处理", "allowedOrigins": [origin]})
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile["id"], "owner": "extraction-test", "ttlMs": 300000})
        base = {"profileId": profile["id"], "lease": lease}
        client.call("browser_navigate", {**base, "url": origin + "/extraction"})
        # 用途：验证文件上传、下载目录、富文本填写和四类结构化断言的真实CDP路径；日期：2026-11-05。
        upload_source = args.output.resolve() / "upload-fixture.txt"
        upload_source.write_text("仅用于本地上传回归", encoding="utf-8")
        upload_ref = client.call("page_locate", {**base, "selector": "#upload"})["ref"]
        uploaded = client.call("page_upload", {**base, "ref": upload_ref, "path": str(upload_source)})
        assert uploaded["uploaded"] and uploaded["fileName"] == upload_source.name
        oversized = args.output.resolve() / "oversized.bin"
        with oversized.open("wb") as stream:
            stream.truncate(100 * 1024 * 1024 + 1)
        linked = args.output.resolve() / "upload-hardlink.txt"
        os.link(upload_source, linked)
        for invalid_path in [args.output.resolve(), oversized, linked, args.output.resolve() / "absent-file.txt"]:
            client.call("page_upload", {**base, "ref": upload_ref, "path": str(invalid_path)}, "UPLOAD_FILE_INVALID")
        linked.unlink()
        client.call("page_upload", {**base, "ref": client.call("page_locate", {**base, "selector": "#field"})["ref"],
                                     "path": str(upload_source)}, "ELEMENT_NOT_FILE_INPUT")
        download_dir = args.output.resolve() / "downloads"
        client.call("page_download_wait", {**base, "fileName": "fixture.csv", "timeoutMs": 1}, "DOWNLOAD_NOT_CONFIGURED")
        configured = client.call("page_download_config", {**base, "directory": str(download_dir)})
        assert configured["configured"] and Path(configured["directory"]).resolve() == download_dir.resolve()
        client.call("page_download_config", {**base, "directory": str(upload_source)}, "DOWNLOAD_DIRECTORY_INVALID")
        client.call("page_download_wait", {**base, "fileName": "fixture.csv", "timeoutMs": 1}, "DOWNLOAD_TIMEOUT")
        client.call("page_download_wait", {**base, "fileName": "../fixture.csv", "timeoutMs": 1}, "INVALID_ARGUMENTS")
        client.call("page_click", {**base, "ref": client.call("page_locate", {**base, "selector": "#download"})["ref"]})
        completion = client.call("page_download_wait", {**base, "fileName": "fixture.csv", "timeoutMs": 10000})
        assert completion["completed"] and completion["completionEvidence"] == "browser-event-and-file"
        downloaded = download_dir / "fixture.csv"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not downloaded.exists():
            time.sleep(0.05)
        assert downloaded.exists() and downloaded.read_text("utf-8").startswith("fixture,value")
        assert completion["sizeBytes"] == downloaded.stat().st_size
        editable = client.call("page_locate", {**base, "selector": "#editable"})["ref"]
        client.call("page_fill", {**base, "ref": editable, "value": "自动填写富文本"})
        assert client.call("page_text", {**base, "ref": editable})["text"] == "自动填写富文本"
        for assertion in [{"type": "urlContains", "expected": "/extraction"},
                          {"type": "urlEquals", "expected": origin + "/extraction"},
                          {"type": "text", "selector": "#title", "expected": "中文😀报告"},
                          {"type": "elementCount", "selector": ".duplicate", "expected": 2},
                          {"type": "elementState", "selector": "#agree", "state": "checked", "expected": True}]:
            assert client.call("page_assert", {**base, "assertion": assertion})["passed"]
        client.call("page_assert", {**base, "assertion": {"type": "text", "selector": "#title", "expected": "错误文本"}},
                    "ASSERTION_FAILED")
        for bad in [{"type": "elementCount", "selector": ".duplicate", "expected": -1},
                    {"type": "elementState", "selector": "#agree", "state": "checked", "expected": "true"},
                    {"type": "urlContains", "expected": ""},
                    {"type": "text", "selector": "#title", "expected": "x", "ignored": True}]:
            client.call("page_assert", {**base, "assertion": bad}, "INVALID_ARGUMENTS")
        report["checks"].append("upload-download-contenteditable-and-structured-assertions")
        spec = extraction_spec()
        value = client.call("page_extract", {**base, **spec})
        assert value["valid"] and not value["partial"] and value["data"] == {
            "title": "中文😀报告", "amount": 12.5, "truth": True, "count": 2, "checked": True, "hint": "公开提示"}
        assert not value["inputValuesReadable"] and value["frameScope"] == "main-frame-only"
        report["checks"].append("typed-public-text-attributes-count-checkbox-and-unicode-bounds")
        # 用途：验证字符串数组提取、元素顺序和数组约束通过同一Schema入口；日期：2026-11-05。
        array_spec = {"schema": {"type": "object", "properties": {
                                      "labels": {"type": "array", "items": {"type": "string"},
                                                  "minItems": 2, "maxItems": 2}},
                                  "required": ["labels"], "additionalProperties": False},
                      "bindings": {"labels": {"selector": ".duplicate", "source": "texts"}}}
        array_value = client.call("page_extract", {**base, **array_spec})
        assert array_value["valid"] and array_value["data"]["labels"] == ["A", "B"]
        assert array_value["dialect"] == "awb-scalar-extraction-v1-array"
        array_spec["schema"]["properties"]["labels"]["minItems"] = 3
        array_spec["schema"]["properties"]["labels"]["maxItems"] = 3
        array_value = client.call("page_extract", {**base, **array_spec})
        assert not array_value["valid"] and array_value["errors"][0]["code"] == "EXTRACTION_CONSTRAINT_FAILED"
        array_spec["schema"]["properties"]["labels"]["maxItems"] = 2
        client.call("page_extract", {**base, **array_spec}, "INVALID_ARGUMENTS")
        array_spec["schema"]["properties"]["labels"]["minItems"] = 0
        for invalid_bound in [2**32, 2**63, -2**63, 2.5, True]:
            array_spec["schema"]["properties"]["labels"]["maxItems"] = invalid_bound
            client.call("page_extract", {**base, **array_spec}, "INVALID_ARGUMENTS")
        array_spec["schema"]["properties"]["labels"]["maxItems"] = 2
        for constraint in [{"type": "string", "minLength": 2}, {"type": "string", "enum": ["A"]}]:
            array_spec["schema"]["properties"]["labels"]["items"] = constraint
            array_value = client.call("page_extract", {**base, **array_spec})
            assert not array_value["valid"] and array_value["errors"][0]["code"] == "EXTRACTION_CONSTRAINT_FAILED"
        array_spec["schema"]["properties"]["labels"]["items"] = {"type": "string"}
        for denied_selector in ["#private", "body", "#editable"]:
            array_spec["bindings"]["labels"]["selector"] = denied_selector
            array_value = client.call("page_extract", {**base, **array_spec})
            assert not array_value["valid"] and array_value["errors"][0]["code"] == "EXTRACTION_SOURCE_DENIED"
        report["checks"].append("bounded-string-array-extraction-and-constraints")
        missing = copy.deepcopy(spec)
        missing["bindings"]["title"]["selector"] = "#absent"
        observed = client.call("page_extract", {**base, **missing})
        assert not observed["valid"] and observed["missing"] == ["title"] and "amount" in observed["data"]
        missing["schema"]["required"].remove("title")
        observed = client.call("page_extract", {**base, **missing})
        assert observed["valid"] and observed["partial"] and "title" not in observed["data"]
        report["checks"].append("required-and-optional-missing-fields-retain-valid-partial-data")
        for selector, code in [(".duplicate", "LOCATOR_AMBIGUOUS"), ("[", "INVALID_SELECTOR"),
                               ("#private", "EXTRACTION_SOURCE_DENIED"), ("body", "EXTRACTION_SOURCE_DENIED"),
                               ("#large", "EXTRACTION_VALUE_TOO_LARGE")]:
            sample = copy.deepcopy(spec)
            sample["bindings"]["title"]["selector"] = selector
            result = client.call("page_extract", {**base, **sample})
            assert not result["valid"] and {"field": "title", "code": code, "required": True} in result["errors"]
            assert "title" not in result["data"]
        for selector in ["#bad-number", "#huge-number"]:
            sample = copy.deepcopy(spec)
            sample["bindings"]["amount"]["selector"] = selector
            result = client.call("page_extract", {**base, **sample})
            assert not result["valid"] and "amount" not in result["data"]
        report["checks"].append("ambiguous-invalid-selector-secret-controls-oversize-and-invalid-numbers")
        for constraint in [{"type": "number", "minimum": 13}, {"type": "number", "enum": [13]}, {"type": "integer"}]:
            sample = copy.deepcopy(spec)
            sample["schema"]["properties"]["amount"] = constraint
            assert not client.call("page_extract", {**base, **sample})["valid"]
        report["checks"].append("schema-type-enum-and-range-counterexamples")
        negatives = []
        for key, value in [("$ref", "http://untrusted.invalid/schema"), ("oneOf", []), ("additionalProperties", True)]:
            sample = copy.deepcopy(spec); sample["schema"][key] = value; negatives.append(sample)
        sample = copy.deepcopy(spec); sample["schema"]["properties"]["title"]["pattern"] = ".*"; negatives.append(sample)
        sample = copy.deepcopy(spec); sample["bindings"]["hint"]["attribute"] = "value"; negatives.append(sample)
        sample = copy.deepcopy(spec); sample["schema"]["required"].append("unknown"); negatives.append(sample)
        sample = copy.deepcopy(spec); sample["schema"]["properties"]["title"]["minLength"] = 5000; negatives.append(sample)
        sample = copy.deepcopy(spec); sample["bindings"]["title"]["ignored"] = True; negatives.append(sample)
        for sample in negatives:
            client.call("page_extract", {**base, **sample}, "INVALID_ARGUMENTS")
        report["checks"].append("unsupported-schema-dialect-and-binding-fields-rejected")
        for bad in [{"type": "not-implemented"}, {"type": "click", "selector": "#increment"},
                    {"type": "fill", "selector": "#field", "value": 3},
                    {"type": "select", "selector": "#choice", "values": ["b", "b"]}]:
            client.call("page_batch", {**base, "actions": [click_action(1), bad]}, "INVALID_ARGUMENTS")
        client.call("page_batch", {**base, "actions": [click_action(1), {"type": "navigate", "url": "http://denied.invalid"}]}, "ORIGIN_DENIED")
        client.call("page_batch", {**base, "actions": [click_action(1)] * 17}, "INVALID_ARGUMENTS")
        client.call("page_wait_text", {**base, "selector": "#clicks", "expected": "0", "timeoutMs": 500})
        report["checks"].append("whole-batch-shape-budget-and-origins-validated-before-first-click")
        diagnostic = client.call("browser_diagnostics_start", base)
        complete_batch = batch_spec()
        complete_batch["actions"].insert(0, {"type": "navigate", "url": origin + "/extraction"})
        result = client.call("page_batch", {**base, **complete_batch})
        assert result["state"] == "succeeded" and result["succeeded"] == 6 and result["notRun"] == 0
        assert not result["atomic"] and not result["rollbackPerformed"] and not result["automaticRetryAllowed"]
        events = client.call("browser_diagnostics_read", {**base, "epoch": diagnostic["epoch"], "limit": 100})["events"]
        assert any(event.get("context", {}).get("batchId") == result["batchId"] for event in events)
        client.call("browser_diagnostics_stop", base)
        failed = {"type": "fill", "selector": "#missing", "value": "not-applied"}
        result = client.call("page_batch", {**base, "actions": [click_action(2), failed, click_action(3)]})
        assert result["state"] == "partial" and [r["state"] for r in result["items"]] == ["succeeded", "failed", "not-run"]
        assert result["failed"] == 1 and result["stopped"]
        result = client.call("page_batch", {**base, "actions": [failed, click_action(3)], "onError": "continue"})
        assert result["state"] == "partial" and result["succeeded"] == 1 and not result["stopped"]
        report["checks"].append("verified-actions-partial-success-stop-and-explicit-safe-continuation")
        result = client.call("page_batch", {**base, "actions": [click_action(99), click_action(5)], "onError": "continue"})
        assert result["state"] == "outcome-unknown" and result["stopped"] and result["notRun"] == 1
        assert result["items"][0]["error"]["code"] == "BATCH_POSTCONDITION_UNCONFIRMED"
        client.call("page_wait_text", {**base, "selector": "#clicks", "expected": "4", "timeoutMs": 500})
        report["checks"].append("postcondition-unconfirmed-forces-stop-without-repeat-or-rollback")
        client.call("browser_navigate", {**base, "url": origin + "/workflow"})
        result = client.call("page_batch", {**base, "actions": [
            {"type": "fill", "selector": "#normalize", "value": "mixed"},
            {"type": "click", "selector": "#increment", "postcondition": {"selector": "#clicks", "expected": "1", "timeoutMs": 500}}],
            "onError": "continue"})
        assert result["outcomeUnknown"] and result["notRun"] == 1
        report["checks"].append("page-mutates-fill-result-stops-following-write")
        fault = {"type": "click", "selector": "#fault", "postcondition": {
            "selector": "#clicks", "expected": "1", "timeoutMs": 500}}
        result = client.call("page_batch", {**base, "actions": [fault, click_action(1)], "onError": "continue"})
        assert result["outcomeUnknown"] and result["notRun"] == 1 and server.fault_commit_count == 1
        assert result["items"][0]["error"]["code"] == "ACTION_OUTCOME_UNKNOWN"
        client.call("browser_stop", {"profileId": profile["id"]})
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        base["lease"] = client.call("lease_acquire", {"profileId": profile["id"], "owner": "extraction-restarted", "ttlMs": 300000})
        report["checks"].append("actual-submit-lost-cdp-receipt-count-one-and-following-write-not-run")
        client.call("browser_navigate", {**base, "url": origin + "/extraction"})
        client.call("lease_revoke", {"profileId": profile["id"]})
        client.call("page_extract", {**base, **spec}, "LEASE_REVOKED")
        client.call("page_batch", {**base, **batch_spec()}, "LEASE_REVOKED")
        client.call("browser_stop", {"profileId": profile["id"]})
        report["checks"].append("revoked-lease-blocks-both-tools")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        try:
            client.close()
        finally:
            server.shutdown(); server.server_close()
            report["commands"] = client.events
            (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "commands": len(client.events)}))


if __name__ == "__main__":
    main()
