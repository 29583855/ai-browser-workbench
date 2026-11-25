"""通过真实HTTP验证模型Provider预算/失败及站点知识包不可变版本。日期：2026-11-05。"""

import argparse
import copy
import hashlib
import json
import threading
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from integration_browser import Client
from integration_workflow import definition, wait


class Handler(BaseHTTPRequestHandler):
    # 用途：不输出模型请求、标头或提示词；日期：2026-11-05。
    def log_message(self, *_args):
        return

    # 用途：提供知识包回放用的真实网页；日期：2026-11-05。
    def do_GET(self):
        body = b'<!doctype html><meta charset="utf-8"><output id="ready">ready</output>'
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # 用途：实现可控兼容模型端点，保留请求计数和授权布尔值而不存正文；日期：2026-11-05。
    def do_POST(self):
        length = int(self.headers["Content-Length"])
        body = json.loads(self.rfile.read(length))
        self.server.calls += 1
        self.server.authorized.append(self.headers.get("Authorization") == "Bearer " + self.server.key)
        assert body["model"] == "local-test-model" and body["stream"] is False
        assert len(body["messages"]) == 2 and body["response_format"]["type"] == "json_object"
        if self.path == "/redirect":
            self.send_response(307)
            self.send_header("Location", self.server.origin + "/valid")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/delay":
            time.sleep(0.5)
        draft = copy.deepcopy(self.server.draft)
        if self.path == "/scope":
            draft["policy"]["allowedOrigins"] = ["https://example.invalid"]
        if self.path == "/invalid":
            draft["steps"][0]["script"] = "forbidden"
        if self.path == "/secret":
            draft["name"] = self.server.key
        response = {"choices": [{"finish_reason": "length" if self.path == "/truncated" else "stop",
                                  "message": {"content": json.dumps(draft)}}]}
        encoded = b"malformed" if self.path == "/badjson" else json.dumps(response).encode()
        self.send_response(503 if self.path == "/error" else 200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            return


# 用途：每次用真实revision配置端点，保留现有预算计数；日期：2026-11-05。
def configure(client, origin, route, secret=None, calls=100, timeout=2000):
    current = client.call("model_status")
    arguments = {"expectedRevision": current["revision"], "endpoint": origin + route, "model": "local-test-model",
                 "timeoutMs": timeout, "maxCalls": calls, "maxOutputTokens": 256, "maxTotalTokens": 1000000}
    if secret:
        arguments["credentialId"] = secret
    return client.call("model_configure", arguments)


# 用途：顺序执行HTTP、不可变导入及真实工作流回放并写入无正文证据；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = Path(tempfile.mkdtemp(prefix="awb-private-test-")) / "模型与知识包"
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.origin = f"http://127.0.0.1:{server.server_port}"
    server.key = "secret-canary-model-" + hashlib.sha256(str(time.time_ns()).encode()).hexdigest()
    server.calls, server.authorized = 0, []
    server.draft = definition(server.origin, "模型草稿", [{"id": "open", "type": "end"}])
    threading.Thread(target=server.serve_forever, daemon=True).start()
    client = Client(args.executable, root, args.browser)
    report = {"date": "2026-11-05", "result": "failed", "checks": [],
              "executableSha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    task = {"task": "model-prompt-private-canary", "allowedOrigins": [server.origin]}
    try:
        assert not client.call("model_status")["configured"]
        client.call("model_generate_workflow", task, "MODEL_NOT_CONFIGURED")
        secret = client.call("credential_create", {"name": "模型接口", "value": server.key, "purpose": "model",
                             "allowedOrigins": [server.origin], "profileIds": []})["credentialId"]
        configured = configure(client, server.origin, "/valid", secret, calls=2)
        bad = {**configured["config"], "expectedRevision": 0}
        client.call("model_configure", bad, "REVISION_CONFLICT")
        for _ in range(2):
            generated = client.call("model_generate_workflow", task)
            assert generated["draft"] == server.draft and generated["validation"]["valid"]
            assert not generated["published"] and not generated["executed"] and generated["requiresReview"]
        assert server.calls == 2 and all(server.authorized)
        client.call("model_generate_workflow", task, "MODEL_BUDGET_EXCEEDED")
        assert server.calls == 2
        assert client.call("workflow_list")["workflows"] == []
        report["checks"].append("unconfigured-fallback-scoped-auth-real-http-valid-draft-and-durable-admission-budget")
        for route, error, timeout in [
            ("/redirect", "MODEL_REDIRECT_DENIED", 2000), ("/error", "MODEL_HTTP_ERROR", 2000),
            ("/invalid", "MODEL_DRAFT_INVALID", 2000), ("/badjson", "MODEL_RESPONSE_INVALID", 2000),
            ("/truncated", "MODEL_RESPONSE_INVALID", 2000), ("/scope", "MODEL_DRAFT_SCOPE_MISMATCH", 2000),
            ("/secret", "MODEL_DRAFT_SECRET_DETECTED", 2000), ("/delay", "MODEL_TIMEOUT", 100)]:
            configure(client, server.origin, route, secret, timeout=timeout)
            before = server.calls
            client.call("model_generate_workflow", task, error)
            assert server.calls == before + 1
            latest = client.call("model_status")["history"][-1]
            assert latest["errorCode"] == error and latest["state"] in ["failed", "outcome-unknown"]
        report["checks"].append("eight-failures-no-retry-no-redirect-no-secret-export-and-timeout-reservation")
        profile = client.call("profile_create", {"name": "知识包环境", "allowedOrigins": [server.origin]})["id"]
        browser_secret = client.call("credential_create", {"name": "浏览器用途", "value": server.key + "browser", "purpose": "browser",
                                     "allowedOrigins": [server.origin], "profileIds": [profile]})["credentialId"]
        configure(client, server.origin, "/valid", browser_secret)
        before = server.calls
        client.call("model_generate_workflow", task, "CREDENTIAL_SCOPE_DENIED")
        assert server.calls == before
        report["checks"].append("browser-purpose-secret-denied-before-model-network")
        workflow = definition(server.origin, "知识包回放", [
            {"id": "open", "type": "navigate", "url": server.origin + "/", "next": "assert"},
            {"id": "assert", "type": "assertText", "selector": "#ready", "expected": "ready", "timeoutMs": 1000, "next": "done"},
            {"id": "done", "type": "end"}])
        validation = client.call("workflow_validate", {"definition": workflow})
        package = {"schemaVersion": "1.0", "packageId": "fixture-site", "name": "站点知识包", "version": 1,
                   "origin": server.origin, "routePaths": ["/"], "requiredCapabilities": ["navigate", "assertText"],
                   "locators": {"ready": {"selector": "#ready", "routePath": "/"}},
                   "workflows": [{"name": "ready-test", "definition": workflow, "sha256": validation["sha256"]}]}
        preview = client.call("site_package_preview", {"package": package})
        assert preview["valid"] and preview["latestVersion"] == 0 and preview["changes"] and not preview["willExecute"]
        installed = client.call("site_package_import", {"package": package, "expectedLatestVersion": 0})
        assert installed["sha256"] == preview["sha256"]
        client.call("site_package_import", {"package": package, "expectedLatestVersion": 0}, "REVISION_CONFLICT")
        assert client.call("site_package_import", {"package": package, "expectedLatestVersion": 1})["existing"]
        exported = client.call("site_package_export", {"packageId": "fixture-site", "version": 1})
        assert exported["package"] == package
        version2 = copy.deepcopy(package)
        version2["version"] = 2
        version2["locators"]["ready"]["role"] = "status"
        assert client.call("site_package_preview", {"package": version2})["latestVersion"] == 1
        client.call("site_package_import", {"package": version2, "expectedLatestVersion": 1})
        assert len(client.call("site_package_list")["packages"]) == 2
        client.call("site_package_import", {"package": package, "expectedLatestVersion": 2}, "SITE_PACKAGE_VERSION_CONFLICT")
        assert client.call("site_package_export", {"packageId": "fixture-site", "version": 1}) == exported
        report["checks"].append("package-preview-cas-immutable-versions-diff-and-exact-export")
        for mutate in [lambda p: p.update(script="forbidden"), lambda p: p.update(packageId="../escape"),
                       lambda p: p.update(routePaths=["/%2e%2e/secret"]), lambda p: p.update(requiredCapabilities=["closedShadowDom"]),
                       lambda p: p.update(requiredCapabilities=[]), lambda p: p["workflows"][0].update(sha256="0" * 64),
                       lambda p: p["locators"]["ready"].update(routePath="/elsewhere"),
                       lambda p: p["workflows"][0]["definition"].update(secretRef=secret),
                       lambda p: p.update(routePaths=["/undeclared"])]:
            bad = copy.deepcopy(package)
            mutate(bad)
            client.call("site_package_preview", {"package": bad}, "SITE_PACKAGE_INVALID")
        report["checks"].append("nine-package-field-path-capability-secret-and-hash-rejections")
        client.call("browser_start", {"profileId": profile, "headless": True})
        base = {"profileId": profile, "lease": client.call("lease_acquire", {"profileId": profile, "owner": "package", "ttlMs": 300000})}
        published = client.call("workflow_publish", {"definition": exported["package"]["workflows"][0]["definition"]})
        run_id = client.call("workflow_run", {**base, "workflowId": published["workflowId"], "input": {}})["runId"]
        assert wait(client, run_id)["state"] == "succeeded"
        client.call("browser_stop", {"profileId": profile})
        report["checks"].append("exported-package-workflow-real-browser-replay")
        model_state = client.call("model_status")
        client.close()
        client = Client(args.executable, root, args.browser)
        assert client.call("model_status")["reservedTokens"] == model_state["reservedTokens"]
        assert client.call("site_package_export", {"packageId": "fixture-site", "version": 1}) == exported
        for path in [root / "model-provider.json", root / "site-packages.json"]:
            data = path.read_bytes()
            assert server.key.encode() not in data and b"model-prompt-private-canary" not in data
        report["checks"].append("restart-preserves-budgets-and-package-hashes-without-prompts-or-keys")
        clean = Client(args.executable, Path(tempfile.mkdtemp(prefix="awb-clean-package-")) / "data", args.browser)
        try:
            assert clean.call("site_package_list")["packages"] == [] and clean.call("credential_list")["credentials"] == []
            clean.call("site_package_import", {"package": exported["package"], "expectedLatestVersion": 0})
            assert clean.call("site_package_export", {"packageId": "fixture-site", "version": 1}) == exported
            clean_profile = clean.call("profile_create", {"name": "干净项目复用", "allowedOrigins": [server.origin]})["id"]
            clean.call("browser_start", {"profileId": clean_profile, "headless": True})
            clean_lease = clean.call("lease_acquire", {"profileId": clean_profile, "owner": "clean-package", "ttlMs": 60000})
            version = clean.call("workflow_publish", {"definition": exported["package"]["workflows"][0]["definition"]})
            run = clean.call("workflow_run", {"profileId": clean_profile, "lease": clean_lease, "workflowId": version["workflowId"], "input": {}})
            assert wait(clean, run["runId"])["state"] == "succeeded"
            clean.call("browser_stop", {"profileId": clean_profile})
        finally:
            clean.close()
        report["checks"].append("fresh-project-import-exact-hash-no-credentials-and-real-replay")
        report["result"] = "passed"
    finally:
        if client:
            report["calls"] = client.events
            client.close()
        server.shutdown()
        server.server_close()
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "checks": report["checks"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
