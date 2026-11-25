"""本地测试夹具；账号仅在测试服务器内存中存在，不用于生产认证。日期：2026-11-03。"""

import hashlib
import hmac
import json
import secrets
import threading
import time
from pathlib import Path
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


class FixtureServer(ThreadingHTTPServer):
    # 用途：创建仅回环可达的测试服务器及独立测试数据；日期：2026-11-03。
    def __init__(self, port=0, public_host="127.0.0.1"):
        super().__init__(("127.0.0.1", port), FixtureHandler)
        if public_host not in ("127.0.0.1", "awb-fixture.invalid"):
            raise ValueError("Only fixed fixture host names are permitted")
        self.public_host = public_host
        self.users = {}
        self.sessions = {}
        self.registration_count = 0
        self.login_count = 0
        self.fault_commit_count = 0
        self.identity_observations = []
        self.data_lock = threading.Lock()


class FixtureHandler(BaseHTTPRequestHandler):
    # 用途：测试日志不输出口令、Cookie或请求正文；日期：2026-11-03。
    def log_message(self, _format, *_args):
        return

    # 用途：统一发送有长度和禁嗅探标头的响应；日期：2026-11-03。
    def send_content(self, status, body, content_type="text/html; charset=utf-8", headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    # 用途：返回结构化夹具业务结果；日期：2026-11-03。
    def send_json(self, status, data, headers=None):
        self.send_content(status, json.dumps(data, ensure_ascii=False), "application/json; charset=utf-8", headers)

    # 用途：从HttpOnly会话Cookie校验当前测试身份；日期：2026-11-03。
    def current_user(self):
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
            token = cookie["awb_session"].value if "awb_session" in cookie else ""
        except (KeyError, ValueError):
            return None
        return self.server.sessions.get(token)

    # 用途：提供登录、注册、存储、诊断、报表及SW真实夹具；日期：2026-11-04。
    def do_GET(self):
        path = urlsplit(self.path).path
        if path in ("/", "/login", "/register", "/app"):
            self.send_content(HTTPStatus.OK, PAGE.replace("{CROSS_ORIGIN}", f"http://localhost:{self.server.server_port}"))
        elif path == "/frame":
            self.send_content(HTTPStatus.OK, '<!doctype html><meta charset="utf-8"><button id="frame-action">跨站框架按钮</button>')
        elif path == "/api/me":
            user = self.current_user()
            self.server.identity_observations.append({"authenticated": bool(user), "tick": time.monotonic()})
            self.send_json(HTTPStatus.OK, {"username": user})
        elif path == "/api/report":
            if not self.current_user():
                self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "LOGIN_REQUIRED"})
            else:
                self.send_json(HTTPStatus.OK, {"rows": [{"date": "2026-11-03", "value": 42}]})
        elif path == "/download/report.csv":
            if not self.current_user():
                self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "LOGIN_REQUIRED"})
            else:
                self.send_content(HTTPStatus.OK, "date,value\n2026-11-03,42\n", "text/csv; charset=utf-8",
                                  {"Content-Disposition": 'attachment; filename="report.csv"'})
        elif path == "/download/fixture.csv":
            self.send_content(HTTPStatus.OK, "fixture,value\npublic,1\n", "text/csv; charset=utf-8",
                              {"Content-Disposition": 'attachment; filename="fixture.csv"'})
        elif path in ("/sw.js", "/fixture.js"):
            resource = Path(__file__).with_name(path[1:]).read_bytes()
            self.send_content(HTTPStatus.OK, resource, "text/javascript; charset=utf-8",
                              {"Service-Worker-Allowed": "/"} if path == "/sw.js" else None)
        elif path == "/health":
            self.send_json(HTTPStatus.OK, {"ready": True})
        elif path == "/inspect":
            self.send_content(HTTPStatus.OK, Path(__file__).with_name("inspect.html").read_bytes())
        elif path == "/workflow":
            self.send_content(HTTPStatus.OK, Path(__file__).with_name("workflow.html").read_bytes())
        elif path in ["/recording", "/recording-next"]:
            self.send_content(HTTPStatus.OK, Path(__file__).with_name("recording.html").read_bytes())
        elif path == "/extraction":
            self.send_content(HTTPStatus.OK, Path(__file__).with_name("extraction.html").read_bytes())
        elif path == "/diagnostics":
            self.send_content(HTTPStatus.OK, Path(__file__).with_name("diagnostics.html").read_bytes())
        elif path == "/diag-delay":
            time.sleep(0.6)
            self.send_content(HTTPStatus.SERVICE_UNAVAILABLE, "diagnostic-secret-canary", "text/plain")
        elif path == "/diag-redirect":
            self.send_content(HTTPStatus.FOUND, "", headers={"Location": "/diag404?redirect=diagnostic-secret-canary"})
        elif path == "/diag-fail":
            self.close_connection = True
            self.connection.close()
        elif path == "/audit-fault":
            self.send_content(HTTPStatus.OK, '''<!doctype html><meta charset="utf-8">
<button id="commit">提交</button><output id="result">ready</output><script>
// 用途：同步请求延迟夹具以稳定复现动作执行后审计文件被占用；日期：2026-11-04。
document.querySelector('#commit').onclick = function () {
 const request = new XMLHttpRequest(); request.open('GET', '/audit-delayed-commit', false);
 request.send(); document.querySelector('#result').textContent = request.responseText;
};</script>''')
        elif path == "/audit-delayed-commit":
            time.sleep(1.5)
            with self.server.data_lock:
                self.server.fault_commit_count += 1
            self.send_content(HTTPStatus.OK, "committed-once", "text/plain; charset=utf-8")
        elif path == "/runtime":
            self.send_content(HTTPStatus.OK, '<!doctype html><meta charset="utf-8"><pre id="runtime"></pre>'
                              '<script>document.querySelector("#runtime").textContent=JSON.stringify('
                              '{userAgent:navigator.userAgent,languages:Array.from(navigator.languages),'
                              'timezone:Intl.DateTimeFormat().resolvedOptions().timeZone});</script>')
        elif path == "/runtime-headers":
            self.send_json(HTTPStatus.OK, {"userAgent": self.headers.get("User-Agent"),
                                           "acceptLanguage": self.headers.get("Accept-Language")})
        elif path == "/pages":
            denied = f"http://localhost:{self.server.server_port}"
            self.send_content(HTTPStatus.OK, '<!doctype html><meta charset="utf-8">'
                              '<button id="popup">打开同源弹窗</button><button id="denied">打开未授权来源</button>'
                              '<pre id="identity">page-one</pre><script>'
                              '// 用途：真实鼠标触发子弹窗以验证归属及来源边界；日期：2026-11-04。\n'
                              'document.querySelector("#popup").onclick=()=>window.open("/runtime","awb-popup");'
                              '// 用途：创建未授权来源弹窗，产品不得自动接管；日期：2026-11-04。\n'
                              f'document.querySelector("#denied").onclick=()=>window.open("{denied}/runtime","awb-denied");'
                              '</script>')
        else:
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "NOT_FOUND"})

    # 用途：校验同源JSON并处理真实注册/登录/注销，保留服务器提交计数；日期：2026-11-03。
    def do_POST(self):
        expected_origin = f"http://{self.server.public_host}:{self.server.server_port}"
        if self.headers.get("Origin") != expected_origin or self.headers.get("Host") != expected_origin[7:]:
            self.send_json(HTTPStatus.FORBIDDEN, {"error": "ORIGIN_DENIED"})
            return
        if urlsplit(self.path).path == "/api/fault-commit":
            if self.headers.get("Content-Length") != "12" or self.rfile.read(12) != b"fixture-only":
                self.send_json(HTTPStatus.BAD_REQUEST, {"error": "FIXTURE_PAYLOAD_REQUIRED"})
                return
            with self.server.data_lock:
                self.server.fault_commit_count += 1
            self.send_json(HTTPStatus.OK, {"committed": True})
            return
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 8192:
                raise ValueError("size")
            data = json.loads(self.rfile.read(size))
            if not isinstance(data, dict):
                raise ValueError("shape")
        except (ValueError, UnicodeError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "INVALID_JSON"})
            return
        username = data.get("username", "")
        password = data.get("password", "")
        path = urlsplit(self.path).path
        if path == "/api/logout":
            self.send_json(HTTPStatus.OK, {"loggedOut": True},
                           {"Set-Cookie": "awb_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict"})
            return
        if (not isinstance(username, str) or not 3 <= len(username) <= 64 or
                not isinstance(password, str) or not 8 <= len(password) <= 128):
            self.send_json(HTTPStatus.UNPROCESSABLE_ENTITY, {"error": "INVALID_FIELDS"})
            return
        with self.server.data_lock:
            if path == "/api/register":
                if username in self.server.users:
                    self.send_json(HTTPStatus.CONFLICT, {"error": "DUPLICATE_USER"})
                    return
                salt = secrets.token_bytes(16)
                digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 100_000)
                self.server.users[username] = (salt, digest)
                self.server.registration_count += 1
                self.send_json(HTTPStatus.CREATED, {"registered": username})
            elif path == "/api/login":
                existing = self.server.users.get(username)
                if not existing or not hmac.compare_digest(
                        existing[1], hashlib.pbkdf2_hmac("sha256", password.encode(), existing[0], 100_000)):
                    self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "INVALID_CREDENTIALS"})
                    return
                token = secrets.token_urlsafe(32)
                self.server.sessions[token] = username
                self.server.login_count += 1
                self.send_json(HTTPStatus.OK, {"username": username},
                               {"Set-Cookie": f"awb_session={token}; Max-Age=86400; Path=/; HttpOnly; SameSite=Strict"})
            else:
                self.send_json(HTTPStatus.NOT_FOUND, {"error": "NOT_FOUND"})


PAGE = """<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<title>AI Browser Workbench 本地功能夹具</title>
<style>body{font:16px system-ui;max-width:850px;margin:30px auto}label{display:block;margin:12px 0}
button,input{font:inherit;padding:8px;margin-right:8px}pre{padding:14px;background:#eef3fa}</style>
<h1>登录注册与浏览器隔离夹具</h1>
<label for="username">用户名</label><input id="username" autocomplete="off">
<label for="password">密码</label><input id="password" type="password" autocomplete="off">
<p><button id="register">注册</button><button id="login">登录</button><button id="logout">退出</button></p>
<p id="result" role="status">ready</p><p id="identity">anonymous</p>
<p id="fixture-diagnostic">not-submitted</p>
<p id="storage">storage-loading</p><p id="sw">sw-loading</p>
<label for="fromDate">开始日期</label><input id="fromDate" type="date" value="2026-11-03">
<button id="filter">筛选报表</button><p id="report">report-empty</p>
<a id="download" href="/download/report.csv" download>下载报表</a>
<button class="duplicate">重复按钮A</button><button class="duplicate">重复按钮B</button>
<button id="disabled" disabled>禁用按钮</button><button id="hidden" hidden>隐藏按钮</button>
<input id="readonly" readonly value="immutable">
<button id="hang-submit">测试提交后丢失回执</button>
<button id="replace">替换DOM按钮</button><div id="replace-zone"><button id="old-node">旧节点</button></div>
<div id="open-shadow"></div><div id="closed-shadow"></div>
<iframe title="同源框架" srcdoc="<button id='same-frame-action'>框架按钮</button>"></iframe>
<iframe title="跨站框架" src="{CROSS_ORIGIN}/frame"></iframe>
<script src="/fixture.js"></script></html>"""


# 用途：手动启动夹具供桌面调试，显式回环地址；日期：2026-11-03。
def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=3000)
    args = parser.parse_args()
    server = FixtureServer(args.port)
    print(f"Fixture: http://127.0.0.1:{server.server_port}/login", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
