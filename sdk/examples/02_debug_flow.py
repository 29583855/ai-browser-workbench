"""示例2：发布已审阅JSON并在首节点暂停，单步后取消。日期：2026-11-05。"""

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from awb_client import Client


# 用途：有界等待具体运行状态；超时只报错，不重新提交工作流；日期：2026-11-05。
def pause(client, run_id, minimum_steps=0):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        state = client.call("workflow_status", {"runId": run_id})
        if state["state"] == "paused" and len(state["steps"]) >= minimum_steps:
            return
        if state["state"] in ["succeeded", "failed", "outcome-unknown", "cancelled", "interrupted"]:
            raise RuntimeError("Run ended: " + state["state"] + "; inspect its status before retrying")
        time.sleep(0.1)
    raise TimeoutError("Inspect runId before any further action: " + run_id)


# 用途：使用显式流程文件和运行输入，首节点单步后停止后续步骤；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", type=Path, required=True)
    parser.add_argument("--profile", required=True)
    parser.add_argument("--flow", type=Path, required=True)
    parser.add_argument("--input", type=Path)
    args = parser.parse_args()
    client = Client.from_file(args.endpoint)
    client.connect()
    definition = json.loads(args.flow.read_text('utf-8'))
    checked = client.call("workflow_validate", {"definition": definition})
    if not checked["valid"]:
        raise ValueError("Flow failed shared validation: " + json.dumps(checked, ensure_ascii=False))
    published = client.call("workflow_publish", {"definition": definition})
    lease = client.call("lease_acquire", {"profileId": args.profile, "owner": "sdk-debug", "ttlMs": 120000})
    run_id = None
    try:
        supplied = json.loads(args.input.read_text('utf-8')) if args.input else {}
        run_id = client.call("workflow_run", {"profileId": args.profile, "lease": lease,
            "workflowId": published["workflowId"], "input": supplied, "debug": {"pauseOnStart": True}})["runId"]
        print("runId=" + run_id)
        pause(client, run_id)
        client.call("workflow_control", {"runId": run_id, "operation": "step"})
        pause(client, run_id, 1)
        print(json.dumps(client.call("workflow_debug", {"runId": run_id, "options": {}}), ensure_ascii=False, indent=2))
    finally:
        if run_id:
            client.call("workflow_control", {"runId": run_id, "operation": "cancel"})
        client.call("lease_revoke", {"profileId": args.profile})


if __name__ == "__main__":
    main()
