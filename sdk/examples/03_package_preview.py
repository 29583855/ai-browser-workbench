"""示例3：预览知识包差异，可显式导入且按固定版本导出。日期：2026-11-05。"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from awb_client import Client


# 用途：默认只读预览，显式选项才以已观察版本CAS导入；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--import-package", action="store_true")
    args = parser.parse_args()
    client = Client.from_file(args.endpoint)
    client.connect()
    package = json.loads(args.package.read_text('utf-8'))
    preview = client.call("site_package_preview", {"package": package})
    print(json.dumps(preview, ensure_ascii=False, indent=2))
    if args.import_package:
        client.call("site_package_import", {"package": package, "expectedLatestVersion": preview["latestVersion"]})
        exported = client.call("site_package_export", {"packageId": package["packageId"], "version": package["version"]})
        print(json.dumps(exported, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
