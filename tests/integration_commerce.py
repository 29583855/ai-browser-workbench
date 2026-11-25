"""验证真实C++后台的账户、支付、席位、时长及浏览器联动。日期：2026-11-05。"""
import argparse
import base64
import concurrent.futures
import hashlib
import hmac
import json
import secrets
import sqlite3
import subprocess
import time
import urllib.error
import urllib.request
from pathlib import Path

from integration_browser import Client


class Admin:
    # 用途：启动真实后台进程并等待就绪JSON，不记录初始化秘密；日期：2026-11-05。
    def __init__(self, executable, root, port=0):
        self.root = root
        self.process = subprocess.Popen([str(executable), "--root", str(root), "--port", str(port)],
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        line = self.process.stdout.readline()
        assert line, self.process.stderr.read()
        self.url = json.loads(line)["url"]
        self.shutdown = json.loads((root / "admin-endpoint.json").read_text("utf-8"))["shutdownToken"]
        self.events = []

    # 用途：发起有界HTTP调用并记录无秘密的结果断言；日期：2026-11-05。
    def call(self, action, body=None, token="", error=None, headers=None, raw=None):
        data = raw if raw is not None else json.dumps(body or {}, ensure_ascii=False).encode("utf-8")
        request = urllib.request.Request(self.url + "/api/" + action, data=data,
                                         headers={"Content-Type": "application/json", "Authorization": "Bearer " + token,
                                                  **(headers or {})})
        try:
            with urllib.request.urlopen(request, timeout=20) as response:
                value = json.load(response)
        except urllib.error.HTTPError as response:
            value = json.load(response)
        if error:
            assert not value["ok"] and value["error"]["code"] == error, (action, error, value)
        else:
            assert value["ok"], (action, value)
        self.events.append({"action": action, "outcome": value.get("error", {}).get("code", "passed")})
        return value.get("result", value)

    # 用途：只用本次端点私有令牌关闭自有后台，检查线程有界退出；日期：2026-11-05。
    def close(self):
        if self.process.poll() is None:
            self.call("shutdown", token=self.shutdown)
            self.process.wait(timeout=15)
        assert self.process.returncode == 0, self.process.stderr.read()


# 用途：创建独立测试账号并返回会话，测试口令不进入报告；日期：2026-11-05。
def account(admin, name, bootstrap=False):
    body = {"email": name + "@example.test", "name": name, "password": "Commerce-test-" + name + "-2026!"}
    if bootstrap:
        body["key"] = (admin.root / "bootstrap.key").read_text("utf-8")
    return admin.call("bootstrap" if bootstrap else "register", body), body


# 用途：发布一个可交易套餐版本，使用整数人民币分；日期：2026-11-05。
def plan(admin, operator, product="team", **extra):
    return admin.call("plan.save", {"productId": product, "name": "团队套餐", "amountMinor": 19900,
                                    "days": 30, "environmentLimit": 30, "concurrentLimit": 12,
                                    "seatLimit": 2, **extra}, operator["token"])


# 用途：创建订单并通过已授权平台账号核实线下收款；日期：2026-11-05。
def purchase(admin, buyer, operator, product):
    order = admin.call("order.create", {"planId": product["id"], "quantity": 1,
                                        "idempotencyKey": secrets.token_hex(16)}, buyer["token"])
    return admin.call("payment.confirm", {"orderId": order["id"], "amountMinor": order["amountMinor"],
                                           "receipt": secrets.token_hex(16), "reason": "test-fixture verified transfer"}, operator["token"])


# 用途：直接编辑自有测试数据库的时钟字段，加速测试续费到期与时长边界；日期：2026-11-05。
def edit_record(admin, kind, identity, changes):
    with sqlite3.connect(admin.root / "control-plane.sqlite3") as database:
        row = json.loads(database.execute("SELECT body FROM records WHERE kind=? AND id=?", (kind, identity)).fetchone()[0])
        row.update(changes)
        database.execute("UPDATE records SET body=? WHERE kind=? AND id=?", (json.dumps(row), kind, identity))


# 用途：在真实受管Chromium中提交登录及套餐表单并检查服务端数据；日期：2026-11-05。
def portal_test(admin, executable, browser, output, credentials):
    client = Client(executable, output / "portal-browser", browser)
    try:
        profile = client.call("profile_create", {"name": "Portal UI", "allowedOrigins": [admin.url]})["id"]
        client.call("browser_start", {"profileId": profile, "headless": True})
        lease = client.call("lease_acquire", {"profileId": profile, "owner": "commerce-ui-test", "ttlMs": 300000})
        common = {"profileId": profile, "lease": lease}
        client.call("browser_navigate", {**common, "url": admin.url})
        # 用途：每次动作重新定位，避免门户刷新后复用失效元素；日期：2026-11-05。
        def act(selector, command, **values):
            ref = client.call("page_locate", {**common, "selector": selector})["ref"]
            return client.call(command, {**common, "ref": ref, **values})
        act('#auth-form [name=email]', "page_fill", value=credentials["email"])
        act('#auth-form [name=password]', "page_fill", value=credentials["password"])
        act('#auth-form button', "page_click")
        client.call("page_wait_text", {**common, "selector": "#notice", "expected": "已登录", "timeoutMs": 10000})
        act('[data-page=operator]', "page_click")
        for key, value in {"productId": "portal-product", "name": "界面发布套餐", "amountMinor": "8800"}.items():
            act('form[data-action="plan.save"] [name=' + key + ']', "page_fill", value=value)
        act('form[data-action="plan.save"] button', "page_click")
        client.call("page_wait_text", {**common, "selector": "#notice", "expected": "操作完成", "timeoutMs": 10000})
        image = client.call("page_screenshot", common)
        (output / "admin-portal.png").write_bytes(base64.b64decode(image.get("base64", image.get("pngBase64", ""))))
        act('#logout', "page_click")
        client.call("page_wait_text", {**common, "selector": "#notice", "expected": "已退出", "timeoutMs": 10000})
        client.call("browser_stop", {"profileId": profile})
        return {"actions": len(client.events), "result": "passed", "screenshot": "admin-portal.png"}
    finally:
        client.close()


# 用途：端到端验证真实浏览器商业准入、设备撤销和停止后的槽释放；日期：2026-11-05。
def bridge_test(admin, executable, browser, output, buyer, credentials):
    client = Client(executable, output / "managed-browser", browser)
    try:
        connected = client.call("account_connect", {"url": admin.url, "email": credentials["email"],
                                                    "password": credentials["password"], "deviceName": "integration device"})
        profile = client.call("profile_create", {"name": "Managed", "allowedOrigins": [admin.url]})["id"]
        client.call("browser_start", {"profileId": profile, "headless": True})
        state = admin.call("snapshot", token=buyer["token"])
        assert any(slot["state"] == "active" for slot in state["slots"])
        assert client.call("account_action", {"action": "snapshot", "body": {}})["entitlement"]["paid"]
        client.call("account_disconnect", expected_error="ACCOUNT_BUSY")
        client.call("account_connect", {"url": admin.url, "email": credentials["email"], "password": credentials["password"]})
        admin.call("device.revoke", {"id": connected["deviceId"]}, buyer["token"])
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if not client.call("browser_status", {"profileId": profile})["running"]:
                break
            time.sleep(0.4)
        else:
            raise AssertionError("Revoked commercial device kept browser running")
        client.call("browser_stop", {"profileId": profile})
        assert not any(slot["state"] == "active" for slot in admin.call("snapshot", token=buyer["token"])["slots"])
        client.call("account_disconnect")
        return {"result": "passed", "actions": len(client.events), "revocationStoppedBrowser": True}
    finally:
        client.close()


# 用途：执行并发、越权、重复支付、续费、秒计费、回调及重启恢复验收；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    executable = (args.binary_dir / "workbench-admin.exe").resolve()
    admin = Admin(executable, (args.output / "admin-data").resolve())
    groups = []
    all_events = []
    try:
        admin.call("snapshot", error="UNAUTHORIZED")
        operator, operator_credentials = account(admin, "operator", True)
        admin.call("bootstrap", {}, error="BOOTSTRAP_CLOSED")
        buyer, credentials = account(admin, "buyer")
        stranger, _ = account(admin, "stranger")
        admin.call("platform.snapshot", token=buyer["token"], error="FORBIDDEN")
        admin.call("snapshot", {"tenantId": buyer["tenantId"]}, stranger["token"], error="FORBIDDEN")
        admin.call("login", {"email": credentials["email"], "password": "wrong"}, error="UNAUTHORIZED")
        product = plan(admin, operator)
        body = {"planId": product["id"], "quantity": 1, "idempotencyKey": "same-key"}
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            orders = list(pool.map(lambda _: admin.call("order.create", body, buyer["token"]), range(8)))
        assert len({row["id"] for row in orders}) == 1
        order = orders[0]
        admin.call("order.create", {**body, "quantity": 2}, buyer["token"], error="IDEMPOTENCY_CONFLICT")
        admin.call("payment.prepare", {"orderId": order["id"], "channel": "gateway"}, buyer["token"], error="PAYMENT_NOT_CONFIGURED")
        admin.call("payment.prepare", {"orderId": order["id"], "channel": "manual"}, buyer["token"])
        admin.call("payment.prepare", {"orderId": order["id"]}, stranger["token"], error="FORBIDDEN")
        receipt = {"orderId": order["id"], "amountMinor": order["amountMinor"], "receipt": "bank-ref-001", "reason": "verified bank transfer"}
        admin.call("payment.confirm", {**receipt, "amountMinor": 1}, operator["token"], error="PAYMENT_AMOUNT")
        admin.call("payment.confirm", receipt, buyer["token"], error="FORBIDDEN")
        admin.call("payment.confirm", receipt, operator["token"])
        admin.call("payment.confirm", receipt, operator["token"])
        assert len(admin.call("snapshot", token=buyer["token"])["ledger"]) == 1
        assert admin.call("snapshot", token=buyer["token"])["entitlement"]["seatLimit"] == 2
        groups.append("identity-rbac-tenant-isolation-price-snapshot-idempotent-orders-manual-payment")

        # 用途：并发争抢最后一个席位，只有一条邀请成功；日期：2026-11-05。
        def invite(index):
            try:
                return admin.call("member.invite", {"email": "invite" + str(index) + "@example.test", "role": "member"}, buyer["token"])
            except AssertionError as error:
                assert error.args[0][1]["error"]["code"] == "SEAT_LIMIT", error
                return None
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            invites = [row for row in pool.map(invite, range(4)) if row]
        assert len(invites) == 1
        accepted_index = invites[0]["email"].split("@")[0]
        teammate, _ = account(admin, accepted_index)
        accepted = admin.call("member.accept", {"invitationToken": invites[0]["invitationToken"]}, teammate["token"])
        admin.call("member.invite", {"tenantId": buyer["tenantId"], "email": "deny@example.test", "role": "owner"}, teammate["token"], error="FORBIDDEN")
        admin.call("member.update", {"id": buyer["tenantId"] + ":" + buyer["userId"], "role": "member", "state": "disabled"}, buyer["token"], error="LAST_OWNER")
        admin.call("member.update", {"id": accepted["id"], "role": "member", "state": "disabled"}, buyer["token"])
        assert admin.call("snapshot", token=buyer["token"])["occupiedSeats"] == 1
        renewal = purchase(admin, buyer, operator, product)
        grants = admin.call("snapshot", token=buyer["token"])["grant"]
        assert grants[1]["start"] == grants[0]["end"] and grants[1]["end"] - grants[1]["start"] == 30 * 86400
        admin.call("invoice.request", {"orderId": renewal["id"], "title": "测试公司", "taxId": "TEST123"}, buyer["token"])
        admin.call("invoice.issue", {"id": renewal["id"], "invoiceNumber": "TEST-INVOICE-001"}, operator["token"])
        admin.call("refund.request", {"orderId": renewal["id"], "reason": "cancel future renewal"}, buyer["token"])
        admin.call("refund.confirm", {"orderId": renewal["id"], "amountMinor": renewal["amountMinor"], "receipt": "refund-bank-001", "reason": "verified refund"}, operator["token"])
        groups.append("concurrent-seat-reservation-accept-reclaim-last-owner-renewal-invoice-refund")

        timed, _ = account(admin, "timed")
        time_product = plan(admin, operator, "time", billingMode="prepaid", timeSeconds=1200, seatLimit=1)
        purchase(admin, timed, operator, time_product)
        device = admin.call("device.register", {"deviceId": "test-time-device", "name": "time fixture"}, timed["token"])
        slots = []
        for index in range(12):
            environment = admin.call("environment.reserve", {"deviceId": device["id"], "profileId": "profile-" + str(index)}, timed["token"])
            slots.append(admin.call("slot.acquire", {"environmentId": environment["id"], "idempotencyKey": "start-" + str(index)}, timed["token"]))
        environment = admin.call("environment.reserve", {"deviceId": device["id"], "profileId": "profile-over-limit"}, timed["token"])
        admin.call("slot.acquire", {"environmentId": environment["id"], "idempotencyKey": "denied"}, timed["token"], error="CONCURRENT_LIMIT")
        assert all(slot["billingModel"] == "free" for slot in slots[:10])
        assert admin.call("snapshot", token=timed["token"])["entitlement"]["timeBalanceSeconds"] == 600
        edit_record(admin, "slot", slots[10]["id"], {"startedAt": int(time.time()) - 250})
        admin.call("slot.heartbeat", {"id": slots[10]["id"]}, timed["token"])
        assert admin.call("snapshot", token=timed["token"])["entitlement"]["timeBalanceSeconds"] == 300
        for slot in slots:
            admin.call("slot.release", {"id": slot["id"], "confirmStopped": slot["id"]}, timed["token"])
        state = admin.call("snapshot", token=timed["token"])
        charged = sum(row.get("chargedSeconds", 0) for row in state["usage"])
        assert state["entitlement"]["timeBalanceSeconds"] == 1200 - charged
        assert sum(row["deltaSeconds"] for row in state["time_ledger"]) == state["entitlement"]["timeBalanceSeconds"]
        groups.append("global-free-ten-concurrent-cap-prepaid-reserve-extend-settle-and-time-ledger")

        ui = portal_test(admin, (args.binary_dir / "workbench-cli.exe").resolve(), args.browser.resolve(), args.output.resolve(), operator_credentials)
        assert any(row["productId"] == "portal-product" for row in admin.call("platform.snapshot", token=operator["token"])["plans"])
        groups.append("real-chromium-portal-login-publish-plan-screenshot-logout")
        bridge = bridge_test(admin, (args.binary_dir / "workbench-cli.exe").resolve(), args.browser.resolve(), args.output.resolve(), buyer, credentials)
        groups.append("real-commercial-browser-admission-device-revocation-stop-and-release")
        all_events.extend(admin.events)
        admin.close()

        # 仅本测试生成的网关适配器密钥，真实服务参数仍由用户后续配置。
        gateway_secret = secrets.token_hex(32)
        (admin.root / "payment-config.json").write_bytes(json.dumps({"enabled": True, "checkoutBaseUrl": "https://gateway.example.test/checkout",
                                                                    "merchantId": "test-merchant", "webhookSecret": gateway_secret}).encode("utf-8"))
        admin = Admin(executable, admin.root)
        assert admin.call("snapshot", token=buyer["token"])["entitlement"]["paid"]
        order = admin.call("order.create", {"planId": product["id"], "quantity": 1, "idempotencyKey": "gateway"}, buyer["token"])
        link = admin.call("payment.prepare", {"orderId": order["id"], "channel": "gateway"}, buyer["token"])
        assert link["checkoutUrl"].startswith("https://gateway.example.test/checkout?payload=")
        event = {"merchantId": "test-merchant", "eventId": "event-paid-1", "transactionId": "gateway-tx-1", "orderId": order["id"],
                 "currency": "CNY", "amountMinor": order["amountMinor"], "type": "paid"}
        raw = json.dumps(event, separators=(",", ":")).encode("utf-8")
        timestamp = str(int(time.time()))
        signature = hmac.new(gateway_secret.encode(), timestamp.encode() + b"." + raw, hashlib.sha256).hexdigest()
        admin.call("webhook", headers={"X-AWB-Timestamp": timestamp, "X-AWB-Signature": "0" * 64}, raw=raw, error="PAYMENT_SIGNATURE")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: admin.call("webhook", headers={"X-AWB-Timestamp": timestamp, "X-AWB-Signature": signature}, raw=raw), range(8)))
        assert sum(not row["duplicate"] for row in results) == 1
        state = admin.call("platform.snapshot", token=operator["token"])
        assert state["reconciliation"]["balanced"]
        assert sum(row["orderId"] == order["id"] for row in state["ledger"]) == 1
        with sqlite3.connect(admin.root / "control-plane.sqlite3") as database:
            assert database.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            try:
                database.execute("UPDATE records SET body='{}' WHERE kind='ledger'")
                raise AssertionError("Ledger update was not blocked")
            except sqlite3.IntegrityError:
                pass
        groups.append("signed-gateway-callback-concurrent-deduplication-restart-integrity-immutable-ledger")
        all_events.extend(admin.events)
        report = {"result": "passed", "date": "2026-11-05", "groups": groups, "httpChecks": len(all_events),
                  "events": all_events, "portal": ui, "browserBridge": bridge,
                  "externalLivePaymentTested": False, "externalParameters": "user supplies later; test gateway uses generated fixture secret"}
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        print(json.dumps({"result": "passed", "groups": len(groups), "httpChecks": len(all_events), "portal": ui, "bridge": bridge}, ensure_ascii=False))
    finally:
        admin.close()


if __name__ == "__main__":
    main()
