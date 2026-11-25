"""Python标准库薄客户端：鉴权、有限等待、保留未知结果，绝不自动重试。日期：2026-11-05。"""

import json
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


class NoRedirect(HTTPRedirectHandler):
    # 用途：拒绝全部重定向，Bearer令牌不转发到其它端点；日期：2026-11-05。
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise AwbError("CLIENT_REDIRECT_DENIED", "API redirect is refused", True)


class AwbError(RuntimeError):
    # 用途：保留业务错误码和未知结果，调用方据此核对而不是重放；日期：2026-11-05。
    def __init__(self, code, message, outcome_unknown=False):
        super().__init__(message)
        self.code = code
        self.outcome_unknown = outcome_unknown


class Client:
    # 用途：固定连接本机鉴权端点，禁用系统代理且不在异常中输出令牌；日期：2026-11-05。
    def __init__(self, url, token, timeout=35):
        endpoint = urlsplit(url)
        if endpoint.scheme != "http" or endpoint.hostname != "127.0.0.1" or not endpoint.port or endpoint.path or endpoint.query or endpoint.fragment or endpoint.username:
            raise ValueError("Expected a loopback API URL from local-api.json")
        if not 1 <= timeout <= 120 or not isinstance(token, str) or not token or any(c in token for c in "\r\n"):
            raise ValueError("Invalid timeout or token")
        self.url, self._token, self.timeout = url, token, timeout
        self._opener = build_opener(ProxyHandler({}), NoRedirect())
        self.contract = json.loads(Path(__file__).with_name("contract_v1.json").read_text("utf-8"))
        self.tools = {tool["name"]: tool for tool in self.contract["tools"]}

    # 用途：只从用户显式指定的配置文件加载每次启动轮换的令牌；日期：2026-11-05。
    @classmethod
    def from_file(cls, path, timeout=35):
        config = json.loads(Path(path).read_text("utf-8"))
        if config["schemaVersion"] != 1:
            raise ValueError("Unsupported endpoint file")
        return cls(config["url"], config["token"], timeout)

    # 用途：发送一次请求，保留错误码且连接失败一律禁止盲重试；日期：2026-11-05。
    def _request(self, path, arguments=None):
        body = None if arguments is None else json.dumps(arguments, ensure_ascii=False).encode("utf-8")
        request = Request(self.url + path, data=body, method="GET" if body is None else "POST",
                          headers={"Authorization": "Bearer " + self._token, "Content-Type": "application/json"})
        try:
            with self._opener.open(request, timeout=self.timeout) as response:
                raw = response.read(16 * 1024 * 1024 + 1)
        except HTTPError as error:
            raw = error.read(16 * 1024 * 1024 + 1)
        except (URLError, TimeoutError, OSError):
            raise AwbError("TRANSPORT_OUTCOME_UNKNOWN", "Connection failed; inspect run/page state before any retry", True) from None
        if len(raw) > 16 * 1024 * 1024:
            raise AwbError("CLIENT_RESPONSE_BUDGET", "Response exceeds sixteen MiB", True)
        try:
            answer = json.loads(raw)
        except ValueError:
            raise AwbError("CLIENT_INVALID_RESPONSE", "Server response is not JSON", True) from None
        if not answer.get("ok"):
            error = answer.get("error", {})
            raise AwbError(error.get("code", "CLIENT_INVALID_RESPONSE"), error.get("message", "Request failed"),
                           error.get("outcomeUnknown", False))
        return answer["result"]

    # 用途：查询实际能力并校验服务器包含当前客户端需要的所有工具；日期：2026-11-05。
    def connect(self):
        actual = self._request("/v1/tools")["tools"]
        if {tool["name"]: tool["inputSchema"] for tool in actual} != {name: tool["inputSchema"] for name, tool in self.tools.items()}:
            raise AwbError("CLIENT_CONTRACT_MISMATCH", "Server tool schemas differ from this client contract")
        return self._request("/v1/health")

    # 用途：仅调用固定注册工具，参数由同一C++服务最终验证；日期：2026-11-05。
    def call(self, tool, arguments=None):
        if tool not in self.tools:
            raise AwbError("CLIENT_UNKNOWN_TOOL", "Tool is absent from the bound contract")
        if arguments is not None and not isinstance(arguments, dict):
            raise ValueError("Tool arguments must be an object")
        return self._request("/v1/tools/" + tool, arguments or {})
