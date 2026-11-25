"""真实浏览器凭据、类型变量、列表循环和调试边界集成验收。日期：2026-11-05。"""

import argparse
import copy
import hashlib
import json
import subprocess
import threading
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from integration_browser import Client
from integration_workflow import definition, inputs, wait


class Handler(BaseHTTPRequestHandler):
    # 用途：测试服务不输出请求、账号或口令；日期：2026-11-05。
    def log_message(self, *_args):
        return

    # 用途：提供能立即回显输入的故意敏感页面，检验截图阻断和结构脱敏；日期：2026-11-05。
    def do_GET(self):
        body = '''<!doctype html><meta charset="utf-8"><input id="user"><input id="password" type="password">
<input id="message"><output id="reflection"></output><output id="ready">ready</output><script>
// 用途：故意回显秘密到普通正文，验证产品出口脱敏；日期：2026-11-05。
document.querySelector('#password').addEventListener('input',function(){document.querySelector('#reflection').textContent=this.value;});
</script>'''.encode('utf-8')
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


# 用途：在活动运行准确暂停后取得下一步骤，避免轮询把旧暂停误认为单步完成；日期：2026-11-05。
def paused(client, run_id, ordinal=None):
    return wait(client, run_id, lambda r: r["state"] == "paused" and
                (ordinal is None or r["nextStep"]["ordinal"] >= ordinal))


# 用途：构造公开声明、保守输入敏感传播和子流程共享输出的DSL1.1样例；日期：2026-11-05。
def flow(origin, secret):
    value = definition(origin, "凭据与调试集成", [
        {"id": "open", "type": "navigate", "url": origin, "next": "secret"},
        {"id": "secret", "type": "fill", "selector": "#password", "value": {"secretRef": secret}, "next": "read"},
        {"id": "read", "type": "readText", "selector": "#reflection", "variable": "captured", "next": "each"},
        {"id": "each", "type": "foreach", "items": {"input": "/items"}, "item": "item", "maxIterations": 3,
         "flow": "body", "next": "copy"},
        {"id": "copy", "type": "set", "variable": "derived", "value": {"op": "trim", "args": [{"input": "/message"}]}, "next": "done"},
        {"id": "done", "type": "end"}],
        inputs({"items": {"type": "array", "items": {"type": "string", "maxLength": 16}, "maxItems": 3},
                "message": {"type": "string", "maxLength": 128}}),
        {"body": {"entry": "increment", "steps": [
            {"id": "increment", "type": "set", "variable": "total", "value": {"op": "add", "args": [{"var": "total"}, 1]}, "next": "fill"},
            {"id": "fill", "type": "fill", "selector": "#message", "value": {"var": "item"}, "next": "done"},
            {"id": "done", "type": "end"}]}})
    value["schemaVersion"] = "1.1"
    value["variables"] = {
        "captured": {"schema": {"type": "string", "maxLength": 16384}, "initial": "", "sensitive": False},
        "item": {"schema": {"type": "string", "maxLength": 16}, "initial": "", "sensitive": False},
        "total": {"schema": {"type": "integer", "minimum": 0, "maximum": 100}, "initial": 0, "sensitive": False},
        "derived": {"schema": {"type": "string", "maxLength": 128}, "initial": "", "sensitive": False}}
    return value


# 用途：执行正反场景并保存实际错误码、摘要和无秘密证据；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = Path(tempfile.mkdtemp(prefix="awb-private-test-")) / "数据"
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    canary = "secret-canary-vault-" + hashlib.sha256(str(time.time_ns()).encode()).hexdigest()
    report = {"date": "2026-11-05", "result": "failed", "checks": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    client = Client(args.executable, root, args.browser)
    try:
        profile = client.call("profile_create", {"name": "凭据测试", "allowedOrigins": [origin]})["id"]
        second = client.call("profile_create", {"name": "未授权环境", "allowedOrigins": [origin]})["id"]
        stored = client.call("credential_create", {"name": "测试口令", "value": canary, "purpose": "browser",
                              "allowedOrigins": [origin], "profileIds": [profile]})
        secret = stored["credentialId"]
        assert stored["revision"] == 1 and "value" not in stored and "ciphertext" not in stored
        encrypted = (root / "credentials.dpapi").read_bytes()
        assert canary.encode() not in encrypted and canary.encode("utf-16-le") not in encrypted
        assert b"ciphertext" not in encrypted
        assert len(client.call("credential_list")["credentials"]) == 1
        client.call("credential_rotate", {"credentialId": secret, "expectedRevision": 2, "value": canary}, "REVISION_CONFLICT")
        client.call("credential_create", {"name": "无范围", "value": canary, "purpose": "browser", "allowedOrigins": [origin],
                                          "profileIds": []}, "CREDENTIAL_SCOPE_REQUIRED")
        report["checks"].append("encrypted-at-rest-metadata-only-scoped-create-and-cas")
        for identity in [profile, second]:
            client.call("browser_start", {"profileId": identity, "headless": True})
        base = {"profileId": profile, "lease": client.call("lease_acquire", {"profileId": profile, "owner": "test", "ttlMs": 300000})}
        other = {"profileId": second, "lease": client.call("lease_acquire", {"profileId": second, "owner": "test", "ttlMs": 300000})}
        for context in [base, other]:
            client.call("browser_navigate", {**context, "url": origin})
        reference = client.call("page_locate", {**other, "selector": "#password"})["ref"]
        client.call("page_fill_secret", {**other, "ref": reference, "credentialId": secret}, "CREDENTIAL_SCOPE_DENIED")
        assert client.call("page_screenshot", other)["mime"] == "image/png"
        reference = client.call("page_locate", {**base, "selector": "#password"})["ref"]
        client.call("page_fill_secret", {**base, "ref": reference, "credentialId": secret})
        reference = client.call("page_locate", {**base, "selector": "#reflection"})["ref"]
        assert client.call("page_text", {**base, "ref": reference})["text"] == "[REDACTED]"
        assert canary not in json.dumps(client.call("page_snapshot", base))
        client.call("page_screenshot", base, "SENSITIVE_CAPTURE_BLOCKED")
        client.call("browser_navigate", {**base, "url": origin})
        client.call("page_screenshot", base, "SENSITIVE_CAPTURE_BLOCKED")
        report["checks"].append("profile-denial-reflection-redaction-and-sticky-screenshot-block")
        value = flow(origin, secret)
        supplied = {"items": ["一", "二", "三"], "message": "  private-input  "}
        checked = client.call("workflow_validate", {"definition": value, "input": supplied})
        assert checked["valid"], checked
        published = client.call("workflow_publish", {"definition": value})
        run_id = client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": supplied,
                             "debug": {"pauseOnStart": True, "breakpoints": [{"flow": "main", "stepId": "each"}]}})["runId"]
        first = paused(client, run_id)
        assert first["steps"] == [] and first["nextStep"]["stepId"] == "open"
        client.call("workflow_control", {"runId": run_id, "operation": "step"})
        assert paused(client, run_id, 1)["nextStep"]["stepId"] == "secret"
        client.call("workflow_control", {"runId": run_id, "operation": "resume"})
        assert paused(client, run_id, 3)["nextStep"]["stepId"] == "each"
        debug = client.call("workflow_debug", {"runId": run_id, "options": {}})
        assert debug["variables"]["captured"] == {"type": "string", "sensitive": True, "value": "[REDACTED]"}
        assert debug["variables"]["total"]["value"] == 0
        client.call("workflow_debug", {"runId": run_id, "options": {"breakpoints": [{"flow": "missing", "stepId": "each"}]}},
                    "WORKFLOW_DEBUG_INVALID")
        client.call("workflow_control", {"runId": run_id, "operation": "stepOver"})
        assert paused(client, run_id, 13)["nextStep"]["stepId"] == "copy"
        debug = client.call("workflow_debug", {"runId": run_id, "options": {}})
        assert debug["variables"]["total"]["value"] == 3 and debug["variables"]["item"]["sensitive"]
        client.call("workflow_control", {"runId": run_id, "operation": "step"})
        assert paused(client, run_id, 14)["nextStep"]["stepId"] == "done"
        debug = client.call("workflow_debug", {"runId": run_id, "options": {}})
        assert debug["variables"]["derived"]["sensitive"] and debug["variables"]["derived"]["value"] == "[REDACTED]"
        client.call("workflow_control", {"runId": run_id, "operation": "resume"})
        assert wait(client, run_id)["state"] == "succeeded"
        client.call("workflow_debug", {"runId": run_id, "options": {}}, "WORKFLOW_CONTROL_INVALID")
        report["checks"].append("typed-variables-read-output-foreach-subflow-results-breakpoints-step-and-stepOver")
        # 用途：进入子流程后单步跳出，验证深度边界且执行过的循环不重放；日期：2026-11-05。
        run_id = client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": supplied,
                             "debug": {"breakpoints": [{"flow": "body", "stepId": "increment"}]}})["runId"]
        nested = paused(client, run_id)
        assert nested["nextStep"]["depth"] == 1
        client.call("workflow_debug", {"runId": run_id, "options": {"breakpoints": []}})
        client.call("workflow_control", {"runId": run_id, "operation": "stepOut"})
        assert paused(client, run_id, 13)["nextStep"]["stepId"] == "copy"
        client.call("workflow_control", {"runId": run_id, "operation": "cancel"})
        assert wait(client, run_id)["state"] == "cancelled"
        report["checks"].append("stepOut-and-cancel-at-exact-depth-boundary")
        negatives = []
        for label, mutation in [
            ("unknown-variable", lambda d: d["steps"][4].update(value={"var": "missing"})),
            ("wrong-assignment-type", lambda d: d["steps"][4].update(value=1)),
            ("list-bound", lambda d: d["steps"][3].update(maxIterations=2)),
            ("secret-expression-escape", lambda d: d["steps"][4].update(value={"secretRef": secret})),
            ("host-expression", lambda d: d["steps"][4].update(value={"eval": "host"})),
            ("invalid-initial", lambda d: d["variables"]["total"].update(initial=101)),
        ]:
            bad = copy.deepcopy(value)
            mutation(bad)
            answer = client.call("workflow_validate", {"definition": bad})
            assert not answer["valid"], label
            negatives.append({"case": label, "errorCode": answer["errors"][0]["code"]})
        report["negativeValidation"] = negatives
        report["checks"].append("six-static-type-scope-and-expression-rejections")
        expiry = client.call("credential_create", {"name": "到期", "value": canary + "expired", "purpose": "browser",
                            "allowedOrigins": [origin], "profileIds": [profile], "expiresAtMs": int(time.time() * 1000) + 1500})
        time.sleep(1.6)
        reference = client.call("page_locate", {**base, "selector": "#password"})["ref"]
        client.call("page_fill_secret", {**base, "ref": reference, "credentialId": expiry["credentialId"]}, "CREDENTIAL_EXPIRED")
        rotated = client.call("credential_rotate", {"credentialId": secret, "expectedRevision": 1, "value": canary + "rotated"})
        assert rotated["revision"] == 2
        client.call("credential_delete", {"credentialId": secret, "expectedRevision": 1}, "REVISION_CONFLICT")
        client.call("credential_delete", {"credentialId": secret, "expectedRevision": 2})
        client.call("page_fill_secret", {**base, "ref": reference, "credentialId": secret}, "CREDENTIAL_NOT_FOUND")
        audit = client.call("credential_audit")
        assert {"create", "use", "rotate", "delete"}.issubset({e["operation"] for e in audit["events"]})
        assert {"authorized", "denied"}.issubset({e["result"] for e in audit["events"]})
        report["checks"].append("expiry-rotation-revision-delete-and-audit")
        for identity in [profile, second]:
            client.call("browser_stop", {"profileId": identity})
        report["calls"] = client.events
        client.close()
        client = Client(args.executable, root, args.browser)
        assert len(client.call("credential_list")["credentials"]) == 1
        assert client.call("workflow_status", {"runId": run_id})["state"] == "cancelled"
        client.close()
        client = None
        owned = [root / "credentials.dpapi", root / "audit.jsonl"] + list((root / "workflows").rglob("*.json"))
        for path in owned:
            if path.exists():
                raw = path.read_bytes()
                assert canary.encode() not in raw and b"private-input" not in raw, path.name
        report["checks"].append("restart-persistence-and-secret-free-product-checkpoints")
        encrypted = bytearray((root / "credentials.dpapi").read_bytes())
        encrypted[-10] ^= 0x40
        (root / "credentials.dpapi").write_bytes(encrypted)
        process = subprocess.run([str(args.executable), "--root", str(root), "--browser", str(args.browser)],
                                 input=b"", capture_output=True, timeout=20)
        assert process.returncode != 0 and b"CREDENTIAL_CRYPTO_FAILED" in process.stderr
        assert canary.encode() not in process.stderr + process.stdout
        report["checks"].append("tampered-ciphertext-fails-closed")
        report["result"] = "passed"
    finally:
        if client:
            client.close()
        server.shutdown()
        server.server_close()
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": report["checks"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
