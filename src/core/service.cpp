#include "awb/service.hpp"
#include "awb/version.hpp"
#include <algorithm>
#include <istream>
#include <set>

namespace awb
{
// 用途：定义字符串参数schema，避免各端参数漂移；日期：2026-11-03。
static Json StringSchema(size_t maximum = 8192)
{
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}
// 用途：生成唯一命令注册表，CLI/MCP/UI共享业务入口；日期：2026-11-05。
Json Service::Tools()
{
    const auto id = StringSchema(32);
    const Json runtime{
        {"type", "object"},
        {"properties",
         {{"userAgent", StringSchema(1024)},
          {"timezone", StringSchema(128)},
          {"languages", {{"type", "array"}, {"minItems", 1}, {"maxItems", 8}, {"items", StringSchema(35)}}}}},
        {"required", Json::array()},
        {"additionalProperties", false}};
    const Json lease{{"type", "object"},
                     {"properties",
                      {{"owner", StringSchema(128)},
                       {"token", StringSchema(32)},
                       {"generation", {{"type", "integer"}, {"minimum", 1}}}}},
                     {"required", Json::array({"owner", "token", "generation"})},
                     {"additionalProperties", false}};
    Json tools = Json::array();
    // 用途：将一条真实命令登记为严格工具定义；日期：2026-11-03。
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
    add("capabilities", "Report implemented technical-preview capabilities and unsupported features",
        Json::object(), Json::array());
    add("doctor", "Read component and local ownership health without starting browsers or changing quotas",
        Json::object(), Json::array());
    add("admin_policy", "Read the local development and commercial-preview policy without changing state",
        Json::object(), Json::array());
    add("admin_summary",
        "Read local profile, running-browser and free-capacity counters for the management UI",
        Json::object(), Json::array());
    add("commerce_entitlements", "Read local free counters or connected control-plane entitlements",
        Json::object(), Json::array());
    add("commerce_quote",
        "Preview whether a requested environment/concurrency/proxy scope is free or billable",
        {{"environmentCount", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100000}}},
         {"concurrentCount", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
         {"proxy", {{"type", "boolean"}}},
         {"durationDays", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3650}}}},
        Json::array({"environmentCount"}));
    add("account_connect", "Login to control plane and enforce global commercial admission",
        {{"url", StringSchema(2048)},
         {"email", StringSchema(254)},
         {"password", StringSchema(256)},
         {"tenantId", StringSchema(32)},
         {"deviceName", StringSchema(128)}},
        Json::array({"url", "email", "password"}));
    add("account_connect_code", "Exchange a one-time SSO desktop connection code and bind this device",
        {{"url", StringSchema(2048)}, {"code", StringSchema(128)}, {"deviceName", StringSchema(128)}},
        Json::array({"url", "code"}));
    add("account_disconnect", "Logout only after every tracked browser is stopped", Json::object(),
        Json::array());
    add("account_status", "Read protected connection metadata or current server entitlement",
        {{"remote", {{"type", "boolean"}}}}, Json::array());
    add("account_action", "Execute a customer portal action with server-side tenant and role checks",
        {{"action",
          {{"type", "string"},
           {"enum", Json::array({"identity",
                                 "snapshot",
                                 "order.create",
                                 "order.cancel",
                                 "payment.prepare",
                                 "refund.request",
                                 "invoice.request",
                                 "member.invite",
                                 "member.update",
                                 "member.accept",
                                 "invite.cancel",
                                 "payment.channel.create",
                                 "payment.channel.query",
                                 "payment.channel.refund-query",
                                 "scheduler.snapshot",
                                 "project.save",
                                 "project.state",
                                 "node.enrollment.create",
                                 "node.state",
                                 "assignment.create",
                                 "assignment.release",
                                 "workflow.remote.publish",
                                 "run.submit",
                                 "run.cancel",
                                 "run.resolve",
                                 "oidc.bindings",
                                 "client.code.create"})}}},
         {"body",
          {{"type", "object"},
           {"properties", Json::object()},
           {"required", Json::array()},
           {"additionalProperties", true}}}},
        Json::array({"action", "body"}));
    add("profile_list", "List owned profiles; active by default, or choose a lifecycle state",
        {{"state",
          {{"type", "string"}, {"enum", Json::array({"active", "archived", "trashed", "purging", "all"})}}}},
        Json::array());
    add("profile_create", "Create an isolated development profile with explicit origin scope",
        {{"name", StringSchema(128)},
         {"proxyServer", StringSchema(2048)},
         {"runtimeSettings", runtime},
         {"allowedOrigins",
          {{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", StringSchema()}}}},
        Json::array({"name", "allowedOrigins"}));
    const Json revision{{"type", "integer"}, {"minimum", 1}};
    const Json configuration{
        {"type", "object"},
        {"properties",
         {{"name", StringSchema(128)},
          {"proxyServer", StringSchema(2048)},
          {"runtimeSettings", runtime},
          {"allowedOrigins",
           {{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", StringSchema()}}}}},
        {"required", Json::array({"name", "allowedOrigins"})},
        {"additionalProperties", false}};
    const Json profile_ids{{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", id}};
    for (const auto *kind : {"group", "template"})
    {
        const auto prefix = std::string("profile_") + kind + "_";
        add(prefix + "list", "List local versioned profile library resources", Json::object(), Json::array());
        Json properties{{"name", StringSchema(128)}};
        Json required = Json::array({"name"});
        if (std::string(kind) == "group")
        {
            properties["profileIds"] = {{"type", "array"}, {"maxItems", 100}, {"items", id}};
            required.push_back("profileIds");
        }
        else
        {
            properties["configuration"] = configuration;
            required.push_back("configuration");
        }
        add(prefix + "create", "Create a local profile group or configuration-only template", properties,
            required);
        properties["id"] = id;
        properties["expectedRevision"] = revision;
        required.push_back("id");
        required.push_back("expectedRevision");
        add(prefix + "update", "Replace a local resource using optimistic revision control", properties,
            required);
        add(prefix + "delete", "Delete a local resource without deleting member profiles",
            {{"id", id}, {"expectedRevision", revision}}, Json::array({"id", "expectedRevision"}));
    }
    add("profile_template_instantiate", "Create a new isolated profile from a pinned local template revision",
        {{"id", id}, {"expectedRevision", revision}, {"name", StringSchema(128)}},
        Json::array({"id", "expectedRevision", "name"}));
    add("profile_template_history", "Read immutable retained template revisions without modifying profiles",
        {{"id", id}}, Json::array({"id"}));
    add("profile_template_rollback", "Copy a retained template revision into a new current revision",
        {{"id", id}, {"expectedRevision", revision}, {"sourceRevision", revision}},
        Json::array({"id", "expectedRevision", "sourceRevision"}));
    add("profile_export",
        "Export configurations only; cookies, credentials and local identities are excluded",
        {{"profileIds", profile_ids}}, Json::array({"profileIds"}));
    add("profile_import",
        "Prevalidate and atomically create 1..32 profiles from a versioned configuration bundle",
        {{"bundle",
          {{"type", "object"},
           {"properties",
            {{"format", {{"type", "string"}, {"enum", Json::array({"awb-profile-config-v1"})}}},
             {"configurations",
              {{"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", configuration}}}}},
           {"required", Json::array({"format", "configurations"})},
           {"additionalProperties", false}}}},
        Json::array({"bundle"}));
    const Json batch_arguments{{"type", "object"},
                               {"properties", Json::object()},
                               {"required", Json::array()},
                               {"additionalProperties", true}};
    add("profile_batch",
        "Prevalidate bounded lifecycle/start/stop operations and report each result without automatic retry",
        {{"operations",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 32},
           {"items",
            {{"type", "object"},
             {"properties", {{"command", StringSchema(64)}, {"arguments", batch_arguments}}},
             {"required", Json::array({"command", "arguments"})},
             {"additionalProperties", false}}}}},
         {"onError", {{"type", "string"}, {"enum", Json::array({"stop", "continue"})}}},
         {"requestId", id}},
        Json::array({"operations"}));
    add("profile_batch_list", "Page durable active or archived batch receipts without replaying actions",
        {{"scope", {{"type", "string"}, {"enum", Json::array({"active", "archived", "all"})}}},
         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
         {"before", id}},
        Json::array());
    add("profile_batch_get", "Read durable item states after disconnect or restart; no automatic replay",
        {{"batchId", id}}, Json::array({"batchId"}));
    add("profile_batch_archive",
        "Compact confirmed terminal batch details while preserving request deduplication",
        {{"batchId", id}, {"confirmBatchId", id}}, Json::array({"batchId", "confirmBatchId"}));
    const Json edit{{"profileId", id}, {"expectedRevision", revision}};
    auto update = edit;
    update["name"] = StringSchema(128);
    update["runtimeSettings"] = runtime;
    update["proxyServer"] = {{"type", "string"}, {"maxLength", 2048}};
    update["allowedOrigins"] = {
        {"type", "array"}, {"minItems", 1}, {"maxItems", 32}, {"items", StringSchema()}};
    add("profile_update", "Update a stopped profile using optimistic revision control", update,
        Json::array({"profileId", "expectedRevision"}));
    add("profile_archive", "Archive a stopped, unreferenced active profile without deleting its data", edit,
        Json::array({"profileId", "expectedRevision"}));
    add("profile_trash", "Move a stopped, unreferenced profile into the recycle bin", edit,
        Json::array({"profileId", "expectedRevision"}));
    add("profile_restore", "Restore an archived or recycled profile with its original identity and data",
        edit, Json::array({"profileId", "expectedRevision"}));
    auto purge = edit;
    purge["confirmProfileId"] = id;
    add("profile_purge", "Permanently delete recycled data; repeat the exact profile ID to confirm", purge,
        Json::array({"profileId", "expectedRevision", "confirmProfileId"}));
    add("browser_start", "Start a new owned browser process tree",
        {{"profileId", id}, {"headless", {{"type", "boolean"}}}}, Json::array({"profileId"}));
    add("browser_status", "Read process ownership and handshake version", {{"profileId", id}},
        Json::array({"profileId"}));
    add("browser_stop", "Stop only this managed process tree; no payment or lease required",
        {{"profileId", id}}, Json::array({"profileId"}));
    add("lease_acquire", "Acquire exclusive local automatic control",
        {{"profileId", id},
         {"owner", StringSchema(128)},
         {"ttlMs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 300000}}}},
        Json::array({"profileId", "owner", "ttlMs"}));
    add("lease_revoke", "Revoke prior control generation before manual takeover", {{"profileId", id}},
        Json::array({"profileId"}));
    const Json base{{"profileId", id}, {"lease", lease}};
    // 用途：给所有页面操作追加同一租约和实例参数；日期：2026-11-03。
    const auto action =
        [&](const std::string &name, const std::string &description, Json extra, Json required)
    {
        Json properties = base;
        properties.update(extra);
        required.push_back("profileId");
        required.push_back("lease");
        add(name, description, properties, required);
    };
    action("browser_navigate", "Navigate within the explicit profile origin scope", {{"url", StringSchema()}},
           Json::array({"url"}));
    action("page_snapshot", "Read a bounded main-frame accessibility tree with document-bound refs",
           Json::object(), Json::array());
    action("page_locate", "Locate exactly one CSS element in the main document",
           {{"selector", StringSchema(2048)}}, Json::array({"selector"}));
    action("page_discover",
           "Find bounded visible function candidates without clicking or reading field values",
           {{"query", {{"type", "string"}, {"maxLength", 128}}},
            {"role", StringSchema(64)},
            {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}}},
           Json::array());
    action("page_inspect", "Inspect a unique element and create action drafts without reading field values",
           {{"selector", StringSchema(2048)}, {"highlight", {{"type", "boolean"}}}},
           Json::array({"selector"}));
    action("page_preview", "Observe current action conditions without scrolling or dispatching page actions",
           {{"ref", StringSchema(64)},
            {"action", {{"type", "string"}, {"enum", Json::array({"click", "fill", "select"})}}}},
           Json::array({"ref", "action"}));
    action("page_clear_highlight", "Clear the managed page debug highlight without changing page styles",
           Json::object(), Json::array());
    action("page_pick_start",
           "Select an element with Chromium native overlay; selection click is intercepted", Json::object(),
           Json::array());
    action("page_pick_read", "Read selected main-document element and unique CSS candidates without clicking",
           Json::object(), Json::array());
    add("page_pick_stop", "Stop element selection without replaying any click; no lease required",
        {{"profileId", id}}, Json::array({"profileId"}));
    action("page_fill", "Fill an input without echoing or logging its value",
           {{"ref", StringSchema(64)}, {"value", {{"type", "string"}, {"maxLength", 65536}}}},
           Json::array({"ref", "value"}));
    action("page_upload", "Bind one bounded local file to a unique file input without reading its contents",
           {{"ref", StringSchema(64)}, {"path", StringSchema(32768)}}, Json::array({"ref", "path"}));
    action("page_download_config", "Configure a managed download directory for the current page",
           {{"directory", StringSchema(32768)}}, Json::array({"directory"}));
    action("page_download_wait",
           "Wait for a unique tracked download completion event and verify its ordinary file",
           {{"fileName", StringSchema(255)},
            {"timeoutMs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}},
           Json::array({"fileName", "timeoutMs"}));
    action("page_fill_secret", "Fill using an origin/profile scoped SecretRef without returning the value",
           {{"ref", StringSchema(64)}, {"credentialId", id}}, Json::array({"ref", "credentialId"}));
    action("page_click", "Dispatch click; business success must be asserted separately",
           {{"ref", StringSchema(64)}}, Json::array({"ref"}));
    action("page_select", "Select exact, unambiguous native option values without echoing them",
           {{"ref", StringSchema(64)},
            {"values",
             {{"type", "array"},
              {"minItems", 0},
              {"maxItems", 32},
              {"items", {{"type", "string"}, {"maxLength", 4096}}}}}},
           Json::array({"ref", "values"}));
    action("page_text", "Read bounded element text for assertions", {{"ref", StringSchema(64)}},
           Json::array({"ref"}));
    action("page_wait_text", "Retry read-only observation until exact text matches",
           {{"selector", StringSchema(2048)},
            {"expected", {{"type", "string"}, {"maxLength", 16384}}},
            {"timeoutMs", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}},
           Json::array({"selector", "expected", "timeoutMs"}));
    // 用途：复用宽松对象schema承载结构化断言参数；日期：2026-11-05。
    const Json document{{"type", "object"},
                        {"properties", Json::object()},
                        {"required", Json::array()},
                        {"additionalProperties", true}};
    action("page_assert", "Assert URL, text, element count or control state with structured evidence",
           {{"assertion", document}}, Json::array({"assertion"}));
    action("page_screenshot", "Capture authorized page as PNG base64", Json::object(), Json::array());
    action("page_list", "List managed pages and adopt authorized popups of managed pages", Json::object(),
           Json::array());
    action("page_open", "Open and select a managed page inside the allowed origin scope",
           {{"url", StringSchema()}}, Json::array({"url"}));
    action("page_switch", "Select a managed page; old element references are invalidated", {{"pageId", id}},
           Json::array({"pageId"}));
    action("page_close", "Close one managed page without stopping the owned browser", {{"pageId", id}},
           Json::array({"pageId"}));
    action("browser_events", "Poll bounded metadata events with explicit cursor gaps; no live subscription",
           {{"after", {{"type", "integer"}, {"minimum", 0}}},
            {"eventEpoch", id},
            {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
           Json::array());
    action("browser_reconnect", "Explicitly reconnect the same owned process without replaying page actions",
           Json::object(), Json::array());
    action("browser_diagnostics_start",
           "Start bounded console, exception and network metadata observation before actions",
           {{"routePaths",
             {{"type", "array"}, {"minItems", 0}, {"maxItems", 32}, {"items", StringSchema(512)}}}},
           Json::array());
    action("browser_diagnostics_read",
           "Read metadata diagnostics and step associations; payloads are omitted",
           {{"after", {{"type", "integer"}, {"minimum", 0}}},
            {"epoch", id},
            {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}}},
           Json::array());
    action("browser_diagnostics_stop", "Stop collecting diagnostics and retain the bounded in-memory records",
           Json::object(), Json::array());
    action("recorder_start", "Observe main-document actions as parameterized metadata; no field values",
           Json::object(), Json::array());
    action("recorder_read", "Read bounded recording events and an unvalidated draft; no page actions",
           Json::object(), Json::array());
    action("recorder_control", "Pause, resume or stop and detach the main-document recorder",
           {{"operation", {{"type", "string"}, {"enum", Json::array({"pause", "resume", "stop"})}}}},
           Json::array({"operation"}));
    action("workflow_repair_draft",
           "Review one selector replacement bound to the exact source hash; never publish or replay",
           {{"definition", document},
            {"expectedSha256", StringSchema(64)},
            {"flow", {{"type", "string"}, {"maxLength", 64}}},
            {"stepId", StringSchema(64)},
            {"selector", StringSchema(2048)}},
           Json::array({"definition", "expectedSha256", "stepId", "selector"}));
    action("page_extract",
           "Read public main-document scalar or bounded string-array data using an explicit Schema dialect",
           {{"schema", document}, {"bindings", document}}, Json::array({"schema", "bindings"}));
    action("page_batch",
           "Prevalidate and execute ordered actions; unknown outcomes force a stop without replay",
           {{"actions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 16}, {"items", document}}},
            {"onError", {{"type", "string"}, {"enum", Json::array({"stop", "continue"})}}}},
           Json::array({"actions"}));
    add("workflow_validate",
        "Validate bounded workflow DSL, graph, input types, capabilities and optional input",
        {{"definition", document}, {"input", document}}, Json::array({"definition"}));
    add("workflow_publish", "Publish a validated immutable workflow version with its SHA-256",
        {{"definition", document}}, Json::array({"definition"}));
    add("workflow_list", "List local immutable workflow versions", Json::object(), Json::array());
    add("workflow_get", "Read an owned published workflow definition", {{"workflowId", id}},
        Json::array({"workflowId"}));
    auto run = base;
    run["workflowId"] = id;
    run["input"] = document;
    run["debug"] = document;
    add("workflow_run", "Start a fixed workflow version asynchronously; disconnect does not cancel it", run,
        Json::array({"workflowId", "profileId", "lease", "input"}));
    add("workflow_status", "Read secret-free run checkpoints and actual step outcomes", {{"runId", id}},
        Json::array({"runId"}));
    add("workflow_runs", "Find local run IDs after a client disconnect without replaying business actions",
        Json::object(), Json::array());
    add("workflow_debug",
        "Configure exact flow/step breakpoints and inspect redacted variables only at paused boundaries",
        {{"runId", id}, {"options", document}}, Json::array({"runId", "options"}));
    add("workflow_control", "Pause at a step boundary, resume a paused run or cancel without business replay",
        {{"runId", id},
         {"operation",
          {{"type", "string"},
           {"enum", Json::array({"pause", "resume", "cancel", "step", "stepOver", "stepOut"})}}}},
        Json::array({"runId", "operation"}));
    CredentialVault::Register(tools);
    ModelProvider::Register(tools);
    SitePackages::Register(tools);
    return tools;
}
// 用途：递归验证本注册表实际用到的类型/长度/界限，不冒充通用draft-07验证器；日期：2026-11-03。
static void ValidateArguments(const Json &schema, const Json &value)
{
    if (schema.contains("enum") &&
        std::find(schema.at("enum").begin(), schema.at("enum").end(), value) == schema.at("enum").end())
        throw Error("INVALID_ARGUMENTS", "Argument is outside the registered choices");
    const std::string type = schema.at("type");
    if ((type == "object" && !value.is_object()) || (type == "string" && !value.is_string()) ||
        (type == "integer" && !value.is_number_integer()) || (type == "boolean" && !value.is_boolean()) ||
        (type == "array" && !value.is_array()))
        throw Error("INVALID_ARGUMENTS", "Argument type does not match the registered contract");
    if (type == "object")
    {
        for (const auto &key : schema.at("required"))
            if (!value.contains(key.get<std::string>()))
                throw Error("INVALID_ARGUMENTS", "Required argument is missing");
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
        {
            if (!schema.at("properties").contains(iterator.key()))
            {
                if (!schema.value("additionalProperties", false))
                    throw Error("INVALID_ARGUMENTS", "Unrecognized argument");
            }
            else
                ValidateArguments(schema.at("properties").at(iterator.key()), iterator.value());
        }
    }
    else if (type == "string")
    {
        const auto &text = value.get_ref<const std::string &>();
        const auto length = UnicodeLength(text);
        if (text.find('\0') != std::string::npos || length < schema.value("minLength", size_t{0}) ||
            length > schema.value("maxLength", size_t{8192}))
            throw Error("INVALID_ARGUMENTS", "String exceeds allowed bounds");
    }
    else if (type == "integer")
    {
        if (value < schema.value("minimum", Json(0)) ||
            value > schema.value("maximum", Json(9223372036854775807LL)))
            throw Error("INVALID_ARGUMENTS", "Integer is outside allowed bounds");
    }
    else if (type == "array")
    {
        if (value.size() < schema.value("minItems", size_t{0}) ||
            value.size() > schema.value("maxItems", size_t{32}))
            throw Error("INVALID_ARGUMENTS", "Array exceeds allowed bounds");
        for (const auto &item : value)
            ValidateArguments(schema.at("items"), item);
    }
}
// 用途：集中返回本地免费档和付费占位规则，供后台、CLI和SDK使用同一份契约；日期：2026-11-05。
static Json CommercePolicy()
{
    return {{"mode", "local-preview"},
            {"freeEnvironmentLimit", 10},
            {"freeConcurrentLimit", 10},
            {"freeProxy", true},
            {"freeRpa", true},
            {"freeLocalApi", true},
            {"freeProxyProtocols", Json::array({"http", "https", "socks5", "ssh"})},
            {"implementedProxyProtocols", Json::array({"http", "socks5"})},
            {"proxyAuthentication", false},
            {"billingUnit", "environment-hour"},
            {"paidExpansion", "control-plane-manual-and-signed-gateway"},
            {"paymentSandboxConfigured", false},
            {"oidcConfigured", false},
            {"codeSigningConfigured", false}};
}
// 用途：按已冻结的免费容量规则计算报价预览，不创建订单、不扣款且不发放正式权益；日期：2026-11-05。
static Json CommerceQuote(const Json &arguments)
{
    const auto policy = CommercePolicy();
    const auto environments = arguments.at("environmentCount").get<long long>();
    const auto concurrent = arguments.value("concurrentCount", 1LL);
    const auto proxy = arguments.value("proxy", false);
    const auto duration = arguments.value("durationDays", 30LL);
    const bool free = environments <= policy.at("freeEnvironmentLimit").get<long long>() &&
                      concurrent <= policy.at("freeConcurrentLimit").get<long long>();
    return {{"classification", free ? "free" : "payment_required"},
            {"environmentCount", environments},
            {"concurrentCount", concurrent},
            {"proxyRequested", proxy},
            {"durationDays", duration},
            {"amountMinor", free ? 0 : -1},
            {"currency", "CNY"},
            {"orderCreated", false},
            {"paymentRequired", !free},
            {"policy", policy}};
}
// 用途：在单写锁内发放有限租约；日期：2026-11-03。
Json LeaseGate::Acquire(const std::string &owner, int ttl_ms)
{
    if (owner.empty() || owner.find('\0') != std::string::npos || UnicodeLength(owner) > 128 || ttl_ms < 1 ||
        ttl_ms > 300000)
        throw Error("INVALID_ARGUMENTS", "Invalid owner or lease lifetime");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!token_.empty() && std::chrono::steady_clock::now() < expires_)
        throw Error("LEASE_BUSY", "Another automatic writer holds the profile");
    owner_ = owner;
    token_ = RandomId();
    ++generation_;
    expires_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
    return {{"owner", owner_}, {"token", token_}, {"generation", generation_}};
}
// 用途：撤销后提升代数，使旧消息/引用的派发资格立即失效；日期：2026-11-03。
// 用途：检查原租约后在同一锁中更新到期时间；日期：2026-11-05。
void LeaseGate::Renew(const Json &lease, int ttl_ms)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (token_.empty() || lease.value("token", "") != token_ || lease.value("owner", "") != owner_ ||
        lease.value("generation", 0ULL) != generation_ || std::chrono::steady_clock::now() >= expires_ ||
        ttl_ms < 1 || ttl_ms > 300000)
        throw Error("LEASE_REVOKED", "Remote local lease cannot be renewed");
    expires_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ttl_ms);
}
Json LeaseGate::Revoke()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    token_.clear();
    owner_.clear();
    expires_ = {};
    return {{"revoked", true}, {"generation", generation_}, {"state", "automatic-dispatch-blocked"}};
}
// 用途：验证租约后在同一锁内发送，避免检查与派发竞态；日期：2026-11-03。
void LeaseGate::Dispatch(const Json &lease, const std::function<void()> &send)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!lease.is_object() || token_.empty() || lease.value("owner", "") != owner_ ||
        lease.value("token", "") != token_ || lease.value("generation", 0ULL) != generation_ ||
        std::chrono::steady_clock::now() >= expires_)
        throw Error("LEASE_REVOKED", "Lease is expired, revoked or belongs to another controller");
    send();
}
// 用途：使用统一版本返回真实能力，未做的中央授权/计费明确为false；日期：2026-11-04。
Json Service::Capabilities()
{
    return {
        {"product", "AI Browser Workbench"},
        {"version", Version},
        {"mode", "development-poc-not-commercial-release"},
        {"language", "C++14"},
        {"profileIsolation", true},
        {"privateBrowserStorageAlias", true},
        {"managedJobObject", true},
        {"originScope", true},
        {"mainFrameSnapshot", true},
        {"uniqueCssLocator", true},
        {"inputClickScreenshot", true},
        {"localLeaseFencing", true},
        {"mcpStdio", true},
        {"localRestApi", true},
        {"nativeWin32Shell", true},
        {"sqliteCatalog", true},
        {"catalogSchemaVersion", 7},
        {"profileGroups", true},
        {"profileConfigurationTemplates", true},
        {"profileTemplateHistory", true},
        {"profileConfigurationImportExport", true},
        {"profileLifecycleBatch", true},
        {"durableProfileBatchReceipts", true},
        {"profileBatchPaginationAndRetention", true},
        {"profileLifecycle", true},
        {"profileRevisionControl", true},
        {"persistentProcessOwnership", true},
        {"runtimeSettings", Json::array({"userAgent", "languages", "timezone"})},
        {"managedPages", true},
        {"metadataEventPolling", true},
        {"liveEventSubscription", false},
        {"explicitCdpReconnect", true},
        {"elementInspection", true},
        {"mainDocumentFunctionDiscovery", true},
        {"reviewableSelectorRepairDraft", true},
        {"readOnlyDoctor", true},
        {"nativeElementPicker", "main-document-preview"},
        {"actionPreview", true},
        {"nativeSelect", true},
        {"oopifActions", false},
        {"closedShadowDom", false},
        {"officialPaymentChannels", Json::array({"alipay-rsa2", "wechat-v3", "stripe-checkout"})},
        {"oidcAuthorizationCodePkce", true},
        {"multiNodeScheduler", "outbound-signed-v1"},
        {"nodeMtlsCertificateSelection", true},
        {"centralEntitlements", true},
        {"globalMultiNodeQuota", true},
        {"commerce", true},
        {"localAdminPreview", true},
        {"localCommerceQuote", true},
        {"freeEnvironmentLimit", 10},
        {"freeConcurrentLimit", 10},
        {"freeProxy", true},
        {"fileUpload", true},
        {"downloadDirectory", true},
        {"downloadCompletion", true},
        {"structuredAssertions", true},
        {"stringArrayExtraction", true},
        {"rpaWorkflowEngine", true},
        {"credentialVault", "DPAPI-current-user-v1"},
        {"modelProvider", "chat-completions-json-v1"},
        {"validatedModelDrafts", true},
        {"siteKnowledgePackages", "data-only-v1"},
        {"scopedSecretRef", true},
        {"secretCapturePolicy", "block-instance-screenshots-after-use"},
        {"workflowDsl", "1.1"},
        {"workflowSupportedVersions", Json::array({"1.0", "1.1"})},
        {"workflowTypedVariables", true},
        {"workflowFileActions", true},
        {"workflowStructuredAssertions", true},
        {"workflowStringArrayOutput", true},
        {"workflowDebugger", Json::array({"breakpoint", "step", "stepOver", "stepOut", "redactedVariables"})},
        {"workflowInputDialect", "awb-bounded-draft07-v1"},
        {"immutableWorkflowVersions", true},
        {"workflowCheckpointMetadata", true},
        {"workflowCrashAutoResume", false},
        {"diagnosticMetadata", true},
        {"diagnosticPayloadCapture", false},
        {"schemaExtraction", "awb-scalar-extraction-v1-array"},
        {"pageActionBatch", true},
        {"visualFlowCanvas", "dsl1-native-editor-preview"},
        {"mainDocumentRecorder", "parameterized-preview"},
        {"recorderFieldValues", false},
        {"crossFrameRecorder", false},
        {"batchAtomic", false},
        {"proxyProtocols", Json::array()},
        {"experimentalUnauthenticatedHttpProxy", true},
        {"experimentalUnauthenticatedSocks5Proxy", true},
        {"productionReady", false}};
}
// 用途：独占私有根目录，迁移旧JSON并载入事务数据库；日期：2026-11-03。
Service::Service(const std::wstring &root, const std::wstring &browser)
    : root_(SafeAbsolute(root)), executable_(browser.empty() ? DiscoverBrowser() : SafeAbsolute(browser))
{
    const auto manifest = root_ + L"\\workbench-poc.json";
    const auto database = SafeAbsolute(root_ + L"\\workbench.sqlite3");
    const DWORD root_attributes = GetFileAttributesW(root_.c_str());
    if (root_attributes != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(database.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        WIN32_FIND_DATAW entry{};
        const auto search = FindFirstFileW((root_ + L"\\*").c_str(), &entry);
        bool populated = false;
        if (search != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name(entry.cFileName);
                if (name != L"." && name != L"..")
                    populated = true;
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }
        if (populated)
            throw Error("UNMANAGED_ROOT", "Existing nonempty root is not an owned POC directory");
    }
    PrivateDirectory(root_);
    root_lock_.reset(CreateFileW((root_ + L"\\.owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
    if (root_lock_.get() == INVALID_HANDLE_VALUE)
        throw Error("ROOT_IN_USE", "Another service owns this data directory");
    try
    {
        catalog_ = std::make_unique<Catalog>(database);
    }
    catch (...)
    {
        if (GetFileAttributesW(database.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            root_lock_.reset();
            DeleteFileW((root_ + L"\\.owner.lock").c_str());
        }
        throw;
    }
    catalog_->ImportLegacy(manifest);
    catalog_->Batches("recover");
    credentials_ = std::make_unique<CredentialVault>(root_);
    model_ = std::make_unique<ModelProvider>(root_, *credentials_);
    site_packages_ = std::make_unique<SitePackages>(root_);
    commercial_ = std::make_unique<CommercialClient>(root_);
    for (const auto &metadata : catalog_->Profiles(true))
    {
        const auto id = metadata.at("id").get<std::string>();
        auto profile = std::make_shared<Profile>();
        profile->metadata = metadata;
        SafeAbsolute(root_ + L"\\" + Wide(id));
        profiles_.emplace(id, profile);
    }
    if (GetFileAttributesW((L"\\\\?\\" + root_ + L"\\workflows").c_str()) != INVALID_FILE_ATTRIBUTES)
        Workflows();
    if (commercial_->Status(false).at("connected") == true)
        StartCommercialWatch();
}
// 用途：只为已连接后台的服务启动授权巡检，纯本地存储操作不创建额外线程；日期：2026-11-05。
void Service::StartCommercialWatch()
{
    std::lock_guard<std::mutex> lock(commercial_watch_mutex_);
    if (commercial_watch_.joinable())
        return;
    // 用途：每十秒核对商业准入，失联或撤销时先停止本机浏览器；日期：2026-11-05。
    commercial_watch_ = std::thread(
        [this]
        {
            while (!commercial_stop_.load())
            {
                for (int tick = 0; tick < 100 && !commercial_stop_.load(); ++tick)
                    Sleep(100);
                if (commercial_stop_.load())
                    break;
                std::vector<std::shared_ptr<Profile>> profiles;
                {
                    std::lock_guard<std::mutex> lock(registry_mutex_);
                    for (const auto &entry : profiles_)
                        profiles.push_back(entry.second);
                }
                bool unavailable = false;
                for (const auto &profile : profiles)
                {
                    std::lock_guard<std::mutex> lock(profile->mutex);
                    if (!profile->browser || !profile->browser->Status().value("running", false))
                        continue;
                    bool authorized = false;
                    try
                    {
                        if (!unavailable)
                        {
                            commercial_->Check(profile->metadata.at("id"));
                            authorized = true;
                        }
                    }
                    catch (const Error &error)
                    {
                        unavailable = unavailable || error.code == "CONTROL_UNAVAILABLE";
                    }
                    catch (...)
                    {
                        unavailable = true;
                    }
                    if (!authorized)
                    {
                        profile->gate.Revoke();
                        try
                        {
                            profile->browser->Stop();
                            // 同一巡检已失联时先停止其余浏览器，不能逐个等待网络超时。
                            if (!unavailable)
                                commercial_->Stop(profile->metadata.at("id"));
                        }
                        catch (...)
                        { /* 持久启动意图保留，恢复后再核对释放。 */
                        }
                    }
                }
            }
        });
}
// 用途：流程回调可能使用环境/数据库，因此在其他成员析构前停止线程；日期：2026-11-04。
Service::~Service()
{
    commercial_stop_.store(true);
    if (commercial_watch_.joinable())
        commercial_watch_.join();
    workflows_.reset();
    // 先停止全部本地进程，再核对远端释放；网络故障不能阻碍其余浏览器退出。
    for (const auto &entry : profiles_)
    {
        try
        {
            if (entry.second->browser)
                entry.second->browser->Stop();
        }
        catch (...)
        { /* 未确认停止的进程由独占Job最终回收，配额保持占用。 */
        }
    }
    for (const auto &entry : profiles_)
    {
        try
        {
            if (!Browser::InspectRecovery(root_ + L"\\" + Wide(entry.first)).value("blocked", false))
                commercial_->Stop(entry.first);
        }
        catch (const Error &error)
        {
            if (error.code == "CONTROL_UNAVAILABLE")
                break;
        }
        catch (...)
        { /* 不伪造释放成功，加密记录和后台槽保留供恢复核对。 */
        }
    }
}
// 用途：仅在首次流程操作时创建私有存储，并保证并发调用共用同一引擎；日期：2026-11-04。
WorkflowEngine &Service::Workflows()
{
    std::lock_guard<std::mutex> lock(workflows_mutex_);
    if (!workflows_)
        workflows_ = std::make_unique<WorkflowEngine>(root_);
    return *workflows_;
}
// 用途：共享租约执行文件、断言、数组输出和重新定位动作，业务写入不自动重发；日期：2026-11-05。
// 用途：在节点使用服务前安装围栏；调用期间不再修改回调对象；日期：2026-11-05。
// 用途：中央授权有效时延长原本地租约，避免长流程更换控制身份；日期：2026-11-05。
void Service::RenewRemoteLease(const std::string &id, const Json &lease)
{
    if (!remote_gate_)
        throw Error("FORBIDDEN", "Remote dispatch gate is required");
    const auto profile = Find(id);
    std::lock_guard<std::mutex> lock(profile->mutex);
    remote_gate_([&] { profile->gate.Renew(lease, 300000); });
}
void Service::SetRemoteDispatchGate(std::function<void(const std::function<void()> &)> gate)
{
    remote_gate_ = std::move(gate);
}
Json Service::WorkflowStep(const std::shared_ptr<Profile> &profile, const Json &lease,
                           const std::string &type, const Json &arguments)
{
    std::lock_guard<std::mutex> lock(profile->mutex);
    if (profile->removed || profile->metadata.at("state") != "active" || !profile->browser ||
        !profile->browser->Status().at("running").get<bool>())
        throw Error("BROWSER_NOT_RUNNING", "Workflow requires the original active browser");
    // 用途：流程与交互入口共享同一代数围栏，每条CDP消息独立核对；日期：2026-11-04。
    const std::weak_ptr<Profile> weak(profile);
    profile->browser->SetDispatchGate(
        [this, weak, lease](const std::function<void()> &send)
        {
            const auto current = weak.lock();
            if (!current)
                throw Error("PROFILE_NOT_FOUND", "Workflow profile was released");
            current->gate.Dispatch(lease,
                                   [&]
                                   {
                                       if (remote_gate_)
                                           remote_gate_(send);
                                       else
                                           send();
                                   });
        });
    // 用途：在定位之前拒绝过期/吊销的租约，不复用预先排队的写资格；日期：2026-11-04。
    profile->gate.Dispatch(lease, [] {});
    auto &browser = *profile->browser;
    commercial_->Check(profile->metadata.at("id"));
    if (type == "$check")
        return Json::object();
    // 用途：把网络请求起点绑定到真实run/step，不宣称异步事件具有唯一业务因果；日期：2026-11-04。
    return browser.Diagnose(
        arguments.at("$diagnosticContext"),
        [&]() -> Json
        {
            if (type == "navigate")
            {
                return browser.Navigate(arguments.at("url"));
            }
            if (type == "assertText")
            {
                return browser.WaitText(arguments.at("selector"), arguments.at("expected"),
                                        arguments.at("timeoutMs"));
            }
            if (type == "downloadConfig")
                return browser.ConfigureDownloads(Wide(arguments.at("directory")));
            if (type == "downloadWait")
                return browser.WaitDownload(arguments.at("fileName"), arguments.at("timeoutMs"));
            if (type == "assert")
                return browser.Assert(arguments.at("assertion"));
            if (type == "readTexts")
            {
                const Json schema{{"type", "object"},
                                  {"properties",
                                   {{"texts",
                                     {{"type", "array"},
                                      {"items", {{"type", "string"}, {"maxLength", 4096}}},
                                      {"maxItems", arguments.at("maxItems")}}}}},
                                  {"required", Json::array({"texts"})},
                                  {"additionalProperties", false}};
                const auto extracted = browser.Extract(
                    schema, {{"texts", {{"selector", arguments.at("selector")}, {"source", "texts"}}}});
                if (!extracted.at("valid").get<bool>())
                    throw Error(extracted.at("errors").at(0).at("code"), "Public array observation failed");
                return extracted.at("data");
            }
            const auto reference = browser.Locate(arguments.at("selector")).at("ref").get<std::string>();
            if (type == "upload")
                return browser.Upload(reference, Wide(arguments.at("path")));
            if (type == "readText")
                return browser.Text(reference);
            if (type == "fill")
            {
                if (arguments.at("value").is_object())
                {
                    const auto origin = browser.CurrentOrigin();
                    // 用途：DSL仅持有SecretRef，使用时才核对来源并解密；日期：2026-11-05。
                    return credentials_->Use(arguments.at("value").at("secretRef"),
                                             profile->metadata.at("id"), origin, "browser",
                                             [&](const std::string &value)
                                             { return browser.FillSecret(reference, value, origin); });
                }
                browser.Fill(reference, arguments.at("value"));
            }
            else if (type == "select")
                browser.Select(reference, arguments.at("values").get<std::vector<std::string>>());
            else if (type == "click")
            {
                browser.Click(reference);
                try
                {
                    const auto &assertion = arguments.at("postcondition");
                    browser.WaitText(assertion.at("selector"), assertion.at("expected"),
                                     assertion.at("timeoutMs"));
                }
                catch (const std::exception &)
                {
                    throw Error(
                        "WORKFLOW_POSTCONDITION_UNCONFIRMED",
                        "Click dispatched but its postcondition was not confirmed; reconcile without replay",
                        true);
                }
            }
            else
                throw Error("WORKFLOW_CAPABILITY_UNSUPPORTED", "Workflow step capability is not implemented");
            return Json::object();
        });
}
// 用途：按受管不透明ID取得环境，禁止跨目录引用；日期：2026-11-03。
std::shared_ptr<Service::Profile> Service::Find(const std::string &id)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto found = profiles_.find(id);
    if (found == profiles_.end())
        throw Error("PROFILE_NOT_FOUND", "Profile is not owned by this service");
    return found->second;
}
// 用途：整批先校验再创建私有目录和SQLite记录，提交未决时保留现场供重启核对；日期：2026-11-05。
Json Service::CreateConfigurations(const Json &configurations)
{
    if (!configurations.is_array() || configurations.empty() || configurations.size() > 32)
        throw Error("INVALID_ARGUMENTS", "Configuration import requires 1..32 items");
    Json rows = Json::array();
    for (const auto &item : configurations)
    {
        auto row = Catalog::Configuration(item);
        row["id"] = RandomId();
        rows.push_back(row);
    }
    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::vector<std::string> allocated;
    try
    {
        for (const auto &row : rows)
        {
            const auto id = row.at("id").get<std::string>();
            PrivateDirectory(root_ + L"\\" + Wide(id));
            allocated.push_back(id);
            auto profile = std::make_shared<Profile>();
            profile->metadata = row;
            profiles_.emplace(id, profile);
        }
        catalog_->InsertProfiles(rows);
    }
    catch (const Error &error)
    {
        if (!error.outcome_unknown)
            for (const auto &id : allocated)
            {
                profiles_.erase(id);
                RemoveDirectoryW((root_ + L"\\" + Wide(id)).c_str());
            }
        throw;
    }
    catch (...)
    {
        for (const auto &id : allocated)
        {
            profiles_.erase(id);
            RemoveDirectoryW((root_ + L"\\" + Wide(id)).c_str());
        }
        throw;
    }
    Json created = Json::array();
    for (const auto &row : rows)
    {
        const auto id = row.at("id").get<std::string>();
        auto &profile = profiles_.at(id);
        profile->metadata = catalog_->Profile(id);
        created.push_back(profile->metadata);
    }
    return {{"profiles", created},
            {"atomicCatalog", true},
            {"browserDataImported", false},
            {"automaticRetryAllowed", false}};
}
// 用途：所有入口执行相同契约/范围/租约，未登记功能直接拒绝；日期：2026-11-03。
Json Service::Execute(const std::string &command, const Json &arguments)
{
    try
    {
        if (command == "capabilities" && arguments.is_object() && arguments.empty())
            return Capabilities();
        return credentials_->Redact(ExecuteInternal(command, arguments));
    }
    catch (const Error &error)
    {
        const auto safe = credentials_->Redact(Json{{"message", error.what()}});
        throw Error(error.code, safe.at("message"), error.outcome_unknown);
    }
}
// 用途：内部严格契约派发，外层统一去除凭据回显；日期：2026-11-05。
Json Service::ExecuteInternal(const std::string &command, const Json &arguments)
{
    bool found = false;
    for (const auto &tool : Tools())
        if (tool.at("name") == command)
        {
            ValidateArguments(tool.at("inputSchema"), arguments);
            found = true;
            break;
        }
    if (!found)
        throw Error("UNSUPPORTED_COMMAND", "Command is not implemented");
    if (command == "capabilities")
        return Capabilities();
    if (command == "doctor")
    {
        Json components = Json::array();
        bool healthy = true;
        try
        {
            catalog_->CheckHealthy();
            components.push_back({{"name", "catalog"}, {"state", "ready"}, {"schemaVersion", 7}});
        }
        catch (const Error &error)
        {
            healthy = false;
            components.push_back({{"name", "catalog"}, {"state", "failed"}, {"errorCode", error.code}});
        }
        const bool browser_available =
            !executable_.empty() && GetFileAttributesW(executable_.c_str()) != INVALID_FILE_ATTRIBUTES;
        healthy = healthy && browser_available;
        components.push_back({{"name", "browser-runtime"},
                              {"state", browser_available ? "available" : "missing"},
                              {"handshakeVerified", false}});
        const auto model = model_->Execute("model_status", Json::object());
        components.push_back(
            {{"name", "model-provider"},
             {"state", model.at("configured").get<bool>() ? "configured" : "not-configured"}});
        components.push_back({{"name", "central-control-and-commerce"}, {"state", "not-implemented"}});
        components.push_back(
            {{"name", "credential-vault"}, {"state", "ready"}, {"binding", "current-Windows-user"}});
        Json profiles = Json::array();
        std::vector<std::shared_ptr<Profile>> owned;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (const auto &entry : profiles_)
                owned.push_back(entry.second);
        }
        for (const auto &profile : owned)
        {
            std::lock_guard<std::mutex> lock(profile->mutex);
            if (profile->removed)
                continue;
            const bool running = profile->browser && profile->browser->Status().at("running").get<bool>();
            const auto recovery =
                running ? Json{{"blocked", false}, {"state", "owned-by-current-service"}}
                        : Browser::InspectRecovery(root_ + L"\\" + Wide(profile->metadata.at("id")));
            healthy = healthy && !recovery.value("blocked", false);
            profiles.push_back({{"profileId", profile->metadata.at("id")},
                                {"state", profile->metadata.at("state")},
                                {"running", running},
                                {"recovery", recovery}});
        }
        return {{"healthy", healthy},      {"localOnly", true},
                {"readOnly", true},        {"components", components},
                {"profiles", profiles},    {"quota", {{"state", "unavailable"}, {"enforced", false}}},
                {"productionReady", false}};
    }
    if (command != "browser_stop" && command != "lease_revoke")
        catalog_->CheckHealthy();
    if (command == "account_connect" || command == "account_connect_code")
    {
        if (commercial_->Status(false).at("connected") == false)
        {
            std::vector<std::shared_ptr<Profile>> profiles;
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                for (const auto &entry : profiles_)
                    profiles.push_back(entry.second);
            }
            for (const auto &profile : profiles)
            {
                std::lock_guard<std::mutex> lock(profile->mutex);
                if (profile->browser && profile->browser->Status().value("running", false))
                    throw Error("ACCOUNT_BUSY", "Stop local browsers before enabling commercial admission");
            }
        }
        const auto result = commercial_->Connect(arguments);
        StartCommercialWatch();
        return result;
    }
    if (command == "account_disconnect")
        return commercial_->Disconnect();
    if (command == "account_status")
        return commercial_->Status(arguments.value("remote", false));
    if (command == "account_action")
        return commercial_->Call(arguments.at("action"), arguments.at("body"));
    if (command == "commerce_entitlements" && commercial_->Status(false).at("connected") == true)
        return commercial_->Status(true);
    if (command == "admin_policy")
        return CommercePolicy();
    if (command == "commerce_quote")
        return CommerceQuote(arguments);
    for (const auto *kind : {"group", "template"})
        for (const auto *operation : {"list", "create", "update", "delete"})
            if (command == std::string("profile_") + kind + "_" + operation)
                return catalog_->Library(kind, operation, arguments);
    if (command == "profile_template_history" || command == "profile_template_rollback")
        return catalog_->Library("template", command == "profile_template_history" ? "history" : "rollback",
                                 arguments);
    if (command == "profile_batch_list")
        return catalog_->Batches("list", arguments);
    if (command == "profile_batch_get")
        return catalog_->Batches("get", {{"id", arguments.at("batchId")}});
    if (command == "profile_batch_archive")
        return catalog_->Batches(
            "archive", {{"id", arguments.at("batchId")}, {"confirmBatchId", arguments.at("confirmBatchId")}});
    if (command == "profile_import")
        return CreateConfigurations(arguments.at("bundle").at("configurations"));
    if (command == "profile_template_instantiate")
    {
        const auto resources = catalog_->Library("template", "list", Json::object());
        for (const auto &item : resources.at("items"))
            if (item.at("id") == arguments.at("id"))
            {
                if (item.at("revision") != arguments.at("expectedRevision"))
                    throw Error("REVISION_CONFLICT",
                                "Pin the current template revision before creating a profile");
                auto configuration = item.at("configuration");
                configuration["name"] = arguments.at("name");
                return CreateConfigurations(Json::array({configuration}));
            }
        throw Error("LIBRARY_NOT_FOUND", "Profile template does not exist");
    }
    if (command == "profile_export")
    {
        std::set<std::string> ids;
        Json configurations = Json::array();
        for (const auto &id : arguments.at("profileIds"))
        {
            if (!ids.insert(id.get<std::string>()).second)
                throw Error("INVALID_ARGUMENTS", "Export profile IDs must be unique");
            const auto profile = catalog_->Profile(id);
            if (profile.at("state") == "purging")
                throw Error("PROFILE_STATE_INVALID", "Profile is being permanently removed");
            Json configuration;
            for (const auto *key : {"name", "allowedOrigins", "proxyServer", "runtimeSettings"})
                if (profile.contains(key))
                    configuration[key] = profile.at(key);
            configurations.push_back(Catalog::Configuration(configuration));
        }
        return {{"format", "awb-profile-config-v1"}, {"configurations", configurations}};
    }
    if (command == "profile_batch")
    {
        const std::set<std::string> allowed{"browser_start", "browser_stop", "profile_archive",
                                            "profile_trash", "profile_restore"};
        const auto tools = Tools();
        for (const auto &operation : arguments.at("operations"))
        {
            const auto name = operation.at("command").get<std::string>();
            if (!allowed.count(name))
                throw Error("INVALID_ARGUMENTS", "Batch command is not an allowed lifecycle operation");
            for (const auto &tool : tools)
                if (tool.at("name") == name)
                    ValidateArguments(tool.at("inputSchema"), operation.at("arguments"));
        }
        const auto batch_id = arguments.value("requestId", RandomId());
        if (batch_id.size() != 32 || batch_id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw Error("INVALID_ARGUMENTS", "Batch requestId must be 32 lowercase hexadecimal characters");
        const auto created = catalog_->Batches("begin", {{"id", batch_id},
                                                         {"operations", arguments.at("operations")},
                                                         {"onError", arguments.value("onError", "stop")}});
        if (!created.at("created").get<bool>())
        {
            auto receipt = created.at("batch");
            receipt["receiptOnly"] = true;
            receipt["replayed"] = false;
            return receipt;
        }
        Json results = Json::array();
        bool stopped = false, unknown = false;
        size_t succeeded = 0, failed = 0;
        for (const auto &operation : arguments.at("operations"))
        {
            Json item{{"index", results.size()}, {"command", operation.at("command")}, {"state", "not-run"}};
            if (!stopped)
            {
                catalog_->Batches("dispatch", {{"id", batch_id}, {"index", results.size()}});
                try
                {
                    item["result"] = ExecuteInternal(operation.at("command"), operation.at("arguments"));
                    item["state"] = "succeeded";
                    ++succeeded;
                }
                catch (const Error &error)
                {
                    item["state"] = error.outcome_unknown ? "outcome-unknown" : "failed";
                    item["error"] = ErrorResult(error).at("error");
                    ++failed;
                    unknown = error.outcome_unknown;
                    stopped = unknown || arguments.value("onError", "stop") == "stop";
                }
                catch (const std::exception &)
                {
                    item["state"] = "outcome-unknown";
                    item["error"] = {{"code", "BATCH_ACTION_EXCEPTION"}, {"outcomeUnknown", true}};
                    ++failed;
                    unknown = stopped = true;
                }
                try
                {
                    Json completion{{"id", batch_id}, {"index", results.size()}, {"state", item.at("state")}};
                    if (item.contains("error"))
                        completion["errorCode"] = item.at("error").at("code");
                    catalog_->Batches("complete", completion);
                }
                catch (const Error &)
                {
                    throw Error(
                        "BATCH_JOURNAL_UNCONFIRMED",
                        "An action was dispatched but its receipt could not be confirmed; inspect batch ID " +
                            batch_id + " after restarting; never replay automatically",
                        true);
                }
            }
            results.push_back(item);
        }
        catalog_->Batches("finish", {{"id", batch_id}});
        return {{"batchId", batch_id},
                {"state", unknown ? "outcome-unknown" : failed ? "partial" : "succeeded"},
                {"items", results},
                {"succeeded", succeeded},
                {"failed", failed},
                {"notRun", results.size() - succeeded - failed},
                {"stopped", stopped},
                {"atomic", false},
                {"rollbackPerformed", false},
                {"automaticRetryAllowed", false}};
    }
    if (command == "admin_summary" || command == "commerce_entitlements")
    {
        const auto metadata = catalog_->Profiles(true);
        size_t active = 0;
        size_t archived = 0;
        size_t trashed = 0;
        for (const auto &profile : metadata)
        {
            const auto state = profile.value("state", "active");
            if (state == "active")
                ++active;
            else if (state == "archived")
                ++archived;
            else if (state == "trashed")
                ++trashed;
        }
        size_t running = 0;
        {
            std::vector<std::shared_ptr<Profile>> owned;
            {
                std::lock_guard<std::mutex> registry_lock(registry_mutex_);
                for (const auto &entry : profiles_)
                    owned.push_back(entry.second);
            }
            for (const auto &profile : owned)
            {
                std::lock_guard<std::mutex> profile_lock(profile->mutex);
                if (!profile->removed && profile->browser &&
                    profile->browser->Status().value("running", false))
                    ++running;
            }
        }
        const auto policy = CommercePolicy();
        const bool within_free = active <= policy.at("freeEnvironmentLimit").get<size_t>() &&
                                 running <= policy.at("freeConcurrentLimit").get<size_t>();
        return {{"scope", "local-development-root"},
                {"profiles", {{"active", active}, {"archived", archived}, {"trashed", trashed}}},
                {"runningBrowsers", running},
                {"freeEntitlement", within_free ? "available" : "exhausted"},
                {"policy", policy},
                {"centralSync", false},
                {"formalBilling", false},
                {"productionReady", false}};
    }
    if (command.compare(0, 11, "credential_") == 0)
        return credentials_->Execute(command, arguments);
    if (command.compare(0, 6, "model_") == 0)
        return model_->Execute(command, arguments);
    if (command.compare(0, 13, "site_package_") == 0)
        return site_packages_->Execute(command, arguments);
    if (command == "workflow_validate")
        return ValidateWorkflow(arguments.at("definition"),
                                arguments.contains("input") ? &arguments.at("input") : nullptr);
    if (command == "workflow_publish")
        return Workflows().Publish(arguments.at("definition"));
    if (command == "workflow_list")
        return Workflows().List();
    if (command == "workflow_get")
        return Workflows().Definition(arguments.at("workflowId"));
    if (command == "workflow_status")
        return Workflows().Status(arguments.at("runId"));
    if (command == "workflow_runs")
        return Workflows().Runs();
    if (command == "workflow_control")
        return Workflows().Control(arguments.at("runId"), arguments.at("operation"));
    if (command == "workflow_debug")
        return Workflows().Debug(arguments.at("runId"), arguments.at("options"));
    if (command == "profile_list")
    {
        Json list = Json::array();
        const auto state = arguments.value("state", "active");
        for (const auto &metadata : catalog_->Profiles(true))
            if (state == "all" || metadata.at("state") == state)
                list.push_back(metadata);
        return {{"profiles", list}, {"scope", "local-development-root"}};
    }
    if (command == "profile_create")
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        std::vector<std::string> origins;
        std::set<std::string> unique_origins;
        for (const auto &origin : arguments.at("allowedOrigins"))
        {
            const auto normalized = Browser::Origin(origin.get<std::string>());
            if (unique_origins.insert(normalized).second)
                origins.push_back(normalized);
        }
        const auto proxy_server = arguments.value("proxyServer", "");
        Browser::ValidateProxyServer(proxy_server);
        const auto id = RandomId();
        const auto directory = root_ + L"\\" + Wide(id);
        PrivateDirectory(directory);
        auto profile = std::make_shared<Profile>();
        profile->metadata = {{"id", id}, {"name", arguments.at("name")}, {"allowedOrigins", origins}};
        if (!proxy_server.empty())
            profile->metadata["proxyServer"] = proxy_server;
        if (arguments.contains("runtimeSettings"))
            profile->metadata["runtimeSettings"] = arguments.at("runtimeSettings");
        profiles_.emplace(id, profile);
        try
        {
            catalog_->InsertProfile(profile->metadata);
            profile->metadata = catalog_->Profile(id);
        }
        catch (...)
        {
            profiles_.erase(id);
            RemoveDirectoryW(directory.c_str());
            throw;
        }
        return profile->metadata;
    }
    const auto profile = Find(arguments.at("profileId").get<std::string>());
    if (command == "lease_revoke")
        return profile->gate.Revoke();
    {
        std::lock_guard<std::mutex> workflow_lock(workflows_mutex_);
        if (workflows_ && workflows_->OwnsProfile(arguments.at("profileId")) && command != "browser_status" &&
            command != "browser_stop" && command != "browser_diagnostics_read")
            throw Error("WORKFLOW_PROFILE_BUSY", "Cancel the active workflow before another controller acts; "
                                                 "revocation and stop remain available");
    }
    std::lock_guard<std::mutex> lock(profile->mutex);
    {
        // 初次检查后可能等待了环境锁；在真正操作前再次核对流程归属。
        std::lock_guard<std::mutex> workflow_lock(workflows_mutex_);
        if (workflows_ && workflows_->OwnsProfile(arguments.at("profileId")) && command != "browser_status" &&
            command != "browser_stop" && command != "browser_diagnostics_read")
            throw Error("WORKFLOW_PROFILE_BUSY",
                        "An active workflow acquired this profile while the request waited");
    }
    if (profile->removed)
        throw Error("PROFILE_NOT_FOUND", "Profile has been permanently removed");
    if (command == "profile_update" || command == "profile_archive" || command == "profile_trash" ||
        command == "profile_restore" || command == "profile_purge")
    {
        if (profile->browser && profile->browser->Status().at("running").get<bool>())
            throw Error("PROFILE_RUNNING", "Stop the browser before changing or deleting this profile");
        if (Browser::InspectRecovery(root_ + L"\\" + Wide(profile->metadata.at("id")))
                .value("blocked", false))
            throw Error("PROCESS_OWNERSHIP_UNCONFIRMED",
                        "Resolve the recorded process before changing profile data");
        const auto revision = arguments.at("expectedRevision").get<long long>();
        const auto id = arguments.at("profileId").get<std::string>();
        if (command == "profile_purge")
        {
            if (arguments.at("confirmProfileId") != id)
                throw Error("PROFILE_CONFIRMATION_REQUIRED",
                            "Confirmation must exactly match the profile ID");
            if (profile->metadata.at("revision") != revision)
                throw Error("REVISION_CONFLICT", "Refresh the profile before permanently deleting it");
            if (profile->metadata.at("state") != "purging")
                profile->metadata = catalog_->ChangeProfile(id, revision, Json::object(), "purging");
            profile->gate.Revoke();
            profile->browser.reset();
            commercial_->Stop(id);
            RemoveProfileDirectory(root_, id);
            catalog_->FinishPurge(id, profile->metadata.at("revision"));
            profile->removed = true;
            std::lock_guard<std::mutex> registry_lock(registry_mutex_);
            profiles_.erase(id);
            return {{"id", id}, {"purged", true}};
        }
        Json patch = arguments;
        patch.erase("profileId");
        patch.erase("expectedRevision");
        if (command == "profile_update" && patch.empty())
            throw Error("INVALID_ARGUMENTS", "Update requires at least one configuration field");
        const auto state = command == "profile_archive"   ? "archived"
                           : command == "profile_trash"   ? "trashed"
                           : command == "profile_restore" ? "active"
                                                          : "";
        auto changed = catalog_->ChangeProfile(id, revision, patch, state);
        profile->gate.Revoke();
        profile->browser.reset();
        profile->metadata = changed;
        return changed;
    }
    if (command != "browser_status" && command != "browser_stop" && profile->metadata.at("state") != "active")
        throw Error("PROFILE_STATE_INVALID", "Restore the profile to active before using it");
    if (command == "lease_acquire")
        return profile->gate.Acquire(arguments.at("owner"), arguments.at("ttlMs"));
    if (command == "workflow_run")
    {
        if (!profile->browser || !profile->browser->Status().at("running").get<bool>())
            throw Error("BROWSER_NOT_RUNNING", "Start the managed browser before running a workflow");
        if (profile->browser->RecordingActive())
            throw Error("RECORDING_ACTIVE", "Stop recording before replaying a workflow on the same profile");
        const auto lease = arguments.at("lease");
        // 用途：启动前验证当前租约，不以未来线程的存在替代控制权；日期：2026-11-04。
        profile->gate.Dispatch(lease, [] {});
        const auto definition = Workflows().Definition(arguments.at("workflowId")).at("definition");
        std::set<std::string> origins;
        for (const auto &origin : definition.at("policy").at("allowedOrigins"))
            origins.insert(Browser::Origin(origin));
        const auto profile_origins = profile->metadata.at("allowedOrigins").get<std::vector<std::string>>();
        if (origins != std::set<std::string>(profile_origins.begin(), profile_origins.end()))
            throw Error("WORKFLOW_SCOPE_MISMATCH",
                        "Workflow and profile must have exactly the same normalized origin scope");
        // 用途：只向执行线程提供必要输入，原始值不进入检查点/状态结果；日期：2026-11-04。
        return Workflows().Start(
            arguments.at("workflowId"), arguments.at("input"), arguments.at("profileId"),
            [this, profile, lease](const std::string &type, const Json &step)
            { return WorkflowStep(profile, lease, type, step); },
            arguments.value("debug", Json::object()));
    }
    if (command == "browser_start")
    {
        if (remote_gate_)
            remote_gate_([] {});
        if (profile->browser && profile->browser->Status().at("running").get<bool>())
            throw Error("PROFILE_ALREADY_RUNNING", "This profile already owns a browser");
        profile->gate.Revoke();
        profile->browser.reset();
        commercial_->Start(profile->metadata.at("id"));
        try
        {
            profile->browser = std::make_unique<Browser>(
                executable_, root_ + L"\\" + Wide(profile->metadata.at("id")),
                arguments.value("headless", false),
                profile->metadata.at("allowedOrigins").get<std::vector<std::string>>(),
                profile->metadata.value("proxyServer", ""),
                profile->metadata.value("runtimeSettings", Json::object()));
        }
        catch (...)
        {
            // 构造失败只在确认无残留进程时释放准入。
            if (!Browser::InspectRecovery(root_ + L"\\" + Wide(profile->metadata.at("id")))
                     .value("blocked", false))
            {
                try
                {
                    commercial_->Stop(profile->metadata.at("id"));
                }
                catch (...)
                {
                }
            }
            throw;
        }
        return profile->browser->Status();
    }
    if (command == "browser_status")
    {
        auto status = profile->browser ? profile->browser->Status() : Json{{"running", false}};
        status["recovery"] = profile->browser && status.at("running").get<bool>()
                                 ? Json{{"state", "owned-by-current-service"}, {"blocked", false}}
                                 : Browser::InspectRecovery(root_ + L"\\" + Wide(profile->metadata.at("id")));
        return status;
    }
    if (command == "browser_stop")
    {
        profile->gate.Revoke();
        if (!profile->browser)
        {
            if (Browser::InspectRecovery(root_ + L"\\" + Wide(profile->metadata.at("id")))
                    .value("blocked", false))
                throw Error("PROCESS_OWNERSHIP_UNCONFIRMED",
                            "Recorded process is not owned by this service; automatic stop refused");
            commercial_->Stop(profile->metadata.at("id"));
            return {{"stopped", true}, {"alreadyStopped", true}};
        }
        const auto stopped = profile->browser->Stop();
        commercial_->Stop(profile->metadata.at("id"));
        return stopped;
    }
    if (!profile->browser || !profile->browser->Status().at("running").get<bool>())
        throw Error("BROWSER_NOT_RUNNING", "Start the managed browser first");
    if (command == "page_pick_stop")
    {
        profile->browser->SetDispatchGate(nullptr);
        return profile->browser->PickerStop();
    }
    commercial_->Check(profile->metadata.at("id"));
    const auto lease = arguments.at("lease");
    // 用途：在每条CDP消息发送前原子验证当前租约；日期：2026-11-03。
    const std::weak_ptr<Profile> weak_profile(profile);
    profile->browser->SetDispatchGate(
        [this, weak_profile, lease](const std::function<void()> &send)
        {
            const auto current = weak_profile.lock();
            if (!current)
                throw Error("PROFILE_NOT_FOUND", "Profile was released");
            current->gate.Dispatch(lease,
                                   [&]
                                   {
                                       if (remote_gate_)
                                           remote_gate_(send);
                                       else
                                           send();
                                   });
        });
    // 用途：入口先检查，空发送回调不产生网页动作；日期：2026-11-03。
    profile->gate.Dispatch(lease, []() {});
    auto &browser = *profile->browser;
    if (browser.RecordingActive() && (command == "page_open" || command == "page_switch" ||
                                      command == "page_close" || command == "browser_reconnect"))
        throw Error("RECORDING_PAGE_LOCKED",
                    "Stop the main-document recording before changing page sessions");
    if (command == "recorder_start")
        return browser.RecordingStart();
    if (command == "page_pick_start")
        return browser.PickerStart();
    if (command == "page_pick_read")
        return browser.PickerRead();
    if (command == "recorder_read")
        return browser.RecordingRead();
    if (command == "recorder_control")
        return browser.RecordingControl(arguments.at("operation"));
    if (command == "browser_diagnostics_start")
        return browser.DiagnosticsStart(arguments.value("routePaths", std::vector<std::string>{}));
    if (command == "browser_diagnostics_read")
        return browser.DiagnosticsRead(arguments.value("after", 0ULL), arguments.value("epoch", ""),
                                       arguments.value("limit", size_t(50)));
    if (command == "browser_diagnostics_stop")
        return browser.DiagnosticsStop();
    // 批次已逐项设置诊断屏障；结束后不能再派发CDP而丢失撤销前的部分结果。
    if (command == "page_batch")
        return browser.Batch(arguments.at("actions"), arguments.value("onError", "stop") == "stop");
    // 用途：交互动作获得独立观察标识，输入值和租约不进入上下文；日期：2026-11-04。
    return browser.Diagnose(
        {{"actionId", RandomId()}, {"command", command}},
        [&]() -> Json
        {
            if (command == "browser_navigate")
                return browser.Navigate(arguments.at("url"));
            if (command == "page_snapshot")
                return browser.Snapshot();
            if (command == "page_extract")
                return browser.Extract(arguments.at("schema"), arguments.at("bindings"));
            if (command == "page_locate")
                return browser.Locate(arguments.at("selector"));
            if (command == "page_discover")
                return browser.Discover(arguments.value("query", ""), arguments.value("role", ""),
                                        arguments.value("limit", size_t{10}));
            if (command == "workflow_repair_draft")
            {
                auto draft = arguments.at("definition");
                const auto source = ValidateWorkflow(draft);
                if (!source.at("valid").get<bool>())
                    throw Error("WORKFLOW_REPAIR_SOURCE_INVALID",
                                "Repair requires a valid immutable source definition");
                if (source.at("sha256") != arguments.at("expectedSha256"))
                    throw Error("REVISION_CONFLICT", "Repair source hash differs from the reviewed source");
                std::set<std::string> origins;
                for (const auto &origin : draft.at("policy").at("allowedOrigins"))
                    origins.insert(Browser::Origin(origin));
                const auto allowed = profile->metadata.at("allowedOrigins").get<std::vector<std::string>>();
                if (origins != std::set<std::string>(allowed.begin(), allowed.end()))
                    throw Error("WORKFLOW_SCOPE_MISMATCH", "Repair source and profile scopes differ");
                const auto flow = arguments.value("flow", "");
                if (!flow.empty() && (!draft.contains("subflows") || !draft.at("subflows").contains(flow)))
                    throw Error("WORKFLOW_REPAIR_STEP_NOT_FOUND", "Repair subflow does not exist");
                auto &steps = flow.empty() ? draft["steps"] : draft["subflows"][flow]["steps"];
                for (auto &step : steps)
                {
                    if (step.at("id") != arguments.at("stepId"))
                        continue;
                    if (!step.contains("selector"))
                        throw Error("WORKFLOW_REPAIR_UNSUPPORTED",
                                    "Only an existing selector can be repaired");
                    const auto before = step.at("selector");
                    step["selector"] = arguments.at("selector");
                    const auto validation = ValidateWorkflow(draft);
                    if (!validation.at("valid").get<bool>())
                        throw Error("WORKFLOW_REPAIR_INVALID", "Replacement failed the shared validator");
                    if (credentials_->Redact(draft) != draft)
                        throw Error("WORKFLOW_REPAIR_SECRET_DETECTED", "Draft contains a stored secret");
                    const auto observed = browser.Inspect(arguments.at("selector"), false);
                    return {{"draft", draft},
                            {"sourceSha256", source.at("sha256")},
                            {"validation", validation},
                            {"change",
                             {{"flow", flow},
                              {"stepId", step.at("id")},
                              {"before", before},
                              {"after", step.at("selector")}}},
                            {"inspection", observed},
                            {"requiresReview", true},
                            {"published", false},
                            {"executed", false}};
                }
                throw Error("WORKFLOW_REPAIR_STEP_NOT_FOUND", "Repair node does not exist");
            }
            if (command == "page_inspect")
                return browser.Inspect(arguments.at("selector"), arguments.value("highlight", false));
            if (command == "page_preview")
                return browser.Preview(arguments.at("ref"), arguments.at("action"));
            if (command == "page_clear_highlight")
                return browser.ClearHighlight();
            if (command == "page_fill")
                return browser.Fill(arguments.at("ref"), arguments.at("value"));
            if (command == "page_upload")
                return browser.Upload(arguments.at("ref"), Wide(arguments.at("path")));
            if (command == "page_download_config")
                return browser.ConfigureDownloads(Wide(arguments.at("directory")));
            if (command == "page_download_wait")
                return browser.WaitDownload(arguments.at("fileName"), arguments.at("timeoutMs"));
            if (command == "page_fill_secret")
            {
                const auto origin = browser.CurrentOrigin();
                // 用途：同步回调只接受本环境/来源的临时值，且沿用逐消息租约围栏；日期：2026-11-05。
                return credentials_->Use(arguments.at("credentialId"), arguments.at("profileId"), origin,
                                         "browser",
                                         [&](const std::string &value)
                                         { return browser.FillSecret(arguments.at("ref"), value, origin); });
            }
            if (command == "page_click")
                return browser.Click(arguments.at("ref"));
            if (command == "page_select")
                return browser.Select(arguments.at("ref"),
                                      arguments.at("values").get<std::vector<std::string>>());
            if (command == "page_text")
                return browser.Text(arguments.at("ref"));
            if (command == "page_wait_text")
                return browser.WaitText(arguments.at("selector"), arguments.at("expected"),
                                        arguments.at("timeoutMs"));
            if (command == "page_assert")
                return browser.Assert(arguments.at("assertion"));
            if (command == "page_screenshot")
                return browser.Screenshot();
            if (command == "page_list")
                return browser.Pages();
            if (command == "page_open")
                return browser.OpenPage(arguments.at("url"));
            if (command == "page_switch")
                return browser.SwitchPage(arguments.at("pageId"));
            if (command == "page_close")
                return browser.ClosePage(arguments.at("pageId"));
            if (command == "browser_events")
                return browser.Events(arguments.value("after", 0ULL), arguments.value("eventEpoch", ""),
                                      arguments.value("limit", size_t(50)));
            if (command == "browser_reconnect")
                return browser.Reconnect();
            throw Error("UNSUPPORTED_COMMAND", "No command handler exists");
        });
}
// 用途：逐字节限制协议行长度，禁止无界getline占满内存；日期：2026-11-03。
bool ReadProtocolLine(std::istream &stream, std::string &line)
{
    line.clear();
    char character = 0;
    while (stream.get(character))
    {
        if (character == '\n')
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }
        if (line.size() >= 1024 * 1024)
            throw Error("INPUT_TOO_LARGE", "Protocol message exceeds 1 MiB");
        line.push_back(character);
    }
    return !line.empty();
}
} // namespace awb
