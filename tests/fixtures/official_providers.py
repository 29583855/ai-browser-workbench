"""本地独立官方协议夹具：验签真实C++请求，生成官方格式支付和OIDC响应。日期：2026-11-05。"""

import base64
import hashlib
import hmac
import json
import secrets
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


# 用途：对测试RSA密钥产生标准PKCS1v1.5/SHA256签名；日期：2026-11-05。
def sign(key, text):
    return base64.b64encode(key.sign(text.encode(), padding.PKCS1v15(), hashes.SHA256())).decode()


# 用途：独立验证产品发来的RSA签名，不复用C++实现；日期：2026-11-05。
def verify(key, text, signature):
    key.public_key().verify(base64.b64decode(signature), text.encode(), padding.PKCS1v15(), hashes.SHA256())


# 用途：编码标准无填充Base64url；日期：2026-11-05。
def b64(data):
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


# 用途：提供测试JWK公钥；日期：2026-11-05。
def jwk(key, kid="fixture-key"):
    numbers = key.public_key().public_numbers()
    return {
        "kty": "RSA",
        "alg": "RS256",
        "use": "sig",
        "kid": kid,
        "n": b64(numbers.n.to_bytes((numbers.n.bit_length() + 7) // 8, "big")),
        "e": b64(numbers.e.to_bytes((numbers.e.bit_length() + 7) // 8, "big")),
    }


# 用途：序列化协议JSON，故意保留空格验证原文验签；日期：2026-11-05。
def wire(value):
    return json.dumps(value, ensure_ascii=False, separators=(", ", ": "))


class Providers:
    # 用途：创建独立RSA密钥、商户状态、测试IdP并监听动态回环端口；日期：2026-11-05。
    def __init__(self):
        self.merchant = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.platform = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.identity = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.orders = {}
        self.codes = {}
        self.claims = {}
        self.jwt_header = {}
        self.calls = []
        self.bad_signature = False
        self.drop_next = False
        self.paid = False
        self.refunded = False
        self.clicks = 0
        self.oidc_email = "sso-new@example.test"
        self.pkce_checks = 0
        provider = self

        class Handler(BaseHTTPRequestHandler):
            # 用途：禁止HTTP默认日志记录授权码和认证头；日期：2026-11-05。
            def log_message(self, *_args):
                return

            # 用途：写出有界JSON、HTML或微信签名响应；日期：2026-11-05。
            def reply(self, value, status=200, wechat=False, content_type="application/json"):
                text = wire(value) if not isinstance(value, str) else value
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                if wechat:
                    timestamp, nonce = str(int(time.time())), secrets.token_hex(8)
                    signature = sign(provider.platform, timestamp + "\n" + nonce + "\n" + text + "\n")
                    if provider.bad_signature:
                        signature = sign(provider.merchant, text)
                    for name, value in {
                        "Wechatpay-Timestamp": timestamp,
                        "Wechatpay-Nonce": nonce,
                        "Wechatpay-Serial": "PUB_FIXTURE",
                        "Wechatpay-Signature": signature,
                    }.items():
                        self.send_header(name, value)
                self.send_header("Content-Length", str(len(text.encode())))
                self.end_headers()
                self.wfile.write(text.encode())

            # 用途：读取表单或JSON并检查实际产品请求的协议参数；日期：2026-11-05。
            def do_POST(self):
                try:
                    body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
                    provider.calls.append({"method": "POST", "path": self.path})
                    if self.path == "/alipay":
                        fields = dict(urllib.parse.parse_qsl(body))
                        assert fields["app_id"] == "app-fixture" and fields["sign_type"] == "RSA2"
                        canonical = "&".join(
                            k + "=" + fields[k] for k in sorted(fields) if k != "sign" and fields[k]
                        )
                        verify(provider.merchant, canonical, fields["sign"])
                        business = json.loads(fields["biz_content"])
                        order = business["out_trade_no"]
                        method = fields["method"]
                        reference = "ali-" + order
                        amount = business.get(
                            "total_amount", provider.orders.get(order, {}).get("amount", "199.00")
                        )
                        provider.orders.setdefault(order, {"amount": amount})
                        result = {
                            "code": "10000",
                            "msg": "Success",
                            "out_trade_no": order,
                            "trade_no": reference,
                        }
                        if method.endswith("precreate"):
                            result["qr_code"] = "https://qr.alipay.test/" + order
                        elif method.endswith("fastpay.refund.query"):
                            result.update(
                                refund_amount=amount,
                                refund_status="REFUND_SUCCESS" if provider.refunded else "REFUND_PROCESSING",
                            )
                        elif method.endswith("refund"):
                            result.update(refund_fee=amount, fund_change="Y" if provider.refunded else "N")
                        else:
                            result.update(
                                total_amount=amount,
                                trade_status="TRADE_SUCCESS" if provider.paid else "WAIT_BUYER_PAY",
                            )
                        signed = wire(result)
                        signature = sign(provider.platform, signed)
                        if provider.bad_signature:
                            signature = sign(provider.merchant, signed)
                        return self.reply(
                            '{"'
                            + method.replace(".", "_")
                            + '_response":'
                            + signed
                            + ',"sign":'
                            + json.dumps(signature)
                            + "}"
                        )
                    if self.path.startswith("/v3/"):
                        self.wechat(body)
                        obj = json.loads(body)
                        order = obj["out_trade_no"]
                        provider.orders.setdefault(order, {"amount": obj["amount"].get("total", 19900)})
                        if self.path.endswith("/native"):
                            return self.reply({"code_url": "weixin://fixture/" + order}, wechat=True)
                        return self.reply(
                            {
                                "out_trade_no": order,
                                "out_refund_no": obj["out_refund_no"],
                                "refund_id": "wx-refund-" + order,
                                "status": "SUCCESS" if provider.refunded else "PROCESSING",
                                "amount": {
                                    "refund": obj["amount"]["refund"],
                                    "total": obj["amount"]["total"],
                                    "currency": "CNY",
                                },
                            },
                            wechat=True,
                        )
                    if self.path.startswith("/v1/"):
                        assert (
                            self.headers["Authorization"] == "Bearer sk_test_fixture"
                            and self.headers["Stripe-Version"] == "2024-06-20"
                        )
                        fields = dict(urllib.parse.parse_qsl(body))
                        order = fields.get("client_reference_id", fields.get("metadata[orderId]"))
                        expected = (
                            "awb-checkout-" if self.path.endswith("/sessions") else "awb-refund-"
                        ) + order
                        assert self.headers["Idempotency-Key"] == expected
                        provider.orders.setdefault(
                            order,
                            {
                                "amount": int(
                                    fields.get(
                                        "line_items[0][price_data][unit_amount]", fields.get("amount", 19900)
                                    )
                                )
                            },
                        )
                        if provider.drop_next:
                            provider.drop_next = False
                            self.close_connection = True
                            return
                        if self.path.endswith("/sessions"):
                            return self.reply(provider.stripe(order))
                        return self.reply(provider.stripe(order, True))
                    if self.path == "/oidc/token":
                        fields = dict(urllib.parse.parse_qsl(body))
                        code = fields["code"]
                        auth = provider.codes.pop(code)
                        assert (
                            fields["grant_type"] == "authorization_code"
                            and fields["redirect_uri"] == auth["redirect_uri"]
                        )
                        assert (
                            b64(hashlib.sha256(fields["code_verifier"].encode()).digest())
                            == auth["code_challenge"]
                        )
                        provider.pkce_checks += 1
                        assert (
                            self.headers["Authorization"]
                            == "Basic " + base64.b64encode(b"client-fixture:secret-fixture").decode()
                        )
                        now = int(time.time())
                        claims = {
                            "iss": provider.url + "/oidc",
                            "aud": "client-fixture",
                            "sub": "subject-fixture",
                            "email": provider.oidc_email,
                            "email_verified": True,
                            "name": "企业登录测试",
                            "nonce": auth["nonce"],
                            "iat": now,
                            "exp": now + 300,
                            **provider.claims,
                        }
                        header = {"alg": "RS256", "kid": "fixture-key", **provider.jwt_header}
                        prefix = b64(wire(header).encode()) + "." + b64(wire(claims).encode())
                        signed = sign(provider.identity, prefix)
                        if provider.bad_signature:
                            signed = sign(provider.platform, prefix)
                        return self.reply(
                            {
                                "id_token": prefix + "." + b64(base64.b64decode(signed)),
                                "token_type": "Bearer",
                                "access_token": "access-fixture",
                            }
                        )
                    if self.path == "/increment":
                        provider.clicks += 1
                        return self.reply({"clicks": provider.clicks})
                    self.reply({"error": "unrecognized"}, 404)
                except Exception as error:
                    provider.calls.append({"fixtureError": type(error).__name__, "path": self.path})
                    self.reply({"error": "fixture validation failed"}, 400)

            # 用途：独立验证微信官方请求签名及商户证书序号；日期：2026-11-05。
            def wechat(self, body):
                authorization = self.headers["Authorization"]
                assert authorization.startswith("WECHATPAY2-SHA256-RSA2048 ")
                fields = dict(x.split("=", 1) for x in authorization.split(" ", 1)[1].split(","))
                fields = {k: v.strip('"') for k, v in fields.items()}
                assert fields["mchid"] == "merchant-fixture" and fields["serial_no"] == "CERT_FIXTURE"
                verify(
                    provider.merchant,
                    self.command
                    + "\n"
                    + self.path
                    + "\n"
                    + fields["timestamp"]
                    + "\n"
                    + fields["nonce_str"]
                    + "\n"
                    + body
                    + "\n",
                    fields["signature"],
                )

            # 用途：提供查单、发现文档、JWKS、授权重定向及真实自动化目标网页；日期：2026-11-05。
            def do_GET(self):
                try:
                    provider.calls.append({"method": "GET", "path": self.path.split("?")[0]})
                    parsed = urllib.parse.urlsplit(self.path)
                    if parsed.path.startswith("/v3/"):
                        self.wechat("")
                        order = parsed.path.rsplit("/", 1)[1].removeprefix("refund-")
                        amount = provider.orders[order]["amount"]
                        if "/refund/" in parsed.path:
                            return self.reply(
                                {
                                    "out_trade_no": order,
                                    "out_refund_no": "refund-" + order,
                                    "refund_id": "wx-refund-" + order,
                                    "status": "SUCCESS" if provider.refunded else "PROCESSING",
                                    "amount": {"refund": amount, "total": amount, "currency": "CNY"},
                                },
                                wechat=True,
                            )
                        return self.reply(
                            {
                                "appid": "app-fixture",
                                "mchid": "merchant-fixture",
                                "out_trade_no": order,
                                "transaction_id": "wx-" + order,
                                "trade_state": "SUCCESS" if provider.paid else "NOTPAY",
                                "amount": {"total": amount, "currency": "CNY"},
                            },
                            wechat=True,
                        )
                    if parsed.path.startswith("/v1/"):
                        assert self.headers["Authorization"] == "Bearer sk_test_fixture"
                        refund = "/refunds/" in parsed.path
                        order = parsed.path.rsplit("/", 1)[1].removeprefix("re-").removeprefix("cs-")
                        return self.reply(provider.stripe(order, refund))
                    if parsed.path == "/oidc/.well-known/openid-configuration":
                        return self.reply(
                            {
                                "issuer": provider.url + "/oidc",
                                "authorization_endpoint": provider.url + "/oidc/authorize",
                                "token_endpoint": provider.url + "/oidc/token",
                                "jwks_uri": provider.url + "/oidc/keys",
                                "end_session_endpoint": provider.url + "/oidc/logout",
                                "code_challenge_methods_supported": ["S256"],
                            }
                        )
                    if parsed.path == "/oidc/keys":
                        return self.reply({"keys": [jwk(provider.identity)]})
                    if parsed.path == "/oidc/authorize":
                        params = dict(urllib.parse.parse_qsl(parsed.query))
                        assert (
                            params["client_id"] == "client-fixture"
                            and params["code_challenge_method"] == "S256"
                        )
                        code = secrets.token_hex(16)
                        provider.codes[code] = params
                        self.send_response(302)
                        self.send_header(
                            "Location",
                            params["redirect_uri"]
                            + "?"
                            + urllib.parse.urlencode({"code": code, "state": params["state"]}),
                        )
                        self.end_headers()
                        return
                    if parsed.path == "/oidc/logout":
                        params = dict(urllib.parse.parse_qsl(parsed.query))
                        self.send_response(302)
                        self.send_header("Location", params["post_logout_redirect_uri"])
                        self.end_headers()
                        return
                    if parsed.path == "/lab":
                        html = """<!doctype html><meta charset="utf-8"><h1 id="ready">ready</h1><button id="publish">发布文章</button><p id="result">idle</p><script>document.querySelector('#publish').onclick=async()=>{const r=await fetch('/increment',{method:'POST'});await r.json();document.querySelector('#result').textContent='published';};</script>"""
                        return self.reply(html, content_type="text/html")
                    self.reply({"error": "not found"}, 404)
                except Exception as error:
                    provider.calls.append({"fixtureError": type(error).__name__, "path": parsed.path})
                    self.reply({"error": "fixture validation failed"}, 400)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:" + str(self.server.server_port)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    # 用途：构造Stripe官方对象形状与同一订单稳定资源身份；日期：2026-11-05。
    def stripe(self, order, refund=False):
        amount = self.orders[order]["amount"]
        if refund:
            return {
                "id": "re-" + order,
                "status": "succeeded" if self.refunded else "pending",
                "currency": "cny",
                "amount": amount,
                "metadata": {"orderId": order},
                "livemode": False,
            }
        return {
            "id": "cs-" + order,
            "url": "https://checkout.stripe.test/" + order,
            "client_reference_id": order,
            "metadata": {"orderId": order},
            "amount_total": amount,
            "currency": "cny",
            "payment_status": "paid" if self.paid else "unpaid",
            "payment_intent": "pi-" + order,
            "mode": "payment",
            "livemode": False,
        }

    # 用途：写出仅在临时测试根启用的支付和IdP配置，真实密钥不进入文档；日期：2026-11-05。
    def configs(self, root, admin_origin):
        private = self.merchant.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
        ).decode()
        public = (
            self.platform.public_key()
            .public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)
            .decode()
        )
        payments = {
            "allowLoopbackTest": True,
            "alipay": {
                "enabled": True,
                "appId": "app-fixture",
                "sellerId": "seller-fixture",
                "privateKeyPem": private,
                "publicKeyPem": public,
                "baseUrl": self.url + "/alipay",
                "notifyUrl": admin_origin + "/payments/alipay/notify",
            },
            "wechat": {
                "enabled": True,
                "appId": "app-fixture",
                "merchantId": "merchant-fixture",
                "merchantSerial": "CERT_FIXTURE",
                "privateKeyPem": private,
                "apiV3Key": "0" * 32,
                "platformKeys": {"PUB_FIXTURE": public},
                "baseUrl": self.url,
                "notifyUrl": admin_origin + "/payments/wechat/notify",
            },
            "stripe": {
                "enabled": True,
                "secretKey": "sk_test_fixture",
                "webhookSecrets": ["whsec_fixture"],
                "liveMode": False,
                "apiVersion": "2024-06-20",
                "baseUrl": self.url,
                "successUrl": admin_origin + "/",
                "cancelUrl": admin_origin + "/",
            },
        }
        oidc = {
            "enabled": True,
            "issuer": self.url + "/oidc",
            "clientId": "client-fixture",
            "clientSecret": "secret-fixture",
            "redirectUri": admin_origin + "/auth/callback",
            "tokenAuthMethod": "client_secret_basic",
            "allowAutoProvision": True,
            "allowLoopbackTest": True,
            "allowedEndpointOrigins": [],
        }
        for name, value in [("official-payments.json", payments), ("oidc-config.json", oidc)]:
            (root / name).write_text(json.dumps(value), encoding="utf-8")

    # 用途：生成三种渠道的真实标准通知原文与签名头，支持认证加密；日期：2026-11-05。
    def notification(self, channel, order, event="event-fixture", amount=19900, refund=False):
        if channel == "alipay":
            fields = {
                "app_id": "app-fixture",
                "seller_id": "seller-fixture",
                "out_trade_no": order,
                "trade_no": "ali-" + order,
                "trade_status": "TRADE_SUCCESS",
                "total_amount": f"{amount//100}.{amount%100:02}",
                "notify_id": event,
                "sign_type": "RSA2",
            }
            fields["sign"] = sign(
                self.platform, "&".join(k + "=" + fields[k] for k in sorted(fields) if k != "sign_type")
            )
            return urllib.parse.urlencode(fields), {"Content-Type": "application/x-www-form-urlencoded"}
        if channel == "wechat":
            data = {
                "mchid": "merchant-fixture",
                "appid": "app-fixture",
                "out_trade_no": order,
                "transaction_id": "wx-" + order,
                "trade_state": "SUCCESS",
                "amount": {"total": amount, "currency": "CNY"},
            }
            if refund:
                data.update(
                    out_refund_no="refund-" + order,
                    refund_id="wx-refund-" + order,
                    refund_status="SUCCESS",
                    amount={"refund": amount, "total": amount, "currency": "CNY"},
                )
            nonce = "0123456789ab"
            aad = "transaction"
            cipher = AESGCM(b"0" * 32).encrypt(nonce.encode(), wire(data).encode(), aad.encode())
            body = wire(
                {
                    "id": event,
                    "event_type": "REFUND.SUCCESS" if refund else "TRANSACTION.SUCCESS",
                    "resource": {
                        "algorithm": "AEAD_AES_256_GCM",
                        "nonce": nonce,
                        "associated_data": aad,
                        "ciphertext": base64.b64encode(cipher).decode(),
                    },
                }
            )
            timestamp = str(int(time.time()))
            nonce = "notify-nonce"
            return body, {
                "Content-Type": "application/json",
                "Wechatpay-Timestamp": timestamp,
                "Wechatpay-Nonce": nonce,
                "Wechatpay-Serial": "PUB_FIXTURE",
                "Wechatpay-Signature": sign(self.platform, timestamp + "\n" + nonce + "\n" + body + "\n"),
            }
        obj = self.stripe(order, refund)
        obj.update(
            {
                "amount" if refund else "amount_total": amount,
                "status" if refund else "payment_status": "succeeded" if refund else "paid",
            }
        )
        body = wire(
            {
                "id": event,
                "type": "refund.updated" if refund else "checkout.session.completed",
                "livemode": False,
                "data": {"object": obj},
            }
        )
        timestamp = str(int(time.time()))
        signature = hmac.new(b"whsec_fixture", (timestamp + "." + body).encode(), hashlib.sha256).hexdigest()
        return body, {
            "Content-Type": "application/json",
            "Stripe-Signature": "t=" + timestamp + ",v1=" + signature,
        }

    # 用途：关闭本轮测试HTTP监听；日期：2026-11-05。
    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
