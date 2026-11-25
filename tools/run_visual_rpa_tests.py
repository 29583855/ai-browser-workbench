"""顺序复验双配置可视化RPA，避免系统剪贴板和桌面焦点互相干扰。日期：2026-11-04。"""

import argparse
import hashlib
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path


# 用途：执行真实程序及浏览器测试并把每轮报告绑定到实际二进制；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3, choices=range(1, 4))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "runs": [],
              "scope": "Main-document recorder, picker, native canvas, desktop publication and recovery; not the full VR01-VR36 acceptance matrix"}
    try:
        for configuration in ["Debug", "Release"]:
            binaries = args.binaries.resolve() / configuration
            for round_number in range(1, args.rounds + 1):
                for suite in ["canvas", "recovery", "shutdown", "visual_desktop", "recording"]:
                    stem = args.output.resolve() / f"{configuration}-{suite}-{round_number}"
                    desktop = binaries / "workbench.exe"
                    executable = binaries / "workbench-cli.exe" if suite == "recording" else desktop
                    if suite == "canvas":
                        evidence = stem.with_suffix(".json")
                        command = [str(desktop), "--flow-self-test", str(evidence)]
                    else:
                        evidence = stem / "report.json"
                        script = "flow_" + suite if suite in ["recovery", "shutdown"] else suite
                        command = [sys.executable, "-X", "utf8", str(root / "tests" / f"integration_{script}.py"),
                                   "--executable", str(executable), "--output", str(stem)]
                        if suite not in ["recovery", "shutdown"]:
                            command.extend(["--browser", str(args.browser.resolve())])
                    result = subprocess.run(command, cwd=root, capture_output=True, timeout=180)
                    stem.with_suffix(".log").write_bytes(result.stdout + result.stderr)
                    entry = {"configuration": configuration, "round": round_number, "suite": suite,
                             "exitCode": result.returncode, "report": str(evidence.relative_to(root)),
                             "binarySha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
                    report["runs"].append(entry)
                    assert result.returncode == 0, f"Suite failed: {configuration}/{round_number}/{suite}"
                    data = json.loads(evidence.read_text(encoding="utf-8"))
                    assert data["result"] == "passed", f"Evidence failed: {evidence}"
                    entry["checks"] = len(data["checks"])
                    if "paintP95Ms" in data:
                        entry["paintP95Ms"] = data["paintP95Ms"]
                    if "commandCount" in data:
                        entry["commands"] = data["commandCount"]
                    print(json.dumps(entry, ensure_ascii=False), flush=True)
        report["result"] = "passed"
        report["checks"] = sum(run["checks"] for run in report["runs"])
    except BaseException as error:
        report["error"] = str(error)
        raise
    finally:
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))


if __name__ == "__main__":
    main()
