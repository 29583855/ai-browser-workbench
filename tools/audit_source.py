"""核验自有源码编码、格式、未完成标记及依赖哈希，不替代行为测试。日期：2026-11-03。"""

import argparse
import ast
import hashlib
import json
import re
import subprocess
from datetime import datetime
from pathlib import Path


# 用途：扫描自有源码及固定依赖，按实际检查日期报告占位/注释/编码/格式，不修改文件；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--formatter", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    errors = []
    inventory = []
    annotations = 0
    for folder in ["include", "src", "tests", "tools", "sdk"]:
        for path in sorted((root / folder).rglob("*")):
            if path.suffix not in [".cpp", ".hpp", ".py", ".ps1", ".js", ".mjs", ".cjs", ".ts"]:
                continue
            data = path.read_bytes()
            text = data.decode("utf-8")
            if data.startswith(b"\xef\xbb\xbf") or b"\r" in data:
                errors.append(f"Unexpected encoding/newline: {path.relative_to(root)}")
            # 扫描标记通过拼接形成，避免扫描器自己的规则被误报为产品占位。
            markers = ["TO" + "DO", "FIX" + "ME", "H" + "ACK", "NotImplemented" + "Error"]
            for marker in markers:
                for line_number, line in enumerate(text.splitlines(), 1):
                    if re.search(r"\b" + marker + r"\b", line, re.I):
                        errors.append(f"Unfinished marker: {path.relative_to(root)}:{line_number}")
            if path.suffix in [".cpp", ".hpp"]:
                formatted = subprocess.run([str(args.formatter), str(path)], capture_output=True, check=True).stdout
                if formatted != data:
                    errors.append(f"Formatting drift: {path.relative_to(root)}")
                annotations += len(re.findall(r"用途：.*日期：\d{4}-\d{2}-\d{2}", text))
            if path.suffix == ".py":
                tree = ast.parse(text)
                lines = text.splitlines()
                for node in ast.walk(tree):
                    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                        preceding = "\n".join(lines[max(0, node.lineno - 4):node.lineno - 1])
                        if "用途：" not in preceding or "日期：" not in preceding:
                            errors.append(f"Missing Chinese function/date: {path.relative_to(root)}:{node.lineno}")
            if path.suffix in [".js", ".mjs", ".cjs"]:
                subprocess.run(["node", "--check", str(path)], capture_output=True, check=True)
                lines = text.splitlines()
                for line_number, line in enumerate(lines):
                    if re.search(r"\bfunction\s+\w+\s*\(|=>", line):
                        preceding = "\n".join(lines[max(0, line_number - 2):line_number])
                        if "用途：" not in preceding or "日期：" not in preceding:
                            errors.append(f"Missing JS function/date: {path.relative_to(root)}:{line_number + 1}")
            inventory.append({"path": path.relative_to(root).as_posix(), "sha256": hashlib.sha256(data).hexdigest(),
                              "bytes": len(data)})
    lock = json.loads((root / "third_party/dependencies.lock.json").read_text(encoding="utf-8"))
    for dependency in lock["dependencies"]:
        for item in dependency["files"]:
            if hashlib.sha256((root / item["path"]).read_bytes()).hexdigest() != item["sha256"]:
                errors.append("Third-party hash mismatch: " + item["path"])
    catalog_source = (root / "src/core/catalog.cpp").read_text(encoding="utf-8")
    migrations = re.findall(r'R"SQL\((.*?)\)SQL"', catalog_source, re.S)
    migration_hashes = re.search(r'MigrationHashes\[\]\s*=\s*\{(.*?)\};', catalog_source, re.S)
    recorded = re.findall(r'"([0-9a-f]{64})"', migration_hashes[1]) if migration_hashes else []
    if not migrations or [hashlib.sha256(sql.encode("utf-8")).hexdigest() for sql in migrations] != recorded:
        errors.append("Catalog migration SQL/checksum drift")
    report = {"date": datetime.now().astimezone().date().isoformat(), "result": "passed" if not errors else "failed", "errors": errors,
              "ownSourceFiles": len(inventory), "chineseDatedCppAnnotations": annotations,
              "files": inventory, "placeholderMarkersFound": len([e for e in errors if e.startswith("Unfinished")]),
              "scope": "Static source check only; full feature completion requires task acceptance evidence"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({key: report[key] for key in ["result", "ownSourceFiles", "placeholderMarkersFound", "errors"]}, ensure_ascii=False))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
