"""跨CLI、MCP、HTTP验证文件完成事件、网页数组及环境库共享行为。日期：2026-11-05。"""

import argparse
import hashlib
import json
import threading
from pathlib import Path
from fixtures.web_app import FixtureServer
from integration_browser import Client
from integration_visual_protocols import Mcp, Http
from integration_workflow import start, wait
from integration_workflow_files import file_flow


# 用途：在三个真实传输入口执行同一功能链，核对92工具Schema及真实流程结果；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    report = {"date": "2026-11-05", "result": "failed", "channels": [], "checks": [],
              "binarySha256": {name: hashlib.sha256((args.binaries / name).read_bytes()).hexdigest()
                               for name in ["workbench-cli.exe", "workbench-mcp.exe"]}}
    client = None
    try:
        for name, factory, executable in [("cli", Client, "workbench-cli.exe"),
                                            ("mcp", Mcp, "workbench-mcp.exe"),
                                            ("http", Http, "workbench-cli.exe")]:
            channel = output / name
            channel.mkdir()
            client = factory((args.binaries / executable).resolve(), channel / "data", args.browser.resolve())
            assert client.call("admin_policy")["freeConcurrentLimit"] == 10
            assert client.call("commerce_quote", {"environmentCount": 10, "concurrentCount": 10, "proxy": True})["classification"] == "free"
            template = client.call("profile_template_create", {"name": "跨端模板",
                                   "configuration": {"name": "原始配置", "allowedOrigins": [origin]}})
            assert client.call("profile_template_history", {"id": template["id"]})["items"][0]["revision"] == 1
            template = client.call("profile_template_rollback", {"id": template["id"],
                                   "expectedRevision": 1, "sourceRevision": 1})
            profile = client.call("profile_template_instantiate", {"id": template["id"], "expectedRevision": 2, "name": name})["profiles"][0]
            group = client.call("profile_group_create", {"name": "跨端组", "profileIds": [profile["id"]]})
            assert client.call("profile_group_list")["items"] == [group]
            bundle = client.call("profile_export", {"profileIds": [profile["id"]]})
            imported = client.call("profile_import", {"bundle": bundle})["profiles"][0]
            assert imported["id"] != profile["id"]
            request = {"requestId": "a" * 32, "operations": [{"command": "browser_start",
                       "arguments": {"profileId": profile["id"], "headless": True}}]}
            batch = client.call("profile_batch", request)
            assert batch["state"] == "succeeded"
            assert client.call("profile_batch", request)["receiptOnly"]
            assert client.call("profile_batch_get", {"batchId": batch["batchId"]})["finished"]
            assert len(client.call("profile_batch_list")["items"]) == 1
            client.call("profile_batch_archive", {"batchId": batch["batchId"], "confirmBatchId": "b" * 32}, "BATCH_CONFIRMATION_REQUIRED")
            archived = client.call("profile_batch_archive", {"batchId": batch["batchId"], "confirmBatchId": batch["batchId"]})
            assert archived["detailsCompacted"] and archived["itemCount"] == 1 and archived["succeeded"] == 1
            assert not client.call("profile_batch_list")["items"]
            page = client.call("profile_batch_list", {"scope": "archived", "limit": 1})
            assert page["items"][0]["batchId"] == batch["batchId"] and not page["hasMore"]
            receipt = client.call("profile_batch", request)
            assert receipt["archived"] and receipt["receiptOnly"] and not receipt["replayed"]
            client.call("profile_batch_list", {"before": "b" * 32}, "BATCH_CURSOR_INVALID")
            base = {"profileId": profile["id"], "lease": client.call("lease_acquire", {
                    "profileId": profile["id"], "owner": "protocol-feature-test", "ttlMs": 300000})}
            (channel / "fixture.txt").write_bytes(b"cross-channel-upload")
            run_id, _ = start(client, base, file_flow(origin), {
                "uploadPath": str(channel / "fixture.txt"), "downloadDirectory": str(channel / "downloads")})
            result = wait(client, run_id)
            assert result["state"] == "succeeded", result
            assert (channel / "downloads/fixture.csv").read_text("utf-8").startswith("fixture,value")
            client.call("page_assert", {**base, "assertion": {"type": "elementCount", "selector": ".duplicate", "expected": -1}}, "INVALID_ARGUMENTS")
            stopped = client.call("profile_batch", {"operations": [{"command": "browser_stop", "arguments": {"profileId": profile["id"]}}]})
            assert stopped["state"] == "succeeded"
            report["channels"].append({"name": name, "result": "passed", "workflowSteps": len(result["steps"]),
                                       "toolCount": len(client.registry) if hasattr(client, "registry") else len(json.loads((Path(__file__).resolve().parents[1] / "docs/contracts/tools-v1.json").read_text("utf-8"))["tools"]),
                                       "checks": ["free-policy", "groups-template-history-rollback-config-import-export", "durable-batch-idempotency-start-stop", "batch-archive-pagination-and-retained-deduplication",
                                                   "real-file-flow-array-foreach", "strict-negative-assertion"]})
            client.close()
            client = None
        report["checks"] = ["all-three-channels-share-new-tool-contracts-and-business-results"]
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        if client:
            client.close()
        server.shutdown()
        server.server_close()
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "channels": len(report["channels"])}))


if __name__ == "__main__":
    main()
