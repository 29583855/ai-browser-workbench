"""官方支付、OIDC及真实多节点浏览器协议集成验收。日期：2026-11-05。"""

import argparse
import base64
import concurrent.futures
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
from fixtures.official_providers import Providers, jwk, sign
from integration_browser import Client
from integration_commerce import Admin, account, plan
from integration_workflow import definition


# 用途：发送原文官方通知并检查HTTP状态与协议响应；日期：2026-11-05。
def notify(admin, provider, channel, order, error=False, **kw):
    body, headers = provider.notification(channel, order, **kw)
    request = urllib.request.Request(
        admin.url + "/payments/" + channel + "/notify", data=body.encode(), headers=headers
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            status = response.status
            content = response.read().decode()
    except urllib.error.HTTPError as response:
        status = response.code
        content = response.read().decode()
    assert (status >= 400) if error else (status in (200, 204)), (channel, status, content)
    if channel == "alipay" and not error:
        assert content == "success"
    return {"channel": channel, "status": status, "rejected": error}


# 用途：创建订单并调用四个官方渠道业务，独立验证重复回调不重复入账；日期：2026-11-05。
def payments_test(admin, provider, operator, buyer):
    product = plan(admin, operator)
    evidence = []
    for channel in ["alipay", "wechat", "stripe"]:
        provider.paid = False
        provider.refunded = False
        order = admin.call(
            "order.create",
            {"planId": product["id"], "quantity": 1, "idempotencyKey": secrets.token_hex(16)},
            buyer["token"],
        )
        common = {"channel": channel, "orderId": order["id"]}
        result = admin.call("payment.channel.create", {**common, "idempotencyKey": "create"}, buyer["token"])
        assert result["state"] == "pending", result
        assert result.get("checkoutCode") or result.get("checkoutUrl"), result
        # 一份持久意图重复调用不再请求官方渠道。
        before = len(provider.calls)
        assert (
            admin.call("payment.channel.create", {**common, "idempotencyKey": "create"}, buyer["token"])
            == result
        )
        assert len(provider.calls) == before
        admin.call(
            "payment.channel.create",
            {
                **common,
                "channel": "wechat" if channel != "wechat" else "stripe",
                "idempotencyKey": "cross-channel",
            },
            buyer["token"],
            error="PAYMENT_CONFLICT",
        )
        pending = admin.call("payment.channel.query", {**common, "idempotencyKey": "pending"}, buyer["token"])
        assert pending["state"] == "pending", pending
        notify(admin, provider, channel, order["id"], error=True, event="bad-amount-" + channel, amount=1)
        provider.paid = True
        paid = admin.call("payment.channel.query", {**common, "idempotencyKey": "paid"}, buyer["token"])
        assert paid["state"] == "confirmed", paid
        evidence.append(notify(admin, provider, channel, order["id"], event="paid-" + channel))
        evidence.append(notify(admin, provider, channel, order["id"], event="paid-" + channel))
        snapshot = admin.call("snapshot", token=buyer["token"])
        assert sum(row["orderId"] == order["id"] for row in snapshot["ledger"]) == 1
        admin.call("refund.request", {"orderId": order["id"], "reason": "协议退款验收"}, buyer["token"])
        admin.call(
            "payment.channel.refund-query",
            {**common, "idempotencyKey": "no-intent"},
            buyer["token"],
            error="ORDER_STATE",
        )
        admin.call(
            "payment.channel.refund",
            {**common, "idempotencyKey": "forbidden"},
            buyer["token"],
            error="FORBIDDEN",
        )
        provider.refunded = True
        refunded = admin.call(
            "payment.channel.refund", {**common, "idempotencyKey": "refund"}, operator["token"]
        )
        assert refunded["state"] == "confirmed", refunded
        queried = admin.call(
            "payment.channel.refund-query", {**common, "idempotencyKey": "refund-query"}, operator["token"]
        )
        assert queried["state"] == "confirmed", queried
        if channel != "alipay":
            evidence.append(
                notify(admin, provider, channel, order["id"], event="refund-" + channel, refund=True)
            )
        snapshot = admin.call("snapshot", token=buyer["token"])
        assert sum(row["orderId"] == order["id"] for row in snapshot["ledger"]) == 2
    # Stripe创建已在服务端发生但响应丢失，查询只用相同官方幂等键恢复。
    provider.paid = False
    provider.refunded = False
    provider.drop_next = True
    order = admin.call(
        "order.create",
        {"planId": product["id"], "quantity": 1, "idempotencyKey": "stripe-drop"},
        buyer["token"],
    )
    common = {"channel": "stripe", "orderId": order["id"]}
    unknown = admin.call("payment.channel.create", {**common, "idempotencyKey": "drop"}, buyer["token"])
    assert unknown["state"] == "unknown", unknown
    admin.call(
        "payment.channel.create",
        {**common, "idempotencyKey": "drop"},
        buyer["token"],
        error="PAYMENT_OPERATION_UNKNOWN",
    )
    recovered = admin.call("payment.channel.query", {**common, "idempotencyKey": "recover"}, buyer["token"])
    assert recovered["state"] == "pending", recovered
    # 损坏官方响应签名不得产生付款事实。
    provider.bad_signature = True
    result = admin.call(
        "payment.channel.query",
        {"channel": "alipay", "orderId": next(iter(provider.orders)), "idempotencyKey": "bad-signature"},
        buyer["token"],
    )
    assert result["state"] == "unknown" and result["error"] == "PAYMENT_SIGNATURE", result
    provider.bad_signature = False
    assert admin.call("platform.snapshot", token=operator["token"])["reconciliation"]["balanced"]
    assert not [x for x in provider.calls if x.get("fixtureError")], provider.calls
    return {
        "channels": 3,
        "notificationChecks": evidence,
        "lostStripeResponse": "recovered-with-same-key",
        "balancedLedger": True,
    }


class SsoBrowser:
    # 用途：创建真实CookieJar并保存HttpOnly浏览器绑定；日期：2026-11-05。
    def __init__(self, admin):
        self.admin = admin
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.cookies))

    # 用途：发起同源OIDC和本地账号请求，Cookie请求携带精确Origin；日期：2026-11-05。
    def api(self, action, body=None, token="", error=None):
        request = urllib.request.Request(
            self.admin.url + "/api/" + action,
            data=json.dumps(body or {}).encode(),
            headers={
                "Content-Type": "application/json",
                "Origin": self.admin.url,
                "Authorization": "Bearer " + token,
            },
        )
        try:
            with self.opener.open(request, timeout=15) as response:
                value = json.load(response)
        except urllib.error.HTTPError as response:
            value = json.load(response)
        if error:
            assert value["error"]["code"] == error, value
        else:
            assert value["ok"], value
        return value.get("result", value)

    # 用途：执行授权和回调，验证Cookie凭据不出现在URL或页面正文；日期：2026-11-05。
    def login(self, expect=None, link=False, token=""):
        start = self.api("oidc.begin", {"link": link}, token)
        assert "browserBinding" not in start
        assert any(
            c.name == "awb-flow-" + start["state"] and c.has_nonstandard_attr("HttpOnly")
            for c in self.cookies
        )
        try:
            with self.opener.open(start["authorizationUrl"], timeout=15) as response:
                assert response.geturl() == self.admin.url + "/?sso=success"
                response.read()
                assert expect is None, expect
        except urllib.error.HTTPError as response:
            value = json.load(response)
            assert expect and value["error"]["code"] == expect, value
        return start


# 用途：验证PKCE、主体绑定、错误声明、桌面码单次和本地注销；日期：2026-11-05。
def oidc_test(admin, provider, operator, buyer, credentials, cli, browser, output):
    session = SsoBrowser(admin)
    start = session.login()
    identity = session.api("identity")
    assert identity["email"] == provider.oidc_email and not identity["platform"]
    assert all(c.has_nonstandard_attr("HttpOnly") for c in session.cookies)
    code = session.api("client.code.create")["code"]
    client = Client(cli, output / "sso-desktop", browser)
    try:
        connected = client.call("account_connect_code", {"url": admin.url, "code": code})
        assert connected["connected"]
        admin.call("client.code.exchange", {"code": code}, error="UNAUTHORIZED")
        client.call("account_disconnect")
    finally:
        client.close()
    old_state = start["state"]
    request = urllib.request.Request(
        admin.url + "/auth/callback?" + urllib.parse.urlencode({"state": old_state, "code": "replay"})
    )
    try:
        session.opener.open(request)
        raise AssertionError("State replay accepted")
    except urllib.error.HTTPError as response:
        assert json.load(response)["error"]["code"] == "OIDC_STATE"
    bad_claims = [
        ({"nonce": "wrong"}, "OIDC_CLAIMS"),
        ({"aud": "other-client"}, "OIDC_CLAIMS"),
        ({"iss": provider.url + "/other"}, "OIDC_CLAIMS"),
        ({"exp": 1}, "OIDC_CLAIMS"),
        ({"azp": "attacker"}, "OIDC_CLAIMS"),
    ]
    for claim, error in bad_claims:
        provider.claims = claim
        SsoBrowser(admin).login(expect=error)
    provider.claims = {}
    provider.jwt_header = {"alg": "none"}
    SsoBrowser(admin).login(expect="OIDC_ALGORITHM")
    provider.jwt_header = {"kid": "unknown"}
    SsoBrowser(admin).login(expect="OIDC_SIGNATURE")
    provider.jwt_header = {}
    provider.bad_signature = True
    SsoBrowser(admin).login(expect="OIDC_SIGNATURE")
    provider.bad_signature = False
    provider.claims = {"sub": "existing-email-sub"}
    provider.oidc_email = credentials["email"]
    SsoBrowser(admin).login(expect="OIDC_ACCOUNT_LINK_REQUIRED")
    link = SsoBrowser(admin)
    link.login(link=True, token=buyer["token"])
    assert link.api("identity")["email"] == credentials["email"]
    assert len(admin.call("oidc.bindings", token=buyer["token"])) == 1
    provider.claims = {"sub": "unverified-sub", "email_verified": False}
    provider.oidc_email = "unverified@example.test"
    SsoBrowser(admin).login(expect="OIDC_EMAIL")
    provider.claims = {}
    provider.oidc_email = "sso-new@example.test"
    session.api("oidc.logout")
    session.api("identity", error="UNAUTHORIZED")
    # 真实Chromium从门户按钮完成跨站授权、回调和会话恢复。
    client = Client(cli, output / "sso-portal", browser)
    try:
        profile = client.call(
            "profile_create", {"name": "SSO门户", "allowedOrigins": [admin.url, provider.url]}
        )["id"]
        client.call("browser_start", {"profileId": profile, "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile, "owner": "oidc-ui", "ttlMs": 300000})
        common = {"profileId": profile, "lease": lease}
        client.call("browser_navigate", {**common, "url": admin.url})
        ref = client.call("page_locate", {**common, "selector": "#sso-login"})["ref"]
        client.call("page_click", {**common, "ref": ref})
        deadline = time.monotonic() + 15
        while True:
            try:
                client.call(
                    "page_wait_text",
                    {**common, "selector": "#account", "expected": "sso-new@example.test", "timeoutMs": 1000},
                )
                break
            except AssertionError as error:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)

        # 用途：每次重新定位门户控件，验证多机表单能创建真实后台项目；日期：2026-11-05。
        def ui(selector, command="page_click", **values):
            ref = client.call("page_locate", {**common, "selector": selector})["ref"]
            return client.call(command, {**common, "ref": ref, **values})

        ui("[data-page=scheduler]")
        ui("#project-form [name=name]", "page_fill", value="企业门户测试项目")
        ui("#project-form [name=origins]", "page_fill", value=provider.url)
        ui("#project-form button")
        client.call(
            "page_wait_text",
            {**common, "selector": "#remote-projects td:nth-child(2)", "expected": "企业门户测试项目", "timeoutMs": 10000},
        )
        image = client.call("page_screenshot", common)
        (output / "sso-portal.png").write_bytes(
            base64.b64decode(image.get("base64", image.get("pngBase64", "")))
        )
        client.call("browser_stop", {"profileId": profile})
    finally:
        client.close()
    return {
        "negativeClaims": len(bad_claims) + 3,
        "emailCollision": "rejected",
        "explicitLink": "passed",
        "HttpOnlyCookie": True,
        "desktopCodeReplay": "rejected",
        "realBrowserSso": "passed",
        "pkceExchanges": provider.pkce_checks,
    }


# 用途：有界轮询实际调度状态，不以固定睡眠判断业务完成；日期：2026-11-05。
def wait_run(admin, token, run_id, predicate=None, timeout=45):
    deadline = time.monotonic() + timeout
    row = None
    while time.monotonic() < deadline:
        snapshot = admin.call("scheduler.snapshot", token=token)
        row = next((r for r in snapshot["runs"] if r["id"] == run_id), None)
        if row and (
            predicate(row)
            if predicate
            else row["state"] in ["succeeded", "failed", "cancelled", "unknown", "expired", "resolved"]
            and row.get("stoppedConfirmed", False)
        ):
            return row
        time.sleep(0.15)
    raise AssertionError(("remote run timeout", row, snapshot["nodes"]))


class Node:
    # 用途：为每个真实执行节点建立独立私有根并启动进程；日期：2026-11-05。
    def __init__(self, executable, root, browser, admin, enrollment, name):
        self.root = root
        root.mkdir()
        config = {
            "root": str(root / "private"),
            "browser": str(browser.resolve()),
            "url": admin.url,
            "name": name,
            "enrollmentToken": enrollment,
            "headless": True,
        }
        self.config = root / "config.json"
        self.config.write_text(json.dumps(config), encoding="utf-8")
        self.executable = executable
        self.start()

    # 用途：只启动已有配置，重启继续使用节点原密钥和持久Inbox；日期：2026-11-05。
    def start(self):
        self.stderr = (self.root / "stderr.log").open("ab")
        self.process = subprocess.Popen(
            [str(self.executable), "--config", str(self.config)],
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            text=True,
            encoding="utf-8",
        )
        line = self.process.stdout.readline()
        assert line, (self.root / "stderr.log").read_text("utf-8")
        self.id = json.loads(line)["nodeId"]

    # 用途：终止本轮自有节点，Windows Job Object同时回收其浏览器进程；日期：2026-11-05。
    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=20)
        self.stderr.close()


# 用途：两个真实节点执行网页发布，校验幂等、隔离、取消、崩溃与恢复不重放；日期：2026-11-05。
def nodes_test(admin, provider, operator, buyer, node_exe, browser, output):
    token = buyer["token"]
    nodes = []
    project = admin.call("project.save", {"name": "跨节点自动化", "allowedOrigins": [provider.url]}, token)

    # 用途：读取当前组织的真实调度快照；日期：2026-11-05。
    def snapshot():
        return admin.call("scheduler.snapshot", token=token)

    try:
        for i in range(2):
            invitation = admin.call(
                "node.enrollment.create", {"projectIds": [project["id"]], "capacity": 1}, token
            )
            node = Node(
                node_exe,
                output / ("node-" + str(i)),
                browser,
                admin,
                invitation["enrollmentToken"],
                "真实节点" + str(i),
            )
            nodes.append(node)
            admin.call("node.state", {"id": node.id, "state": "active"}, token)
        flow = definition(
            provider.url,
            "remote-publish",
            [
                {"id": "open", "type": "navigate", "url": provider.url + "/lab", "next": "publish"},
                {
                    "id": "publish",
                    "type": "click",
                    "selector": "#publish",
                    "postcondition": {"selector": "#result", "expected": "published", "timeoutMs": 5000},
                    "next": "done",
                },
                {"id": "done", "type": "end"},
            ],
        )
        version = admin.call(
            "workflow.remote.publish", {"projectId": project["id"], "definition": flow}, token
        )
        assignments = []
        for node in nodes:
            assignment = admin.call(
                "assignment.create",
                {
                    "projectId": project["id"],
                    "nodeId": node.id,
                    "configuration": {"name": "发布环境", "allowedOrigins": [provider.url]},
                },
                token,
            )
            assignments.append(assignment)
        runs = []
        before = provider.clicks
        for assignment in assignments:
            body = {
                "assignmentId": assignment["id"],
                "versionId": version["id"],
                "input": {},
                "timeoutSeconds": 120,
                "idempotencyKey": secrets.token_hex(16),
            }
            run = admin.call("run.submit", body, token)
            assert admin.call("run.submit", body, token)["id"] == run["id"]
            runs.append(run)
        outcomes = [wait_run(admin, token, run["id"]) for run in runs]
        assert all(row["state"] == "succeeded" for row in outcomes), outcomes
        assert provider.clicks - before == 2, provider.clicks
        # 节点维护期间队列不能派发，取消排队任务不产生网页动作。
        admin.call("node.state", {"id": nodes[0].id, "state": "maintenance"}, token)
        body = {
            "assignmentId": assignments[0]["id"],
            "versionId": version["id"],
            "input": {},
            "timeoutSeconds": 120,
            "idempotencyKey": "queued-cancel",
        }
        queued = admin.call("run.submit", body, token)
        admin.call("run.cancel", {"id": queued["id"]}, token)
        assert (
            wait_run(admin, token, queued["id"], lambda r: r["state"] == "cancelled")["state"] == "cancelled"
        )
        admin.call("node.state", {"id": nodes[0].id, "state": "active"}, token)
        # 等待一个尚无后置动作的长步骤，测试取消与进程重启。
        waiting = definition(
            provider.url,
            "remote-wait",
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
        pending_version = admin.call(
            "workflow.remote.publish", {"projectId": project["id"], "definition": waiting}, token
        )
        body = {
            "assignmentId": assignments[0]["id"],
            "versionId": pending_version["id"],
            "input": {},
            "timeoutSeconds": 120,
            "idempotencyKey": "running-cancel",
        }
        run = admin.call("run.submit", body, token)
        wait_run(admin, token, run["id"], lambda r: r["state"] in ["accepted", "running"])
        admin.call("run.cancel", {"id": run["id"]}, token)
        cancelled = wait_run(admin, token, run["id"])
        assert cancelled["state"] == "cancelled", cancelled
        body["idempotencyKey"] = "node-crash"
        crash = admin.call("run.submit", body, token)
        wait_run(admin, token, crash["id"], lambda r: r["state"] in ["accepted", "running"])
        nodes[0].kill()
        nodes[0].start()
        recovered = wait_run(admin, token, crash["id"])
        assert recovered["state"] == "unknown" and recovered["stoppedConfirmed"], recovered
        admin.call(
            "run.resolve",
            {"id": crash["id"], "confirmRunId": crash["id"], "reason": "目标站确认没有发布，无自动重放"},
            token,
        )
        assert provider.clicks - before == 2
        # 节点签名不正确不能心跳，用户会话不能伪装执行节点。
        admin.call(
            "node.poll",
            {
                "nodeId": nodes[1].id,
                "sequence": 999,
                "timestamp": int(time.time()),
                "payload": {"bootId": "wrong"},
                "signature": "AAAA",
            },
            token,
            error="NODE_SIGNATURE",
        )
        outsider, _ = account(admin, "scheduler-outsider")
        admin.call("run.cancel", {"id": runs[0]["id"]}, outsider["token"], error="FORBIDDEN")
        admin.call("node.state", {"id": nodes[1].id, "state": "revoked"}, token)
        admin.call("node.state", {"id": nodes[1].id, "state": "active"}, token, error="NODE_STATE")
        return {
            "realNodeProcesses": 2,
            "actualBrowserSubmissions": 2,
            "duplicateSubmit": "one-command",
            "queuedCancel": "passed",
            "runningCancel": "passed",
            "nodeCrash": "unknown-confirmed-stopped-no-replay",
            "signedIdentity": "enforced",
            "crossTenant": "rejected",
            "revocation": "irreversible",
        }
    finally:
        for node in nodes:
            node.kill()


# 用途：隔离所有测试数据，输出不含令牌或密钥的验收结果；日期：2026-11-05。
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
        operator, _ = account(admin, "integrations-operator", True)
        buyer, credentials = account(admin, "integrations-buyer")
        report = {
            "date": "2026-11-05",
            "bin": str(args.bin),
            "externalServices": "local-independent-protocol-fixtures",
        }
        report["payments"] = payments_test(admin, provider, operator, buyer)
        report["oidc"] = oidc_test(
            admin,
            provider,
            operator,
            buyer,
            credentials,
            args.bin / "workbench-cli.exe",
            args.browser,
            args.output,
        )
        report["scheduler"] = nodes_test(
            admin, provider, operator, buyer, args.bin / "workbench-node.exe", args.browser, args.output
        )
        report["httpChecks"] = len(admin.events)
        report["fixtureRequests"] = len(provider.calls)
        report["result"] = "passed"
        (args.output / "integration-report.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        print(json.dumps(report, ensure_ascii=False))
    finally:
        if admin and admin.process.poll() is None:
            admin.close()
        provider.close()


if __name__ == "__main__":
    main()
