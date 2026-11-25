"""使用官方Python MCP SDK检验真实stdio服务与浏览器工具。日期：2026-11-03。"""

import argparse
import asyncio
import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from jsonschema import Draft7Validator
from fixtures.web_app import FixtureServer
from integration_workflow import definition, scenarios
from integration_extraction import extraction_spec, batch_spec


# 用途：实际握手和工具/进程验证，按执行时间保存无秘密记录；日期：2026-11-04。
async def run(args):
    args.output.mkdir(parents=True, exist_ok=True)
    root = args.output.resolve() / "mcp-profiles"
    assert not root.exists(), "Use a new evidence directory"
    server = StdioServerParameters(command=str(args.executable.resolve()),
                                    args=["--root", str(root), "--browser", str(args.browser.resolve())])
    transcript = {"date": datetime.now().astimezone().isoformat(), "sdk": "mcp==1.12.4", "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                  "steps": [], "result": "failed"}
    editor_schema = json.loads((Path(__file__).resolve().parent.parent / "docs/schemas/workflow-v1.schema.json").read_text("utf-8"))
    Draft7Validator.check_schema(editor_schema)
    editor = Draft7Validator(editor_schema)
    for sample, _supplied, _count in scenarios(args.origin):
        editor.validate(sample)
    editor.validate(json.loads(Path(__file__).with_name("fixtures").joinpath("workflow-register-login.json").read_text("utf-8")))
    invalid = definition(args.origin, "unknown-field")
    invalid["arbitraryScript"] = "not-supported"
    assert not editor.is_valid(invalid)
    transcript["workflowEditorSchemaChecks"] = {"draft07SchemaVerified": True, "positiveSamples": 21, "unknownFieldRejected": True,
                                                "semanticValidation": "C++ additionally checks graph, type references, origins and budgets"}
    async with stdio_client(server) as (read, write):
        async with ClientSession(read, write) as session:
            initialized = await session.initialize()
            transcript["initialize"] = initialized.model_dump(mode="json")
            tools = await session.list_tools()
            assert len(tools.tools) >= 42
            names = {tool.name for tool in tools.tools}
            assert {"browser_start", "page_snapshot", "lease_revoke"}.issubset(names)
            transcript["tools"] = [tool.model_dump(mode="json") for tool in tools.tools]

            # 用途：调用工具并检查内容schema/失败标志；日期：2026-11-03。
            async def call(name, arguments, error=None):
                result = await session.call_tool(name, arguments)
                content = json.loads(result.content[0].text)
                if error:
                    assert result.isError and content["error"]["code"] == error, result
                else:
                    assert not result.isError, result
                transcript["steps"].append({"tool": name, "outcome": error or "passed"})
                return content

            capabilities = await call("capabilities", {})
            assert capabilities["mcpStdio"] and not capabilities["productionReady"]
            await call("profile_list", {"ignored": True}, "INVALID_ARGUMENTS")
            await call("not_a_tool", {}, "UNSUPPORTED_COMMAND")
            profile = await call("profile_create", {"name": "MCP中文环境", "allowedOrigins": [args.origin]})
            await call("browser_start", {"profileId": profile["id"], "headless": True})
            status = await call("browser_status", {"profileId": profile["id"]})
            assert status["running"] and status["pid"] > 0
            lease = await call("lease_acquire", {"profileId": profile["id"], "owner": "official-mcp", "ttlMs": 300000})
            base = {"profileId": profile["id"], "lease": lease}
            diagnostic = await call("browser_diagnostics_start", {**base, "routePaths": ["/workflow"]})
            await call("browser_navigate", {**base, "url": args.origin + "/extraction"})
            assert (await call("page_extract", {**base, **extraction_spec()}))["valid"]
            assert (await call("page_batch", {**base, **batch_spec()}))["state"] == "succeeded"
            await call("browser_navigate", {**base, "url": args.origin + "/inspect"})
            inspected = await call("page_inspect", {**base, "selector": "#single", "highlight": True})
            preview = await call("page_preview", {**base, "ref": inspected["ref"], "action": "select"})
            assert preview["readyNow"] and not preview["executed"]
            await call("page_clear_highlight", base)
            selected = await call("page_select", {**base, "ref": inspected["ref"], "values": ["cn"]})
            assert selected["selectedCount"] == 1
            await call("page_wait_text", {**base, "selector": "#single-value", "expected": "cn", "timeoutMs": 5000})
            flow = definition(args.origin, "official-mcp-workflow")
            assert (await call("workflow_validate", {"definition": flow, "input": {}}))["valid"]
            published = await call("workflow_publish", {"definition": flow})
            started = await call("workflow_run", {**base, "workflowId": published["workflowId"], "input": {}})
            for _index in range(100):
                state = await call("workflow_status", {"runId": started["runId"]})
                if state["state"] in ("succeeded", "failed", "outcome-unknown"):
                    break
                await asyncio.sleep(0.05)
            assert state["state"] == "succeeded" and len(state["steps"]) == 3
            diagnostic_events = await call("browser_diagnostics_read", {**base, "epoch": diagnostic["epoch"], "limit": 100})
            assert diagnostic_events["events"] and not diagnostic_events["payloadCapture"]
            assert any(event.get("context", {}).get("runId") == started["runId"] for event in diagnostic_events["events"])
            assert not (await call("browser_diagnostics_stop", base))["active"]
            assert any(item["runId"] == started["runId"] for item in (await call("workflow_runs", {}))["runs"])
            await call("browser_stop", {"profileId": profile["id"]})
            await session.send_ping()
            transcript["result"] = "passed"
    (args.output / "report.json").write_text(json.dumps(transcript, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": "passed", "tools": len(tools.tools), "report": str(args.output / "report.json")}))


# 用途：限制测试参数并执行官方SDK异步互操作；日期：2026-11-03。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    args.origin = f"http://127.0.0.1:{server.server_port}"
    try:
        asyncio.run(run(args))
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
