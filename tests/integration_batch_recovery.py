"""在批次已落盘派发但未确认时终止真实CLI，验证重启保留未知结果且不重放。日期：2026-11-05。"""

import argparse
import hashlib
import json
import sqlite3
import time
from datetime import datetime
from pathlib import Path
from integration_browser import Client


# 用途：观察真实批次日志的派发边界后终止本测试进程，再验证恢复、去重和未知详情保护；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "profiles"
    client = Client(args.executable.resolve(), root, args.browser.resolve())
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "checks": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        profile = client.call("profile_create", {"name": "真实崩溃边界", "allowedOrigins": ["https://example.com"]})
        request_id = "e" * 32
        request = {"requestId": request_id, "operations": [{"command": "browser_start",
                   "arguments": {"profileId": profile["id"], "headless": True}}]}
        client.process.stdin.write(json.dumps({"id": 999, "command": "profile_batch", "arguments": request}) + "\n")
        client.process.stdin.flush()
        observed = None
        deadline = time.monotonic() + 10
        with sqlite3.connect(root / "workbench.sqlite3", timeout=1) as database:
            while time.monotonic() < deadline:
                row = database.execute("SELECT record_json FROM profile_batches WHERE id=?", (request_id,)).fetchone()
                if row:
                    current = json.loads(row[0])
                    if not current["finished"] and current["items"][0]["state"] == "dispatching":
                        client.process.kill()
                        client.process.wait(timeout=5)
                        observed = current
                        break
                    assert not current["finished"], "Fixture missed dispatch boundary; cannot claim crash coverage"
                time.sleep(0.001)
        assert observed, "Durable dispatch boundary was not observed"
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        recovered = client.call("profile_batch_get", {"batchId": request_id})
        assert recovered["state"] == "outcome-unknown" and recovered["finished"]
        assert recovered["items"][0]["state"] == "outcome-unknown" and recovered["recoveredAfterRestart"]
        receipt = client.call("profile_batch", request)
        assert receipt["receiptOnly"] and not receipt["replayed"] and receipt["state"] == "outcome-unknown"
        client.call("profile_batch_archive", {"batchId": request_id, "confirmBatchId": request_id}, "BATCH_RECONCILIATION_REQUIRED")
        assert client.call("profile_batch_get", {"batchId": request_id})["items"] == recovered["items"]
        client.call("browser_stop", {"profileId": profile["id"]})
        report["checks"] = ["observed-durable-dispatch-before-real-cli-termination", "restart-marks-unconfirmed-action-unknown",
                            "same-request-is-receipt-only-and-unknown-details-cannot-be-compacted", "explicit-browser-cleanup-after-recovery"]
        report["result"] = "passed"
    except Exception as error:
        report["error"] = repr(error)
        raise
    finally:
        client.close()
        report["commands"] = client.events
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
