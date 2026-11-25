"""从实际C++注册表导出契约和薄客户端清单，支持差异检查。日期：2026-11-05。"""

import argparse
import json
import subprocess
from pathlib import Path


# 用途：递归映射真实Schema到TS参数类型，保留嵌套必填、枚举和数组元素；日期：2026-11-05。
def typescript_type(schema):
    if "enum" in schema:
        return " | ".join(json.dumps(choice, ensure_ascii=False) for choice in schema["enum"])
    kind = schema["type"]
    if kind == "array":
        return "Array<" + typescript_type(schema["items"]) + ">"
    if kind == "object":
        if schema.get("additionalProperties", True):
            return "Record<string, unknown>"
        fields = [json.dumps(name) + ("" if name in schema.get("required", []) else "?") + ": " + typescript_type(field)
                  for name, field in schema.get("properties", {}).items()]
        return "{ " + "; ".join(fields) + " }"
    return {"string": "string", "integer": "number", "boolean": "boolean"}[kind]


# 用途：生成确定性UTF8产物，检查模式拒绝源码契约漂移；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    process = subprocess.run([str(args.executable.resolve()), "--print-contract"], capture_output=True, timeout=20, check=True)
    contract = json.loads(process.stdout)
    tools = contract["tools"]
    names = [tool["name"] for tool in tools]
    assert contract["contractVersion"] == 1 and len(set(names)) == len(names)
    for tool in tools:
        assert tool["inputSchema"]["additionalProperties"] is False
    data = (json.dumps(contract, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    mapping = {"docs/contracts/tools-v1.json": data,
               "sdk/python/contract_v1.json": data, "sdk/typescript/contract_v1.json": data}
    definitions = ["// 从C++工具注册表生成；用途：提供完整工具名/参数类型；日期：2026-11-05。", "export interface ToolArguments {"]
    for tool in tools:
        definitions.append(f"  {json.dumps(tool['name'])}: " + typescript_type(tool["inputSchema"]) + ";")
    definitions.extend(["}", "export type ToolName = keyof ToolArguments;",
        "export const toolSchemas = " + json.dumps({t["name"]: t["inputSchema"] for t in tools},
                                                  ensure_ascii=False, separators=(",", ":")) + " as const;", ""])
    mapping["sdk/typescript/contract_v1.ts"] = "\n".join(definitions).encode("utf-8")
    api = {"openapi": "3.0.3", "info": {"title": "AI Browser Workbench Local Tool API", "version": contract["productVersion"]},
           "servers": [{"url": "http://127.0.0.1:{port}", "variables": {"port": {"default": "9228"}}}],
           "security": [{"BearerAuth": []}],
           "components": {"securitySchemes": {"BearerAuth": {"type": "http", "scheme": "bearer"}}}, "paths": {}}
    manual = ["# 工具契约目录 v1（生成文件）", "", "日期：2026-11-05。程序：" + contract["productVersion"] +
              "。CLI/MCP/HTTP均复用C++注册表；HTTP路由为 POST /v1/tools/{工具名}。详细参数见 tools-v1.json 或 openapi-v1.json。", "",
              "| 工具 | 用途 | 必填参数 |", "| --- | --- | --- |"]
    for tool in tools:
        api["paths"]["/v1/tools/" + tool["name"]] = {"post": {"operationId": tool["name"], "description": tool["description"],
            "requestBody": {"required": True, "content": {"application/json": {"schema": tool["inputSchema"]}}},
            "responses": {"200": {"description": "Envelope with ok=true and result; business completion requires postconditions"},
                          "default": {"description": "Envelope with ok=false and error.code/message/outcomeUnknown; no automatic retry"}}}}
        manual.append("| " + tool["name"] + " | " + tool["description"] + " | " +
                      ", ".join(tool["inputSchema"]["required"]) + " |")
    mapping["docs/contracts/openapi-v1.json"] = (json.dumps(api, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    mapping["docs/contracts/TOOLS_V1.md"] = ("\n".join(manual) + "\n").encode("utf-8")
    errors = []
    for relative, content in mapping.items():
        path = root / relative
        if args.check:
            if not path.exists() or path.read_bytes() != content:
                errors.append(relative)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    print(json.dumps({"result": "failed" if errors else "passed", "tools": len(tools), "drift": errors}, ensure_ascii=False))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
