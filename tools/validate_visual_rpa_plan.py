"""核对可视化RPA专项任务、估算、依赖和完成证据，不替代功能测试。日期：2026-11-04。"""

import argparse
import json
import re
from pathlib import Path


# 用途：校验专项与原父包的真实状态，禁止无证据勾选或重复增加工时；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    plan = (root / "docs/31_可视化录制拖拽编排与网站自动适配开发计划.md").read_text(encoding="utf-8")
    ledger = json.loads((root / "docs/execution/visual-rpa-tasks.json").read_text(encoding="utf-8"))
    allocation = json.loads((root / "docs/execution/visual-rpa-scope-allocation.json").read_text(encoding="utf-8"))
    parents = {task["id"] for task in json.loads((root / "docs/execution/tasks.json").read_text(encoding="utf-8"))["tasks"]}
    rows = [[part.strip() for part in line.strip("|").split("|")] for line in plan.splitlines()
            if re.match(r"^\| VE\d{3} \|", line)]
    tasks = ledger["tasks"]
    errors = []
    expected = [f"VE{i:03}" for i in range(1, 29)]
    if [row[0] for row in rows] != expected or [task["id"] for task in tasks] != expected:
        errors.append("专项编号不完整或重复")
    if len(allocation["tasks"]) != 28 or sum(item["estimatedRemainingDays"] for item in allocation["tasks"]) != 146:
        errors.append("工时归集数量或总工时不一致")
    graph = {}
    for row, task, estimate in zip(rows, tasks, allocation["tasks"]):
        identity = task["id"]
        graph[identity] = set(re.findall(r"VE\d{3}", row[4]))
        if row[6] != ("[x] 完成" if task["state"] == "完成" else "[ ] " + task["state"]):
            errors.append(identity + "计划和专项台账状态不一致")
        if int(row[3]) != estimate["estimatedRemainingDays"] or estimate["primaryParent"] not in parents:
            errors.append(identity + "工时或主父包不一致")
        if estimate["existingScopeRemainingDays"] + estimate["confirmedNewScopeDays"] + estimate["replacedWorkDays"] != int(row[3]):
            errors.append(identity + "工时归集不平衡")
        if task["state"] == "完成" and (not task["evidence"] or task["remaining"]):
            errors.append(identity + "完成项缺证据或仍有验收缺口")
        if task["state"] == "进行中" and not task["remaining"]:
            errors.append(identity + "进行中项缺少剩余边界")
        for evidence in task["evidence"]:
            if not (root / evidence).is_file():
                errors.append(identity + "证据文件不存在: " + evidence)
    pending = {key: set(value) for key, value in graph.items()}
    order = []
    while pending:
        ready = sorted(key for key, dependencies in pending.items() if not dependencies)
        if not ready:
            errors.append("依赖缺失或存在循环")
            break
        for key in ready:
            del pending[key]
        for dependencies in pending.values():
            dependencies.difference_update(ready)
        order.extend(ready)
    report = {"date": ledger["updated"], "result": "failed" if errors else "passed", "errors": errors,
              "tasks": len(tasks), "completed": sum(task["state"] == "完成" for task in tasks),
              "inProgress": sum(task["state"] == "进行中" for task in tasks),
              "unstarted": sum(task["state"] == "未开始" for task in tasks), "estimatedPersonDays": 146,
              "dependencyOrder": order, "scope": "专项任务与文档静态一致性检查"}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps(report, ensure_ascii=False))
    raise SystemExit(1 if errors else 0)


if __name__ == "__main__":
    main()
