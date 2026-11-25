"""仅允许指定测试域名及端口的SOCKS5夹具，不接受任意外网出口。日期：2026-11-04。"""

import select
import socket
import socketserver
import struct
import threading


# 用途：接收固定字节数，协议截断立即失败；日期：2026-11-04。
def receive(stream, length):
    result = b""
    while len(result) < length:
        chunk = stream.recv(length - len(result))
        if not chunk:
            raise ConnectionError("Truncated SOCKS request")
        result += chunk
    return result


class SocksServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = False
    daemon_threads = True

    # 用途：绑定本机随机端口，唯一远端映射为本轮夹具；日期：2026-11-04。
    def __init__(self, target_port):
        super().__init__(("127.0.0.1", 0), SocksHandler)
        self.target_port = target_port
        self.forwarded = []
        self.lock = threading.Lock()
        self.streams = set()
        self.disconnected = False

    # 用途：断开既有隧道及拒绝新会话，避免只关监听端口而继续转发；日期：2026-11-04。
    def disconnect(self):
        with self.lock:
            self.disconnected = True
            streams = list(self.streams)
        for stream in streams:
            try:
                stream.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class SocksHandler(socketserver.BaseRequestHandler):
    # 用途：完成真实SOCKS5握手并双向转发，仅接受固定域名和目标端口；日期：2026-11-04。
    def handle(self):
        downstream = self.request
        downstream.settimeout(5)
        upstream = None
        try:
            with self.server.lock:
                if self.server.disconnected:
                    return
                self.server.streams.add(downstream)
            version, count = receive(downstream, 2)
            methods = receive(downstream, count)
            if version != 5 or 0 not in methods:
                downstream.sendall(b"\x05\xff")
                return
            downstream.sendall(b"\x05\x00")
            version, command, reserved, kind = receive(downstream, 4)
            if kind == 3:
                name = receive(downstream, receive(downstream, 1)[0]).decode("ascii")
            elif kind == 1:
                name = socket.inet_ntoa(receive(downstream, 4))
            elif kind == 4:
                name = socket.inet_ntop(socket.AF_INET6, receive(downstream, 16))
            else:
                return
            port = struct.unpack("!H", receive(downstream, 2))[0]
            if version != 5 or command != 1 or reserved != 0 or name != "awb-fixture.invalid" or port != self.server.target_port:
                downstream.sendall(b"\x05\x02\x00\x01" + b"\x00" * 6)
                return
            upstream = socket.create_connection(("127.0.0.1", port), timeout=5)
            with self.server.lock:
                if self.server.disconnected:
                    return
                self.server.streams.add(upstream)
                self.server.forwarded.append({"host": name, "port": port, "addressType": kind})
            downstream.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
            while True:
                ready, _, _ = select.select([downstream, upstream], [], [], 5)
                if not ready:
                    return
                for source in ready:
                    data = source.recv(65536)
                    if not data:
                        return
                    (upstream if source is downstream else downstream).sendall(data)
        except (OSError, UnicodeError):
            return
        finally:
            with self.server.lock:
                self.server.streams.discard(downstream)
                self.server.streams.discard(upstream)
            if upstream:
                upstream.close()
