"""新增集成边界验收：官方通知篡改、OIDC绑定、调度序号/配额/租约及真实断线恢复。日期：2026-11-05。"""

import argparse
import hashlib
import http.cookiejar
import json
import secrets
import sqlite3
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import rsa
from fixtures.official_providers import Providers, jwk, sign, wire
from integration_browser import ProcessExit
from integration_commerce import Admin, account, plan, edit_record
from integration_official_oidc_nodes import Node, SsoBrowser, wait_run
from integration_workflow import definition


# 用途：生成与标准JSON对象键排序一致的UTF8协议字节，不共用产品实现；日期：2026-11-05。
def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


class SignedNode:
    # 用途：使用独立Python RSA实现加入真实后台，验证标准节点身份协议；日期：2026-11-05。
    def __init__(self, admin, token, project, contract):
        self.admin = admin
        self.user_token = token
        self.sequence = 0
        self.boot = secrets.token_hex(16)
        self.contract = contract
        self.key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        invitation = admin.call(
            "node.enrollment.create", {"projectIds": [project["id"]], "capacity": 1}, token
        )
        public = jwk(self.key)
        enrollment = invitation["enrollmentToken"]
        name = "independent-protocol-node"
        self.body = {
            "enrollmentToken": enrollment,
            "name": name,
            "publicKey": public,
            "signature": sign(
                self.key, "AWB-NODE-ENROLL\n" + enrollment + "\n" + canonical(public) + "\n" + name
            ),
        }
        reply = admin.call("node.enroll", self.body)
        assert admin.call("node.enroll", self.body) == reply
        admin.call("node.enroll", {**self.body, "name": "conflict"}, error="NODE_SIGNATURE")
        self.id = reply["nodeId"]
        self.credential = reply["credential"]
        admin.call("node.state", {"id": self.id, "state": "active"}, token)
        self.open()

    # 用途：构建签名请求并可显式重发同序号或错误身份；日期：2026-11-05。
    def request(self, action, payload=None, sequence=None, error=None, credential=None):
        if sequence is None:
            self.sequence += 1
            sequence = self.sequence
        payload = {"bootId": self.boot, **(payload or {})}
        timestamp = int(time.time())
        signed = (
            "AWB-NODE/1\n"
            + action
            + "\n"
            + self.id
            + "\n"
            + str(sequence)
            + "\n"
            + str(timestamp)
            + "\n"
            + canonical(payload)
            + "\n"
        )
        body = {
            "nodeId": self.id,
            "sequence": sequence,
            "timestamp": timestamp,
            "payload": payload,
            "signature": sign(self.key, signed),
        }
        return self.admin.call(
            action, body, credential if credential is not None else self.credential, error=error
        )

    # 用途：以固定工具契约和产品版本打开节点会话；日期：2026-11-05。
    def open(self):
        tools_hash = hashlib.sha256(canonical(self.contract["tools"]).encode()).hexdigest()
        return self.request(
            "node.open",
            {
                "protocolVersion": 1,
                "agentVersion": self.contract["productVersion"],
                "contractHash": tools_hash,
                "interactiveSession": True,
                "capabilities": {"browser": True},
            },
        )


# 用途：发送一份已经构造的原始通知并验证预期拒绝原因；日期：2026-11-05。
def post_notification(admin, channel, body, headers, expected):
    request = urllib.request.Request(
        admin.url + "/payments/" + channel + "/notify", data=body.encode(), headers=headers
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            assert expected is None
            return response.status
    except urllib.error.HTTPError as response:
        value = json.load(response)
        assert value["error"]["code"] == expected, value
        return response.status


# 用途：测试三渠道原文篡改、未知密钥、认证加密失败和过期Webhook；日期：2026-11-05。
def payment_edges(admin, provider, operator, buyer):
    product = plan(admin, operator)
    ids = {}
    for channel in ["alipay", "wechat", "stripe"]:
        order = admin.call(
            "order.create",
            {"planId": product["id"], "quantity": 1, "idempotencyKey": channel + "-edge"},
            buyer["token"],
        )
        ids[channel] = order["id"]
        admin.call(
            "payment.channel.create",
            {"channel": channel, "orderId": order["id"], "idempotencyKey": "edge"},
            buyer["token"],
        )
        body, headers = provider.notification(channel, order["id"], event=channel + "-edge")
        # 原文增添字节后官方签名不再成立。
        tampered = body.replace("199.00", "198.00") if channel == "alipay" else body + " "
        post_notification(admin, channel, tampered, headers, "PAYMENT_SIGNATURE")
        if channel == "wechat":
            wrong = {**headers, "Wechatpay-Serial": "unknown-key"}
            post_notification(admin, channel, body, wrong, "PAYMENT_SIGNATURE")
            envelope = json.loads(body)
            envelope["resource"]["ciphertext"] = (
                "A" if envelope["resource"]["ciphertext"][0] != "A" else "B"
            ) + envelope["resource"]["ciphertext"][1:]
            modified = wire(envelope)
            timestamp = headers["Wechatpay-Timestamp"]
            nonce = headers["Wechatpay-Nonce"]
            updated = {
                **headers,
                "Wechatpay-Signature": sign(
                    provider.platform, timestamp + "\n" + nonce + "\n" + modified + "\n"
                ),
            }
            post_notification(admin, channel, modified, updated, "PAYMENT_SIGNATURE")
        if channel == "stripe":
            old = {**headers, "Stripe-Signature": "t=1,v1=" + "0" * 64}
            post_notification(admin, channel, body, old, "PAYMENT_SIGNATURE")
        post_notification(admin, channel, body, headers, None)
    # 官方退款调用之前拒绝已经花费的预付包，不向渠道发出真实退款。
    prepaid = plan(admin, operator, "time-edge", billingMode="prepaid", timeSeconds=600)
    order = admin.call(
        "order.create",
        {"planId": prepaid["id"], "quantity": 1, "idempotencyKey": "prepaid"},
        buyer["token"],
    )
    common = {"channel": "stripe", "orderId": order["id"]}
    admin.call("payment.channel.create", {**common, "idempotencyKey": "create"}, buyer["token"])
    body, headers = provider.notification("stripe", order["id"], event="prepaid-paid")
    post_notification(admin, "stripe", body, headers, None)
    admin.call(
        "refund.request", {"orderId": order["id"], "reason": "prepaid boundary"}, buyer["token"]
    )
    edit_record(admin, "wallet", buyer["tenantId"], {"seconds": 599})
    before = len(provider.calls)
    admin.call(
        "payment.channel.refund",
        {**common, "idempotencyKey": "blocked"},
        operator["token"],
        error="REFUND_TIME_USED",
    )
    assert len(provider.calls) == before
    edit_record(admin, "wallet", buyer["tenantId"], {"seconds": 600})
    provider.refunded = False
    result = admin.call(
        "payment.channel.refund", {**common, "idempotencyKey": "reserve"}, operator["token"]
    )
    assert result["state"] == "pending"
    assert admin.call("snapshot", token=buyer["token"])["entitlement"]["timeBalanceSeconds"] == 0
    provider.refunded = True
    result = admin.call(
        "payment.channel.refund-query", {**common, "idempotencyKey": "settle"}, operator["token"]
    )
    assert result["state"] == "confirmed"
    assert admin.call("snapshot", token=buyer["token"])["entitlement"]["timeBalanceSeconds"] == 0
    return {
        "tamperRejected": 3,
        "wrongWechatSerial": "rejected",
        "badAesTag": "rejected",
        "oldStripeSignature": "rejected",
        "prepaidRefundReservation": "no-double-debit",
    }


# 用途：测试同浏览器绑定、Cookie CSRF、JWKS换钥和停用账号；日期：2026-11-05。
def oidc_edges(admin, provider, operator):
    browser = SsoBrowser(admin)
    start = browser.api("oidc.begin")
    # 另一浏览器窃取state和code仍然没有HttpOnly绑定Cookie。
    callback = (
        admin.url
        + "/auth/callback?"
        + urllib.parse.urlencode({"state": start["state"], "code": "stolen"})
    )
    try:
        urllib.request.urlopen(callback)
        raise AssertionError("Cross-browser callback accepted")
    except urllib.error.HTTPError as response:
        assert json.load(response)["error"]["code"] == "INVALID_ARGUMENTS"
    with browser.opener.open(start["authorizationUrl"]) as response:
        response.read()
    identity = browser.api("identity")
    # Cookie认证POST没有Origin必须拒绝。
    request = urllib.request.Request(
        admin.url + "/api/identity", data=b"{}", headers={"Content-Type": "application/json"}
    )
    try:
        browser.opener.open(request)
        raise AssertionError("Cookie CSRF accepted")
    except urllib.error.HTTPError as response:
        assert json.load(response)["error"]["code"] == "FORBIDDEN"
    provider.identity = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    SsoBrowser(admin).login()
    admin.call("user.state", {"id": identity["userId"], "state": "suspended"}, operator["token"])
    SsoBrowser(admin).login(expect="FORBIDDEN")
    admin.call("user.state", {"id": identity["userId"], "state": "active"}, operator["token"])
    logged = browser.api("oidc.end-session")
    assert logged["logoutUrl"].startswith(provider.url + "/oidc/logout?")
    browser.api("identity", error="UNAUTHORIZED")
    return {
        "browserBinding": "enforced",
        "cookieCsrf": "rejected",
        "jwksRotation": "passed",
        "disabledIdentity": "rejected",
        "rpLogout": "local-session-revoked",
    }


# 用途：用独立节点协议测试序号、Outbox重投、固定hash、容量与租约核对；日期：2026-11-05。
def scheduler_edges(admin, provider, operator, buyer, contract):
    token = buyer["token"]
    project = admin.call(
        "project.save", {"name": "协议边界", "allowedOrigins": [provider.url]}, token
    )
    node = SignedNode(admin, token, project, contract)
    first = node.request("node.poll")
    seq = node.sequence
    assert node.request("node.poll", sequence=seq) == first
    node.request("node.poll", {"extra": True}, sequence=seq, error="NODE_REPLAY")
    node.request("node.poll", sequence=seq - 1, error="NODE_REPLAY")
    node.request("node.poll", credential=token, error="NODE_SIGNATURE")
    admin.call("identity", token=node.credential, error="UNAUTHORIZED")
    flow = definition(
        provider.url,
        "protocol-fixed",
        [
            {"id": "open", "type": "navigate", "url": provider.url + "/lab", "next": "done"},
            {"id": "done", "type": "end"},
        ],
    )
    version = admin.call(
        "workflow.remote.publish", {"projectId": project["id"], "definition": flow}, token
    )
    assignment = admin.call(
        "assignment.create",
        {
            "projectId": project["id"],
            "nodeId": node.id,
            "configuration": {"name": "scope", "allowedOrigins": [provider.url]},
        },
        token,
    )
    body = {
        "assignmentId": assignment["id"],
        "versionId": version["id"],
        "input": {},
        "timeoutSeconds": 120,
        "idempotencyKey": "protocol-1",
    }
    run = admin.call("run.submit", body, token)
    admin.call("run.submit", {**body, "timeoutSeconds": 121}, token, error="IDEMPOTENCY_CONFLICT")
    commands = node.request("node.poll")["commands"]
    assert len(commands) == 1
    command = commands[0]
    repeated = node.request("node.poll")["commands"]
    assert repeated == commands
    admin.call("assignment.release", {"id": assignment["id"]}, token, error="ASSIGNMENT_BUSY")
    ack = {"runId": run["id"], "commandId": command["id"], "payloadHash": command["payloadHash"]}
    node.request("node.ack", {**ack, "payloadHash": "wrong"}, error="NODE_COMMAND_CONFLICT")
    node.request("node.ack", ack)
    assert not node.request("node.poll")["commands"]
    run2 = admin.call("run.submit", {**body, "idempotencyKey": "protocol-2"}, token)
    assert not node.request("node.poll")["commands"]
    node.request(
        "node.complete",
        {
            "runId": run["id"],
            "commandId": command["id"],
            "stoppedConfirmed": False,
            "result": {"state": "succeeded"},
        },
        error="CONFIRMATION_REQUIRED",
    )
    # 时间夹具只前移已绑定租约，不更改经济事实。
    edit_record(admin, "remote_run", run["id"], {"leaseUntil": int(time.time()) - 1})
    assert (
        next(
            r for r in admin.call("scheduler.snapshot", token=token)["runs"] if r["id"] == run["id"]
        )["state"]
        == "unknown"
    )
    admin.call(
        "run.resolve",
        {"id": run["id"], "confirmRunId": run["id"], "reason": "not stopped"},
        token,
        error="RECONCILIATION_REQUIRED",
    )
    assert not node.request("node.poll")["commands"]
    completion = {
        "runId": run["id"],
        "commandId": command["id"],
        "stoppedConfirmed": True,
        "result": {"state": "unknown"},
    }
    node.request("node.complete", completion)
    assert node.request("node.complete", completion)["duplicate"]
    node.request(
        "node.complete",
        {**completion, "result": {"state": "succeeded"}},
        error="NODE_COMMAND_CONFLICT",
    )
    admin.call(
        "run.resolve",
        {"id": run["id"], "confirmRunId": run["id"], "reason": "已核实目标站没有执行"},
        token,
    )
    new = node.request("node.poll")["commands"]
    assert len(new) == 1 and new[0]["payload"]["runId"] == run2["id"]
    # 项目来源缩小后，旧分配不能继续获得租约。
    admin.call(
        "project.save",
        {"id": project["id"], "name": "scope changed", "allowedOrigins": ["https://example.test"]},
        token,
    )
    revoked = node.request("node.poll")
    assert revoked["stops"] and not revoked["grants"]
    return {
        "sequenceReplay": "cached",
        "conflictingReplay": "rejected",
        "userAsNode": "rejected",
        "outboxRetry": "same-command",
        "localCapacity": "one-per-node",
        "unknownSlotHeld": True,
        "stopBeforeResolve": "required",
        "completionDedup": "passed",
        "scopeRemoval": "fenced",
    }


# 用途：真实节点执行中强杀协调器，验证进程停止、epoch提高和无业务重放；日期：2026-11-05。
def outage_test(admin, provider, buyer, node_exe, browser, output):
    token = buyer["token"]
    project = admin.call("project.save", {"name": "断线", "allowedOrigins": [provider.url]}, token)
    invitation = admin.call(
        "node.enrollment.create", {"projectIds": [project["id"]], "capacity": 1}, token
    )
    node = Node(
        node_exe,
        output / "outage-node",
        browser,
        admin,
        invitation["enrollmentToken"],
        "outage-real-node",
    )
    try:
        admin.call("node.state", {"id": node.id, "state": "active"}, token)
        assignment = admin.call(
            "assignment.create",
            {
                "projectId": project["id"],
                "nodeId": node.id,
                "configuration": {"name": "outage", "allowedOrigins": [provider.url]},
            },
            token,
        )
        flow = definition(
            provider.url,
            "outage-wait",
            [
                {"id": "open", "type": "navigate", "url": provider.url + "/lab", "next": "wait"},
                {
                    "id": "wait",
                    "type": "assertText",
                    "selector": "#result",
                    "expected": "never",
                    "timeoutMs": 10000,
                    "next": "done",
                },
                {"id": "done", "type": "end"},
            ],
        )
        version = admin.call(
            "workflow.remote.publish", {"projectId": project["id"], "definition": flow}, token
        )
        run = admin.call(
            "run.submit",
            {
                "assignmentId": assignment["id"],
                "versionId": version["id"],
                "input": {},
                "timeoutSeconds": 120,
                "idempotencyKey": "outage",
            },
            token,
        )
        wait_run(admin, token, run["id"], lambda r: r["state"] == "running")
        ownership = list((node.root / "private" / "assignments").rglob("awb-process-owner.json"))
        assert len(ownership) == 1
        record = json.loads(ownership[0].read_text("utf-8"))
        process = ProcessExit(record["pid"])
        old_epoch = admin.call("scheduler.snapshot", token=token)["epoch"]
        port = int(admin.url.rsplit(":", 1)[1])
        count = provider.clicks
        admin.process.kill()
        admin.process.wait(timeout=10)
        start = time.monotonic()
        process.wait()
        stopped = time.monotonic() - start
        restarted = Admin(node_exe.parent / "workbench-admin.exe", admin.root, port)
        admin.process = restarted.process
        admin.shutdown = restarted.shutdown
        admin.url = restarted.url
        recovered = wait_run(admin, token, run["id"], timeout=30)
        assert recovered["state"] == "unknown" and recovered["stoppedConfirmed"], recovered
        assert admin.call("scheduler.snapshot", token=token)["epoch"] > old_epoch
        assert provider.clicks == count
        # 正常停止入口有界退出，重启原身份仍可核对。
        subprocess.run(
            [str(node_exe), "--stop", "--config", str(node.config)],
            check=True,
            timeout=15,
            capture_output=True,
        )
        node.process.wait(timeout=15)
        assert node.process.returncode == 0
        return {
            "stopAfterSeconds": round(stopped, 3),
            "coordinatorRestart": "epoch-increased",
            "oldTask": "unknown-stopped-no-replay",
            "gracefulStop": "passed",
        }
    finally:
        node.kill()


# 用途：核验公网Host白名单、节点mTLS代理证明及正式Secure Cookie，测试不假称真实TLS握手；日期：2026-11-05。
def transport_edges(admin, buyer):
    port = int(admin.url.rsplit(":", 1)[1])
    admin.close()
    secret = secrets.token_hex(32)
    config = {
        "publicOrigin": "https://portal.example.test",
        "requireNodeTransportProof": True,
        "nodeTransportSecret": secret,
    }
    (admin.root / "server-config.json").write_text(json.dumps(config), encoding="utf-8")
    identity_path = admin.root / "oidc-config.json"
    identity = json.loads(identity_path.read_text("utf-8"))
    identity["redirectUri"] = "https://portal.example.test/auth/callback"
    identity_path.write_text(json.dumps(identity), encoding="utf-8")
    restarted = Admin(Path(admin.process.args[0]), admin.root, port)
    admin.process, admin.shutdown = restarted.process, restarted.shutdown
    for action in [
        "node.enroll",
        "node.open",
        "node.poll",
        "node.ack",
        "node.complete",
        "node.event",
    ]:
        admin.call(action, {}, error="NODE_TRANSPORT")
        admin.call(action, {}, headers={"Host": "portal.example.test"}, error="NODE_TRANSPORT")
    admin.call(
        "node.poll",
        {},
        headers={"Host": "portal.example.test", "X-AWB-Node-Transport": secret},
        error="INVALID_ARGUMENTS",
    )
    public = {"Host": "portal.example.test", "Origin": "https://portal.example.test"}
    project = admin.call(
        "project.save",
        {"name": "正式门户管理测试", "allowedOrigins": ["https://web.example.test"]},
        buyer["token"],
        headers=public,
    )
    invitation = admin.call(
        "node.enrollment.create",
        {"projectIds": [project["id"]], "capacity": 1},
        buyer["token"],
        headers=public,
    )
    assert invitation["enrollmentToken"]
    snapshot = admin.call("scheduler.snapshot", token=buyer["token"], headers=public)
    node = next(row for row in snapshot["nodes"] if row["state"] == "active")
    admin.call(
        "node.state", {"id": node["id"], "state": "maintenance"}, buyer["token"], headers=public
    )
    admin.call("node.state", {"id": node["id"], "state": "active"}, buyer["token"], headers=public)
    admin.call("status", {}, headers={"Host": "attacker.example.test"}, error="FORBIDDEN")
    request = urllib.request.Request(
        admin.url + "/api/oidc.begin",
        data=b"{}",
        headers={
            "Host": "portal.example.test",
            "Origin": "https://portal.example.test",
            "Content-Type": "application/json",
        },
    )
    with urllib.request.urlopen(request) as response:
        cookie = response.headers["Set-Cookie"]
        assert "; Secure" in cookie and "HttpOnly" in cookie and "SameSite=Lax" in cookie
        result = json.load(response)["result"]
        assert "browserBinding" not in result
    return {
        "publicHostAllowlist": True,
        "nodeProxyProofRequired": True,
        "portalNodeManagement": "passed-without-node-certificate",
        "secureHttpOnlyCookie": True,
        "realTlsHandshakeTested": False,
    }


# 用途：运行边界组并输出脱敏证据，不把临时私钥带进交付包；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    provider = Providers()
    admin = None
    try:
        root = args.output / "admin"
        admin = Admin(args.bin / "workbench-admin.exe", root)
        origin = admin.url
        port = int(origin.rsplit(":", 1)[1])
        admin.close()
        provider.configs(root, origin)
        admin = Admin(args.bin / "workbench-admin.exe", root, port)
        operator, _ = account(admin, "edge-operator", True)
        buyer, _ = account(admin, "edge-buyer")
        contract = json.loads(
            subprocess.check_output([str(args.bin / "workbench-cli.exe"), "--print-contract"])
        )
        report = {
            "date": "2026-11-05",
            "payments": payment_edges(admin, provider, operator, buyer),
            "oidc": oidc_edges(admin, provider, operator),
            "scheduler": scheduler_edges(admin, provider, operator, buyer, contract),
            "outage": outage_test(
                admin, provider, buyer, args.bin / "workbench-node.exe", args.browser, args.output
            ),
            "result": "passed",
        }
        report["transport"] = transport_edges(admin, buyer)
        report["httpChecks"] = len(admin.events)
        (args.output / "edge-report.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(json.dumps(report, ensure_ascii=False))
    finally:
        if admin and admin.process.poll() is None:
            admin.close()
        provider.close()


if __name__ == "__main__":
    main()
