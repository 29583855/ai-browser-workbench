#include "awb/model_provider.hpp"
#include "awb/browser.hpp"
#include "awb/workflow.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <winhttp.h>

namespace awb
{
namespace
{
struct Endpoint
{
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool secure = false;
    std::string origin;
};
// 用途：只接受完整HTTPS端点及本机HTTP测试端点，拒绝凭据、查询及片段；日期：2026-11-05。
Endpoint ParseEndpoint(const std::string &url)
{
    Endpoint endpoint;
    endpoint.origin = Browser::Origin(url);
    const auto wide = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &parts) || parts.dwExtraInfoLength)
        throw Error("MODEL_ENDPOINT_INVALID", "Endpoint must be a full URL without query or fragment");
    endpoint.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    endpoint.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    endpoint.port = parts.nPort;
    endpoint.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (endpoint.path.empty() || endpoint.path == L"/" ||
        (!endpoint.secure && endpoint.host != L"127.0.0.1" && endpoint.host != L"localhost" &&
         endpoint.host != L"[::1]"))
        throw Error("MODEL_ENDPOINT_INVALID",
                    "Use HTTPS or explicit loopback HTTP with a full completion path");
    return endpoint;
}
// 用途：执行一次有界JSON请求，禁用重定向/系统代理/自动凭据且不重试；日期：2026-11-05。
Json Request(const Endpoint &endpoint, const Json &body, const std::string &key, int timeout)
{
    InternetHandle session(WinHttpOpen(L"AI-Browser-Workbench-Model/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get() || !WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout))
        throw Error("MODEL_TRANSPORT_FAILED", "Cannot initialize model HTTP session");
    InternetHandle connection(WinHttpConnect(session.get(), endpoint.host.c_str(), endpoint.port, 0));
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"POST", endpoint.path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              endpoint.secure ? WINHTTP_FLAG_SECURE : 0));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled = WINHTTP_DISABLE_COOKIES;
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!request.get() ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon)))
        throw Error("MODEL_TRANSPORT_FAILED", "Cannot configure model request boundaries");
    if (key.find_first_of("\r\n") != std::string::npos || key.find('\0') != std::string::npos)
        throw Error("MODEL_KEY_INVALID", "Model credential cannot contain HTTP control characters");
    auto headers = std::wstring(L"Content-Type: application/json\r\n");
    if (!key.empty())
        headers += L"Authorization: Bearer " + Wide(key) + L"\r\n";
    auto serialized = body.dump();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    const auto sent =
        WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()), &serialized[0],
                           static_cast<DWORD>(serialized.size()), static_cast<DWORD>(serialized.size()), 0);
    SecureZeroMemory(&headers[0], headers.size() * sizeof(wchar_t));
    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr))
        throw Error(GetLastError() == ERROR_WINHTTP_TIMEOUT ? "MODEL_TIMEOUT" : "MODEL_TRANSPORT_FAILED",
                    "Model request was not confirmed; budget remains reserved and no automatic retry occurs",
                    true);
    if (std::chrono::steady_clock::now() >= deadline)
        throw Error("MODEL_TIMEOUT", "Model response exceeded its request deadline", true);
    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
        throw Error("MODEL_RESPONSE_INVALID", "Cannot read model HTTP status");
    if (status != 200)
        throw Error(status >= 300 && status < 400 ? "MODEL_REDIRECT_DENIED" : "MODEL_HTTP_ERROR",
                    "Model service returned a non-success status; response body is omitted");
    std::string response;
    for (;;)
    {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        if (remaining <= 0)
            throw Error("MODEL_TIMEOUT", "Model response exceeded its total read deadline", true);
        DWORD receive_timeout = static_cast<DWORD>(remaining);
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &receive_timeout,
                              sizeof(receive_timeout)))
            throw Error("MODEL_TRANSPORT_FAILED", "Cannot enforce response read timeout");
        char chunk[8192]{};
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk, sizeof(chunk), &read))
            throw Error("MODEL_TRANSPORT_FAILED", "Model response ended unexpectedly", true);
        if (std::chrono::steady_clock::now() >= deadline)
            throw Error("MODEL_TIMEOUT", "Model response exceeded its total read deadline", true);
        if (!read)
            break;
        if (response.size() + read > 1024 * 1024)
            throw Error("MODEL_RESPONSE_BUDGET", "Model response exceeds one MiB");
        response.append(chunk, read);
    }
    try
    {
        return ParseExternalJson(response, 40);
    }
    catch (const std::exception &)
    {
        throw Error("MODEL_RESPONSE_INVALID", "Model returned malformed JSON");
    }
}
// 用途：构建纯草稿生成提示，说明方言和完整边界且不向模型授予执行权限；日期：2026-11-05。
std::string Instructions(const Json &origins)
{
    return "Return only one JSON workflow object, without markdown. You generate drafts, never execute "
           "actions. "
           "Use schemaVersion 1.0, name, integer version=1, inputSchema, policy, entry, steps; optional "
           "subflows. "
           "inputSchema is a closed draft07 subset: object properties, required, additionalProperties=false. "
           "Strings need maxLength <=65536; integers need minimum/maximum within +-1e9; lists need "
           "items/maxItems<=32. "
           "Use mandatory input references {\"input\":\"/name\"} for fill values; never embed credentials. "
           "Nodes: navigate(id,type,url,next); fill(id,type,selector,value,next); "
           "click(id,type,selector,postcondition:{selector,expected,timeoutMs},next); "
           "select(id,type,selector,values:[],next); assertText(id,type,selector,expected,timeoutMs,next); "
           "condition(id,type,when:{op:eq|ne|lt|le|gt|ge,left,right},then,else); "
           "call(id,type,flow,next); loop(id,type,count,maxIterations<=100,flow,next); end(id,type). "
           "Subflows map names to {entry,steps}. No implicit cycles or unreachable nodes, no recursive "
           "calls. "
           "No JavaScript or unknown fields. Each click must have a meaningful text postcondition. "
           "Use only known selectors supplied by the user; if information is missing, return a minimal end "
           "draft. "
           "policy must contain maxSteps=1024,maxDurationMs=300000,allowedOrigins=" +
           origins.dump() + ".";
}
} // namespace

// 用途：加载无密钥配置和预算，未配置时保持显式不可用状态；日期：2026-11-05。
ModelProvider::ModelProvider(const std::wstring &root, CredentialVault &vault)
    : path_(SafeAbsolute(root + L"\\model-provider.json")), vault_(vault)
{
    state_ = {{"format", 1},        {"revision", 0},       {"config", nullptr},
              {"reservedCalls", 0}, {"reservedTokens", 0}, {"history", Json::array()}};
    if (GetFileAttributesW((L"\\\\?\\" + path_).c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    try
    {
        state_ = ParseExternalJson(ReadFileBounded(path_, 256 * 1024), 16);
        if (state_.size() != 6 || state_.at("format") != 1 || !state_.at("history").is_array() ||
            state_.at("history").size() > 128 || !state_.at("reservedCalls").is_number_unsigned() ||
            !state_.at("reservedTokens").is_number_unsigned() || !state_.at("revision").is_number_unsigned())
            throw Error("MODEL_STORAGE_INVALID", "Model configuration metadata is invalid");
        if (!state_.at("config").is_null())
            ParseEndpoint(state_.at("config").at("endpoint"));
        bool recovered = false;
        for (auto &entry : state_["history"])
            if (entry.at("state") == "reserved")
            {
                entry["state"] = "interrupted-no-retry";
                recovered = true;
            }
        if (recovered)
            Save(state_);
    }
    catch (const std::exception &)
    {
        throw Error("MODEL_STORAGE_INVALID", "Cannot load model configuration and durable budget");
    }
}
// 用途：在磁盘提交后替换内存状态，失败不放行模型请求；日期：2026-11-05。
void ModelProvider::Save(const Json &next)
{
    AtomicWrite(path_, next.dump(2) + "\n");
    state_ = next;
}
// 用途：执行模型配置/状态/草稿生成，预留预算先落盘且失败不回滚调用次数；日期：2026-11-05。
Json ModelProvider::Execute(const std::string &command, const Json &arguments)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (command == "model_status")
    {
        auto result = state_;
        result["configured"] = !state_.at("config").is_null();
        result["protocol"] = "chat-completions-json-v1";
        return result;
    }
    if (command == "model_configure")
    {
        if (arguments.at("expectedRevision") != state_.at("revision"))
            throw Error("REVISION_CONFLICT", "Refresh model status before changing configuration");
        const auto endpoint = ParseEndpoint(arguments.at("endpoint"));
        if (endpoint.secure && !arguments.contains("credentialId"))
            throw Error("MODEL_CREDENTIAL_REQUIRED",
                        "Remote model endpoints require a scoped model credential");
        auto next = state_;
        next["revision"] = state_.at("revision").get<unsigned long long>() + 1;
        auto config = arguments;
        config.erase("expectedRevision");
        next["config"] = config;
        Save(next);
        return {{"configured", true}, {"revision", next.at("revision")}, {"config", config}};
    }
    if (state_.at("config").is_null())
        throw Error("MODEL_NOT_CONFIGURED",
                    "Configure a model endpoint or continue using manual workflow editing");
    const auto config = state_.at("config");
    const auto endpoint = ParseEndpoint(config.at("endpoint"));
    std::set<std::string> normalized;
    for (const auto &origin : arguments.at("allowedOrigins"))
        normalized.insert(Browser::Origin(origin));
    const Json origins = normalized;
    const Json body{{"model", config.at("model")},
                    {"stream", false},
                    {"max_tokens", config.at("maxOutputTokens")},
                    {"response_format", {{"type", "json_object"}}},
                    {"messages", Json::array({{{"role", "system"}, {"content", Instructions(origins)}},
                                              {{"role", "user"}, {"content", arguments.at("task")}}})}};
    const auto reservation = static_cast<unsigned long long>(body.dump().size()) +
                             config.at("maxOutputTokens").get<unsigned long long>();
    if (state_.at("reservedCalls").get<unsigned long long>() >=
            config.at("maxCalls").get<unsigned long long>() ||
        state_.at("reservedTokens").get<unsigned long long>() + reservation >
            config.at("maxTotalTokens").get<unsigned long long>())
        throw Error("MODEL_BUDGET_EXCEEDED", "Configured call or conservative token budget is exhausted");
    auto next = state_;
    next["reservedCalls"] = state_.at("reservedCalls").get<unsigned long long>() + 1;
    next["reservedTokens"] = state_.at("reservedTokens").get<unsigned long long>() + reservation;
    const auto request_id = RandomId();
    const auto slot = next.at("history").size() >= 128 ? size_t{127} : next.at("history").size();
    if (next.at("history").size() >= 128)
        next["history"].erase(next["history"].begin());
    next["history"].push_back({{"requestId", request_id},
                               {"state", "reserved"},
                               {"reservedTokens", reservation},
                               {"errorCode", ""}});
    Save(next);
    const auto started = std::chrono::steady_clock::now();
    try
    {
        Json response;
        if (config.contains("credentialId"))
            // 用途：密钥仅发送至已配置且被凭据授予的精确来源，禁止HTTP重定向；日期：2026-11-05。
            response = vault_.Use(config.at("credentialId"), "", endpoint.origin, "model",
                                  [&](const std::string &key)
                                  { return Request(endpoint, body, key, config.at("timeoutMs")); });
        else
            response = Request(endpoint, body, "", config.at("timeoutMs"));
        if (!response.contains("choices") || !response.at("choices").is_array() ||
            response.at("choices").size() != 1 ||
            response.at("choices")[0].value("finish_reason", "") != "stop")
            throw Error("MODEL_RESPONSE_INVALID",
                        "Model completion must contain one complete stopped choice");
        const auto content = response.at("choices")[0].at("message").at("content").get<std::string>();
        if (content.size() > 256 * 1024)
            throw Error("MODEL_RESPONSE_BUDGET", "Workflow draft exceeds definition budget");
        Json draft;
        try
        {
            draft = ParseExternalJson(content, 32);
        }
        catch (const std::exception &)
        {
            throw Error("MODEL_RESPONSE_INVALID", "Model content is not a bounded JSON object");
        }
        const auto validation = ValidateWorkflow(draft);
        if (!validation.at("valid").get<bool>())
            throw Error("MODEL_DRAFT_INVALID", "Generated draft failed the shared workflow validator");
        if (vault_.Redact(draft) != draft)
            throw Error("MODEL_DRAFT_SECRET_DETECTED",
                        "Generated draft contains a stored secret and cannot be exported");
        std::set<std::string> actual;
        for (const auto &origin : draft.at("policy").at("allowedOrigins"))
            actual.insert(Browser::Origin(origin));
        if (actual != normalized)
            throw Error("MODEL_DRAFT_SCOPE_MISMATCH", "Generated draft changed the requested origin scope");
        next = state_;
        next["history"][slot]["state"] = "succeeded";
        next["history"][slot]["durationMs"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count();
        Save(next);
        return {{"requestId", request_id}, {"draft", draft},    {"validation", validation},
                {"published", false},      {"executed", false}, {"requiresReview", true}};
    }
    catch (const Error &error)
    {
        next = state_;
        next["history"][slot]["state"] = error.outcome_unknown ? "outcome-unknown" : "failed";
        next["history"][slot]["errorCode"] = error.code;
        Save(next);
        throw;
    }
    catch (const std::exception &)
    {
        next = state_;
        next["history"][slot]["state"] = "failed";
        next["history"][slot]["errorCode"] = "MODEL_RESPONSE_INVALID";
        Save(next);
        throw Error("MODEL_RESPONSE_INVALID", "Model response fields do not match the configured protocol");
    }
}
// 用途：登记配置、状态及草稿工具的严格参数范围；日期：2026-11-05。
void ModelProvider::Register(Json &tools)
{
    // 用途：构造有界字符串参数，密钥仅能通过凭据ID引用；日期：2026-11-05。
    const auto text = [](size_t maximum) -> Json {
        return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
    };
    // 用途：构造限定整数预算；日期：2026-11-05。
    const auto number = [](long long low, long long high) -> Json {
        return {{"type", "integer"}, {"minimum", low}, {"maximum", high}};
    };
    // 用途：追加不允许未知参数的真实模型接口；日期：2026-11-05。
    const auto add =
        [&](const std::string &name, const std::string &description, Json properties, Json required)
    {
        tools.push_back({{"name", name},
                         {"description", description},
                         {"inputSchema",
                          {{"type", "object"},
                           {"properties", properties},
                           {"required", required},
                           {"additionalProperties", false}}}});
    };
    add("model_configure",
        "Configure an HTTPS or loopback OpenAI-compatible completion endpoint with durable conservative "
        "budgets",
        {{"expectedRevision", number(0, 1000000)},
         {"endpoint", text(8192)},
         {"model", text(128)},
         {"credentialId", text(32)},
         {"timeoutMs", number(100, 30000)},
         {"maxCalls", number(1, 1000000)},
         {"maxOutputTokens", number(128, 32768)},
         {"maxTotalTokens", number(1024, 1000000000)}},
        Json::array({"expectedRevision", "endpoint", "model", "timeoutMs", "maxCalls", "maxOutputTokens",
                     "maxTotalTokens"}));
    add("model_status", "Read model configuration and secret-free durable budget and outcome history",
        Json::object(), Json::array());
    add("model_generate_workflow",
        "Generate and validate a workflow draft without publishing or executing it; no automatic retry",
        {{"task", text(16384)},
         {"allowedOrigins", {{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", text(8192)}}}},
        Json::array({"task", "allowedOrigins"}));
}
} // namespace awb
