"""示例1：在已有受管页面发现功能并读取候选，不触发发布。日期：2026-11-05。"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from awb_client import Client


# 用途：从显式端点/环境取得短租约，查询后撤销自己的控制权；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", type=Path, required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--query", default="发布")
    args = parser.parse_args()
    client = Client.from_file(args.endpoint)
    client.connect()
    lease = client.call("lease_acquire", {"profileId": args.profile, "owner": "sdk-discovery", "ttlMs": 60000})
    try:
        result = client.call("page_discover", {"profileId": args.profile, "lease": lease, "query": args.query, "limit": 10})
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        client.call("lease_revoke", {"profileId": args.profile})


if __name__ == "__main__":
    main()
