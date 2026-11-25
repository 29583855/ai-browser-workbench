"""验证分组、版本化模板、原子配置导入导出和逐项批操作。日期：2026-11-05。"""

import argparse
import copy
import hashlib
import json
from pathlib import Path
from integration_browser import Client


# 用途：执行版本冲突、整批预检、真实浏览器状态及重启持久化反例；日期：2026-11-05。
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
    report = {"date": "2026-11-05", "result": "failed", "checks": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        configuration = {"name": "中文模板环境", "allowedOrigins": ["https://example.com"],
                         "runtimeSettings": {"languages": ["zh-CN", "zh"], "timezone": "Asia/Shanghai"}}
        profile = client.call("profile_create", configuration)
        (root / profile["id"] / "preserved-marker.txt").write_text("仅原环境数据", encoding="utf-8")
        bundle = client.call("profile_export", {"profileIds": [profile["id"]]})
        assert bundle["format"] == "awb-profile-config-v1" and len(bundle["configurations"]) == 1
        assert set(bundle["configurations"][0]) == set(configuration)
        client.call("profile_export", {"profileIds": [profile["id"], profile["id"]]}, "INVALID_ARGUMENTS")
        before = client.call("profile_list")["profiles"]
        invalid = copy.deepcopy(bundle)
        invalid["configurations"].append({**configuration, "allowedOrigins": ["file:///C:/Windows"]})
        client.call("profile_import", {"bundle": invalid}, "ORIGIN_DENIED")
        assert client.call("profile_list")["profiles"] == before
        invalid = copy.deepcopy(bundle)
        invalid["configurations"][0]["cookies"] = []
        client.call("profile_import", {"bundle": invalid}, "INVALID_ARGUMENTS")
        imported = client.call("profile_import", {"bundle": bundle})
        cloned = imported["profiles"][0]
        assert imported["atomicCatalog"] and not imported["browserDataImported"]
        assert cloned["id"] != profile["id"] and cloned["runtimeSettings"] == profile["runtimeSettings"]
        assert not (root / cloned["id"] / "preserved-marker.txt").exists()
        report["checks"].append("configuration-only-export-atomic-prevalidation-and-new-identity-import")
        group = client.call("profile_group_create", {"name": "开发组", "profileIds": [profile["id"], cloned["id"]]})
        assert group["revision"] == 1 and set(group["profileIds"]) == {profile["id"], cloned["id"]}
        client.call("profile_group_update", {"id": group["id"], "expectedRevision": 2, "name": "过期写入", "profileIds": []}, "REVISION_CONFLICT")
        client.call("profile_group_update", {"id": group["id"], "expectedRevision": 1, "name": "应回滚",
                                              "profileIds": [profile["id"], "0" * 32]}, "PROFILE_NOT_FOUND")
        assert client.call("profile_group_list")["items"] == [group]
        client.call("profile_group_create", {"name": "重复成员", "profileIds": [profile["id"], profile["id"]]}, "INVALID_ARGUMENTS")
        assert len(client.call("profile_group_list")["items"]) == 1
        group = client.call("profile_group_update", {"id": group["id"], "expectedRevision": 1, "name": "开发组V2", "profileIds": group["profileIds"]})
        assert group["revision"] == 2
        template = client.call("profile_template_create", {"name": "华东模板", "configuration": configuration})
        template = client.call("profile_template_update", {"id": template["id"], "expectedRevision": 1,
                                                             "name": "华东模板V2", "configuration": {**configuration, "runtimeSettings": {"languages": ["en-US"]}}})
        client.call("profile_template_instantiate", {"id": template["id"], "expectedRevision": 1, "name": "过期模板"}, "REVISION_CONFLICT")
        instance = client.call("profile_template_instantiate", {"id": template["id"], "expectedRevision": 2, "name": "模板实例"})["profiles"][0]
        assert instance["id"] not in {profile["id"], cloned["id"]} and instance["name"] == "模板实例"
        assert len(client.call("profile_list")["profiles"]) == 3
        history = client.call("profile_template_history", {"id": template["id"]})
        assert [item["revision"] for item in history["items"]] == [2, 1]
        client.call("profile_template_rollback", {"id": template["id"], "expectedRevision": 1, "sourceRevision": 1}, "REVISION_CONFLICT")
        client.call("profile_template_rollback", {"id": template["id"], "expectedRevision": 2, "sourceRevision": 99}, "TEMPLATE_VERSION_NOT_FOUND")
        template = client.call("profile_template_rollback", {"id": template["id"], "expectedRevision": 2, "sourceRevision": 1})
        assert template["revision"] == 3 and template["name"] == "华东模板" and template["configuration"]["runtimeSettings"] == configuration["runtimeSettings"]
        assert len(client.call("profile_template_history", {"id": template["id"]})["items"]) == 3
        report["checks"].append("group-membership-rollback-pinned-template-and-immutable-history-restore")
        first = {"command": "browser_start", "arguments": {"profileId": profile["id"], "headless": True}}
        malformed = {"command": "profile_purge", "arguments": {"profileId": cloned["id"]}}
        client.call("profile_batch", {"operations": [first, malformed]}, "INVALID_ARGUMENTS")
        assert not client.call("browser_status", {"profileId": profile["id"]})["running"]
        malformed = {"command": "browser_start", "arguments": {"profileId": cloned["id"], "unknown": True}}
        client.call("profile_batch", {"operations": [first, malformed]}, "INVALID_ARGUMENTS")
        assert not client.call("browser_status", {"profileId": profile["id"]})["running"]
        batch_request = {"requestId": "d" * 32, "operations": [first,
            {"command": "browser_start", "arguments": {"profileId": cloned["id"], "headless": True}}]}
        started = client.call("profile_batch", batch_request)
        assert started["state"] == "succeeded" and started["succeeded"] == 2
        receipt = client.call("profile_batch", batch_request)
        assert receipt["receiptOnly"] and not receipt["replayed"] and receipt["succeeded"] == 2
        client.call("profile_batch", {**batch_request, "onError": "continue"}, "BATCH_ID_CONFLICT")
        assert client.call("profile_batch_get", {"batchId": "d" * 32})["finished"]
        assert len(client.call("profile_batch_list")["items"]) == 1
        failed = {"command": "profile_archive", "arguments": {"profileId": profile["id"], "expectedRevision": profile["revision"]}}
        stopping = {"command": "browser_stop", "arguments": {"profileId": cloned["id"]}}
        stopped = client.call("profile_batch", {"operations": [failed, stopping]})
        assert [item["state"] for item in stopped["items"]] == ["failed", "not-run"]
        assert client.call("browser_status", {"profileId": cloned["id"]})["running"]
        partial = client.call("profile_batch", {"operations": [failed, stopping], "onError": "continue"})
        assert partial["state"] == "partial" and partial["failed"] == partial["succeeded"] == 1
        assert not partial["atomic"] and not partial["automaticRetryAllowed"]
        assert not client.call("browser_status", {"profileId": cloned["id"]})["running"]
        client.call("browser_stop", {"profileId": profile["id"]})
        report["checks"].append("whole-batch-preflight-real-start-stop-and-per-item-failure-policy")
        report["eventsBeforeRestart"] = client.events
        client.close()
        client = Client(args.executable.resolve(), root, args.browser.resolve())
        assert client.call("profile_group_list")["items"] == [group]
        assert client.call("profile_template_list")["items"] == [template]
        receipt = client.call("profile_batch", batch_request)
        assert receipt["receiptOnly"] and not receipt["replayed"]
        assert not client.call("browser_status", {"profileId": profile["id"]})["running"]
        assert [item["revision"] for item in client.call("profile_template_history", {"id": template["id"]})["items"]] == [3, 2, 1]
        assert len(client.call("profile_list")["profiles"]) == 3
        trashed = client.call("profile_trash", {"profileId": cloned["id"], "expectedRevision": cloned["revision"]})
        client.call("profile_purge", {"profileId": cloned["id"], "expectedRevision": trashed["revision"], "confirmProfileId": cloned["id"]})
        assert client.call("profile_group_list")["items"][0]["profileIds"] == [profile["id"]]
        client.call("profile_template_delete", {"id": template["id"], "expectedRevision": 1}, "REVISION_CONFLICT")
        client.call("profile_template_delete", {"id": template["id"], "expectedRevision": 3})
        client.call("profile_template_history", {"id": template["id"]}, "LIBRARY_NOT_FOUND")
        client.call("profile_group_delete", {"id": group["id"], "expectedRevision": 2})
        assert not client.call("profile_group_list")["items"] and not client.call("profile_template_list")["items"]
        assert len(client.call("profile_list")["profiles"]) == 2
        assert (root / profile["id"] / "preserved-marker.txt").read_text("utf-8") == "仅原环境数据"
        report["checks"].append("restart-persistence-member-purge-cascade-resource-delete-preserves-profiles")
        report["result"] = "passed"
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        client.close()
        report["commands"] = client.events
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"]),
                      "commands": len(report["commands"]) + len(report.get("eventsBeforeRestart", []))}))


if __name__ == "__main__":
    main()
