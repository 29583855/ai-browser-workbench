"""验证控制面异常边界、掉电恢复、支付冲突及真实原生账号表单。日期：2026-11-05。"""
import argparse
import ctypes
import hashlib
import hmac
import json
import secrets
import sqlite3
import subprocess
import time
from ctypes import wintypes
from pathlib import Path

from integration_commerce import Admin, account, plan, purchase, edit_record
from integration_desktop import USER, windows, kind, main_window, message, text, set_text
from integration_library_desktop import capture


# 用途：对真实原生账号面板执行密码掩码、异步登录、实时授权和退出验收；日期：2026-11-05。
def native_account(admin, binary, browser, output, credentials):
    process = subprocess.Popen([str(binary / "workbench.exe"), "--root", str(output / "native-data"), "--browser", str(browser)])
    owner = main_window(process)
    USER.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    USER.PostMessageW(owner, 0x0111, 918, 0)
    try:
        deadline = time.monotonic() + 10
        panel = None
        while time.monotonic() < deadline:
            for window in windows():
                pid = wintypes.DWORD()
                USER.GetWindowThreadProcessId(window, ctypes.byref(pid))
                if pid.value == process.pid and kind(window) == "AWB.AccountPanel" and USER.GetDlgItem(window, 1421):
                    panel = window
                    break
            if panel:
                break
            time.sleep(0.02)
        assert panel, "Account panel did not open"
        output_control = USER.GetDlgItem(panel, 1421)
        # 用途：等待异步按钮结果并验证不泄露用户密码；日期：2026-11-05。
        def wait_result():
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                content = text(output_control)
                if content and USER.IsWindowEnabled(USER.GetDlgItem(panel, 1410)):
                    result = json.loads(content)
                    assert result["ok"], result
                    assert credentials["password"] not in content
                    return result["result"]
                time.sleep(0.02)
            raise TimeoutError("Native account request did not complete")
        wait_result()
        set_text(USER.GetDlgItem(panel, 1401), admin.url)
        set_text(USER.GetDlgItem(panel, 1402), credentials["email"])
        set_text(USER.GetDlgItem(panel, 1403), credentials["password"])
        assert message(USER.GetDlgItem(panel, 1403), 0x00D2) != 0, "Password field is not masked"
        set_text(output_control, "")
        message(USER.GetDlgItem(panel, 1410), 0x00F5)
        assert wait_result()["connected"]
        assert message(USER.GetDlgItem(panel, 1403), 0x000E) == 0, "Password remained in edit control"
        set_text(output_control, "")
        message(USER.GetDlgItem(panel, 1411), 0x00F5)
        assert wait_result()["entitlement"]["environmentLimit"] == 10
        capture(panel, output / "native-account.png")
        set_text(output_control, "")
        message(USER.GetDlgItem(panel, 1412), 0x00F5)
        assert not wait_result()["connected"]
        message(panel, 0x0010)
        message(owner, 0x0010)
        process.wait(timeout=15)
        assert process.returncode == 0
        return {"result": "passed", "checks": 7, "screenshot": "native-account.png"}
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)


# 用途：对生成的自有测试网关事件签名，生产代码不读取测试秘密；日期：2026-11-05。
def callback(admin, secret, event, error=None, old=False):
    raw = json.dumps(event, separators=(",", ":")).encode("utf-8")
    timestamp = str(int(time.time()) - (600 if old else 0))
    signature = hmac.new(secret.encode(), timestamp.encode() + b"." + raw, hashlib.sha256).hexdigest()
    return admin.call("webhook", raw=raw, headers={"X-AWB-Timestamp": timestamp, "X-AWB-Signature": signature}, error=error)


# 用途：验证免费资源、异常时长、到期回落、回调核验与原生表单等边界；日期：2026-11-05。
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--browser", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    root = (args.output / "admin-data").resolve()
    root.mkdir()
    secret = secrets.token_hex(32)
    (root / "payment-config.json").write_bytes(json.dumps({"enabled": True, "merchantId": "fixture",
                                                          "checkoutBaseUrl": "https://gateway.example.test/checkout",
                                                          "webhookSecret": secret}).encode("utf-8"))
    executable = (args.binary_dir / "workbench-admin.exe").resolve()
    admin = Admin(executable, root)
    groups = []
    checks = []
    try:
        operator, _ = account(admin, "edge-operator", True)
        buyer, credentials = account(admin, "edge-buyer")
        admin.call("register", {"email": credentials["email"], "password": "invalid", "name": "duplicate"}, error="ACCOUNT_EXISTS")
        admin.call("register", {"email": "short@example.test", "password": "short", "name": "short"}, error="PASSWORD_POLICY")
        admin.call("status", headers={"Origin": "https://attacker.example"}, error="FORBIDDEN")
        admin.call("status", headers={"Host": "attacker.example"}, error="FORBIDDEN")
        for _ in range(5):
            admin.call("login", {"email": credentials["email"], "password": "wrong"}, error="UNAUTHORIZED")
        admin.call("login", {"email": credentials["email"], "password": credentials["password"]}, error="UNAUTHORIZED")
        edit_record(admin, "user", buyer["userId"], {"lockedUntil": 0})
        buyer = admin.call("login", {"email": credentials["email"], "password": credentials["password"]})
        admin.call("password.change", {"oldPassword": credentials["password"], "password": "Replacement-secret-2026!"}, buyer["token"])
        admin.call("snapshot", token=buyer["token"], error="UNAUTHORIZED")
        credentials["password"] = "Replacement-secret-2026!"
        buyer = admin.call("login", {"email": credentials["email"], "password": credentials["password"]})
        groups.append("registration-input-origin-host-lockout-password-session-revocation")

        devices = [admin.call("device.register", {"deviceId": "device-" + str(i), "name": "fixture"}, buyer["token"]) for i in range(2)]
        environments = []
        slots = []
        for index in range(10):
            environment = admin.call("environment.reserve", {"deviceId": devices[index % 2]["id"], "profileId": "profile-" + str(index)}, buyer["token"])
            environments.append(environment)
            slots.append(admin.call("slot.acquire", {"environmentId": environment["id"], "idempotencyKey": "free-" + str(index)}, buyer["token"]))
        admin.call("environment.reserve", {"deviceId": devices[0]["id"], "profileId": "denied"}, buyer["token"], error="ENVIRONMENT_LIMIT")
        assert not admin.call("slot.lookup", {"idempotencyKey": "never-sent"}, buyer["token"])["found"]
        assert admin.call("slot.lookup", {"idempotencyKey": "free-0"}, buyer["token"])["slot"]["id"] == slots[0]["id"]
        admin.call("slot.acquire", {"environmentId": environments[0]["id"], "idempotencyKey": "duplicate-start"}, buyer["token"], error="SLOT_BUSY")
        admin.call("environment.release", {"deviceId": devices[0]["id"], "profileId": "profile-0"}, buyer["token"], error="ENVIRONMENT_RUNNING")
        checks.extend(admin.events)
        admin.process.kill()
        admin.process.wait(timeout=5)
        admin = Admin(executable, root)
        state = admin.call("snapshot", token=buyer["token"])
        assert len([slot for slot in state["slots"] if slot["state"] == "active"]) == 10
        admin.call("slot.acquire", {"environmentId": environments[0]["id"], "idempotencyKey": "free-0"}, buyer["token"])
        admin.call("slot.resolve", {"id": slots[0]["id"], "confirmStopped": "wrong", "reason": "fixture"}, buyer["token"], error="CONFIRMATION_REQUIRED")
        admin.call("tenant.state", {"id": buyer["tenantId"], "state": "suspended"}, operator["token"])
        admin.call("slot.heartbeat", {"id": slots[0]["id"]}, buyer["token"], error="TENANT_SUSPENDED")
        assert admin.call("slot.lookup", {"idempotencyKey": "free-0"}, buyer["token"])["found"]
        for slot in slots:
            admin.call("slot.resolve", {"id": slot["id"], "confirmStopped": slot["id"], "reason": "fixture verifies stopped"}, buyer["token"])
        admin.call("tenant.state", {"id": buyer["tenantId"], "state": "active"}, operator["token"])
        groups.append("cross-device-free-capacity-crash-recovery-no-automatic-reuse-suspension-and-resolution")

        product = plan(admin, operator, "edge", seatLimit=3)
        order = purchase(admin, buyer, operator, product)
        invite = admin.call("member.invite", {"email": "expired@example.test", "role": "admin"}, buyer["token"])
        edit_record(admin, "invite", invite["id"], {"expiresAt": int(time.time()) - 1})
        invited, _ = account(admin, "expired")
        admin.call("member.accept", {"invitationToken": invite["invitationToken"]}, invited["token"], error="INVITATION_INVALID")
        assert admin.call("snapshot", token=buyer["token"])["occupiedSeats"] == 1
        finance, _ = account(admin, "finance-edge")
        finance_invite = admin.call("member.invite", {"email": "finance-edge@example.test", "role": "finance"}, buyer["token"])
        admin.call("member.accept", {"invitationToken": finance_invite["invitationToken"]}, finance["token"])
        admin.call("order.create", {"tenantId": buyer["tenantId"], "planId": product["id"], "quantity": 1, "idempotencyKey": "finance-order"}, finance["token"])
        admin.call("member.invite", {"tenantId": buyer["tenantId"], "email": "denied@example.test", "role": "owner"}, finance["token"], error="FORBIDDEN")
        admin.call("platform.snapshot", token=finance["token"], error="FORBIDDEN")
        edit_record(admin, "grant", order["id"], {"end": int(time.time()) - 1})
        state = admin.call("snapshot", token=buyer["token"])
        assert not state["entitlement"]["paid"] and state["entitlement"]["environmentLimit"] == 10
        groups.append("invitation-expiration-reclaims-reservation-and-subscription-free-fallback")

        product = plan(admin, operator, "gateway-edge")
        order = admin.call("order.create", {"planId": product["id"], "quantity": 1, "idempotencyKey": "gateway-edge"}, buyer["token"])
        event = {"merchantId": "fixture", "orderId": order["id"], "amountMinor": order["amountMinor"],
                 "currency": "CNY", "eventId": "edge-paid", "transactionId": "edge-tx", "type": "paid"}
        callback(admin, secret, event, error="PAYMENT_SIGNATURE", old=True)
        callback(admin, secret, {**event, "amountMinor": 1}, error="PAYMENT_AMOUNT")
        callback(admin, secret, {**event, "currency": "USD"}, error="PAYMENT_EVENT")
        callback(admin, secret, {**event, "merchantId": "other"}, error="PAYMENT_EVENT")
        callback(admin, secret, event)
        callback(admin, secret, {**event, "transactionId": "another-tx"}, error="PAYMENT_CONFLICT")
        other = admin.call("order.create", {"planId": product["id"], "quantity": 1, "idempotencyKey": "other-gateway"}, buyer["token"])
        callback(admin, secret, {**event, "eventId": "other-event", "orderId": other["id"]}, error="PAYMENT_CONFLICT")
        admin.call("invoice.request", {"orderId": order["id"], "title": "edge", "taxId": "EDGE"}, buyer["token"])
        admin.call("invoice.issue", {"id": order["id"], "invoiceNumber": "EDGE001"}, operator["token"])
        refund = {**event, "eventId": "edge-refund", "transactionId": "edge-refund-tx", "type": "refunded"}
        callback(admin, secret, refund, error="ORDER_STATE")
        admin.call("refund.request", {"orderId": order["id"], "reason": "fixture"}, buyer["token"])
        callback(admin, secret, refund)
        assert admin.call("snapshot", token=buyer["token"])["invoice"][0]["state"] == "credit_note_required"
        admin.call("invoice.credit", {"id": order["id"], "creditNoteNumber": "CREDIT001"}, operator["token"])
        callback(admin, secret, refund)
        callback(admin, secret, event)
        admin.call("order.cancel", {"orderId": other["id"]}, buyer["token"])
        late = {**event, "eventId": "late-event", "orderId": other["id"], "transactionId": "late-tx"}
        assert callback(admin, secret, late)["reconciliationRequired"]
        admin.call("payment.reconcile", {"orderId": other["id"], "amountMinor": other["amountMinor"], "reviewId": "late-event", "receipt": "verified", "reason": "fixture checked late payment"}, operator["token"])
        assert admin.call("platform.snapshot", token=operator["token"])["reconciliation"]["balanced"]
        groups.append("gateway-amount-currency-merchant-replay-conflicts-refund-credit-note-and-late-payment-review")

        timed, _ = account(admin, "time-edge")
        pack = plan(admin, operator, "time-edge", billingMode="prepaid", timeSeconds=300, concurrentLimit=11, seatLimit=1)
        purchase(admin, timed, operator, pack)
        device = admin.call("device.register", {"deviceId": "time-edge", "name": "fixture"}, timed["token"])
        running = []
        for index in range(11):
            environment = admin.call("environment.reserve", {"deviceId": device["id"], "profileId": str(index)}, timed["token"])
            running.append(admin.call("slot.acquire", {"environmentId": environment["id"], "idempotencyKey": str(index)}, timed["token"]))
        extra = running[-1]
        edit_record(admin, "slot", extra["id"], {"startedAt": int(time.time()) - 350})
        admin.call("slot.heartbeat", {"id": extra["id"]}, timed["token"], error="TIME_EXHAUSTED")
        admin.call("slot.release", {"id": extra["id"], "confirmStopped": extra["id"]}, timed["token"])
        state = admin.call("snapshot", token=timed["token"])
        assert state["entitlement"]["timeBalanceSeconds"] <= -50
        assert len(state["usage"]) == 1 and state["usage"][0]["chargedSeconds"] >= 350
        purchase(admin, timed, operator, pack)
        assert admin.call("snapshot", token=timed["token"])["entitlement"]["timeBalanceSeconds"] <= 250
        groups.append("time-exhaustion-disconnected-overrun-debt-and-recharge-offset")

        clean, clean_credentials = account(admin, "native")
        native = native_account(admin, args.binary_dir.resolve(), args.browser.resolve(), args.output.resolve(), clean_credentials)
        assert clean["tenantId"]
        groups.append("native-account-form-masked-password-login-entitlement-query-and-logout")
        checks.extend(admin.events)
        report = {"date": "2026-11-05", "result": "passed", "groups": groups, "httpChecks": len(checks), "events": checks, "native": native}
        (args.output / "report.json").write_bytes((json.dumps(report, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
        print(json.dumps({"result": "passed", "groups": len(groups), "httpChecks": len(checks), "native": native}, ensure_ascii=False))
    finally:
        admin.close()


if __name__ == "__main__":
    main()
