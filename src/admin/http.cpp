#include "awb/backend_extensions.hpp"
#include "awb/browser.hpp"
#include "awb/control_plane.hpp"
#include "portal.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
#include <thread>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace awb
{
namespace
{
constexpr size_t MaximumHeader = 16384;
constexpr size_t MaximumBody = 1024 * 1024;
std::atomic<HANDLE> shutdown_event{nullptr};
// 用途：控制台关闭信号仅设置退出事件，由主循环完成释放；日期：2026-11-04。
BOOL WINAPI ConsoleStop(DWORD signal)
{
    const auto event = shutdown_event.load();
    if (event && (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT))
    {
        SetEvent(event);
        return TRUE;
    }
    return FALSE;
}
class ShutdownGuard
{
  public:
    // 用途：将本次服务的退出事件登记到控制台处理器；日期：2026-11-04。
    explicit ShutdownGuard(HANDLE event)
    {
        shutdown_event.store(event);
        if (!SetConsoleCtrlHandler(ConsoleStop, TRUE))
        {
            shutdown_event.store(nullptr);
            throw Error("API_START_FAILED", "Cannot install local shutdown handler");
        }
    }
    // 用途：撤销处理器后释放全局引用，防止引用已关闭事件；日期：2026-11-04。
    ~ShutdownGuard()
    {
        SetConsoleCtrlHandler(ConsoleStop, FALSE);
        shutdown_event.store(nullptr);
    }
};
class Socket
{
  public:
    // 用途：接管唯一套接字，确保异常路径释放本地端点；日期：2026-11-04。
    explicit Socket(SOCKET socket) : value(socket)
    {
    }
    // 用途：关闭本对象持有的套接字；日期：2026-11-04。
    ~Socket()
    {
        if (value != INVALID_SOCKET)
            closesocket(value);
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    SOCKET value;
};

class Network
{
  public:
    // 用途：初始化Windows套接字库，不申请系统HTTP URL保留或管理员权限；日期：2026-11-04。
    Network()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            throw Error("API_BIND_FAILED", "Cannot initialize local sockets");
    }
    // 用途：释放本次套接字库引用；日期：2026-11-04。
    ~Network()
    {
        WSACleanup();
    }
};

struct Request
{
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

// 用途：仅将ASCII标头名称转换为小写；日期：2026-11-04。
std::string Lower(std::string text)
{
    for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
// 用途：读取严格十进制有界数，拒绝符号、空白、截断和整数溢出；日期：2026-11-04。
unsigned long long Decimal(const std::string &text, unsigned long long maximum)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw Error("INVALID_REQUEST", "Expected a bounded decimal integer");
    unsigned long long value = 0;
    for (const auto c : text)
    {
        const auto digit = static_cast<unsigned long long>(c - '0');
        if (digit > maximum || value > (maximum - digit) / 10)
            throw Error("INPUT_TOO_LARGE", "Integer exceeds request budget");
        value = value * 10 + digit;
    }
    return value;
}
// 用途：有界接收一个非管线HTTP请求，拒绝重复标头和不明确的消息长度；日期：2026-11-04。
Request Receive(SOCKET socket)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string bytes;
    char buffer[4096];
    size_t separator = std::string::npos;
    while ((separator = bytes.find("\r\n\r\n")) == std::string::npos)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error("INVALID_REQUEST", "HTTP request deadline exceeded");
        const int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0)
            throw Error("INVALID_REQUEST", "HTTP header was truncated or timed out");
        bytes.append(buffer, static_cast<size_t>(count));
        if (bytes.size() > MaximumHeader + sizeof(buffer))
            throw Error("INPUT_TOO_LARGE", "HTTP headers exceed budget");
    }
    if (separator > MaximumHeader)
        throw Error("INPUT_TOO_LARGE", "HTTP headers exceed budget");
    const auto head = bytes.substr(0, separator);
    if (head.find('\0') != std::string::npos)
        throw Error("INVALID_REQUEST", "NUL is forbidden in HTTP headers");
    const auto line_end = head.find("\r\n");
    const auto first = head.substr(0, line_end);
    const auto method_end = first.find(' ');
    const auto path_end = first.find(' ', method_end == std::string::npos ? first.size() : method_end + 1);
    if (method_end == std::string::npos || path_end == std::string::npos ||
        first.substr(path_end + 1) != "HTTP/1.1")
        throw Error("INVALID_REQUEST", "Expected an HTTP/1.1 request line");
    Request request{
        first.substr(0, method_end), first.substr(method_end + 1, path_end - method_end - 1), {}, {}};
    if (request.path.empty() || request.path[0] != '/' ||
        request.path.find_first_of("\r\n\t #") != std::string::npos)
        throw Error("INVALID_REQUEST", "Only origin-form API paths are supported");
    size_t offset = line_end == std::string::npos ? head.size() : line_end + 2;
    while (offset < head.size())
    {
        const auto end = head.find("\r\n", offset);
        const auto line = head.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0 ||
            line.substr(0, colon).find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos)
            throw Error("INVALID_REQUEST", "Invalid HTTP header name");
        const auto name = Lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        const auto start = value.find_first_not_of(" \t");
        value =
            start == std::string::npos ? "" : value.substr(start, value.find_last_not_of(" \t") - start + 1);
        for (const auto c : value)
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) == 127)
                throw Error("INVALID_REQUEST", "Invalid HTTP header value");
        if (!request.headers.emplace(name, value).second)
            throw Error("INVALID_REQUEST", "Duplicate HTTP headers are rejected");
        offset = end == std::string::npos ? head.size() : end + 2;
    }
    if (request.headers.count("transfer-encoding") || request.headers.count("content-encoding") ||
        request.headers.count("expect"))
        throw Error("INVALID_REQUEST", "Transfer encodings, compression and Expect are unsupported");
    const auto length = request.headers.count("content-length")
                            ? static_cast<size_t>(Decimal(request.headers.at("content-length"), MaximumBody))
                            : 0;
    request.body = bytes.substr(separator + 4);
    while (request.body.size() < length)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error("INVALID_REQUEST", "HTTP request deadline exceeded");
        const int count =
            recv(socket, buffer, static_cast<int>(std::min(sizeof(buffer), length - request.body.size())), 0);
        if (count <= 0)
            throw Error("INVALID_REQUEST", "HTTP body was truncated or timed out");
        request.body.append(buffer, static_cast<size_t>(count));
    }
    if (request.body.size() != length)
        throw Error("INVALID_REQUEST", "Pipelined or extra request bytes are rejected");
    return request;
}
// 用途：固定长度比较本地Bearer令牌，回复或日志不包含秘密；日期：2026-11-04。
bool Authorized(const std::string &supplied, const std::string &expected)
{
    if (supplied.size() != expected.size())
        return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(supplied[i] ^ expected[i]);
    return difference == 0;
}
// 用途：有界重试被端点读取或扫描短暂阻塞的原子发布，仅重写相同元数据；日期：2026-11-05。
void PublishEndpoint(const std::wstring &path, const std::string &bytes)
{
    for (size_t attempt = 0;; ++attempt)
    {
        try
        {
            AtomicWrite(path, bytes);
            return;
        }
        catch (const Error &error)
        {
            const std::string message = error.what();
            const bool contention = error.code == "WINDOWS_ERROR" && (message == "MoveFileEx failed: 5" ||
                                                                      message == "MoveFileEx failed: 32" ||
                                                                      message == "MoveFileEx failed: 33");
            if (!contention || attempt >= 40)
                throw;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
// 用途：写完整HTTP响应并设置同源、禁止缓存和脚本策略；日期：2026-11-05。
void Reply(SOCKET socket, int status, const std::string &type, const std::string &body,
           const std::string &extra = "")
{
    const auto bytes =
        "HTTP/1.1 " + std::to_string(status) + " Result\r\nContent-Type: " + type +
        "; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "object-src 'none'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n"
        "Referrer-Policy: no-referrer\r\nConnection: close\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const int count = send(socket, bytes.data() + offset,
                               static_cast<int>((std::min)(bytes.size() - offset, size_t(65536))), 0);
        if (count <= 0)
            return;
        offset += static_cast<size_t>(count);
    }
}
// 用途：统一转换鉴权、输入和业务冲突的HTTP状态；日期：2026-11-05。
int Status(const std::exception &error)
{
    const auto *typed = dynamic_cast<const Error *>(&error);
    if (!typed)
        return dynamic_cast<const Json::exception *>(&error) ? 400 : 500;
    if (typed->code == "UNAUTHORIZED")
        return 401;
    if (typed->code == "FORBIDDEN" || typed->code == "PAYMENT_SIGNATURE")
        return 403;
    if (typed->code == "NOT_FOUND")
        return 404;
    if (typed->code == "INVALID_ARGUMENTS" || typed->code == "INVALID_REQUEST")
        return 400;
    return 409;
}
// 用途：读取唯一Cookie并拒绝重复键，Cookie只能来自HttpOnly服务器路径；日期：2026-11-05。
std::string Cookie(const Request &request, const std::string &name)
{
    const auto it = request.headers.find("cookie");
    if (it == request.headers.end())
        return "";
    std::string result;
    size_t start = 0;
    while (start < it->second.size())
    {
        const auto end = it->second.find(';', start);
        auto part = it->second.substr(start, end == std::string::npos ? end : end - start);
        const auto trim = part.find_first_not_of(' ');
        if (trim != std::string::npos)
            part.erase(0, trim);
        if (part.find(name + "=") == 0)
        {
            if (!result.empty())
                throw Error("INVALID_REQUEST", "Duplicate session cookie");
            result = part.substr(name.size() + 1);
        }
        start = end == std::string::npos ? it->second.size() : end + 1;
    }
    return result;
}
// 用途：生成固定Host作用域的HttpOnly Cookie，正式入口要求Secure；日期：2026-11-05。
std::string SetCookie(const std::string &name, const std::string &value, int seconds, bool secure)
{
    return "Set-Cookie: " + name + "=" + value +
           "; Path=/; HttpOnly; SameSite=Lax; Max-Age=" + std::to_string(seconds) +
           (secure ? "; Secure" : "") + "\r\n";
}
// 用途：处理已限流的单个HTTP连接，网关回调只接受签名正文，门户只接受同源JSON；日期：2026-11-05。
void Serve(SOCKET client, ControlPlane &plane, const std::string &host, HANDLE stop,
           const std::string &shutdown_token, const std::string &public_origin, const Json &server_settings)
{
    Socket connection(client);
    DWORD timeout = 1500;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
    try
    {
        const auto request = Receive(client);
        const auto supplied_host = request.headers.count("host") ? request.headers.at("host") : "";
        const bool public_request = !public_origin.empty() && supplied_host == public_origin.substr(8);
        if (supplied_host != host && !public_request)
            throw Error("FORBIDDEN", "Host is not the configured control-plane endpoint");
        const auto origin = public_request ? public_origin : "http://" + host;
        if (request.headers.count("origin") && request.headers.at("origin") != origin)
            throw Error("FORBIDDEN", "Cross-origin requests are denied");
        if (request.method == "GET" && request.body.empty() &&
            (request.path == "/" || request.path == "/?sso=success" || request.path == "/portal.js"))
        {
            Reply(client, 200, request.path != "/portal.js" ? "text/html" : "text/javascript",
                  request.path != "/portal.js" ? PortalHtml : PortalJs);
            return;
        }
        if (request.method == "GET" && request.path.find("/auth/callback?") == 0)
        {
            const auto parameters = DecodeForm(request.path.substr(15));
            const auto state = parameters.count("state") ? parameters.at("state") : "";
            if (state.size() != 64 || state.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw Error("OIDC_STATE", "Invalid callback state");
            const auto config = plane.Execute("status", Json::object()).at("oidc");
            if (ProtocolOrigin(config.value("redirectUri", "")) != origin)
                throw Error("FORBIDDEN", "Callback arrived at the wrong configured origin");
            const auto result =
                plane.Execute("oidc.callback",
                              {{"state", state},
                               {"code", parameters.count("code") ? parameters.at("code") : "provider-denied"},
                               {"browserBinding", Cookie(request, "awb-flow-" + state)}});
            Reply(client, 303, "text/plain", "Signed in",
                  "Location: /?sso=success\r\n" +
                      SetCookie("awb-session", result.at("token"), 8 * 3600, public_request) +
                      SetCookie("awb-flow-" + state, "", 0, public_request));
            return;
        }
        for (const auto *channel : {"alipay", "wechat", "stripe"})
            if (request.method == "POST" && request.path == std::string("/payments/") + channel + "/notify")
            {
                plane.OfficialWebhook(channel, request.body, request.headers);
                Reply(client, channel == std::string("wechat") ? 204 : 200,
                      channel == std::string("alipay") ? "text/plain" : "application/json",
                      channel == std::string("wechat")   ? ""
                      : channel == std::string("alipay") ? "success"
                                                         : "{\"received\":true}");
                return;
            }
        if (request.method != "POST" || request.path.find("/api/") != 0 ||
            !request.headers.count("content-type") ||
            (request.headers.at("content-type") != "application/json" &&
             request.headers.at("content-type") != "application/json; charset=utf-8"))
            throw Error("INVALID_REQUEST", "Expected POST /api/action with JSON");
        const auto bearer = request.headers.find("authorization");
        auto token = bearer != request.headers.end() && bearer->second.find("Bearer ") == 0
                         ? bearer->second.substr(7)
                         : "";
        if (token.empty())
        {
            token = Cookie(request, "awb-session");
            if (!token.empty() &&
                (!request.headers.count("origin") || request.headers.at("origin") != origin))
                throw Error("FORBIDDEN", "Cookie requests require the exact same origin");
        }
        const auto action = request.path.substr(5);
        // 用途：仅执行节点出站协议要求mTLS代理证明，门户的注册审批等管理动作沿用账号判权；日期：2026-11-05。
        const bool node_protocol = action == "node.enroll" || action == "node.open" ||
                                   action == "node.poll" || action == "node.ack" ||
                                   action == "node.complete" || action == "node.event";
        if (node_protocol && server_settings.value("requireNodeTransportProof", false))
        {
            const auto proof = request.headers.count("x-awb-node-transport")
                                   ? request.headers.at("x-awb-node-transport")
                                   : "";
            if (!public_request || !SecureEqual(proof, server_settings.at("nodeTransportSecret")))
                throw Error("NODE_TRANSPORT", "Verified node mTLS proxy transport is required");
        }
        if (action == "oidc.callback")
            throw Error("FORBIDDEN", "Use the bound browser callback route");
        Json result;
        std::string extra;
        if (action == "shutdown")
        {
            if (!Authorized(token, shutdown_token))
                throw Error("UNAUTHORIZED", "Private shutdown token required");
            SetEvent(stop);
            result = {{"stopping", true}};
        }
        else if (action == "webhook")
        {
            result = plane.Webhook(
                request.body,
                request.headers.count("x-awb-timestamp") ? request.headers.at("x-awb-timestamp") : "",
                request.headers.count("x-awb-signature") ? request.headers.at("x-awb-signature") : "");
        }
        else
            result = plane.Execute(action, ParseExternalJson(request.body), token);
        if (action == "oidc.begin")
        {
            const auto state = result.at("state").get<std::string>();
            const auto configured = plane.Execute("status", Json::object()).at("oidc");
            if (ProtocolOrigin(configured.value("redirectUri", "")) != origin)
                throw Error("OIDC_CONFIG", "Open the portal at the configured redirect origin");
            extra += SetCookie("awb-flow-" + state, result.at("browserBinding"), 300, public_request);
            result.erase("browserBinding");
        }
        if (action == "login" || action == "bootstrap" || action == "register")
            extra += SetCookie("awb-session", "", 0, public_request);
        if (action == "logout" || action == "oidc.logout" || action == "oidc.end-session")
            extra += SetCookie("awb-session", "", 0, public_request);
        if (result.is_object() && result.value("authenticationFailed", false))
            throw Error("UNAUTHORIZED", result.at("message"));
        Reply(client, 200, "application/json", Json{{"ok", true}, {"result", result}}.dump(), extra);
    }
    catch (const std::exception &error)
    {
        Reply(client, Status(error), "application/json", ErrorResult(error).dump());
    }
}
} // namespace

// 用途：四个有界工作线程共享事务存储，回环监听配合正式HTTPS反向代理部署；日期：2026-11-05。
int RunAdmin(const std::wstring &root, unsigned short port)
{
    ControlPlane plane(root);
    const auto server_config = SafeAbsolute(root + L"\\server-config.json");
    if (GetFileAttributesW(server_config.c_str()) == INVALID_FILE_ATTRIBUTES)
        AtomicWrite(
            server_config,
            "{\"publicOrigin\":\"\",\"requireNodeTransportProof\":false,\"nodeTransportSecret\":\"\"}\n");
    const auto server_settings = ParseExternalJson(ReadFileBounded(server_config, 16384));
    const auto configured_origin = server_settings.value("publicOrigin", "");
    const auto public_origin = configured_origin.empty() ? "" : ProtocolOrigin(configured_origin);
    if (server_settings.value("requireNodeTransportProof", false) &&
        (public_origin.empty() || Field(server_settings, "nodeTransportSecret", 256).size() < 32))
        throw Error("SERVER_CONFIG",
                    "Node mTLS proxy mode requires publicOrigin and a private 32-byte transport secret");
    if (!public_origin.empty() &&
        (public_origin.find("https://") != 0 ||
         (configured_origin != public_origin && configured_origin != Browser::Origin(configured_origin))))
        throw Error("SERVER_CONFIG", "publicOrigin must be an exact HTTPS origin without path");
    Network network;
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    BOOL exclusive = TRUE;
    if (listener.value == INVALID_SOCKET ||
        setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)))
        throw Error("API_BIND_FAILED", "Cannot create admin listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        listen(listener.value, 32))
        throw Error("API_BIND_FAILED", "Admin port is unavailable");
    int size = sizeof(address);
    if (getsockname(listener.value, reinterpret_cast<sockaddr *>(&address), &size))
        throw Error("API_BIND_FAILED", "Cannot inspect admin port");
    const auto host = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const auto shutdown_token = RandomId() + RandomId();
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop.get())
        throw Error("API_START_FAILED", "Cannot create admin stop event");
    ShutdownGuard shutdown(stop.get());
    PublishEndpoint(
        SafeAbsolute(root + L"\\admin-endpoint.json"),
        Json{{"url", "http://" + host}, {"pid", GetCurrentProcessId()}, {"shutdownToken", shutdown_token}}
                .dump(2) +
            "\n");
    std::cout << Json{{"ready", true}, {"url", "http://" + host}, {"root", Utf8(root)}}.dump() << std::endl;
    std::vector<std::thread> workers;
    u_long nonblocking = 1;
    if (ioctlsocket(listener.value, FIONBIO, &nonblocking))
        throw Error("API_START_FAILED", "Cannot configure nonblocking admin listener");
    for (int i = 0; i < 4; ++i)
        workers.emplace_back(
            [&]
            {
                while (WaitForSingleObject(stop.get(), 0) != WAIT_OBJECT_0)
                {
                    fd_set ready;
                    FD_ZERO(&ready);
                    FD_SET(listener.value, &ready);
                    timeval wait{0, 200000};
                    if (select(0, &ready, nullptr, nullptr, &wait) <= 0)
                        continue;
                    // 多个工作线程同时发现可读时，非阻塞accept避免退出时挂起。
                    const auto client = accept(listener.value, nullptr, nullptr);
                    if (client != INVALID_SOCKET)
                    {
                        u_long blocking = 0;
                        ioctlsocket(client, FIONBIO, &blocking);
                        Serve(client, plane, host, stop.get(), shutdown_token, public_origin,
                              server_settings);
                    }
                }
            });
    // accept使用非阻塞监听，连接本身恢复阻塞以应用收发期限。
    for (auto &worker : workers)
        worker.join();
    return 0;
}
} // namespace awb
