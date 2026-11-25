"""通过真实本机API检验Python/TS客户端及重定向/体积预算。日期：2026-11-05。"""

import argparse
import hashlib
import importlib.util
import json
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from integration_api import start
from integration_browser import FixtureServer
from integration_workflow import definition

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("awb_sdk", ROOT / "sdk/python/awb_client.py")
SDK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK)


class Handler(BaseHTTPRequestHandler):
    # 用途：不输出认证头或业务请求；日期：2026-11-05。
    def log_message(self, *_args):
        return

    # 用途：提供示例独立执行的主文档和明确功能按钮；日期：2026-11-05。
    def do_GET(self):
        body = '<!doctype html><meta charset="utf-8"><button id="publish">发布</button>'.encode('utf-8')
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # 用途：提供拒绝重定向和超大响应的真实传输夹具；日期：2026-11-05。
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.server.calls += 1
        if self.server.mode == "redirect":
            self.send_response(307)
            self.send_header("Location", f"http://127.0.0.1:{self.server.server_port}/target")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Length", str(17 * 1024 * 1024))
        self.end_headers()
        try:
            for _ in range(17):
                self.wfile.write(b"x" * 1024 * 1024)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            return


# 用途：对SDK错误逐项核对且禁止任何自动重发；日期：2026-11-05。
def reject(client, code):
    try:
        client.call("profile_list")
    except SDK.AwbError as error:
        assert error.code == code and error.outcome_unknown
        return
    raise AssertionError("Expected SDK rejection: " + code)


# 用途：运行编译后的TS客户端，证据仅含固定验收字段；日期：2026-11-05。
def typescript(config, expected=None):
    compiled = ROOT / "build/sdk-compiled/client.js"
    assert compiled.is_file(), "Compile SDK with pinned TypeScript before integration"
    command = ["node", str(ROOT / "tests/sdk_client_smoke.cjs"), str(compiled), str(config)]
    if expected:
        command.append(expected)
    result = subprocess.run(command, capture_output=True, timeout=45, check=True)
    answer = json.loads(result.stdout)
    assert answer["result"] == "passed"
    return answer


# 用途：以单独进程运行交付示例，防止示例仅通过语法检查；日期：2026-11-05。
def example(name, endpoint, arguments):
    import sys
    result = subprocess.run([sys.executable, "-X", "utf8", str(ROOT / "sdk/examples" / name),
                             "--endpoint", str(endpoint)] + arguments, capture_output=True, timeout=45, check=True)
    assert b"Traceback" not in result.stderr
    return result.stdout


# 用途：检验真实契约/错误透传、网页操作以及两个客户端的有限读取；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    private = Path(tempfile.mkdtemp(prefix="awb-sdk-"))
    process, config = start(args.executable.resolve(), args.browser.resolve(), private / "data")
    report = {"date": "2026-11-05", "result": "failed", "checks": []}
    report["executableSha256"] = hashlib.sha256(args.executable.read_bytes()).hexdigest()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    login_server = FixtureServer()
    threading.Thread(target=login_server.serve_forever, daemon=True).start()
    try:
        client = SDK.Client.from_file(private / "data/local-api.json")
        health = client.connect()
        assert health["workflowDsl"] == "1.1" and health["commerce"]
        assert not client.call("account_status")["connected"]
        origin = f"http://127.0.0.1:{server.server_port}"
        profile = client.call("profile_create", {"name": "SDK环境", "allowedOrigins": [origin]})
        assert client.call("profile_list")["profiles"][0]["id"] == profile["id"]
        try:
            client.call("browser_start", {"profileId": "0" * 32})
        except SDK.AwbError as error:
            assert error.code == "PROFILE_NOT_FOUND" and not error.outcome_unknown
        else:
            raise AssertionError("Python business error was lost")
        report["checks"].append("python-complete-schema-handshake-real-crud-and-business-errors")
        report["typescript"] = typescript(private / "data/local-api.json")
        report["checks"].append("typescript-strict-compiled-contract-handshake-shared-state-and-business-errors")
        broken = SDK.Client(config["url"], config["token"])
        broken.tools.pop("profile_list")
        try:
            broken.connect()
        except SDK.AwbError as error:
            assert error.code == "CLIENT_CONTRACT_MISMATCH"
        else:
            raise AssertionError("Contract drift was accepted")
        report["checks"].append("full-contract-drift-rejected")
        endpoint_path = private / "data/local-api.json"
        client.call("browser_start", {"profileId": profile["id"], "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile["id"], "owner": "sdk-fixture", "ttlMs": 60000})
        client.call("browser_navigate", {"profileId": profile["id"], "lease": lease, "url": origin})
        client.call("lease_revoke", {"profileId": profile["id"]})
        discovered = json.loads(example("01_discover.py", endpoint_path, ["--profile", profile["id"]]))
        assert discovered["candidates"][0]["selector"] == "#publish"
        flow = definition(origin, "SDK示例", [{"id": "open", "type": "navigate", "url": origin + "/", "next": "done"},
                                              {"id": "done", "type": "end"}])
        flow_path = private / "example-flow.json"
        flow_path.write_bytes(json.dumps(flow).encode('utf-8'))
        assert b"runId=" in example("02_debug_flow.py", endpoint_path, ["--profile", profile["id"], "--flow", str(flow_path)])
        sha = client.call("workflow_validate", {"definition": flow})["sha256"]
        package = {"schemaVersion": "1.0", "packageId": "sdk-example", "name": "SDK package", "version": 1,
            "origin": origin, "routePaths": ["/"], "requiredCapabilities": ["navigate"], "locators": {},
            "workflows": [{"name": "sample", "definition": flow, "sha256": sha}]}
        package_path = private / "package.json"
        package_path.write_bytes(json.dumps(package).encode('utf-8'))
        example("03_package_preview.py", endpoint_path, ["--package", str(package_path), "--import-package"])
        assert len(client.call("site_package_list")["packages"]) == 1
        client.call("browser_stop", {"profileId": profile["id"]})
        report["checks"].append("three-shipped-examples-separate-process-real-discovery-debug-and-package-import-export")
        login_origin = f"http://127.0.0.1:{login_server.server_port}"
        login_profile = client.call("profile_create", {"name": "SDK登录互操作", "allowedOrigins": [login_origin]})["id"]
        secret = "sdk-login-canary-20261105"
        salt = b"sdk-fixture-salt"
        login_server.users["sdk-user"] = (salt, hashlib.pbkdf2_hmac("sha256", secret.encode(), salt, 100_000))
        credential = client.call("credential_create", {"name": "SDK登录", "value": secret, "purpose": "browser",
                                "allowedOrigins": [login_origin], "profileIds": [login_profile]})["credentialId"]
        client.call("browser_start", {"profileId": login_profile, "headless": True})
        scenario = {"endpointFile": str(endpoint_path), "profileId": login_profile, "url": login_origin + "/login",
                    "username": "sdk-user", "credentialId": credential, "usernameSelector": "#username",
                    "passwordSelector": "#password", "submitSelector": "#login", "successSelector": "#result",
                    "successText": "logged-in"}
        scenario_path = private / "login-scenario.json"
        scenario_path.write_bytes(json.dumps(scenario).encode('utf-8'))
        import sys
        python_login = subprocess.run([sys.executable, "-X", "utf8", str(ROOT / "sdk/examples/04_login.py"),
                                      "--scenario", str(scenario_path)], capture_output=True, timeout=45, check=True)
        assert json.loads(python_login.stdout)["businessAssertion"] and login_server.login_count == 1
        # 切换为全新浏览器实例，要求TS靠表单提交取得身份，不能借用Python已登录会话。
        client.call("browser_stop", {"profileId": login_profile})
        ts_profile = client.call("profile_create", {"name": "TS独立登录环境", "allowedOrigins": [login_origin]})["id"]
        ts_credential = client.call("credential_create", {"name": "TS登录", "value": secret, "purpose": "browser",
                                   "allowedOrigins": [login_origin], "profileIds": [ts_profile]})["credentialId"]
        client.call("browser_start", {"profileId": ts_profile, "headless": True})
        scenario.update({"profileId": ts_profile, "credentialId": ts_credential})
        scenario_path.write_bytes(json.dumps(scenario).encode('utf-8'))
        ts_login = subprocess.run(["node", str(ROOT / "sdk/examples/04_login.cjs"),
                                  str(ROOT / "build/sdk-compiled/client.js"), str(scenario_path)],
                                 capture_output=True, timeout=45, check=True)
        assert json.loads(ts_login.stdout)["businessAssertion"] and login_server.login_count == 2
        assert set(login_server.sessions.values()) == {"sdk-user"} and len(login_server.sessions) == 2
        client.call("browser_stop", {"profileId": ts_profile})
        report["checks"].append("python-and-typescript-login-examples-independent-profiles-secretref-server-identities-two-submits")
        report["loginBusinessEvidence"] = {"successfulLogins": login_server.login_count,
                                           "authenticatedSessions": len(login_server.sessions), "distinctProfiles": 2}
        endpoint = {"schemaVersion": 1, "url": f"http://127.0.0.1:{server.server_port}", "token": "private-test-token"}
        endpoint_file = private / "fixture-api.json"
        endpoint_file.write_bytes(json.dumps(endpoint).encode('utf-8'))
        fake = SDK.Client.from_file(endpoint_file)
        for mode, code, ts_code in [("redirect", "CLIENT_REDIRECT_DENIED", "TRANSPORT_OUTCOME_UNKNOWN"),
                                    ("large", "CLIENT_RESPONSE_BUDGET", "CLIENT_RESPONSE_BUDGET")]:
            server.mode, server.calls = mode, 0
            reject(fake, code)
            assert server.calls == 1
            typescript(endpoint_file, ts_code)
            assert server.calls == 2
            report["checks"].append(mode + "-both-clients-refuse-without-retry")
        report["result"] = "passed"
    finally:
        if process.poll() is None:
            SDK.Client(config["url"], config["token"])._request("/v1/shutdown", {})
            process.wait(timeout=10)
        server.shutdown()
        login_server.shutdown()
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode('utf-8'))
    print(json.dumps({"result": report["result"], "checks": len(report["checks"])}))


if __name__ == "__main__":
    main()
