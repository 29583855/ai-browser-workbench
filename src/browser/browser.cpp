#include "awb/browser.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

namespace awb
{
class SocketDeadline
{
  public:
    // 用途：为同步WebSocket补上总截止时间，超时关闭专用连接；日期：2026-11-03。
    explicit SocketDeadline(InternetHandle &socket) : socket_(socket)
    {
        // 用途：关闭本连接解除阻塞，不重发任何已提交消息；日期：2026-11-03。
        worker_ = std::thread(
            [this]
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 用途：正常完成时提前结束取消线程；日期：2026-11-03。
                if (!condition_.wait_for(lock, std::chrono::seconds(8), [this] { return finished_; }))
                    socket_.reset();
            });
    }
    // 用途：等待取消线程结束后才允许再次访问句柄；日期：2026-11-03。
    ~SocketDeadline()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        condition_.notify_one();
        worker_.join();
    }

  private:
    InternetHandle &socket_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    std::thread worker_;
};
// 用途：检查WinHTTP调用结果并保留错误类型；日期：2026-11-03。
static void HttpCheck(bool success, const char *operation)
{
    if (!success)
        throw Error("TRANSPORT_FAILED", std::string(operation) + ": " + std::to_string(GetLastError()));
}
// 用途：接管WinHTTP句柄；日期：2026-11-03。
InternetHandle::InternetHandle(HINTERNET value) : value_(value)
{
}
// 用途：关闭WinHTTP句柄；日期：2026-11-03。
InternetHandle::~InternetHandle()
{
    reset();
}
// 用途：借用WinHTTP句柄；日期：2026-11-03。
HINTERNET InternetHandle::get() const
{
    return value_;
}
// 用途：替换WinHTTP句柄并释放旧句柄；日期：2026-11-03。
void InternetHandle::reset(HINTERNET value)
{
    if (value_)
        WinHttpCloseHandle(value_);
    value_ = value;
}
// 用途：生成不含用户名口令的来源标识，拒绝非HTTP协议；日期：2026-11-03。
std::string Browser::Origin(const std::string &url)
{
    const auto wide = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength =
        parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (url.find('\0') != std::string::npos || url.size() > 8192 ||
        !WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength)
        throw Error("ORIGIN_DENIED", "Only explicit HTTP(S) origins without credentials are supported");
    std::string host = Utf8(std::wstring(parts.lpszHostName, parts.dwHostNameLength));
    std::transform(host.begin(), host.end(), host.begin(),
                   // 用途：规范ASCII主机名大小写；日期：2026-11-03。
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::string(parts.nScheme == INTERNET_SCHEME_HTTPS ? "https://" : "http://") + host + ":" +
           std::to_string(parts.nPort);
}
// 用途：严格准入无认证HTTP或SOCKS5端点，拒绝凭据、路径和隐式端口；日期：2026-11-04。
void Browser::ValidateProxyServer(const std::string &proxy_server)
{
    if (proxy_server.empty())
        return;
    try
    {
        const auto canonical =
            proxy_server.find("socks5://") == 0 ? "http://" + proxy_server.substr(9) : proxy_server;
        if (canonical.find("http://") == 0 && Origin(canonical) == canonical &&
            canonical.substr(canonical.find_last_of(':') + 1) != "0")
            return;
    }
    catch (const Error &)
    {
        // 非Web端点或含凭据统一按代理契约拒绝。
    }
    throw Error("PROXY_UNSUPPORTED",
                "Use canonical http://host:port or socks5://host:port without credentials");
}
// 用途：在落盘和启动前拒绝未知字段、控制字符和非法语言标记；日期：2026-11-04。
void Browser::ValidateRuntimeSettings(const Json &settings)
{
    if (!settings.is_object())
        throw Error("INVALID_ARGUMENTS", "Runtime settings must be an object");
    for (auto it = settings.begin(); it != settings.end(); ++it)
    {
        const auto &value = it.value();
        if (it.key() == "userAgent" || it.key() == "timezone")
        {
            if (!value.is_string())
                throw Error("INVALID_ARGUMENTS", "Runtime setting must be text");
            const auto text = value.get<std::string>();
            // 用途：拒绝可注入HTTP标头的控制字符；日期：2026-11-04。
            const auto control = [](unsigned char c) { return c < 32 || c == 127; };
            if (text.empty() || text.find('\0') != std::string::npos ||
                UnicodeLength(text) > (it.key() == "userAgent" ? 1024U : 128U) ||
                std::any_of(text.begin(), text.end(), control))
                throw Error("INVALID_ARGUMENTS", "Runtime setting exceeds its text boundary");
            if (it.key() == "timezone" &&
                text.find_first_not_of(
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/-+") !=
                    std::string::npos)
                throw Error("INVALID_ARGUMENTS", "Timezone must be an IANA identifier");
        }
        else if (it.key() == "languages")
        {
            if (!value.is_array() || value.empty() || value.size() > 8)
                throw Error("INVALID_ARGUMENTS", "Languages must contain between one and eight tags");
            std::set<std::string> unique;
            for (const auto &language : value)
            {
                if (!language.is_string())
                    throw Error("INVALID_ARGUMENTS", "Language tags must be text");
                const auto text = language.get<std::string>();
                if (text.size() < 2 || text.size() > 35 || text.front() == '-' || text.back() == '-' ||
                    text.find("--") != std::string::npos ||
                    text.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                        std::string::npos ||
                    !unique.insert(text).second)
                    throw Error("INVALID_ARGUMENTS", "Invalid or duplicate language tag");
            }
        }
        else
            throw Error("INVALID_ARGUMENTS", "Unknown runtime configuration field");
    }
}
// 用途：检查目标是否在显式允许来源集合；日期：2026-11-03。
void Browser::CheckOrigin(const std::string &url) const
{
    if (url == "about:blank")
        return;
    const auto origin = Origin(url);
    if (std::find(origins_.begin(), origins_.end(), origin) == origins_.end())
        throw Error("ORIGIN_DENIED", "Target origin is outside the profile scope");
}
// 用途：使用校验后的存储路径启动自有Job，兼容CacheStorage与ServiceWorker；日期：2026-11-04。
Browser::Browser(const std::wstring &executable, const std::wstring &directory, bool headless,
                 const std::vector<std::string> &origins, const std::string &proxy_server,
                 const Json &runtime_settings)
    : executable_(SafeAbsolute(executable)), runtime_settings_(runtime_settings), origins_(origins)
{
    const auto profile = SafeAbsolute(directory);
    if (InspectRecovery(profile).value("blocked", false))
        throw Error("PROCESS_OWNERSHIP_UNCONFIRMED",
                    "Resolve the recorded process before starting this profile");
    directory_ = profile;
    ownership_file_ = profile + L"\\awb-process-owner.json";
    ValidateProxyServer(proxy_server);
    ValidateRuntimeSettings(runtime_settings);
    storage_path_ = std::make_unique<BrowserDataPath>(profile);
    const auto active_port = profile + L"\\DevToolsActivePort";
    if (!DeleteFileW((L"\\\\?\\" + active_port).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        throw Error("PROFILE_BUSY", "Cannot clear stale discovery file");
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    HttpCheck(job_.get() != nullptr, "CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    HttpCheck(SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits)) != FALSE,
              "SetInformationJobObject");
    std::vector<std::wstring> arguments = {executable_,
                                           L"--user-data-dir=" + storage_path_->get(),
                                           L"--remote-debugging-port=0",
                                           L"--remote-debugging-address=127.0.0.1",
                                           L"--no-first-run",
                                           L"--no-default-browser-check",
                                           L"--disable-sync",
                                           L"--disable-background-networking",
                                           L"--disable-default-apps",
                                           L"--disable-component-update",
                                           L"--disable-features=msEdgeSidebarV2",
                                           L"--noerrdialogs",
                                           L"about:blank"};
    if (headless)
        arguments.insert(arguments.end() - 1, L"--headless=new");
    if (!proxy_server.empty())
    {
        arguments.insert(arguments.end() - 1, L"--proxy-server=" + Wide(proxy_server));
        arguments.insert(arguments.end() - 1, L"--proxy-bypass-list=<-loopback>");
        arguments.insert(arguments.end() - 1, L"--disable-quic");
    }
    std::wstring command;
    for (const auto &argument : arguments)
        command += (command.empty() ? L"" : L" ") + QuoteArgument(argument);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable_.c_str(), &command[0], nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &info))
        throw Error("BROWSER_START_FAILED", "CreateProcess failed: " + std::to_string(GetLastError()));
    process_.reset(info.hProcess);
    Handle thread(info.hThread);
    process_id_ = info.dwProcessId;
    if (!AssignProcessToJobObject(job_.get(), process_.get()))
    {
        TerminateProcess(process_.get(), 1);
        WaitForSingleObject(process_.get(), 3000);
        throw Error("PROCESS_OWNERSHIP_FAILED", "Cannot place browser in the owned Job");
    }
    try
    {
        FILETIME exit{}, kernel{}, user{};
        HttpCheck(GetProcessTimes(process_.get(), &process_created_, &exit, &kernel, &user) != FALSE,
                  "GetProcessTimes");
        const unsigned long long created =
            (static_cast<unsigned long long>(process_created_.dwHighDateTime) << 32) |
            process_created_.dwLowDateTime;
        AtomicWrite(ownership_file_, Json{{"schemaVersion", 1},
                                          {"pid", process_id_},
                                          {"processCreatedFileTime", created},
                                          {"executable", Utf8(executable_)},
                                          {"instanceId", reference_epoch_},
                                          {"servicePid", GetCurrentProcessId()},
                                          {"ownership", "private-job-object"}}
                                             .dump(2) +
                                         "\n");
        HttpCheck(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "ResumeThread");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        std::string discovery;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0)
                throw Error("BROWSER_EXITED", "Browser exited before CDP handshake");
            try
            {
                discovery = ReadFileBounded(active_port, 4096);
                if (discovery.find('\n') != std::string::npos)
                    break;
            }
            catch (const Error &error)
            {
                if (error.code != "FILE_READ_FAILED")
                    throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        Connect(discovery, false);
        const auto target = Call("Target.createTarget", {{"url", "about:blank"}}, false).at("targetId");
        session_ =
            Call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}, false).at("sessionId");
        active_page_ = RandomId();
        PageSession page;
        page.target = target.get<std::string>();
        page.session = session_;
        pages_.emplace(active_page_, page);
        InitializePage();
        pages_.at(active_page_).runtime = runtime_report_;
        Call("Target.setDiscoverTargets", {{"discover", true}}, false);
    }
    catch (...)
    {
        // 构造失败也要确认进程树退出，不能只依赖Job句柄的异步清理。
        TerminateJobObject(job_.get(), 1);
        WaitForSingleObject(process_.get(), 3000);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do
        {
            if (!QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation, &accounting,
                                           sizeof(accounting), nullptr) ||
                accounting.ActiveProcesses == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < cleanup_deadline);
        throw;
    }
}
// 用途：严格读取自有发现数据并重建专用回环WebSocket，不接收任意远端；日期：2026-11-04。
void Browser::Connect(const std::string &discovery, bool controlled)
{
    const auto split = discovery.find('\n');
    if (split == std::string::npos)
        throw Error("BROWSER_START_TIMEOUT", "No CDP discovery within 15 seconds");
    const int port = std::stoi(discovery.substr(0, split));
    std::string endpoint = discovery.substr(split + 1);
    while (!endpoint.empty() && (endpoint.back() == '\n' || endpoint.back() == '\r'))
        endpoint.pop_back();
    if (port < 1 || port > 65535 || endpoint.find("/devtools/browser/") != 0 || endpoint.size() > 1024)
        throw Error("CDP_DISCOVERY_INVALID", "Invalid owned-browser discovery data");
    http_.reset(WinHttpOpen(L"AIBrowserWorkbench/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0));
    HttpCheck(http_.get() != nullptr, "WinHttpOpen");
    HttpCheck(WinHttpSetTimeouts(http_.get(), 3000, 3000, 3000, 5000) != FALSE, "WinHttpSetTimeouts");
    connection_.reset(WinHttpConnect(http_.get(), L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0));
    HttpCheck(connection_.get() != nullptr, "WinHttpConnect");
    InternetHandle request(WinHttpOpenRequest(connection_.get(), L"GET", Wide(endpoint).c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    HttpCheck(request.get() != nullptr, "WinHttpOpenRequest");
    HttpCheck(WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) != FALSE,
              "WebSocket upgrade");
    HttpCheck(WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                 0, 0) != FALSE,
              "WebSocket request");
    HttpCheck(WinHttpReceiveResponse(request.get(), nullptr) != FALSE, "WebSocket response");
    websocket_.reset(WinHttpWebSocketCompleteUpgrade(request.get(), 0));
    HttpCheck(websocket_.get() != nullptr, "WebSocket complete upgrade");
    version_ = Call("Browser.getVersion", Json::object(), false, controlled);
}
// 用途：为当前主目标启用域并应用实际区域设置，不向其他环境传播；日期：2026-11-04。
void Browser::InitializePage()
{
    Call("Page.enable");
    Call("Runtime.enable");
    Call("DOM.enable");
    Call("Accessibility.enable");
    if (diagnostics_active_)
        Call("Network.enable",
             {{"maxTotalBufferSize", 65536}, {"maxResourceBufferSize", 32768}, {"maxPostDataSize", 0}});
    try
    {
        if (runtime_settings_.contains("userAgent") || runtime_settings_.contains("languages"))
        {
            Json override{{"userAgent", runtime_settings_.value(
                                            "userAgent", version_.at("userAgent").get<std::string>())}};
            if (runtime_settings_.contains("languages"))
            {
                std::string languages;
                for (const auto &language : runtime_settings_.at("languages"))
                    languages += (languages.empty() ? "" : ",") + language.get<std::string>();
                override["acceptLanguage"] = languages;
            }
            Call("Network.setUserAgentOverride", override);
        }
        if (runtime_settings_.contains("timezone"))
            Call("Emulation.setTimezoneOverride", {{"timezoneId", runtime_settings_.at("timezone")}});
    }
    catch (const Error &error)
    {
        if (error.code != "CDP_COMMAND_FAILED")
            throw;
        throw Error("RUNTIME_CONFIGURATION_REJECTED", "Browser rejected requested runtime settings");
    }
    const auto actual =
        Call("Runtime.evaluate",
             {{"expression", "({userAgent:navigator.userAgent,languages:Array.from(navigator.languages),"
                             "timezone:Intl.DateTimeFormat().resolvedOptions().timeZone})"},
              {"returnByValue", true}})
            .at("result")
            .at("value");
    runtime_report_ = {{"requested", runtime_settings_},
                       {"effective", actual},
                       {"scope", "primary-target"},
                       {"kernelFingerprintModification", false}};
}
// 用途：关闭本Job进程树，不对其他浏览器进程执行全局终止；日期：2026-11-03。
Browser::~Browser()
{
    websocket_.reset();
    job_.reset();
}
// 用途：发送一条CDP请求并按ID取对应回复，事件不可占用无限内存；日期：2026-11-03。
Json Browser::Call(const std::string &method, const Json &parameters, bool page, bool controlled)
{
    if (stopped_)
        throw Error("BROWSER_NOT_RUNNING", "Browser is stopped");
    if (!websocket_.get())
        throw Error("CDP_DISCONNECTED",
                    "Owned process may still be running; explicitly reconnect or stop it");
    if (page && session_.empty())
        throw Error("PAGE_REQUIRED", "Select or open a managed page before using page actions");
    const int id = ++request_sequence_;
    Json message{{"id", id}, {"method", method}, {"params", parameters}};
    if (page)
        message["sessionId"] = session_;
    auto bytes = message.dump();
    if (bytes.size() > 1024 * 1024)
        throw Error("INPUT_TOO_LARGE", "CDP input exceeds 1 MiB");
    const auto socket = websocket_.get();
    SocketDeadline socket_deadline(websocket_);
    DWORD sent = NO_ERROR;
    // 用途：只在有效租约的派发锁内提交CDP消息，网络收取在锁外；日期：2026-11-03。
    const auto send = [&]()
    {
        sent = WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &bytes[0],
                                    static_cast<DWORD>(bytes.size()));
    };
    if ((page || controlled) && dispatch_gate_)
        dispatch_gate_(send);
    else
        send();
    if (sent != NO_ERROR)
        throw Error("CDP_TRANSPORT_FAILED", "CDP send failed: " + std::to_string(sent), true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string frame;
        for (;;)
        {
            char buffer[32768];
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD result = WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &received, &type);
            if (result != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                throw Error("CDP_RESPONSE_UNKNOWN",
                            "CDP response unavailable; do not retry a submitted action", true);
            frame.append(buffer, received);
            if (frame.size() > 16 * 1024 * 1024)
                throw Error("CDP_RESPONSE_TOO_LARGE", "CDP response exceeds 16 MiB", true);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                break;
            if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                std::chrono::steady_clock::now() >= deadline)
                throw Error("CDP_RESPONSE_INVALID", "Unexpected or expired CDP frame", true);
        }
        const Json response = Json::parse(frame);
        if (!response.contains("id"))
        {
            RecordEvent(response);
            continue;
        }
        if (response["id"] != id)
            continue;
        if (response.contains("error"))
            throw Error("CDP_COMMAND_FAILED", method + ": CDP command rejected");
        return response.value("result", Json::object());
    }
    throw Error("CDP_RESPONSE_UNKNOWN", "CDP deadline exceeded; submitted operations need reconciliation",
                true);
}
// 用途：返回进程指纹及协议事实，不泄露调试端口；日期：2026-11-03。
Json Browser::Status() const
{
    const bool running = !stopped_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
    const unsigned long long created =
        (static_cast<unsigned long long>(process_created_.dwHighDateTime) << 32) |
        process_created_.dwLowDateTime;
    return {{"running", running},
            {"pid", process_id_},
            {"processCreatedFileTime", created},
            {"executable", Utf8(executable_)},
            {"version", version_},
            {"runtime", runtime_report_},
            {"storagePathMode", storage_path_->get() == directory_ ? "canonical" : "private-junction-alias"},
            {"activePageId", active_page_},
            {"managedPageCount", pages_.size()},
            {"cdpConnected", websocket_.get() != nullptr},
            {"ownership", "private-job-object"}};
}
// 用途：重建同一自有进程的连接和会话，任何失败都不重发之前的网页业务；日期：2026-11-04。
Json Browser::Reconnect()
{
    if (stopped_ || WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT)
        throw Error("BROWSER_NOT_RUNNING", "Reconnect requires the same live owned browser process");
    const auto previous_active = active_page_;
    diagnostics_pending_dropped_ += diagnostic_requests_.size();
    diagnostic_requests_.clear();
    diagnostic_contexts_.clear();
    diagnostics_suspended_ = true;
    highlighted_ = false;
    references_.clear();
    session_.clear();
    active_page_.clear();
    runtime_report_ = Json::object();
    websocket_.reset();
    connection_.reset();
    http_.reset();
    for (auto &page : pages_)
        page.second.session.clear();
    try
    {
        Connect(ReadFileBounded(directory_ + L"\\DevToolsActivePort", 4096), true);
        const auto targets = Call("Target.getTargets", Json::object(), false, true).at("targetInfos");
        std::map<std::string, std::string> available;
        for (const auto &target : targets)
            if (target.value("type", "") == "page")
                available.emplace(target.at("targetId").get<std::string>(), target.value("url", ""));
        std::vector<std::string> missing;
        for (auto &page : pages_)
        {
            const auto found = available.find(page.second.target);
            if (found == available.end())
            {
                missing.push_back(page.first);
                continue;
            }
            try
            {
                CheckOrigin(found->second);
            }
            catch (const Error &)
            {
                continue;
            }
            page.second.session = Call("Target.attachToTarget",
                                       {{"targetId", page.second.target}, {"flatten", true}}, false, true)
                                      .at("sessionId")
                                      .get<std::string>();
            session_ = page.second.session;
            InitializePage();
            page.second.runtime = runtime_report_;
        }
        for (const auto &id : missing)
            pages_.erase(id);
        session_.clear();
        if (pages_.count(previous_active) && !pages_.at(previous_active).session.empty())
            active_page_ = previous_active;
        else
            for (const auto &page : pages_)
                if (!page.second.session.empty())
                {
                    active_page_ = page.first;
                    break;
                }
        if (!active_page_.empty())
        {
            session_ = pages_.at(active_page_).session;
            runtime_report_ = pages_.at(active_page_).runtime;
        }
        Call("Target.setDiscoverTargets", {{"discover", true}}, false, true);
    }
    catch (...)
    {
        diagnostics_suspended_ = false;
        if (diagnostics_active_)
            PushDiagnostic({{"kind", "connection-gap"}, {"restored", false}, {"correlation", "unavailable"}});
        websocket_.reset();
        session_.clear();
        active_page_.clear();
        runtime_report_ = Json::object();
        for (auto &page : pages_)
            page.second.session.clear();
        throw Error("CDP_RECONNECT_FAILED", "Connection was not restored; no page action was replayed", true);
    }
    diagnostics_suspended_ = false;
    if (diagnostics_active_)
        PushDiagnostic({{"kind", "connection-gap"}, {"restored", true}, {"correlation", "unavailable"}});
    events_.push_back({{"sequence", ++event_sequence_},
                       {"method", "workbench.connectionReplaced"},
                       {"pageId", active_page_},
                       {"gap", true}});
    while (events_.size() > 256)
        events_.pop_front();
    return {{"reconnected", true},
            {"pid", process_id_},
            {"activePageId", active_page_},
            {"replayedPageActions", 0},
            {"eventContinuity", "gap-during-disconnection"}};
}
// 用途：缓存有界页面及下载元数据，下载记录超限显式拒绝等待而不误配旧结果；日期：2026-11-05。
void Browser::RecordEvent(const Json &message)
{
    if (picker_active_ && message.value("sessionId", "") == session_ &&
        message.value("method", "") == "Overlay.inspectNodeRequested")
        picker_node_ = message.at("params").value("backendNodeId", 0);
    RecordRecording(message);
    RecordDiagnostic(message);
    const auto method = message.value("method", "");
    const auto session = message.value("sessionId", "");
    const auto parameters = message.value("params", Json::object());
    if (!download_directory_.empty() && method == "Browser.downloadWillBegin")
    {
        if (downloads_.size() >= 64)
            download_events_lost_ = true;
        else
        {
            const auto guid = parameters.value("guid", "");
            Json download{{"guid", guid},
                          {"fileName", parameters.value("suggestedFilename", "")},
                          {"state", "inProgress"},
                          {"receivedBytes", 0}};
            try
            {
                CheckOrigin(parameters.at("url"));
            }
            catch (...)
            {
                download["error"] = "DOWNLOAD_ORIGIN_DENIED";
            }
            downloads_[guid] = download;
        }
        return;
    }
    if (!download_directory_.empty() && method == "Browser.downloadProgress")
    {
        const auto found = downloads_.find(parameters.value("guid", ""));
        if (found != downloads_.end())
        {
            found->second["state"] = parameters.value("state", "inProgress");
            found->second["receivedBytes"] = parameters.value("receivedBytes", Json(0));
            if (parameters.contains("filePath"))
                found->second["filePath"] = parameters.at("filePath");
        }
        return;
    }
    std::string page_id;
    for (const auto &page : pages_)
        if ((!session.empty() && page.second.session == session) ||
            (method == "Target.targetDestroyed" && page.second.target == parameters.value("targetId", "")) ||
            (method == "Target.detachedFromTarget" &&
             page.second.session == parameters.value("sessionId", "")))
        {
            page_id = page.first;
            break;
        }
    if (page_id.empty() || (method != "Page.frameNavigated" && method != "Page.frameDetached" &&
                            method != "Page.loadEventFired" && method != "Runtime.executionContextsCleared" &&
                            method != "Target.targetDestroyed" && method != "Target.detachedFromTarget"))
        return;
    Json event{{"sequence", ++event_sequence_}, {"pageId", page_id}, {"method", method}};
    if (method == "Page.frameNavigated")
    {
        const auto frame = parameters.value("frame", Json::object());
        event["frameId"] = frame.value("id", "");
        event["parentFrameId"] = frame.value("parentId", "");
        event["loaderId"] = frame.value("loaderId", "");
    }
    if (method == "Target.targetDestroyed" || method == "Target.detachedFromTarget")
    {
        ForgetDiagnosticSession(pages_.at(page_id).session);
        pages_.at(page_id).session.clear();
        if (active_page_ == page_id)
        {
            session_.clear();
            references_.clear();
        }
    }
    events_.push_back(event);
    while (events_.size() > 256)
        events_.pop_front();
}
// 用途：连接一个确定属于本实例的页面，初始化失败时保留记录供列表核对/关闭；日期：2026-11-04。
std::string Browser::AttachPage(const std::string &target)
{
    if (pages_.size() >= 32)
        throw Error("PAGE_BUDGET_EXCEEDED", "At most 32 managed pages are allowed in this browser");
    const auto id = RandomId();
    PageSession page;
    page.target = target;
    pages_.emplace(id, page);
    const auto previous_session = session_;
    const auto previous_runtime = runtime_report_;
    try
    {
        const auto attached =
            Call("Target.attachToTarget", {{"targetId", target}, {"flatten", true}}, false, true);
        pages_.at(id).session = attached.at("sessionId").get<std::string>();
        session_ = pages_.at(id).session;
        InitializePage();
        pages_.at(id).runtime = runtime_report_;
    }
    catch (...)
    {
        session_ = previous_session;
        runtime_report_ = previous_runtime;
        throw;
    }
    session_ = previous_session;
    runtime_report_ = previous_runtime;
    return id;
}
// 用途：只列已登记目标和其允许来源子弹窗，浏览器其他页面不自动接管；日期：2026-11-04。
Json Browser::Pages()
{
    const auto targets = Call("Target.getTargets", Json::object(), false, true).at("targetInfos");
    std::map<std::string, Json> available;
    for (const auto &target : targets)
        if (target.value("type", "") == "page")
            available.emplace(target.at("targetId").get<std::string>(), target);
    std::vector<std::string> missing;
    for (const auto &page : pages_)
        if (!available.count(page.second.target))
            missing.push_back(page.first);
    for (const auto &id : missing)
    {
        pages_.erase(id);
        if (active_page_ == id)
        {
            highlighted_ = false;
            active_page_.clear();
            session_.clear();
            runtime_report_ = Json::object();
            references_.clear();
        }
    }
    for (const auto &entry : available)
    {
        const auto &target = entry.second;
        bool known = false;
        bool owned_opener = false;
        for (const auto &page : pages_)
        {
            known = known || page.second.target == entry.first;
            owned_opener = owned_opener || page.second.target == target.value("openerId", "");
        }
        if (known || !owned_opener || pages_.size() >= 32)
            continue;
        if (target.value("url", "") == "about:blank")
            continue;
        try
        {
            CheckOrigin(target.value("url", ""));
        }
        catch (const Error &)
        {
            continue;
        }
        AttachPage(entry.first);
    }
    Json result = Json::array();
    for (const auto &page : pages_)
    {
        const auto &info = available.at(page.second.target);
        bool allowed = true;
        try
        {
            CheckOrigin(info.value("url", ""));
        }
        catch (const Error &)
        {
            allowed = false;
        }
        Json item{{"pageId", page.first},
                  {"active", active_page_ == page.first},
                  {"attached", !page.second.session.empty()},
                  {"allowedOrigin", allowed}};
        if (allowed)
        {
            item["url"] = info.value("url", "");
            item["title"] = info.value("title", "");
        }
        result.push_back(item);
    }
    return {{"pages", result}, {"activePageId", active_page_}, {"maximumPages", 32}};
}
// 用途：创建后显式选择新页面，导航失败保留可列出核对的页面且不自动重发；日期：2026-11-04。
Json Browser::OpenPage(const std::string &url)
{
    CheckOrigin(url);
    if (pages_.size() >= 32)
        throw Error("PAGE_BUDGET_EXCEEDED", "Close an existing managed page before creating another");
    const auto target =
        Call("Target.createTarget", {{"url", "about:blank"}}, false, true).at("targetId").get<std::string>();
    const auto id = AttachPage(target);
    SwitchPage(id);
    Navigate(url);
    return {{"pageId", id}, {"activePageId", id}, {"runtime", runtime_report_}};
}
// 用途：只切换登记且仍连接的页面，域检查成功后失效旧元素引用；日期：2026-11-04。
Json Browser::SwitchPage(const std::string &id)
{
    const auto page = pages_.find(id);
    if (page == pages_.end())
        throw Error("PAGE_NOT_FOUND", "Page is not managed by this browser instance");
    if (page->second.session.empty())
        throw Error("PAGE_DETACHED", "Page session is detached; close it or stop and restart the browser");
    HideHighlight();
    const auto previous_session = session_;
    session_ = page->second.session;
    try
    {
        Document();
        Call("Target.activateTarget", {{"targetId", page->second.target}}, false, true);
    }
    catch (...)
    {
        session_ = previous_session;
        throw;
    }
    active_page_ = id;
    runtime_report_ = page->second.runtime;
    references_.clear();
    return {{"pageId", id}, {"active", true}, {"runtime", runtime_report_}};
}
// 用途：关闭指定本地页面并选择剩余页面，关闭最后一页后要求显式新建/选择；日期：2026-11-04。
Json Browser::ClosePage(const std::string &id)
{
    const auto page = pages_.find(id);
    if (page == pages_.end())
        throw Error("PAGE_NOT_FOUND", "Page is not managed by this browser instance");
    const auto closed_session = page->second.session;
    const auto closed = Call("Target.closeTarget", {{"targetId", page->second.target}}, false, true);
    if (!closed.value("success", false))
        throw Error("PAGE_CLOSE_UNKNOWN", "Page closure was not confirmed; list pages before retry", true);
    ForgetDiagnosticSession(closed_session);
    pages_.erase(id);
    if (active_page_ == id)
    {
        highlighted_ = false;
        active_page_.clear();
        session_.clear();
        runtime_report_ = Json::object();
        references_.clear();
        for (const auto &candidate : pages_)
            if (!candidate.second.session.empty())
            {
                active_page_ = candidate.first;
                session_ = candidate.second.session;
                runtime_report_ = candidate.second.runtime;
                break;
            }
    }
    return {{"pageId", id}, {"closed", true}, {"activePageId", active_page_}};
}
// 用途：以只读屏障收取已有事件并返回有界游标分页，缓冲溢出时明确gap；日期：2026-11-04。
Json Browser::Events(unsigned long long after, const std::string &epoch, size_t limit)
{
    if (limit < 1 || limit > 100 || (!epoch.empty() && epoch != reference_epoch_) ||
        (after > 0 && epoch.empty()) || after > event_sequence_)
        throw Error("EVENT_CURSOR_INVALID", "Use the event epoch and sequence returned by this instance");
    Call("Browser.getVersion", Json::object(), false, true);
    const auto oldest =
        events_.empty() ? event_sequence_ + 1 : events_.front().at("sequence").get<unsigned long long>();
    Json result = Json::array();
    auto next = after;
    for (const auto &event : events_)
        if (event.at("sequence").get<unsigned long long>() > after && result.size() < limit)
        {
            result.push_back(event);
            next = event.at("sequence").get<unsigned long long>();
        }
    return {{"events", result},
            {"cursor", {{"eventEpoch", reference_epoch_}, {"after", next}}},
            {"gap", after < oldest - 1},
            {"oldestAvailable", oldest},
            {"currentSequence", event_sequence_},
            {"more", next < event_sequence_},
            {"bufferLimit", 256},
            {"liveSubscription", false}};
}
// 用途：重启时只观察记录中的同一进程对象，不使用PID执行终止或接管；日期：2026-11-04。
Json Browser::InspectRecovery(const std::wstring &directory)
{
    try
    {
        const auto record = SafeAbsolute(directory + L"\\awb-process-owner.json");
        if (GetFileAttributesW((L"\\\\?\\" + record).c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return {{"state", "no-record"}, {"blocked", false}};
            throw Error("PROCESS_RECORD_INVALID", "Cannot inspect process ownership record");
        }
        const auto saved = Json::parse(ReadFileBounded(record, 8192));
        if (!saved.is_object() || saved.value("schemaVersion", 0) != 1 ||
            !saved.at("pid").is_number_integer() || saved.at("pid") < 1 || saved.at("pid") > MAXDWORD ||
            !saved.at("processCreatedFileTime").is_number_integer() ||
            saved.at("processCreatedFileTime") < 1 || !saved.at("executable").is_string() ||
            saved.value("ownership", "") != "private-job-object")
            throw Error("PROCESS_RECORD_INVALID", "Process ownership record shape is invalid");
        const auto pid = saved.at("pid").get<DWORD>();
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!process.get())
        {
            if (GetLastError() == ERROR_INVALID_PARAMETER)
                return {{"state", "recorded-process-exited"}, {"blocked", false}, {"previousPid", pid}};
            return {{"state", "process-unverifiable"}, {"blocked", true}, {"previousPid", pid}};
        }
        if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
            return {{"state", "recorded-process-exited"}, {"blocked", false}, {"previousPid", pid}};
        FILETIME created{}, exit{}, kernel{}, user{};
        std::vector<wchar_t> image(32768);
        DWORD length = static_cast<DWORD>(image.size());
        if (!GetProcessTimes(process.get(), &created, &exit, &kernel, &user) ||
            !QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
            return {{"state", "process-unverifiable"}, {"blocked", true}, {"previousPid", pid}};
        const unsigned long long identity =
            (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const auto expected_image = SafeAbsolute(Wide(saved.at("executable")));
        if (identity != saved.at("processCreatedFileTime").get<unsigned long long>() ||
            _wcsicmp(expected_image.c_str(), std::wstring(image.data(), length).c_str()) != 0)
            return {{"state", "stale-process-identity"}, {"blocked", false}, {"previousPid", pid}};
        return {{"state", "live-process-unowned"},
                {"blocked", true},
                {"previousPid", pid},
                {"observedRunning", true},
                {"automaticKillAllowed", false}};
    }
    catch (const std::exception &)
    {
        return {{"state", "invalid-record"}, {"blocked", true}, {"automaticKillAllowed", false}};
    }
}
// 用途：取主文档loader及当前来源，重定向后重新校验；日期：2026-11-03。
Json Browser::Document()
{
    const auto frame = Call("Page.getFrameTree").at("frameTree").at("frame");
    CheckOrigin(frame.at("url").get<std::string>());
    return frame;
}
// 用途：只跳转明确获授来源并等待就绪；日期：2026-11-03。
Json Browser::Navigate(const std::string &url)
{
    CheckOrigin(url);
    HideHighlight();
    const auto navigation = Call("Page.navigate", {{"url", url}});
    references_.clear();
    if (navigation.contains("errorText"))
        throw Error("NAVIGATION_FAILED", navigation["errorText"].get<std::string>());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    const auto expected_loader = navigation.value("loaderId", "");
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto frame = Document();
        const auto state =
            Call("Runtime.evaluate", {{"expression", "document.readyState"}, {"returnByValue", true}});
        if ((url == "about:blank" || frame.value("url", "about:blank") != "about:blank") &&
            (expected_loader.empty() || frame.value("loaderId", "") == expected_loader) &&
            state.at("result").value("value", "") == "complete")
            return {{"url", frame["url"]}, {"loaderId", frame["loaderId"]}, {"state", "loaded"}};
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw Error("NAVIGATION_TIMEOUT", "Document did not become ready", true);
}
// 用途：创建不可跨文档重用的元素引用；日期：2026-11-03。
std::string Browser::Reference(int backend_node, const std::string &loader)
{
    if (references_.size() >= 2048)
        throw Error("REFERENCE_BUDGET_EXCEEDED", "Navigate or refresh snapshot to replace old references");
    const std::string id = "e" + reference_epoch_ + "-" + std::to_string(++ref_sequence_);
    references_.emplace(id, NodeReference{backend_node, loader});
    return id;
}
// 用途：取有界AX快照，节点引用绑定当前主文档；日期：2026-11-03。
Json Browser::Snapshot()
{
    const auto document = Document();
    const auto loader = document.value("loaderId", "");
    references_.clear();
    Json nodes = Json::array();
    const auto response = Call("Accessibility.getFullAXTree", {{"depth", 12}, {"frameId", document["id"]}});
    bool truncated = false;
    for (const auto &node : response.at("nodes"))
    {
        if (node.value("ignored", false) || !node.contains("backendDOMNodeId"))
            continue;
        if (nodes.size() >= 500)
        {
            truncated = true;
            break;
        }
        const auto role = node.value("role", Json::object()).value("value", "");
        const auto name = node.value("name", Json::object()).value("value", "");
        nodes.push_back({{"ref", Reference(node["backendDOMNodeId"].get<int>(), loader)},
                         {"role", role},
                         {"name", name},
                         {"scope", "main-frame"}});
    }
    return {{"url", document["url"]},
            {"loaderId", loader},
            {"nodes", nodes},
            {"truncated", truncated},
            {"frameScope", "main-frame-only"}};
}
// 用途：按契约的Unicode码点限制唯一CSS定位，不用UTF-8字节数误拒中文；日期：2026-11-04。
Json Browser::Locate(const std::string &selector)
{
    if (selector.empty() || UnicodeLength(selector) > 2048)
        throw Error("LOCATOR_INVALID", "CSS selector must contain 1..2048 Unicode code points");
    const auto document = Document();
    const auto root = Call("DOM.getDocument", {{"depth", 0}}).at("root").at("nodeId");
    const auto nodes = Call("DOM.querySelectorAll", {{"nodeId", root}, {"selector", selector}}).at("nodeIds");
    if (nodes.empty())
        throw Error("ELEMENT_NOT_FOUND", "No matching element");
    if (nodes.size() != 1)
        throw Error("LOCATOR_AMBIGUOUS", "Selector must match exactly one element");
    const auto node = Call("DOM.describeNode", {{"nodeId", nodes[0]}}).at("node");
    return {{"ref", Reference(node.at("backendNodeId").get<int>(), document.value("loaderId", ""))},
            {"nodeName", node["nodeName"]}};
}
// 用途：将文档/节点一致性验证后的引用解析成短期Runtime对象；日期：2026-11-03。
std::string Browser::Resolve(const std::string &reference)
{
    const auto found = references_.find(reference);
    if (found == references_.end())
        throw Error("STALE_ELEMENT_REFERENCE", "Unknown or replaced element reference");
    if (Document().value("loaderId", "") != found->second.loader)
        throw Error("STALE_ELEMENT_REFERENCE", "Element belongs to an earlier document");
    const auto object = Call("DOM.resolveNode", {{"backendNodeId", found->second.backend_node}}).at("object");
    if (!object.contains("objectId"))
        throw Error("STALE_ELEMENT_REFERENCE", "Element was removed");
    return object.at("objectId").get<std::string>();
}
// 用途：读取可操作性事实而不读取字段值、文本、属性全集或执行网页业务；日期：2026-11-04。
Json Browser::InspectReference(const std::string &reference)
{
    const auto object = Resolve(reference);
    const auto result =
        Call("Runtime.callFunctionOn",
             {{"objectId", object}, {"returnByValue", true}, {"functionDeclaration", R"JS(function(){
    if(!this.isConnected) return {detached:true};
    const r=this.getBoundingClientRect();
    let visible=r.width>0&&r.height>0;
    for(let n=this;n instanceof Element;n=n.parentElement){
        const s=getComputedStyle(n);
        if(s.display==='none'||s.visibility!=='visible'||Number(s.opacity)===0) visible=false;
    }
    const x=r.x+r.width/2,y=r.y+r.height/2;
    const inViewport=x>=0&&y>=0&&x<innerWidth&&y<innerHeight;
    const hit=inViewport?document.elementFromPoint(x,y):null;
    const receivesPointerEvents=!!hit&&(hit===this||this.contains(hit));
    const enabled=!this.matches(':disabled')&&this.getAttribute('aria-disabled')!=='true'&&!this.closest('[inert]');
    const inputType=this instanceof HTMLInputElement?this.type:'';
    const editable=this.matches('[contenteditable]:not([contenteditable="false"])');
    const fillSupported=editable||this instanceof HTMLTextAreaElement||(this instanceof HTMLInputElement&&
        !['hidden','file','checkbox','radio','button','submit','reset','image'].includes(inputType));
    const selectable=this instanceof HTMLSelectElement;
    const readOnly=!!this.readOnly||this.getAttribute('aria-readonly')==='true';
    return {tagName:this.tagName,inputType,visible,enabled,readOnly,fillSupported,selectable,
        multiple:selectable&&this.multiple,optionCount:selectable?this.options.length:0,
        inViewport,receivesPointerEvents,requiresScroll:!inViewport,
        clickReady:visible&&enabled&&inViewport&&receivesPointerEvents,
        fillReady:visible&&enabled&&fillSupported&&!readOnly,selectReady:visible&&enabled&&selectable,
        geometry:{x:r.x,y:r.y,width:r.width,height:r.height,centerX:x,centerY:y}};
})JS"}});
    Call("Runtime.releaseObject", {{"objectId", object}});
    if (result.contains("exceptionDetails") || !result.at("result").contains("value"))
        throw Error("ELEMENT_INSPECTION_FAILED", "Could not inspect the current element");
    auto facts = result.at("result").at("value");
    if (facts.value("detached", false))
        throw Error("STALE_ELEMENT_REFERENCE", "Element was removed");
    facts["ref"] = reference;
    facts["pageId"] = active_page_;
    facts["frameScope"] = "main-frame-only";
    return facts;
}
// 用途：清理旧会话的DevTools覆盖层，不注入或恢复网页样式；日期：2026-11-04。
void Browser::HideHighlight()
{
    if (!highlighted_)
        return;
    try
    {
        Call("Overlay.hideHighlight");
    }
    catch (const Error &error)
    {
        if (error.code != "CDP_COMMAND_FAILED")
            throw;
        const auto current = pages_.find(active_page_);
        const auto targets = Call("Target.getTargets", Json::object(), false, true).at("targetInfos");
        for (const auto &target : targets)
            if (current != pages_.end() && target.value("targetId", "") == current->second.target)
                throw;
    }
    highlighted_ = false;
}
// 用途：清除当前受管页面高亮；撤销租约仍拒绝新的调试派发；日期：2026-11-04。
Json Browser::ClearHighlight()
{
    Document();
    HideHighlight();
    return {{"cleared", true}, {"pageId", active_page_}};
}
// 用途：唯一定位、生成不含输入值的动作草稿，并可显示原生DevTools高亮；日期：2026-11-04。
Json Browser::Inspect(const std::string &selector, bool highlight)
{
    const auto located = Locate(selector);
    const auto reference = located.at("ref").get<std::string>();
    auto facts = InspectReference(reference);
    facts["highlighted"] = false;
    if (highlight)
    {
        HideHighlight();
        if (facts.at("visible").get<bool>())
        {
            Call("Overlay.enable");
            Call("Overlay.highlightNode",
                 {{"backendNodeId", references_.at(reference).backend_node},
                  {"highlightConfig",
                   {{"showInfo", false},
                    {"contentColor", {{"r", 40}, {"g", 140}, {"b", 255}, {"a", 0.2}}},
                    {"borderColor", {{"r", 30}, {"g", 100}, {"b", 255}, {"a", 0.9}}}}}});
            highlighted_ = true;
            facts["highlighted"] = true;
        }
    }
    Json drafts = Json::array();
    drafts.push_back({{"tool", "page_click"},
                      {"arguments", {{"ref", reference}}},
                      {"requiredInputs", Json::array({"profileId", "lease"})},
                      {"readyNow", facts.at("clickReady")}});
    if (facts.at("fillSupported").get<bool>())
        drafts.push_back({{"tool", "page_fill"},
                          {"arguments", {{"ref", reference}}},
                          {"requiredInputs", Json::array({"profileId", "lease", "value"})},
                          {"readyNow", facts.at("fillReady")}});
    if (facts.at("selectable").get<bool>())
        drafts.push_back({{"tool", "page_select"},
                          {"arguments", {{"ref", reference}}},
                          {"requiredInputs", Json::array({"profileId", "lease", "values"})},
                          {"readyNow", facts.at("selectReady")}});
    facts["drafts"] = drafts;
    facts["executed"] = false;
    return facts;
}
// 用途：预览操作当前条件及阻断原因，不滚动、不聚焦、不填写或点击；日期：2026-11-04。
Json Browser::Preview(const std::string &reference, const std::string &action)
{
    if (action != "click" && action != "fill" && action != "select")
        throw Error("INVALID_ARGUMENTS", "Preview action must be click, fill or select");
    const auto facts = InspectReference(reference);
    Json blocked = Json::array();
    if (!facts.at("visible").get<bool>())
        blocked.push_back("not-visible");
    if (!facts.at("enabled").get<bool>())
        blocked.push_back("disabled-or-inert");
    if (action == "click")
    {
        if (!facts.at("inViewport").get<bool>())
            blocked.push_back("requires-scroll");
        else if (!facts.at("receivesPointerEvents").get<bool>())
            blocked.push_back("covered-or-pointer-events");
    }
    if ((action == "fill" && !facts.at("fillSupported").get<bool>()) ||
        (action == "select" && !facts.at("selectable").get<bool>()))
        blocked.push_back("unsupported-control");
    if (action == "fill" && facts.at("readOnly").get<bool>())
        blocked.push_back("read-only");
    return {{"ref", reference},
            {"pageId", active_page_},
            {"action", action},
            {"readyNow", blocked.empty()},
            {"blockedBy", blocked},
            {"executed", false},
            {"selectionValuesValidated", false},
            {"conditions", facts}};
}
// 用途：选择前验证全部值唯一且可用，派发input/change后确认结果而不回显选项值；日期：2026-11-04。
Json Browser::Select(const std::string &reference, const std::vector<std::string> &values)
{
    if (values.size() > 32 || std::set<std::string>(values.begin(), values.end()).size() != values.size())
        throw Error("INVALID_ARGUMENTS", "Selection requires at most 32 distinct values");
    for (const auto &value : values)
        if (value.find('\0') != std::string::npos || UnicodeLength(value) > 4096)
            throw Error("INVALID_ARGUMENTS", "Selection value exceeds allowed bounds");
    HideHighlight();
    const auto facts = InspectReference(reference);
    if (!facts.at("selectReady").get<bool>())
        throw Error("ELEMENT_NOT_SELECTABLE", "Selection requires a visible, enabled native select");
    const auto object = Resolve(reference);
    const auto result = Call("Runtime.callFunctionOn", {{"objectId", object},
                                                        {"returnByValue", true},
                                                        {"arguments", Json::array({{{"value", values}}})},
                                                        {"functionDeclaration", R"JS(function(values){
    if(!this.isConnected||!(this instanceof HTMLSelectElement)||this.matches(':disabled')||
        this.getAttribute('aria-disabled')==='true'||this.closest('[inert]')) return {blocked:'ELEMENT_NOT_SELECTABLE'};
    const rect=this.getBoundingClientRect();
    if(rect.width<=0||rect.height<=0) return {blocked:'ELEMENT_NOT_SELECTABLE'};
    for(let n=this;n instanceof Element;n=n.parentElement){
        const style=getComputedStyle(n);
        if(style.display==='none'||style.visibility!=='visible'||Number(style.opacity)===0)
            return {blocked:'ELEMENT_NOT_SELECTABLE'};
    }
    if(!this.multiple&&values.length!==1) return {blocked:'SELECTION_COUNT_INVALID'};
    const options=Array.from(this.options),chosen=[];
    for(const value of values){
        const matches=options.filter(o=>o.value===value);
        if(matches.length===0) return {blocked:'OPTION_NOT_FOUND'};
        if(matches.length!==1) return {blocked:'OPTION_AMBIGUOUS'};
        const option=matches[0];
        if(option.disabled||(option.parentElement instanceof HTMLOptGroupElement&&option.parentElement.disabled))
            return {blocked:'OPTION_DISABLED'};
        chosen.push(option);
    }
    for(const option of options) option.selected=chosen.includes(option);
    this.dispatchEvent(new Event('input',{bubbles:true}));
    this.dispatchEvent(new Event('change',{bubbles:true}));
    const actual=Array.from(this.selectedOptions);
    return {selectionMatchesRequested:actual.length===chosen.length&&chosen.every(o=>actual.includes(o)),selectedCount:actual.length};
})JS"}});
    try
    {
        Call("Runtime.releaseObject", {{"objectId", object}});
    }
    catch (const Error &)
    {
        throw Error("ACTION_OUTCOME_UNKNOWN", "Selection cleanup failed; reconcile before retry", true);
    }
    if (result.contains("exceptionDetails") || !result.at("result").contains("value"))
        throw Error("ACTION_OUTCOME_UNKNOWN", "Selection events may have run; reconcile before retry", true);
    const auto actual = result.at("result").at("value");
    if (actual.contains("blocked"))
        throw Error(actual.at("blocked"), "Selection was rejected before modifying any option");
    if (!actual.value("selectionMatchesRequested", false))
        throw Error("ACTION_POSTCONDITION_FAILED",
                    "Page changed selection during events; inspect before retry", true);
    return {{"selected", true},
            {"selectedCount", actual.at("selectedCount")},
            {"ref", reference},
            {"businessOutcome", "not-asserted"}};
}
// 用途：填写可编辑输入并派发input/change，返回结果不包含字段值；日期：2026-11-03。
Json Browser::Fill(const std::string &reference, const std::string &value)
{
    if (UnicodeLength(value) > 65536)
        throw Error("INPUT_TOO_LARGE", "Input value exceeds 65536 Unicode code points");
    HideHighlight();
    if (!InspectReference(reference).at("fillReady").get<bool>())
        throw Error("ELEMENT_NOT_EDITABLE", "Input must be visible, enabled, editable and supported");
    const auto object = Resolve(reference);
    const auto result = Call(
        "Runtime.callFunctionOn",
        {{"objectId", object},
         {"functionDeclaration",
          "function(value){if(!this.isConnected)throw Error('removed');"
          "if(this.matches(':disabled')||this.readOnly||this.closest('[inert]')||"
          "this.getAttribute('aria-disabled')==='true')throw Error('not editable');"
          "const r=this.getBoundingClientRect();if(r.width<=0||r.height<=0)throw Error('hidden');"
          "for(let n=this;n instanceof Element;n=n.parentElement){const s=getComputedStyle(n);"
          "if(s.display==='none'||s.visibility!=='visible'||Number(s.opacity)===0)throw Error('hidden');}"
          "if(this.matches(\"[contenteditable]:not([contenteditable='false'])\")){"
          "this.focus();this.textContent=value;"
          "}else{let proto=this instanceof "
          "HTMLTextAreaElement?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
          "if(!(this instanceof HTMLInputElement)&&!(this instanceof HTMLTextAreaElement))throw "
          "Error('unsupported input');"
          "this.focus();Object.getOwnPropertyDescriptor(proto,'value').set.call(this,value);}"
          "this.dispatchEvent(new Event('input',{bubbles:true}));this.dispatchEvent(new "
          "Event('change',{bubbles:true}));"
          "return "
          "{filled:true,valueMatchesRequested:(this.matches(\"[contenteditable]:not([contenteditable='false']"
          ")\")?this.textContent:this.value)===value};}"},
         {"arguments", Json::array({{{"value", value}}})},
         {"returnByValue", true}});
    try
    {
        Call("Runtime.releaseObject", {{"objectId", object}});
    }
    catch (const Error &)
    {
        throw Error("ACTION_OUTCOME_UNKNOWN",
                    "Input was submitted before cleanup failed; reconcile before retry", true);
    }
    if (result.contains("exceptionDetails"))
        throw Error("ELEMENT_NOT_EDITABLE", "Input rejected or removed; inspect current snapshot", true);
    if (!result.at("result").at("value").value("valueMatchesRequested", false))
        throw Error("ACTION_POSTCONDITION_FAILED",
                    "Page did not retain the requested input; reconcile before retry", true);
    return {{"filled", true},
            {"valueMatchesRequested", result.at("result").at("value").at("valueMatchesRequested")},
            {"ref", reference},
            {"businessOutcome", "not-asserted"}};
}
// 用途：根据真实盒模型派发鼠标输入，拒绝隐藏或被覆盖的元素；日期：2026-11-03。
Json Browser::Click(const std::string &reference)
{
    HideHighlight();
    const auto object = Resolve(reference);
    const auto ready =
        Call("Runtime.callFunctionOn",
             {{"objectId", object},
              {"returnByValue", true},
              {"functionDeclaration",
               "function(){if(!this.isConnected||this.matches(':disabled'))return false;"
               "this.scrollIntoView({block:'center',inline:'center'});const r=this.getBoundingClientRect();"
               "const e=document.elementFromPoint(r.x+r.width/2,r.y+r.height/2);"
               "return r.width>0&&r.height>0&&(e===this||this.contains(e));}"}});
    if (!ready.at("result").value("value", false))
    {
        Call("Runtime.releaseObject", {{"objectId", object}});
        throw Error("ELEMENT_NOT_ACTIONABLE", "Element is hidden, covered, disabled or detached");
    }
    Call("Runtime.releaseObject", {{"objectId", object}});
    const auto facts = InspectReference(reference);
    if (!facts.at("clickReady").get<bool>())
        throw Error("ELEMENT_NOT_ACTIONABLE", "Element is hidden, covered, disabled or detached");
    const double x = facts.at("geometry").at("centerX");
    const double y = facts.at("geometry").at("centerY");
    Call("Input.dispatchMouseEvent", {{"type", "mouseMoved"}, {"x", x}, {"y", y}});
    bool pressed = false;
    try
    {
        Call("Input.dispatchMouseEvent",
             {{"type", "mousePressed"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}});
        pressed = true;
        Call("Input.dispatchMouseEvent",
             {{"type", "mouseReleased"}, {"x", x}, {"y", y}, {"button", "left"}, {"clickCount", 1}});
    }
    catch (const Error &error)
    {
        if (pressed || error.outcome_unknown)
            throw Error("ACTION_OUTCOME_UNKNOWN",
                        "Mouse input may have reached the page; reconcile before retry", true);
        throw;
    }
    return {{"dispatched", true}, {"businessOutcome", "not-asserted"}, {"ref", reference}};
}
// 用途：读取元素文本，不读取输入口令或任意页面脚本；日期：2026-11-03。
Json Browser::Text(const std::string &reference)
{
    const auto object = Resolve(reference);
    const auto result =
        Call("Runtime.callFunctionOn",
             {{"objectId", object},
              {"returnByValue", true},
              {"functionDeclaration", "function(){if(!this.isConnected)throw Error('removed');return "
                                      "(this.innerText||this.textContent||'').slice(0,16384);}"}});
    Call("Runtime.releaseObject", {{"objectId", object}});
    if (result.contains("exceptionDetails"))
        throw Error("STALE_ELEMENT_REFERENCE", "Element was removed");
    return {{"text", result.at("result").value("value", "")}, {"ref", reference}};
}
// 用途：等待业务结果文本，重试只读观察而非点击提交；日期：2026-11-03。
Json Browser::WaitText(const std::string &selector, const std::string &expected, int timeout_ms)
{
    if (timeout_ms < 1 || timeout_ms > 10000)
        throw Error("INVALID_TIMEOUT", "Wait timeout must be 1..10000 ms");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        try
        {
            const auto ref = Locate(selector).at("ref").get<std::string>();
            const auto actual = Text(ref).at("text").get<std::string>();
            references_.erase(ref);
            if (actual == expected)
                return {{"matched", true}, {"text", actual}};
        }
        catch (const Error &error)
        {
            if (error.code != "ELEMENT_NOT_FOUND")
                throw;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    throw Error("ASSERTION_TIMEOUT", "Expected text was not observed");
}
// 用途：抓取当前授权主页面PNG；日期：2026-11-03。
Json Browser::Screenshot()
{
    if (sensitive_capture_blocked_)
        throw Error("SENSITIVE_CAPTURE_BLOCKED",
                    "Restart this managed browser before capturing after secret use");
    Document();
    const auto screenshot =
        Call("Page.captureScreenshot", {{"format", "png"}, {"captureBeyondViewport", false}});
    return {{"mime", "image/png"}, {"base64", screenshot.at("data")}};
}
// 用途：解析当前真实页面来源，拒绝离开已授权范围的文档；日期：2026-11-05。
std::string Browser::CurrentOrigin()
{
    return Origin(Document().at("url"));
}
// 用途：秘密写入前核对来源且永久阻断本实例截图，覆盖反射、跳转及新页面残留；日期：2026-11-05。
Json Browser::FillSecret(const std::string &reference, const std::string &value, const std::string &origin)
{
    if (CurrentOrigin() != origin)
        throw Error("CREDENTIAL_SCOPE_DENIED", "Document origin changed before secret dispatch");
    sensitive_capture_blocked_ = true;
    return Fill(reference, value);
}
// 用途：将受控本地文件绑定到唯一文件输入控件，返回文件元数据而不读取文件正文；日期：2026-11-05。
Json Browser::Upload(const std::string &reference, const std::wstring &input)
{
    const auto path = SafeAbsolute(input);
    const auto attributes = GetFileAttributesW((L"\\\\?\\" + path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("UPLOAD_FILE_INVALID", "Upload requires an ordinary local file");
    Handle file(CreateFileW((L"\\\\?\\" + path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
        !GetFileSizeEx(file.get(), &size) || info.nNumberOfLinks != 1 || size.QuadPart < 0 ||
        size.QuadPart > 100LL * 1024 * 1024)
        throw Error("UPLOAD_FILE_INVALID", "Upload file is unavailable or exceeds 100 MiB");
    const auto node = references_.find(reference);
    if (node == references_.end())
        throw Error("STALE_ELEMENT_REFERENCE", "Unknown or replaced element reference");
    HideHighlight();
    const auto facts = InspectReference(reference);
    if (!facts.value("visible", false) || !facts.value("enabled", false) ||
        facts.value("inputType", "") != "file")
        throw Error("ELEMENT_NOT_FILE_INPUT", "Upload requires a visible enabled file input");
    Call("DOM.setFileInputFiles",
         {{"files", Json::array({Utf8(path)})}, {"backendNodeId", node->second.backend_node}});
    return {{"uploaded", true},
            {"ref", reference},
            {"fileName", Utf8(path.substr(path.find_last_of(L'\\') + 1))},
            {"sizeBytes", size.QuadPart},
            {"businessOutcome", "not-asserted"}};
}
// 用途：为本环境设置下载目录并开启完成事件，活动下载期间禁止重置跟踪；日期：2026-11-05。
Json Browser::ConfigureDownloads(const std::wstring &input)
{
    Call("Browser.getVersion", Json::object(), false, true);
    for (const auto &item : downloads_)
        if (item.second.at("state") == "inProgress")
            throw Error("DOWNLOAD_BUSY", "Wait for active downloads before reconfiguring");
    const auto path = SafeAbsolute(input);
    auto attributes = GetFileAttributesW((L"\\\\?\\" + path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            throw Error("DOWNLOAD_DIRECTORY_INVALID", "Cannot inspect download directory");
        if (!CreateDirectoryW((L"\\\\?\\" + path).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            throw Error("DOWNLOAD_DIRECTORY_INVALID", "Cannot create download directory");
        attributes = GetFileAttributesW((L"\\\\?\\" + path).c_str());
    }
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Error("DOWNLOAD_DIRECTORY_INVALID", "Download directory must be an ordinary directory");
    Call("Browser.setDownloadBehavior",
         {{"behavior", "allow"}, {"downloadPath", Utf8(path)}, {"eventsEnabled", true}}, false, true);
    download_directory_ = path;
    downloads_.clear();
    download_events_lost_ = false;
    return {{"configured", true},
            {"directory", Utf8(path)},
            {"scope", "profile-browser"},
            {"completionEvents", true}};
}
// 用途：泵送下载事件并校验完成文件，旧文件、重复名称、取消或超时不算成功；日期：2026-11-05。
Json Browser::WaitDownload(const std::string &file_name, int timeout_ms)
{
    if (file_name.empty() || UnicodeLength(file_name) > 255 || file_name == "." || file_name == ".." ||
        file_name.find_first_of("\\/:*?\"<>|") != std::string::npos ||
        file_name.find('\0') != std::string::npos || timeout_ms < 1 || timeout_ms > 10000)
        throw Error("INVALID_ARGUMENTS", "Download wait requires a file name and timeout in 1..10000 ms");
    if (download_directory_.empty())
        throw Error("DOWNLOAD_NOT_CONFIGURED", "Configure download tracking before triggering a download");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do
    {
        Call("Browser.getVersion", Json::object(), false, true);
        if (download_events_lost_)
            throw Error("DOWNLOAD_EVENT_GAP", "Download tracking exceeded its event budget; reconcile files");
        Json match;
        for (const auto &item : downloads_)
            if (item.second.at("fileName") == file_name)
            {
                if (!match.is_null())
                    throw Error("DOWNLOAD_AMBIGUOUS", "More than one tracked download has this name");
                match = item.second;
            }
        if (!match.is_null())
        {
            if (match.contains("error"))
                throw Error(match.at("error"), "Tracked download source is outside the allowed scope");
            if (match.at("state") == "canceled")
                throw Error("DOWNLOAD_CANCELED", "Browser reported the download canceled");
            if (match.at("state") == "completed")
            {
                if (!match.contains("filePath"))
                    throw Error("DOWNLOAD_PATH_UNAVAILABLE",
                                "Browser did not report the completed file path");
                const auto path = SafeAbsolute(Wide(match.at("filePath")));
                const auto parent = path.substr(0, path.find_last_of(L'\\'));
                if (_wcsicmp(parent.c_str(), download_directory_.c_str()) != 0)
                    throw Error("DOWNLOAD_PATH_INVALID",
                                "Completed file is outside the configured directory");
                Handle file(CreateFileW((L"\\\\?\\" + path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr));
                BY_HANDLE_FILE_INFORMATION info{};
                LARGE_INTEGER size{};
                if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
                    !GetFileSizeEx(file.get(), &size) || info.nNumberOfLinks != 1 ||
                    (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                    throw Error("DOWNLOAD_FILE_INVALID",
                                "Completed download is not an available ordinary file");
                return {{"completed", true},
                        {"guid", match.at("guid")},
                        {"fileName", file_name},
                        {"path", Utf8(path)},
                        {"sizeBytes", size.QuadPart},
                        {"state", "completed"},
                        {"completionEvidence", "browser-event-and-file"}};
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    throw Error("DOWNLOAD_TIMEOUT",
                "Download completion was not observed; timeout does not cancel or retry it");
}
// 用途：在任何网页动作之前校验断言字段及类型，拒绝未实现字段和无界参数；日期：2026-11-05。
void Browser::ValidateAssertion(const Json &assertion)
{
    if (!assertion.is_object() || !assertion.contains("type") || !assertion.at("type").is_string())
        throw Error("INVALID_ARGUMENTS", "Assertion requires a type");
    const auto type = assertion.at("type").get<std::string>();
    if (type != "urlEquals" && type != "urlContains" && type != "text" && type != "elementCount" &&
        type != "elementState")
        throw Error("INVALID_ARGUMENTS", "Unsupported assertion type");
    std::set<std::string> allowed{"type", "expected"};
    if (type == "text" || type == "elementCount" || type == "elementState")
        allowed.insert("selector");
    if (type == "elementState")
        allowed.insert("state");
    for (auto it = assertion.begin(); it != assertion.end(); ++it)
        if (!allowed.count(it.key()))
            throw Error("INVALID_ARGUMENTS", "Unsupported assertion field");
    // 用途：按Unicode码点限制断言文本，嵌入零字符不能改变实际CSS或URL含义；日期：2026-11-05。
    const auto bounded = [&](const char *key, size_t minimum, size_t maximum)
    {
        if (!assertion.contains(key) || !assertion.at(key).is_string())
            throw Error("INVALID_ARGUMENTS", "Assertion text field is required");
        const auto value = assertion.at(key).get<std::string>();
        if (value.find('\0') != std::string::npos || UnicodeLength(value) < minimum ||
            UnicodeLength(value) > maximum)
            throw Error("INVALID_ARGUMENTS", "Assertion text field is outside its bounds");
    };
    if (allowed.count("selector"))
        bounded("selector", 1, 2048);
    if (type == "elementCount")
    {
        if (!assertion.value("expected", Json()).is_number_integer() || assertion.at("expected") < 0 ||
            assertion.at("expected") > 1000000)
            throw Error("INVALID_ARGUMENTS", "Count assertion requires an integer in 0..1000000");
    }
    else if (type == "elementState")
    {
        bounded("state", 1, 32);
        const std::set<std::string> states{"visible",   "enabled",     "clickReady",
                                           "fillReady", "selectReady", "checked"};
        if (!states.count(assertion.at("state").get<std::string>()) ||
            (assertion.contains("expected") && !assertion.at("expected").is_boolean()))
            throw Error("INVALID_ARGUMENTS",
                        "State assertion requires a supported state and boolean expected");
    }
    else
        bounded("expected", type == "urlContains" ? 1 : 0, type == "text" ? 16384 : 8192);
}
// 用途：执行已校验的URL、文本、数量和控件状态断言，失败返回确定性错误；日期：2026-11-05。
Json Browser::Assert(const Json &assertion)
{
    ValidateAssertion(assertion);
    const auto type = assertion.at("type").get<std::string>();
    const auto document = Document();
    Json result{{"type", type}, {"passed", false}, {"url", document.at("url")}};
    if (type == "urlEquals" || type == "urlContains")
    {
        if (!assertion.value("expected", Json()).is_string())
            throw Error("INVALID_ARGUMENTS", "URL assertion requires expected text");
        const auto expected = assertion.at("expected").get<std::string>();
        result["actual"] = document.at("url");
        result["passed"] = type == "urlEquals"
                               ? document.at("url") == expected
                               : document.at("url").get<std::string>().find(expected) != std::string::npos;
    }
    else if (type == "text")
    {
        if (!assertion.value("selector", Json()).is_string() ||
            !assertion.value("expected", Json()).is_string())
            throw Error("INVALID_ARGUMENTS", "Text assertion requires selector and expected text");
        const auto ref = Locate(assertion.at("selector")).at("ref").get<std::string>();
        const auto observed = Text(ref);
        result["actual"] = observed.at("text");
        result["passed"] = observed.at("text") == assertion.at("expected");
    }
    else if (type == "elementCount")
    {
        if (!assertion.value("selector", Json()).is_string() ||
            !assertion.value("expected", Json()).is_number_integer())
            throw Error("INVALID_ARGUMENTS", "Count assertion requires selector and integer expected");
        const auto selector = assertion.at("selector").get<std::string>();
        if (selector.empty() || UnicodeLength(selector) > 2048)
            throw Error("INVALID_ARGUMENTS", "Assertion selector is outside its bounds");
        const auto root = Call("DOM.getDocument", {{"depth", 0}}).at("root").at("nodeId");
        const auto nodes =
            Call("DOM.querySelectorAll", {{"nodeId", root}, {"selector", selector}}).at("nodeIds");
        result["actual"] = nodes.size();
        result["passed"] = nodes.size() == assertion.at("expected").get<size_t>();
    }
    else
    {
        if (!assertion.value("selector", Json()).is_string() || !assertion.value("state", Json()).is_string())
            throw Error("INVALID_ARGUMENTS", "State assertion requires selector and state");
        const auto ref = Locate(assertion.at("selector")).at("ref").get<std::string>();
        const auto facts = InspectReference(ref);
        const auto state = assertion.at("state").get<std::string>();
        const std::set<std::string> states{"visible",   "enabled",     "clickReady",
                                           "fillReady", "selectReady", "checked"};
        if (!states.count(state))
            throw Error("INVALID_ARGUMENTS", "Unsupported element state");
        Json actual = facts.value(state, false);
        if (state == "checked")
        {
            const auto object = Resolve(ref);
            const auto checked =
                Call("Runtime.callFunctionOn",
                     {{"objectId", object},
                      {"returnByValue", true},
                      {"functionDeclaration",
                       "function(){return this instanceof HTMLInputElement&&this.checked;}"}});
            Call("Runtime.releaseObject", {{"objectId", object}});
            actual = checked.at("result").value("value", false);
        }
        result["actual"] = actual;
        result["passed"] = actual == assertion.value("expected", true);
    }
    if (!result.at("passed").get<bool>())
        throw Error("ASSERTION_FAILED", "Page assertion did not match the observed state");
    return result;
}
// 用途：优先正常关闭，必要时仅终止本Job并确认退出；日期：2026-11-03。
Json Browser::Stop()
{
    if (stopped_)
        return {{"stopped", true}, {"alreadyStopped", true}};
    bool forced = false;
    try
    {
        Call("Browser.close", Json::object(), false);
    }
    catch (const Error &)
    {
        // 关闭连接可能先于close回执；必须继续检查进程，不能直接宣称已停。
    }
    if (WaitForSingleObject(process_.get(), 3000) != WAIT_OBJECT_0)
    {
        forced = true;
        HttpCheck(TerminateJobObject(job_.get(), 1) != FALSE, "TerminateJobObject");
        if (WaitForSingleObject(process_.get(), 3000) != WAIT_OBJECT_0)
            throw Error("STOP_OUTCOME_UNKNOWN", "Owned process termination is not confirmed", true);
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do
    {
        HttpCheck(QueryInformationJobObject(job_.get(), JobObjectBasicAccountingInformation, &accounting,
                                            sizeof(accounting), nullptr) != FALSE,
                  "QueryInformationJobObject");
        if (accounting.ActiveProcesses == 0)
            break;
        forced = true;
        HttpCheck(TerminateJobObject(job_.get(), 1) != FALSE, "TerminateJobObject descendants");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    if (accounting.ActiveProcesses != 0)
        throw Error("STOP_OUTCOME_UNKNOWN", "Owned descendant processes have not all exited", true);
    stopped_ = true;
    websocket_.reset();
    job_.reset();
    return {{"stopped", true},
            {"pid", process_id_},
            {"forced", forced},
            {"processExitConfirmed", true},
            {"jobEmptyConfirmed", true}};
}
// 用途：由共享应用服务安装当前动作的租约门；日期：2026-11-03。
void Browser::SetDispatchGate(std::function<void(const std::function<void()> &)> gate)
{
    dispatch_gate_ = std::move(gate);
}
} // namespace awb
