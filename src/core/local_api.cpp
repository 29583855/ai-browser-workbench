#include "awb/local_api.hpp"
#include "awb/service.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <limits>
#include <map>
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
// 用途：写入不含正文/令牌/URL的有界审计日志，刷盘失败禁止假称已记录；日期：2026-11-04。
void Audit(const std::wstring &root, const std::string &request_id, const std::string &phase,
           const std::string &code)
{
    const auto path = SafeAbsolute(root + L"\\local-api-audit.jsonl");
    Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
        !GetFileSizeEx(file.get(), &length) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw Error("AUDIT_WRITE_FAILED", "Cannot open local API audit file", phase == "completed");
    if (length.QuadPart >= 8 * 1024 * 1024)
    {
        file.reset();
        for (int index = 2; index >= 0; --index)
        {
            const auto source = SafeAbsolute(path + (index == 0 ? L"" : L"." + std::to_wstring(index)));
            const auto destination = SafeAbsolute(path + L"." + std::to_wstring(index + 1));
            if (!MoveFileExW(source.c_str(), destination.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) &&
                GetLastError() != ERROR_FILE_NOT_FOUND)
                throw Error("AUDIT_WRITE_FAILED", "Cannot rotate bounded API audit log",
                            phase == "completed");
        }
        file.reset(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE)
            throw Error("AUDIT_WRITE_FAILED", "Cannot create rotated API audit log", phase == "completed");
    }
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    const unsigned long long timestamp =
        (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    const auto line =
        Json{{"requestId", request_id}, {"phase", phase}, {"code", code}, {"recordedFileTime", timestamp}}
            .dump() +
        "\n";
    DWORD written = 0;
    if (!WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
        written != line.size() || !FlushFileBuffers(file.get()))
        throw Error("AUDIT_WRITE_FAILED", "Cannot durably write local API audit log", phase == "completed");
}
// 用途：路由到共享契约，PATCH/生命周期要求If-Match而不是静默覆盖；日期：2026-11-04。
Json Route(Service &service, const Request &request)
{
    if (request.method == "GET")
    {
        if (!request.body.empty())
            throw Error("INVALID_REQUEST", "GET requests cannot contain a body");
        if (request.path == "/v1/health")
            return service.Execute("capabilities", Json::object());
        if (request.path == "/v1/tools")
            return {{"tools", Service::Tools()}};
        if (request.path == "/v1/profiles")
            return service.Execute("profile_list", Json::object());
        const std::string prefix = "/v1/profiles?state=";
        if (request.path.find(prefix) == 0)
            return service.Execute("profile_list", {{"state", request.path.substr(prefix.size())}});
        throw Error("API_NOT_FOUND", "API route is not registered");
    }
    if (request.method != "POST" && request.method != "PATCH")
        throw Error("API_METHOD_NOT_ALLOWED", "API supports GET, POST and PATCH");
    const auto type = request.headers.find("content-type");
    if (type == request.headers.end() ||
        (type->second != "application/json" && type->second != "application/json; charset=utf-8"))
        throw Error("API_CONTENT_TYPE", "Send application/json encoded as UTF-8");
    const auto body = ParseExternalJson(request.body);
    if (!body.is_object())
        throw Error("INVALID_ARGUMENTS", "API body must be an object");
    if (request.method == "POST" && request.path == "/v1/profiles")
        return service.Execute("profile_create", body);
    const std::string tool_prefix = "/v1/tools/";
    if (request.method == "POST" && request.path.find(tool_prefix) == 0)
        return service.Execute(request.path.substr(tool_prefix.size()), body);
    const std::string profile_prefix = "/v1/profiles/";
    if (request.path.find(profile_prefix) == 0)
    {
        const auto resource = request.path.substr(profile_prefix.size());
        const auto slash = resource.find('/');
        const auto id = resource.substr(0, slash);
        if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw Error("API_NOT_FOUND", "Unknown profile path");
        const auto match = request.headers.find("if-match");
        if (match == request.headers.end())
            throw Error("API_PRECONDITION_REQUIRED", "Send If-Match with the quoted profile revision");
        const auto revision = match->second;
        if (revision.size() < 3 || revision.front() != '"' || revision.back() != '"')
            throw Error("INVALID_REQUEST", "If-Match must contain one quoted revision");
        Json arguments = body;
        if (arguments.contains("profileId") || arguments.contains("expectedRevision"))
            throw Error("INVALID_ARGUMENTS", "Resource and revision must come from path and If-Match");
        arguments["profileId"] = id;
        arguments["expectedRevision"] =
            Decimal(revision.substr(1, revision.size() - 2), (std::numeric_limits<long long>::max)());
        if (request.method == "PATCH" && slash == std::string::npos)
            return service.Execute("profile_update", arguments);
        if (request.method == "POST" && slash != std::string::npos)
        {
            const auto action = resource.substr(slash + 1);
            if (action == "archive" || action == "trash" || action == "restore" || action == "purge")
                return service.Execute("profile_" + action, arguments);
        }
    }
    throw Error("API_NOT_FOUND", "API route is not registered");
}
// 用途：按结构化错误码映射HTTP状态，不能将业务失败返回为成功；日期：2026-11-04。
int StatusCode(const std::exception &error)
{
    const auto *typed = dynamic_cast<const Error *>(&error);
    if (!typed)
        return dynamic_cast<const Json::exception *>(&error) ? 400 : 500;
    const auto &code = typed->code;
    if (code == "API_UNAUTHORIZED")
        return 401;
    if (code == "API_ORIGIN_DENIED" || code == "API_HOST_DENIED")
        return 403;
    if (code == "API_NOT_FOUND" || code == "PROFILE_NOT_FOUND")
        return 404;
    if (code == "API_METHOD_NOT_ALLOWED")
        return 405;
    if (code == "API_CONTENT_TYPE")
        return 415;
    if (code == "API_PRECONDITION_REQUIRED")
        return 428;
    if (code == "INPUT_TOO_LARGE")
        return 413;
    if (code == "REVISION_CONFLICT" || code == "PROFILE_RUNNING" || code == "PROFILE_STATE_INVALID" ||
        code == "LEASE_BUSY" || code == "PROFILE_REFERENCED")
        return 409;
    if (code == "INVALID_ARGUMENTS" || code == "INVALID_REQUEST")
        return 400;
    return 422;
}
// 用途：完整写入JSON响应并关闭连接，不开放CORS或缓存令牌相关结果；日期：2026-11-04。
void Reply(SOCKET socket, int status, const Json &body, const std::string &request_id)
{
    const auto payload = body.dump();
    const auto bytes = "HTTP/1.1 " + std::to_string(status) +
                       " API\r\nContent-Type: application/json; charset=utf-8\r\n"
                       "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n"
                       "X-Request-Id: " +
                       request_id + "\r\nContent-Length: " + std::to_string(payload.size()) + "\r\n\r\n" +
                       payload;
    size_t offset = 0;
    while (offset < bytes.size())
    {
        const int sent = send(socket, bytes.data() + offset,
                              static_cast<int>(std::min<size_t>(bytes.size() - offset, 65536)), 0);
        if (sent <= 0)
            return;
        offset += static_cast<size_t>(sent);
    }
}
} // namespace

// 用途：独占回环端口并轮换启动令牌，端点描述仅存放在用户私有根；日期：2026-11-04。
int RunLocalApi(const std::wstring &root, const std::wstring &browser, unsigned short port)
{
    Service service(root, browser);
    Network network;
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    BOOL exclusive = TRUE;
    if (listener.value == INVALID_SOCKET ||
        setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) != 0)
        throw Error("API_BIND_FAILED", "Cannot create an exclusive local listener");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener.value, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener.value, 16) != 0)
        throw Error("API_BIND_FAILED", "Local API port is unavailable");
    int size = sizeof(address);
    if (getsockname(listener.value, reinterpret_cast<sockaddr *>(&address), &size) != 0)
        throw Error("API_BIND_FAILED", "Cannot inspect bound local endpoint");
    const auto host = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const auto token = RandomId() + RandomId();
    const auto endpoint = SafeAbsolute(root + L"\\local-api.json");
    AtomicWrite(endpoint, Json{{"schemaVersion", 1},
                               {"url", "http://" + host},
                               {"token", token},
                               {"pid", GetCurrentProcessId()},
                               {"tokenRotation", "every-start"},
                               {"browserOriginsAllowed", false}}
                                  .dump(2) +
                              "\n");
    std::cout << Json{{"ready", true}, {"url", "http://" + host}, {"configFile", Utf8(endpoint)}}.dump()
              << std::endl;
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop.get())
        throw Error("API_START_FAILED", "Cannot create local shutdown event");
    ShutdownGuard shutdown(stop.get());
    while (WaitForSingleObject(stop.get(), 0) != WAIT_OBJECT_0)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener.value, &readable);
        timeval poll_timeout{0, 250000};
        const auto selected = select(0, &readable, nullptr, nullptr, &poll_timeout);
        if (selected == 0)
            continue;
        if (selected == SOCKET_ERROR)
            throw Error("API_ACCEPT_FAILED", "Local listener selection failed");
        Socket client(accept(listener.value, nullptr, nullptr));
        if (client.value == INVALID_SOCKET)
            throw Error("API_ACCEPT_FAILED", "Local listener stopped accepting requests");
        DWORD timeout = 5000;
        setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout));
        setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
                   sizeof(timeout));
        const auto request_id = RandomId();
        try
        {
            const auto request = Receive(client.value);
            const auto supplied_host = request.headers.find("host");
            if (supplied_host == request.headers.end() || supplied_host->second != host)
                throw Error("API_HOST_DENIED", "Host must exactly match the owned loopback endpoint");
            if (request.headers.count("origin") || request.headers.count("sec-fetch-site"))
                throw Error("API_ORIGIN_DENIED",
                            "Browser-origin requests are not allowed on the local automation endpoint");
            const auto authorization = request.headers.find("authorization");
            if (authorization == request.headers.end() ||
                !Authorized(authorization->second, "Bearer " + token))
                throw Error("API_UNAUTHORIZED", "A current local Bearer token is required");
            Audit(root, request_id, "accepted", "authorized");
            Json answer;
            if (request.method == "POST" && request.path == "/v1/shutdown")
            {
                if (request.body != "{}" || !request.headers.count("content-type") ||
                    request.headers.at("content-type") != "application/json")
                    throw Error("INVALID_ARGUMENTS", "Shutdown requires an empty application/json object");
                answer = {{"stopping", true}};
                SetEvent(stop.get());
            }
            else
                answer = Route(service, request);
            const auto outcome = answer.value("outcomeUnknown", false)    ? "outcome-unknown"
                                 : answer.value("state", "") == "partial" ? "partial"
                                 : answer.contains("valid") && answer.at("valid") == false
                                     ? "validation-failed"
                                     : "passed";
            Audit(root, request_id, "completed", outcome);
            Reply(client.value, 200, {{"ok", true}, {"requestId", request_id}, {"result", answer}},
                  request_id);
        }
        catch (const std::exception &error)
        {
            auto body = ErrorResult(error);
            body["requestId"] = request_id;
            try
            {
                Audit(root, request_id, "failed", body.at("error").value("code", "INTERNAL_ERROR"));
            }
            catch (const std::exception &)
            {
                body["auditRecorded"] = false;
            }
            Reply(client.value, StatusCode(error), body, request_id);
        }
    }
    DeleteFileW(endpoint.c_str());
    return 0;
}
} // namespace awb
