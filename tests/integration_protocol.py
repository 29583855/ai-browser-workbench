"""验证异常JSONL/JSON-RPC输入及错误后的恢复，不操作外部网站。日期：2026-11-04。"""

import argparse
import hashlib
import json
import subprocess
from datetime import datetime
from pathlib import Path


# 用途：一次有界会话发送原始协议输入并捕获真实回复；日期：2026-11-04。
def exchange(executable, browser, root, lines):
    result = subprocess.run([str(executable), "--root", str(root), "--browser", str(browser)],
                            input="\n".join(lines) + "\n", text=True, encoding="utf-8",
                            capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    return [json.loads(line) for line in result.stdout.splitlines()]


# 用途：逐项记录预期与实际错误码，保留失败而不提前跳过其他检查；日期：2026-11-04。
def check(report, name, condition, actual):
    report["checks"].append({"name": name, "result": "passed" if condition else "failed", "actual": actual})


# 用途：验证不合法信封/ID/参数、通知无回复及合法后续请求；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--mcp", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "binarySha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in [args.cli, args.mcp]}}
    cli_inputs = ["{", "[]", '{"id":{},"command":"capabilities"}',
                  '{"id":true,"command":"capabilities"}', '{"id":5,"command":3}',
                  '{"id":6,"command":"capabilities","arguments":[]}',
                  '{"id":7,"command":"capabilities","arguments":{}}']
    replies = exchange(args.cli.resolve(), args.browser.resolve(), args.output.resolve() / "cli", cli_inputs)
    assert len(replies) == len(cli_inputs), replies
    for index, expected in enumerate(["INVALID_JSON", "INVALID_REQUEST", "INVALID_REQUEST", "INVALID_REQUEST", "INVALID_REQUEST", "INVALID_ARGUMENTS"]):
        actual = replies[index].get("error", {}).get("code")
        check(report, "cli-invalid-" + str(index + 1), actual == expected, actual)
    check(report, "cli-recovers-after-errors", replies[-1].get("ok") is True and replies[-1]["id"] == 7, replies[-1].get("ok"))
    deeply_nested = '{"id":1,"command":"workflow_validate","arguments":' + '[' * 96 + '0' + ']' * 96 + '}'
    duplicate = '{"id":2,"command":"capabilities","command":"profile_list","arguments":{}}'
    replies = exchange(args.cli.resolve(), args.browser.resolve(), args.output.resolve() / "cli-bounded-json",
                       [deeply_nested, duplicate, '{"id":3,"command":"capabilities"}'])
    check(report, "cli-depth-before-schema", replies[0].get("error", {}).get("code") == "INPUT_TOO_DEEP", replies[0])
    check(report, "cli-duplicate-fields-rejected", replies[1].get("error", {}).get("code") == "INVALID_JSON", replies[1])
    check(report, "cli-recovers-after-json-budget-rejection", replies[2].get("ok") is True, replies[2].get("ok"))
    mcp_inputs = ["{", "[]", '{"jsonrpc":4,"id":1,"method":"ping"}',
                  '{"jsonrpc":"2.0","id":{},"method":"ping"}',
                  '{"jsonrpc":"2.0","id":true,"method":"ping"}',
                  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
                  '{"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"negative-tests","version":"1"}}}',
                  '{"jsonrpc":"2.0","method":"notifications/initialized"}',
                  '{"jsonrpc":"2.0","method":"not-supported"}',
                  '{"jsonrpc":"2.0","id":4,"method":"tools/list","params":[]}',
                  '{"jsonrpc":"2.0","id":5,"method":"tools/list"}',
                  '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"profile_list","arguments":{"ignored":true}}}',
                  '{"jsonrpc":"2.0","id":7,"method":"ping"}']
    replies = exchange(args.mcp.resolve(), args.browser.resolve(), args.output.resolve() / "mcp", mcp_inputs)
    assert len(replies) == 11, replies
    for index, expected in enumerate([-32700, -32600, -32600, -32600, -32600, -32002]):
        actual = replies[index].get("error", {}).get("code")
        check(report, "mcp-invalid-" + str(index + 1), actual == expected, actual)
    check(report, "mcp-initialize", replies[6].get("result", {}).get("protocolVersion") == "2025-03-26", replies[6].get("id"))
    check(report, "mcp-invalid-params", replies[7].get("error", {}).get("code") == -32602, replies[7].get("error"))
    names = {tool["name"] for tool in replies[8].get("result", {}).get("tools", [])}
    required = {"profile_list", "profile_update", "profile_archive", "profile_trash", "profile_restore", "profile_purge", "browser_start", "page_fill"}
    check(report, "mcp-recovers-and-lists-current-tools", required.issubset(names) and len(names) >= 21, len(names))
    tool_result = replies[9].get("result", {})
    tool_error = json.loads(tool_result.get("content", [{"text": "{}"}])[0]["text"]).get("error", {}).get("code")
    check(report, "mcp-tool-validation", tool_result.get("isError") is True and tool_error == "INVALID_ARGUMENTS", tool_error)
    check(report, "mcp-notifications-no-reply-and-ping", replies[-1] == {"jsonrpc": "2.0", "id": 7, "result": {}}, replies[-1])
    rpc_deep = '{"jsonrpc":"2.0","id":8,"method":"ping","params":' + '[' * 96 + '0' + ']' * 96 + '}'
    rpc_duplicate = '{"jsonrpc":"2.0","id":9,"method":"ping","method":"tools/list"}'
    bounded = exchange(args.mcp.resolve(), args.browser.resolve(), args.output.resolve() / "mcp-bounded-json",
                       [rpc_deep, rpc_duplicate, '{"jsonrpc":"2.0","id":10,"method":"ping"}'])
    check(report, "mcp-depth-rejected-before-tool", bounded[0].get("error", {}).get("code") == -32602, bounded[0])
    check(report, "mcp-duplicate-fields-rejected", bounded[1].get("error", {}).get("code") == -32602, bounded[1])
    check(report, "mcp-recovers-after-json-budget-rejection", bounded[2] == {"jsonrpc": "2.0", "id": 10, "result": {}}, bounded[2])
    report["result"] = "passed" if all(item["result"] == "passed" for item in report["checks"]) else "failed"
    (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]), "failures": [item for item in report["checks"] if item["result"] == "failed"]}))
    raise SystemExit(0 if report["result"] == "passed" else 1)


if __name__ == "__main__":
    main()
