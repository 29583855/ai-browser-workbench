"""验证真实鉴权回环HTTP API、浏览器动作、协议拒绝和退出清理。日期：2026-11-04。"""

import argparse
import ctypes
import hashlib
import http.client
import json
import queue
import socket
import subprocess
import threading
import time
from datetime import datetime
from pathlib import Path
from urllib.parse import urlsplit
from integration_browser import FixtureServer, ProcessExit
from integration_workflow import definition
from integration_extraction import extraction_spec, batch_spec


# 用途：占用本测试审计文件，验证接受前失败阻断和执行后结果未知；日期：2026-11-04。
def audit_lock(path):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                                  ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        handle = kernel.CreateFileW(str(path), 0x80000000, 0, None, 3, 0x80, None)
        if handle != ctypes.c_void_p(-1).value:
            return kernel, handle
        time.sleep(0.01)
    raise AssertionError("Cannot obtain test audit lock")


# 用途：启动本测试API服务，有界等待首条不含令牌的准备通知；日期：2026-11-04。
def start(executable, browser, root):
    process = subprocess.Popen([str(executable), "--root", str(root), "--browser", str(browser), "--http", "0"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    ready = queue.Queue()
    # 用途：在线程等待一条启动通知，防止启动失败导致主测试无限等待；日期：2026-11-04。
    def read():
        ready.put(process.stdout.readline())
    threading.Thread(target=read, daemon=True).start()
    try:
        notice = ready.get(timeout=15)
        assert json.loads(notice)["ready"] is True
        config = json.loads((root / "local-api.json").read_text("utf-8"))
        assert config["token"] not in notice and config["pid"] == process.pid
        return process, config
    except BaseException:
        process.kill()
        process.wait(timeout=5)
        raise


class Api:
    # 用途：使用私有端点配置创建测试客户端，证据不记录令牌；日期：2026-11-04。
    def __init__(self, config, report):
        self.config = config
        self.url = urlsplit(config["url"])
        self.report = report

    # 用途：真实发出HTTP请求，校验状态、请求ID和业务错误；日期：2026-11-04。
    def call(self, method, path, data=None, error=None, status=200, headers=None):
        connection = http.client.HTTPConnection(self.url.hostname, self.url.port, timeout=25)
        values = {"Authorization": "Bearer " + self.config["token"], "Content-Type": "application/json"}
        values.update(headers or {})
        try:
            connection.request(method, path, body=None if data is None else json.dumps(data, ensure_ascii=False).encode("utf-8"), headers=values)
            response = connection.getresponse()
            raw = response.read()
            body = json.loads(raw)
            assert response.status == status, (path, response.status, body)
            assert response.getheader("X-Request-Id") == body["requestId"]
            assert response.getheader("Access-Control-Allow-Origin") is None
            assert self.config["token"].encode() not in raw
            if error:
                assert not body["ok"] and body["error"]["code"] == error, body
            else:
                assert body["ok"], body
            self.report["requests"].append({"method": method, "path": path, "status": response.status,
                                             "requestId": body["requestId"], "outcome": error or "passed"})
            return body.get("result", body)
        finally:
            connection.close()

    # 用途：发送原始边界输入，禁止HTTP库自动修复无效报文；日期：2026-11-04。
    def raw(self, wire, expected):
        with socket.create_connection((self.url.hostname, self.url.port), timeout=10) as stream:
            stream.sendall(wire)
            stream.shutdown(socket.SHUT_WR)
            received = b""
            while True:
                chunk = stream.recv(65536)
                if not chunk:
                    break
                received += chunk
        head, body = received.split(b"\r\n\r\n", 1)
        result = json.loads(body)
        assert result["error"]["code"] == expected, result
        assert self.config["token"].encode() not in body
        self.report["rawRejections"].append({"code": expected, "requestId": result["requestId"]})


# 用途：以真实注册/登录验证工具桥接，并覆盖鉴权、Host、Origin、消息长度及版本冲突；日期：2026-11-04。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / "api-data"
    server = FixtureServer()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    process = None
    report = {"date": datetime.now().astimezone().isoformat(), "result": "failed", "requests": [], "rawRejections": [],
              "binarySha256": hashlib.sha256(args.executable.read_bytes()).hexdigest()}
    try:
        process, config = start(args.executable.resolve(), args.browser.resolve(), root)
        api = Api(config, report)
        api.call("GET", "/v1/health", error="API_UNAUTHORIZED", status=401, headers={"Authorization": "invalid"})
        api.call("GET", "/v1/health", error="API_ORIGIN_DENIED", status=403, headers={"Origin": "http://evil.invalid"})
        api.call("GET", "/v1/health", error="API_ORIGIN_DENIED", status=403, headers={"Sec-Fetch-Site": "same-origin"})
        api.call("GET", "/v1/health", error="API_HOST_DENIED", status=403, headers={"Host": "evil.invalid"})
        assert api.call("GET", "/v1/health")["mcpStdio"]
        assert len(api.call("GET", "/v1/tools")["tools"]) >= 31
        assert api.call("GET", "/v1/profiles")["profiles"] == []
        api.call("DELETE", "/v1/profiles", error="API_METHOD_NOT_ALLOWED", status=405)
        api.call("POST", "/v1/profiles", {}, error="API_CONTENT_TYPE", status=415, headers={"Content-Type": "text/plain"})
        api.call("POST", "/v1/profiles", [], error="INVALID_ARGUMENTS", status=400)
        api.call("GET", "/unregistered", error="API_NOT_FOUND", status=404)
        origin = f"http://127.0.0.1:{server.server_port}"
        p = api.call("POST", "/v1/profiles", {"name": "HTTP注册登录", "allowedOrigins": [origin]})
        route = "/v1/profiles/" + p["id"]
        api.call("PATCH", route, {"name": "拒绝无版本"}, error="API_PRECONDITION_REQUIRED", status=428)
        p = api.call("PATCH", route, {"name": "环境已更新"}, headers={"If-Match": '"1"'})
        api.call("PATCH", route, {"name": "旧版本"}, error="REVISION_CONFLICT", status=409, headers={"If-Match": '"1"'})
        browser = api.call("POST", "/v1/tools/browser_start", {"profileId": p["id"], "headless": True})
        lease = api.call("POST", "/v1/tools/lease_acquire", {"profileId": p["id"], "owner": "api-test", "ttlMs": 300000})
        base = {"profileId": p["id"], "lease": lease}
        pages = api.call("POST", "/v1/tools/page_list", base)
        assert len(pages["pages"]) == 1
        extra = api.call("POST", "/v1/tools/page_open", {**base, "url": "about:blank"})
        api.call("POST", "/v1/tools/page_switch", {**base, "pageId": pages["activePageId"]})
        api.call("POST", "/v1/tools/page_close", {**base, "pageId": extra["pageId"]})
        assert api.call("POST", "/v1/tools/browser_events", base)["bufferLimit"] == 256
        api.call("POST", "/v1/tools/browser_navigate", {**base, "url": origin + "/login"})
        inspected = api.call("POST", "/v1/tools/page_inspect", {**base, "selector": "#username", "highlight": True})
        preview = api.call("POST", "/v1/tools/page_preview", {**base, "ref": inspected["ref"], "action": "fill"})
        assert preview["readyNow"] and not preview["executed"]
        assert api.call("POST", "/v1/tools/page_clear_highlight", base)["cleared"]
        for selector, value in [("#username", "api-user-test"), ("#password", "secret-canary-api-123")]:
            reference = api.call("POST", "/v1/tools/page_locate", {**base, "selector": selector})["ref"]
            api.call("POST", "/v1/tools/page_fill", {**base, "ref": reference, "value": value})
        for selector, expected in [("#register", "registered"), ("#login", "logged-in")]:
            reference = api.call("POST", "/v1/tools/page_locate", {**base, "selector": selector})["ref"]
            api.call("POST", "/v1/tools/page_click", {**base, "ref": reference})
            api.call("POST", "/v1/tools/page_wait_text", {**base, "selector": "#result", "expected": expected, "timeoutMs": 5000})
        assert server.registration_count == 1 and server.login_count == 1
        api.call("POST", "/v1/tools/browser_navigate", {**base, "url": origin + "/extraction"})
        assert api.call("POST", "/v1/tools/page_extract", {**base, **extraction_spec()})["valid"]
        assert api.call("POST", "/v1/tools/page_batch", {**base, **batch_spec()})["state"] == "succeeded"
        partial = api.call("POST", "/v1/tools/page_batch", {**base, "actions": [
            {"type": "click", "selector": "#increment", "postcondition": {
                "selector": "#clicks", "expected": "99", "timeoutMs": 100}},
            {"type": "fill", "selector": "#field", "value": "must-not-be-written"}], "onError": "continue"})
        assert partial["state"] == "outcome-unknown" and partial["notRun"] == 1
        report["batchUnknownRequestId"] = report["requests"][-1]["requestId"]
        report["batchUnknownOutcome"] = {"followingWriteNotDispatched": True, "automaticRetryAllowed": False}
        api.call("POST", "/v1/tools/browser_navigate", {**base, "url": origin + "/inspect"})
        select = api.call("POST", "/v1/tools/page_inspect", {**base, "selector": "#single"})
        assert api.call("POST", "/v1/tools/page_select", {**base, "ref": select["ref"], "values": ["cn"]})["selectedCount"] == 1
        api.call("POST", "/v1/tools/page_wait_text", {**base, "selector": "#single-value", "expected": "cn", "timeoutMs": 5000})
        flow = definition(origin, "http-workflow")
        assert api.call("POST", "/v1/tools/workflow_validate", {"definition": flow, "input": {}})["valid"]
        diagnostic = api.call("POST", "/v1/tools/browser_diagnostics_start", {**base, "routePaths": ["/workflow"]})
        published = api.call("POST", "/v1/tools/workflow_publish", {"definition": flow})
        running = api.call("POST", "/v1/tools/workflow_run", {**base, "workflowId": published["workflowId"], "input": {}})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            observed = api.call("POST", "/v1/tools/workflow_status", {"runId": running["runId"]})
            if observed["state"] in ("succeeded", "failed", "outcome-unknown"):
                break
            time.sleep(0.05)
        assert observed["state"] == "succeeded" and len(observed["steps"]) == 3
        diagnostic_events = api.call("POST", "/v1/tools/browser_diagnostics_read", {**base, "epoch": diagnostic["epoch"], "limit": 100})
        assert diagnostic_events["events"] and not diagnostic_events["payloadCapture"]
        assert any(event.get("context", {}).get("runId") == running["runId"] for event in diagnostic_events["events"])
        assert not api.call("POST", "/v1/tools/browser_diagnostics_stop", base)["active"]
        assert any(item["runId"] == running["runId"] for item in api.call("POST", "/v1/tools/workflow_runs", {})["runs"])
        audit_path = root / "local-api-audit.jsonl"
        kernel, lock = audit_lock(audit_path)
        try:
            blocked = api.call("POST", "/v1/profiles", {"name": "must-not-exist", "allowedOrigins": [origin]},
                               error="AUDIT_WRITE_FAILED", status=422)
            assert blocked["error"]["outcomeUnknown"] is False and blocked["auditRecorded"] is False
        finally:
            kernel.CloseHandle(lock)
        assert len(api.call("GET", "/v1/profiles")["profiles"]) == 1
        api.call("POST", "/v1/tools/browser_navigate", {**base, "url": origin + "/audit-fault"})
        reference = api.call("POST", "/v1/tools/page_locate", {**base, "selector": "#commit"})["ref"]
        before = audit_path.stat().st_size
        release = threading.Event()
        locked = threading.Event()
        lock_errors = []
        # 用途：等待本次请求接受记录刷盘后独占审计文件，动作结束才释放；日期：2026-11-04。
        def lock_after_accept():
            try:
                deadline = time.monotonic() + 5
                while audit_path.stat().st_size <= before:
                    assert time.monotonic() < deadline, "Missing accepted record"
                    time.sleep(0.005)
                owner, handle = audit_lock(audit_path)
                try:
                    locked.set()
                    assert release.wait(15), "Audit lock release timed out"
                finally:
                    owner.CloseHandle(handle)
            except BaseException as error:
                lock_errors.append(str(error))
        locker = threading.Thread(target=lock_after_accept, daemon=True)
        locker.start()
        try:
            unknown = api.call("POST", "/v1/tools/page_click", {**base, "ref": reference},
                               error="AUDIT_WRITE_FAILED", status=422)
            assert locked.is_set() and unknown["error"]["outcomeUnknown"] is True
            assert unknown["auditRecorded"] is False and server.fault_commit_count == 1
        finally:
            release.set()
            locker.join(timeout=5)
        assert not locker.is_alive() and not lock_errors, lock_errors
        api.call("POST", "/v1/tools/page_wait_text", {**base, "selector": "#result", "expected": "committed-once", "timeoutMs": 5000})
        report["auditFailureBoundaries"] = {"acceptedFailureBlocksCreate": True,
                                            "completedFailureOutcomeUnknown": True,
                                            "actualCommitCount": server.fault_commit_count}
        api.call("POST", "/v1/tools/browser_stop", {"profileId": p["id"]})
        p = api.call("POST", route + "/archive", {}, headers={"If-Match": '"2"'})
        assert len(api.call("GET", "/v1/profiles?state=archived")["profiles"]) == 1
        p = api.call("POST", route + "/restore", {}, headers={"If-Match": '"3"'})
        host = f"127.0.0.1:{api.url.port}"
        head = f"POST /v1/profiles HTTP/1.1\r\nHost: {host}\r\nAuthorization: Bearer {config['token']}\r\nContent-Type: application/json\r\n"
        for suffix, code in [("Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", "INVALID_REQUEST"),
                             ("Transfer-Encoding: chunked\r\n\r\n", "INVALID_REQUEST"),
                             ("Content-Length: 1048577\r\n\r\n", "INPUT_TOO_LARGE"),
                             ("Content-Length: 1\r\n\r\n{", "INVALID_JSON"),
                             ('Content-Length: 13\r\n\r\n{"a":1,"a":2}', "INVALID_JSON"),
                             ("Content-Length: 2\r\n\r\n{}GET /v1/health HTTP/1.1\r\n\r\n", "INVALID_REQUEST")]:
            api.raw((head + suffix).encode(), code)
        other = output / "second-root"
        conflict = subprocess.run([str(args.executable.resolve()), "--root", str(other), "--browser", str(args.browser.resolve()),
                                   "--http", str(api.url.port)], capture_output=True, text=True, encoding="utf-8", timeout=15)
        assert conflict.returncode == 1 and "API_BIND_FAILED" in conflict.stderr
        api.call("POST", "/v1/shutdown", {})
        process.wait(timeout=15)
        assert process.returncode == 0 and not (root / "local-api.json").exists()
        log = (root / "local-api-audit.jsonl").read_text("utf-8")
        assert any(json.loads(line).get("requestId") == report["batchUnknownRequestId"] and
                   json.loads(line).get("code") == "outcome-unknown" for line in log.splitlines())
        assert config["token"] not in log and "secret-canary" not in log and "api-user-test" not in log
        assert all(json.loads(line)["requestId"] for line in log.splitlines())
        previous = config["token"]
        process, config = start(args.executable.resolve(), args.browser.resolve(), root)
        assert config["token"] != previous
        api = Api(config, report)
        api.call("GET", "/v1/profiles", error="API_UNAUTHORIZED", status=401, headers={"Authorization": "Bearer " + previous})
        status = api.call("POST", "/v1/tools/browser_start", {"profileId": p["id"], "headless": True})
        exited = ProcessExit(status["pid"])
        api.call("POST", "/v1/shutdown", {})
        process.wait(timeout=15)
        exited.wait()
        assert process.returncode == 0
        report.update(result="passed", registrationCount=server.registration_count, loginCount=server.login_count,
                      tokenRotated=True, portOwnershipExclusive=True, auditHasNoSecrets=True, shutdownJobCleaned=True)
    finally:
        if process and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        server.shutdown()
        server.server_close()
        (output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(json.dumps({"result": report["result"], "requests": len(report["requests"]), "report": str(output / "report.json")}))


if __name__ == "__main__":
    main()
