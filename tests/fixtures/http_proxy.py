"""只转发到指定本地夹具的HTTP代理，用于浏览器真实出口验证。日期：2026-11-03。"""

import http.client
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


class ProxyServer(ThreadingHTTPServer):
    # 用途：绑定随机回环端口并限制唯一目标，禁止成为开放代理；日期：2026-11-03。
    def __init__(self, target_port):
        super().__init__(("127.0.0.1", 0), ProxyHandler)
        self.target_port = target_port
        self.forwarded = []
        self.lock = threading.Lock()


class ProxyHandler(BaseHTTPRequestHandler):
    # 用途：代理测试不打印URL查询或请求敏感字段；日期：2026-11-03。
    def log_message(self, _format, *_args):
        return

    # 用途：只转发到本测试HTTP夹具，记录事实后返回完整响应；日期：2026-11-03。
    def do_GET(self):
        target = urlsplit(self.path)
        if (target.scheme != "http" or target.hostname != "127.0.0.1" or
                target.port != self.server.target_port or target.username or target.password):
            self.send_error(403, "Test proxy destination denied")
            return
        connection = http.client.HTTPConnection("127.0.0.1", self.server.target_port, timeout=3)
        try:
            connection.request("GET", target.path or "/")
            response = connection.getresponse()
            body = response.read(1024 * 1024)
            with self.server.lock:
                self.server.forwarded.append({"path": target.path, "status": response.status})
            self.send_response(response.status)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Type", response.getheader("Content-Type", "text/plain"))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        finally:
            connection.close()

    # 用途：拒绝隧道，避免把只测HTTP误报为HTTPS代理能力；日期：2026-11-03。
    def do_CONNECT(self):
        self.send_error(501, "CONNECT is outside this fixture")
