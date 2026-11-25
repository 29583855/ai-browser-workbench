"""打包不包含数据库及浏览器私有数据的独立内容练习源码。日期：2026-11-04。"""

import argparse
import hashlib
import json
import zipfile
from pathlib import Path


# 用途：按固定白名单打包练习站、脚本与说明，并复核归档文件哈希；日期：2026-11-04。
def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    names = [
        "README.md",
        "server.py",
        "index.html",
        "app.js",
        "style.css",
        "runner.py",
        "workflow-publish-search.json",
        "inputs.example.json",
        "Start-Site.cmd",
        "Start-Automation.cmd",
    ]
    payload = {
        "content-lab/" + name: (root / "examples/content-lab" / name).read_bytes()
        for name in names
    }
    manifest = {
        "date": "2026-11-04",
        "type": "content-lab-example-source",
        "requires": [
            "Python 3.10+",
            "existing complete Windows x64 Workbench portable program",
        ],
        "runtimeDataIncluded": False,
        "files": [
            {
                "path": name,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            for name, data in payload.items()
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(
        args.output, "x", zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for name, data in payload.items():
            archive.writestr(name, data)
        archive.writestr(
            "content-lab/package-manifest.json",
            (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"),
        )
    with zipfile.ZipFile(args.output) as archive:
        assert archive.testzip() is None
        for item in manifest["files"]:
            assert (
                hashlib.sha256(archive.read(item["path"])).hexdigest() == item["sha256"]
            )
    print(
        json.dumps(
            {
                "result": "passed",
                "path": str(args.output),
                "sourceFiles": len(payload),
                "bytes": args.output.stat().st_size,
                "sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
            }
        )
    )


if __name__ == "__main__":
    main()
