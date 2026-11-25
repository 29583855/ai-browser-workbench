"""示例4：使用本机SecretRef填写登录表单，并核对指定业务成功文本。日期：2026-11-05。"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from awb_client import Client


# 用途：读取不含口令的场景配置，登录只提交一次，失败或未知均不重发；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", type=Path, required=True)
    args = parser.parse_args()
    scenario = json.loads(args.scenario.read_text("utf-8"))
    client = Client.from_file(scenario["endpointFile"])
    client.connect()
    profile = scenario["profileId"]
    lease = client.call("lease_acquire", {"profileId": profile, "owner": "sdk-python-login", "ttlMs": 60000})
    base = {"profileId": profile, "lease": lease}
    try:
        client.call("browser_navigate", {**base, "url": scenario["url"]})
        username = client.call("page_locate", {**base, "selector": scenario["usernameSelector"]})["ref"]
        client.call("page_fill", {**base, "ref": username, "value": scenario["username"]})
        password = client.call("page_locate", {**base, "selector": scenario["passwordSelector"]})["ref"]
        client.call("page_fill_secret", {**base, "ref": password, "credentialId": scenario["credentialId"]})
        submit = client.call("page_locate", {**base, "selector": scenario["submitSelector"]})["ref"]
        client.call("page_click", {**base, "ref": submit})
        client.call("page_wait_text", {**base, "selector": scenario["successSelector"],
                                       "expected": scenario["successText"], "timeoutMs": 10000})
        print(json.dumps({"result": "passed", "businessAssertion": True, "submittedOnce": True}))
    finally:
        client.call("lease_revoke", {"profileId": profile})


if __name__ == "__main__":
    main()
