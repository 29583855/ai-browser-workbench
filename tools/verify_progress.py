"""核对执行台账与四份计划的任务状态、证据和独立工时分母。日期：2026-11-03。"""

import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path


# 用途：逐任务核对状态及验收文件，拒绝无证据勾选和丢失工作包；日期：2026-11-03。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    ledger_path = root / "docs/execution/tasks.json"
    ledger = json.loads(ledger_path.read_text("utf-8"))
    errors = []
    plans = {}
    for name in ["03_开发进度与任务清单.md", "10_管理后台功能与开发计划.md",
                 "11_环境销售计费与商业运营后台计划.md", "12_浏览器客户端与后台联动开发计划.md"]:
        for line in (root / "docs" / name).read_text("utf-8").splitlines():
            match = re.match(r"\| ((?:T|ADM|COM|BR)\d{3}) \|", line)
            if match:
                identity = match[1]
                if identity in plans:
                    errors.append("Duplicate plan ID: " + identity)
                plans[identity] = [part.strip() for part in line.split("|")[1:-1]]
    tasks = ledger["tasks"]
    if len({task["id"] for task in tasks}) != len(tasks):
        errors.append("Duplicate ledger task")
    if set(plans) != {task["id"] for task in tasks}:
        errors.append("Ledger IDs differ from original plan IDs")
    for task in tasks:
        if task["state"] not in ["未开始", "进行中", "完成", "已替代", "受阻"]:
            errors.append("Unknown task state: " + task["id"])
        if plans[task["id"]][-1] != task["state"]:
            errors.append("Plan/ledger state mismatch: " + task["id"])
        if task["state"] == "完成" and (not task["evidence"] or task["remaining"]):
            errors.append("Completion lacks evidence or retains unfinished acceptance: " + task["id"])
        if task["state"] == "进行中" and not task["remaining"]:
            errors.append("Partial task must identify remaining acceptance: " + task["id"])
        for item in task["evidence"]:
            path = root / item
            if not path.is_file():
                errors.append("Missing evidence: " + item)
            elif path.suffix == ".json" and path.name in ["report.json", "source-audit.json"]:
                if json.loads(path.read_text("utf-8")).get("result") != "passed":
                    errors.append("Failed evidence used by task: " + task["id"] + ":" + item)
    groups = {}
    for group, baseline in [("local", 317), ("admin", 252), ("commerce", 223)]:
        selected = [task for task in tasks if task["group"] == group]
        completed = [task for task in selected if task["state"] == "完成"]
        weight = sum(task["baselineDays"] for task in completed)
        if sum(task["baselineDays"] for task in selected) != baseline:
            errors.append("Original baseline changed: " + group)
        groups[group] = {"tasks": len(selected), "states": dict(Counter(task["state"] for task in selected)),
                         "completedBaselineDays": weight, "baselineDays": baseline,
                         "weightedTaskAcceptancePercent": round(weight / baseline * 100, 2)}
    report = {"date": ledger["updated"], "result": "failed" if errors else "passed", "errors": errors,
              "taskCountIncludingOverlappingClientAndHistorical": len(tasks), "groups": groups,
              "clientViewIsNonAdditive": True, "releaseReady": False,
              "ledgerSha256": hashlib.sha256(ledger_path.read_bytes()).hexdigest()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps(report, ensure_ascii=False))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
