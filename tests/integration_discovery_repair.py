"""验证真实功能发现、哈希绑定定位修复、错误路由和短凭据句柄。日期：2026-11-05。"""

import argparse
import copy
import hashlib
import json
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from integration_browser import Client
from integration_workflow import definition, wait


class Handler(BaseHTTPRequestHandler):
    # 用途：避免测试服务输出正文或凭据；日期：2026-11-05。
    def log_message(self, *_args):
        return

    # 用途：同时提供同名、隐藏、禁用和有标签控件以检验候选边界；日期：2026-11-05。
    def do_GET(self):
        body = '''<!doctype html><meta charset="utf-8"><button id="publish">发布</button>
<button id="second">发布文章</button><button id="disabled" disabled>发布禁用</button>
<button hidden>发布隐藏</button><div inert><button>发布惰性</button></div>
<label for="password">登录口令</label><input id="password" type="password" value="never-read-this">
<input id="search" placeholder="搜索文章"><output id="ready">ready</output>
<button id="counter" onclick="this.textContent='clicked'">未点击</button>'''.encode('utf-8')
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# 用途：执行真实CDP及流程运行，输出无敏感值的验收报告；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(tempfile.mkdtemp(prefix="awb-discovery-")) / "data"
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    client = Client(args.executable, root, args.browser)
    report = {"date": "2026-11-05", "result": "failed", "checks": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        doctor = client.call("doctor")
        assert doctor["healthy"] and doctor["readOnly"] and not doctor["quota"]["enforced"]
        assert not doctor["productionReady"]
        profile = client.call("profile_create", {"name": "候选与修复", "allowedOrigins": [origin]})["id"]
        client.call("browser_start", {"profileId": profile, "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile, "owner": "discovery-test", "ttlMs": 300000})
        base = {"profileId": profile, "lease": lease}
        client.call("browser_navigate", {**base, "url": origin})
        observed = client.call("page_discover", {**base, "query": "发布", "role": "button"})
        assert observed["matched"] == 3 and len(observed["candidates"]) == 3
        assert observed["candidates"][0]["selector"] == "#publish"
        assert not next(c for c in observed["candidates"] if c["selector"] == "#disabled")["enabled"]
        assert not observed["executed"] and observed["scope"] == "visible-main-document"
        limited = client.call("page_discover", {**base, "query": "发布", "limit": 1})
        assert limited["truncated"] and len(limited["candidates"]) == 1
        labels = client.call("page_discover", {**base, "query": "登录"})
        assert labels["candidates"][0]["selector"] == "#password" and "never-read-this" not in json.dumps(labels)
        ref = client.call("page_locate", {**base, "selector": "#counter"})["ref"]
        assert client.call("page_text", {**base, "ref": ref})["text"] == "未点击"
        report["checks"].append("visible-bounded-ranked-candidates-labels-disabled-no-values-and-no-clicks")

        draft = definition(origin, "定位修复", [{"id": "read", "type": "assertText", "selector": "#old-ready",
                             "expected": "ready", "timeoutMs": 50, "next": "done"}, {"id": "done", "type": "end"}])
        draft["entry"] = "read"
        validation = client.call("workflow_validate", {"definition": draft})
        assert validation["valid"], validation
        sha = validation["sha256"]
        args_repair = {**base, "definition": draft, "expectedSha256": sha, "stepId": "read", "selector": "#ready"}
        repair = client.call("workflow_repair_draft", args_repair)
        expected = copy.deepcopy(draft)
        expected["steps"][0]["selector"] = "#ready"
        assert repair["draft"] == expected and repair["sourceSha256"] == sha and repair["validation"]["valid"]
        assert repair["requiresReview"] and not repair["published"] and not repair["executed"]
        assert client.call("workflow_list")["workflows"] == []
        client.call("workflow_repair_draft", {**args_repair, "expectedSha256": "0" * 64}, "REVISION_CONFLICT")
        client.call("workflow_repair_draft", {**args_repair, "selector": "button"}, "LOCATOR_AMBIGUOUS")
        client.call("workflow_repair_draft", {**args_repair, "stepId": "done"}, "WORKFLOW_REPAIR_UNSUPPORTED")
        report["checks"].append("source-hash-bound-one-selector-draft-shared-validation-no-publish-no-replay")

        routed = definition(origin, "只读错误路由", [
            {"id": "read", "type": "readText", "selector": "#missing", "variable": "text", "next": "done",
             "onError": {"codes": ["ELEMENT_NOT_FOUND"], "next": "recover"}},
            {"id": "recover", "type": "assertText", "selector": "#ready", "expected": "ready", "timeoutMs": 50, "next": "done"},
            {"id": "done", "type": "end"}])
        routed["schemaVersion"] = "1.1"
        routed["entry"] = "read"
        routed["variables"] = {"text": {"schema": {"type": "string", "maxLength": 16384}, "initial": "", "sensitive": False}}
        published = client.call("workflow_publish", {"definition": routed})
        run = client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": {}})
        result = wait(client, run["runId"], lambda r: r["state"] in ["succeeded", "failed", "outcome-unknown"])
        assert result["state"] == "succeeded" and result["steps"][0]["state"] == "handled-error"
        assert result["steps"][0]["errorCode"] == "ELEMENT_NOT_FOUND"
        malformed = copy.deepcopy(routed)
        malformed["steps"][0]["onError"]["codes"] = ["ACTION_OUTCOME_UNKNOWN"]
        assert not client.call("workflow_validate", {"definition": malformed})["valid"]
        malformed = copy.deepcopy(routed)
        malformed["steps"][0]["onError"]["next"] = "read"
        assert not client.call("workflow_validate", {"definition": malformed})["valid"]
        report["checks"].append("handled-read-error-continues-once-unknown-code-and-error-cycle-rejected")

        malformed = copy.deepcopy(routed)
        malformed["steps"][0] = {"id": "read", "type": "set", "variable": "text", "next": "recover",
            "value": {"literal": "ok", "schema": {"type": "string", "maxLength": 3, "enum": ["ok", 42]}}}
        assert not client.call("workflow_validate", {"definition": malformed})["valid"]
        report["checks"].append("expression-literal-enum-all-choices-validated")

        credential = client.call("credential_create", {"name": "短值测试", "value": "a", "purpose": "browser",
                                  "allowedOrigins": [origin], "profileIds": [profile]})
        assert len(credential["credentialId"]) == 32
        ref = client.call("page_locate", {**base, "selector": "#password"})["ref"]
        client.call("page_fill_secret", {**base, "ref": ref, "credentialId": credential["credentialId"]})
        assert client.call("browser_status", {"profileId": profile})["running"]
        assert client.call("capabilities")["workflowDsl"] == "1.1"
        client.call("credential_delete", {"credentialId": credential["credentialId"], "expectedRevision": 1})
        report["checks"].append("one-character-secret-preserves-contract-keys-and-opaque-control-identities")
        client.call("browser_stop", {"profileId": profile})
        report["result"] = "passed"
    finally:
        client.close()
        server.shutdown()
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode('utf-8'))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
