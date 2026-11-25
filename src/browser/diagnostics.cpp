#include "awb/browser.hpp"
#include <algorithm>
#include <set>

namespace awb
{
// 用途：限制网页枚举，禁止任意控制台/错误文本进入元数据；日期：2026-11-04。
static std::string Choice(const Json &value, const std::set<std::string> &allowed)
{
    const auto text = value.is_string() ? value.get<std::string>() : "";
    return allowed.count(text) ? text : "other";
}
// 用途：仅展示显式白名单中的路径，不保存URL查询和片段；日期：2026-11-04。
static std::string Route(const std::string &url, const std::vector<std::string> &routes)
{
    const auto scheme = url.find("://");
    const auto start = scheme == std::string::npos ? std::string::npos : url.find('/', scheme + 3);
    const auto path =
        start == std::string::npos ? "/" : url.substr(start, url.find_first_of("?#", start) - start);
    return std::find(routes.begin(), routes.end(), path) != routes.end() ? path : "[omitted]";
}
// 用途：追加有限记录，显式统计溢出；日期：2026-11-04。
void Browser::PushDiagnostic(Json event)
{
    event["sequence"] = ++diagnostics_sequence_;
    event["observedTickMs"] = GetTickCount64();
    diagnostics_.push_back(std::move(event));
    while (diagnostics_.size() > 512)
    {
        diagnostics_.pop_front();
        ++diagnostics_dropped_;
    }
}
// 用途：在会话关闭后回收预算，丢失的未决请求不能被补记为成功；日期：2026-11-04。
void Browser::ForgetDiagnosticSession(const std::string &session)
{
    if (session.empty())
        return;
    const auto prefix = session + ":";
    for (auto it = diagnostic_contexts_.begin(); it != diagnostic_contexts_.end();)
        if (it->first.find(prefix) == 0)
            it = diagnostic_contexts_.erase(it);
        else
            ++it;
    for (auto it = diagnostic_requests_.begin(); it != diagnostic_requests_.end();)
        if (it->first.find(prefix) == 0)
        {
            it = diagnostic_requests_.erase(it);
            ++diagnostics_pending_dropped_;
        }
        else
            ++it;
}
// 用途：动作前启用已有会话的观察域，不拦截或修改网络；日期：2026-11-04。
Json Browser::DiagnosticsStart(const std::vector<std::string> &routes)
{
    if (diagnostics_active_)
        throw Error("DIAGNOSTICS_ALREADY_ACTIVE", "Stop the existing collection before starting a new epoch");
    for (const auto &route : routes)
        if (route.empty() || route.front() != '/' || route.size() > 512 ||
            route.find_first_of("?#\\") != std::string::npos ||
            std::any_of(route.begin(), route.end(),
                        // 用途：拒绝路径白名单中的控制字符；日期：2026-11-04。
                        [](unsigned char c) { return c < 32 || c == 127; }))
            throw Error("INVALID_ARGUMENTS", "Route paths must be absolute without queries or fragments");
    Call("Browser.getVersion", Json::object(), false, true);
    diagnostics_epoch_ = RandomId();
    diagnostics_sequence_ = diagnostics_dropped_ = diagnostics_pending_dropped_ = diagnostics_scope_dropped_ =
        0;
    diagnostics_.clear();
    diagnostic_requests_.clear();
    diagnostic_context_ = Json::object();
    diagnostic_routes_ = routes;
    const auto previous = session_;
    try
    {
        for (const auto &page : pages_)
            if (!page.second.session.empty())
            {
                session_ = page.second.session;
                Call("Network.enable", {{"maxTotalBufferSize", 65536},
                                        {"maxResourceBufferSize", 32768},
                                        {"maxPostDataSize", 0}});
            }
        session_ = previous;
        diagnostics_active_ = true;
    }
    catch (...)
    {
        session_ = previous;
        throw;
    }
    return {{"active", true}, {"epoch", diagnostics_epoch_}, {"bufferLimit", 512}, {"payloadCapture", false}};
}
// 用途：按游标读取元数据并报告丢失/未决请求，不补造成功事件；日期：2026-11-04。
Json Browser::DiagnosticsRead(unsigned long long after, const std::string &epoch, size_t limit)
{
    if (limit < 1 || limit > 100 || (after && epoch.empty()) ||
        (!epoch.empty() && epoch != diagnostics_epoch_) || after > diagnostics_sequence_)
        throw Error("DIAGNOSTIC_CURSOR_INVALID", "Use this collection's epoch and sequence");
    if (diagnostics_active_)
        Call("Browser.getVersion", Json::object(), false, true);
    const auto oldest = diagnostics_.empty() ? diagnostics_sequence_ + 1
                                             : diagnostics_.front().at("sequence").get<unsigned long long>();
    Json records = Json::array();
    auto next = after;
    for (const auto &event : diagnostics_)
        if (event.at("sequence").get<unsigned long long>() > after && records.size() < limit)
        {
            records.push_back(event);
            next = event.at("sequence").get<unsigned long long>();
        }
    return {{"active", diagnostics_active_},
            {"events", records},
            {"cursor", {{"epoch", diagnostics_epoch_}, {"after", next}}},
            {"gap", after < oldest - 1},
            {"more", next < diagnostics_sequence_},
            {"oldestAvailable", oldest},
            {"currentSequence", diagnostics_sequence_},
            {"bufferLimit", 512},
            {"droppedEvents", diagnostics_dropped_},
            {"pendingRequests", diagnostic_requests_.size()},
            {"pendingLimit", 128},
            {"droppedPendingRequests", diagnostics_pending_dropped_},
            {"scopeFilteredEvents", diagnostics_scope_dropped_},
            {"payloadCapture", false},
            {"liveSubscription", false}};
}
// 用途：保留已有内存证据并停止收集，未完成请求明确计数；日期：2026-11-04。
Json Browser::DiagnosticsStop()
{
    if (diagnostics_active_)
    {
        Call("Browser.getVersion", Json::object(), false, true);
        PushDiagnostic({{"kind", "collection-stopped"},
                        {"unfinishedRequests", diagnostic_requests_.size()},
                        {"correlation", "unavailable"}});
        diagnostics_active_ = false;
        diagnostic_requests_.clear();
        diagnostic_context_ = Json::object();
    }
    return {{"active", false},
            {"epoch", diagnostics_epoch_},
            {"retainedEvents", diagnostics_.size()},
            {"networkInterception", false}};
}
// 用途：关联动作观察窗口，异常时恢复上下文并保留原始业务错误，不重试动作；日期：2026-11-04。
Json Browser::Diagnose(const Json &context, const std::function<Json()> &operation)
{
    if (!diagnostics_active_)
        return operation();
    Call("Browser.getVersion", Json::object(), false, true);
    const auto previous = diagnostic_context_;
    diagnostic_context_ = context;
    try
    {
        const auto result = operation();
        Call("Browser.getVersion", Json::object(), false, true);
        diagnostic_context_ = previous;
        return result;
    }
    catch (...)
    {
        diagnostic_context_ = previous;
        throw;
    }
}
// 用途：在受管会话/获授来源内提取固定字段，并将请求开始上下文保留到异步结果；日期：2026-11-04。
void Browser::RecordDiagnostic(const Json &message)
{
    const auto method = message.value("method", "");
    const auto session = message.value("sessionId", "");
    std::string page_id;
    for (const auto &page : pages_)
        if (!session.empty() && page.second.session == session)
            page_id = page.first;
    if (page_id.empty())
        return;
    const auto params = message.value("params", Json::object());
    const auto prefix = session + ":";
    if (method == "Runtime.executionContextsCleared")
    {
        for (auto it = diagnostic_contexts_.begin(); it != diagnostic_contexts_.end();)
            if (it->first.find(prefix) == 0)
                it = diagnostic_contexts_.erase(it);
            else
                ++it;
    }
    else if (method == "Runtime.executionContextCreated")
    {
        const auto context = params.at("context");
        bool allowed = false;
        try
        {
            const auto origin = Origin(context.value("origin", ""));
            allowed = std::find(origins_.begin(), origins_.end(), origin) != origins_.end();
        }
        catch (const Error &)
        {
            // 无法确认来源的上下文不采集诊断。
        }
        if (diagnostic_contexts_.size() < 256)
            diagnostic_contexts_[prefix + context.at("id").dump()] = allowed;
    }
    else if (method == "Runtime.executionContextDestroyed")
        diagnostic_contexts_.erase(prefix + params.at("executionContextId").dump());
    if (!diagnostics_active_ || diagnostics_suspended_)
        return;
    Json event{
        {"pageId", page_id}, {"context", diagnostic_context_}, {"correlation", "observed-during-context"}};
    if (method == "Runtime.consoleAPICalled" || method == "Runtime.exceptionThrown")
    {
        const auto detail = method == "Runtime.exceptionThrown" ? params.at("exceptionDetails") : params;
        const auto key = prefix + detail.value("executionContextId", Json(0)).dump();
        if (!diagnostic_contexts_.count(key) || !diagnostic_contexts_.at(key))
        {
            ++diagnostics_scope_dropped_;
            return;
        }
        event["contentOmitted"] = true;
        if (method == "Runtime.consoleAPICalled")
        {
            event["kind"] = "console";
            event["level"] =
                Choice(params.value("type", Json()), {"log", "debug", "info", "error", "warning", "assert",
                                                      "trace", "dir", "table", "clear"});
            const auto args = params.value("args", Json::array());
            event["argumentCount"] = args.size();
            event["argumentTypes"] = Json::array();
            for (size_t index = 0; index < args.size() && index < 16; ++index)
                event["argumentTypes"].push_back(
                    Choice(args[index].value("type", Json()), {"object", "function", "undefined", "string",
                                                               "number", "boolean", "symbol", "bigint"}));
        }
        else
        {
            event["kind"] = "exception";
            event["lineNumber"] = detail.value("lineNumber", 0);
            event["columnNumber"] = detail.value("columnNumber", 0);
            event["exceptionClass"] =
                Choice(detail.value("exception", Json::object()).value("className", Json()),
                       {"Error", "TypeError", "ReferenceError", "SyntaxError", "RangeError", "URIError",
                        "EvalError", "AggregateError"});
        }
        PushDiagnostic(std::move(event));
        return;
    }
    if (method.find("Network.") != 0 || !params.contains("requestId"))
        return;
    const auto key = prefix + params.at("requestId").get<std::string>();
    auto pending = diagnostic_requests_.find(key);
    if (method == "Network.requestWillBeSent")
    {
        std::string previous;
        if (pending != diagnostic_requests_.end())
        {
            auto redirect = pending->second;
            event["context"] = redirect.at("context");
            event["correlation"] = "redirect-from-request-start-context-not-proven-cause";
            previous = redirect.at("requestId").get<std::string>();
            redirect["kind"] = "request-redirect";
            redirect["status"] = params.value("redirectResponse", Json::object()).value("status", 0);
            PushDiagnostic(redirect);
            diagnostic_requests_.erase(pending);
        }
        const auto request = params.at("request");
        const auto url = request.value("url", "");
        try
        {
            event["origin"] = Origin(url);
            CheckOrigin(url);
        }
        catch (const Error &)
        {
            ++diagnostics_scope_dropped_;
            return;
        }
        event["kind"] = "request-start";
        event["requestId"] = RandomId();
        event["route"] = Route(url, diagnostic_routes_);
        event["method"] = Choice(request.value("method", Json()),
                                 {"GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"});
        event["resourceType"] =
            Choice(params.value("type", Json()),
                   {"Document", "Stylesheet", "Image", "Media", "Font", "Script", "TextTrack", "XHR", "Fetch",
                    "EventSource", "WebSocket", "Manifest", "Other", "Preflight"});
        event["frameId"] = params.value("frameId", "");
        if (!previous.empty())
            event["redirectFrom"] = previous;
        if (diagnostic_requests_.size() >= 128)
        {
            diagnostic_requests_.erase(diagnostic_requests_.begin());
            ++diagnostics_pending_dropped_;
        }
        diagnostic_requests_[key] = event;
        PushDiagnostic(event);
    }
    else if (pending != diagnostic_requests_.end() &&
             (method == "Network.responseReceived" || method == "Network.loadingFailed" ||
              method == "Network.loadingFinished"))
    {
        event = pending->second;
        event["correlation"] = "request-start-context-not-proven-cause";
        if (method == "Network.responseReceived")
        {
            const auto response = params.at("response");
            event["kind"] = "response";
            event["status"] = response.value("status", 0);
            event["httpError"] = response.value("status", 0) >= 400;
            event["fromDiskCache"] = response.value("fromDiskCache", false);
            event["fromServiceWorker"] = response.value("fromServiceWorker", false);
        }
        else
        {
            event["kind"] = method == "Network.loadingFailed" ? "request-failed" : "request-finished";
            if (method == "Network.loadingFailed")
            {
                event["cancelled"] = params.value("canceled", false);
                event["failure"] =
                    Choice(params.value("errorText", Json()),
                           {"net::ERR_FAILED", "net::ERR_ABORTED", "net::ERR_CONNECTION_REFUSED",
                            "net::ERR_CONNECTION_RESET", "net::ERR_NAME_NOT_RESOLVED", "net::ERR_TIMED_OUT",
                            "net::ERR_BLOCKED_BY_CLIENT", "net::ERR_INTERNET_DISCONNECTED"});
            }
            diagnostic_requests_.erase(pending);
        }
        PushDiagnostic(event);
    }
}
} // namespace awb
