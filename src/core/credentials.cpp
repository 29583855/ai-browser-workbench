#include "awb/credentials.hpp"
#include "awb/browser.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <wincrypt.h>

namespace awb
{
namespace
{
constexpr size_t MaximumCredentials = 128;
constexpr size_t MaximumAudit = 512;
// 用途：取得UTC毫秒用于凭据过期和无业务内容审计；日期：2026-11-05。
long long Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
// 用途：在作用域退出时清零解密临时字节，不承诺清除宿主或网页的副本；日期：2026-11-05。
struct Sensitive
{
    std::string value;
    // 用途：接管敏感临时缓冲区；日期：2026-11-05。
    explicit Sensitive(std::string input) : value(std::move(input))
    {
    }
    // 用途：释放前尽力清零当前明文缓冲区；日期：2026-11-05。
    ~Sensitive()
    {
        if (!value.empty())
            SecureZeroMemory(&value[0], value.size());
    }
};
// 用途：使用用户DPAPI及固定格式熵保护数据，系统错误不带输入；日期：2026-11-05。
std::string Protect(const std::string &bytes, bool decrypt)
{
    DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(bytes.data()))};
    const std::string context = "AI-Browser-Workbench-credential-v1";
    DATA_BLOB entropy{static_cast<DWORD>(context.size()),
                      reinterpret_cast<BYTE *>(const_cast<char *>(context.data()))};
    DATA_BLOB output{};
    const auto ok = decrypt ? CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                                                 CRYPTPROTECT_UI_FORBIDDEN, &output)
                            : CryptProtectData(&input, L"AWB credentials v1", &entropy, nullptr, nullptr,
                                               CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok)
        throw Error("CREDENTIAL_CRYPTO_FAILED", "DPAPI operation failed for this Windows user");
    const std::string result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
// 用途：编码DPAPI密文为普通JSON字段，明文不参与日志；日期：2026-11-05。
std::string Hex(const std::string &bytes)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes)
    {
        result.push_back(alphabet[static_cast<unsigned char>(byte) >> 4]);
        result.push_back(alphabet[static_cast<unsigned char>(byte) & 15]);
    }
    return result;
}
// 用途：严格解析有界密文字段，拒绝损坏、奇数长度或非十六进制；日期：2026-11-05。
std::string Unhex(const std::string &value)
{
    if (value.empty() || value.size() % 2 || value.size() > 600000 ||
        value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw Error("CREDENTIAL_STORAGE_INVALID", "Encrypted credential field is invalid");
    std::string result;
    for (size_t index = 0; index < value.size(); index += 2)
        result.push_back(static_cast<char>(std::stoul(value.substr(index, 2), nullptr, 16)));
    return result;
}
// 用途：限制凭据标识为生成的ID，阻止路径和名称冒充引用；日期：2026-11-05。
void Id(const std::string &value)
{
    if (value.size() != 32 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw Error("CREDENTIAL_ID_INVALID", "Expected a generated credential ID");
}
// 用途：仅从凭据条目返回元数据，密文也不向调用者输出；日期：2026-11-05。
Json Metadata(Json value)
{
    value.erase("ciphertext");
    value["secretAvailable"] = true;
    return value;
}
// 用途：替换完整敏感子串，包括回显在URL、名称或错误文本中的情况；日期：2026-11-05。
std::string Scrub(std::string value, const std::vector<std::string> &secrets)
{
    for (const auto &secret : secrets)
    {
        size_t index = 0;
        while ((index = value.find(secret, index)) != std::string::npos)
        {
            value.replace(index, secret.size(), "[REDACTED]");
            index += 10;
        }
    }
    return value;
}
// 用途：保留公开协议身份和固定枚举，短口令不会破坏控制句柄；业务正文仍逐子串脱敏；日期：2026-11-05。
bool PublicMetadata(const std::string &key, const std::string &value)
{
    if (key == "ref" && value.size() >= 35 && value.front() == 'e' && value[33] == '-' &&
        value.substr(1, 32).find_first_not_of("0123456789abcdefABCDEF") == std::string::npos &&
        value.substr(34).find_first_not_of("0123456789") == std::string::npos)
        return true;
    if ((key == "code" || key == "errorCode") && !value.empty() && value.size() <= 128 &&
        value.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") == std::string::npos)
        return true;
    const std::set<std::string> identities{"id",         "credentialId", "profileId", "workflowId",  "runId",
                                           "pageId",     "frameId",      "loaderId",  "token",       "epoch",
                                           "eventEpoch", "ref",          "sha256",    "sourceSha256"};
    if (identities.count(key) && (value.size() == 32 || value.size() == 64) &&
        value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        return true;
    const std::map<std::string, std::set<std::string>> choices{
        {"schemaVersion", {"1.0", "1.1"}},
        {"purpose", {"browser", "model"}},
        {"type",
         {"object", "array", "string", "integer", "boolean", "navigate", "fill", "click", "select",
          "assertText", "set", "readText", "foreach", "condition", "repeat", "call", "end"}},
        {"state",
         {"active",         "archived",        "trashed",    "purging",   "running",     "queued",
          "paused",         "succeeded",       "failed",     "cancelled", "interrupted", "outcome-unknown",
          "ready",          "stopped",         "picking",    "picked",    "reserved",    "configured",
          "not-configured", "not-implemented", "unavailable"}},
        {"status", {"pending", "running", "succeeded", "failed", "handled-error", "cancelled"}}};
    const auto found = choices.find(key);
    return found != choices.end() && found->second.count(value) != 0;
}
// 用途：字段名视为公开契约，任意业务值脱敏；显式业务容器不享受身份豁免；日期：2026-11-05。
Json ScrubJson(Json value, const std::vector<std::string> &secrets, const std::string &key = "",
               bool payload = false)
{
    if (value.is_string())
        return !payload && PublicMetadata(key, value.get_ref<const std::string &>())
                   ? value
                   : Json(Scrub(value.get<std::string>(), secrets));
    if (value.is_object())
    {
        Json result = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            result[payload ? Scrub(it.key(), secrets) : it.key()] =
                ScrubJson(it.value(), secrets, it.key(),
                          payload || it.key() == "data" || it.key() == "value" || it.key() == "literal" ||
                              it.key() == "initial");
        return result;
    }
    if (value.is_array())
        for (auto &item : value)
            item = ScrubJson(std::move(item), secrets, key, payload);
    return value;
}
} // namespace

// 用途：读取普通独占文件并验证加密库格式和所有条目；日期：2026-11-05。
CredentialVault::CredentialVault(const std::wstring &root)
    : path_(SafeAbsolute(root + L"\\credentials.dpapi"))
{
    document_ = {{"format", 1}, {"credentials", Json::object()}, {"audit", Json::array()}, {"sequence", 0}};
    if (GetFileAttributesW((L"\\\\?\\" + path_).c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    Handle file(CreateFileW((L"\\\\?\\" + path_).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (file.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.get(), &info) ||
        info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        !GetFileSizeEx(file.get(), &length) || length.QuadPart < 1 || length.QuadPart > 20000000)
        throw Error("CREDENTIAL_STORAGE_INVALID", "Cannot read an ordinary bounded credential store");
    std::string encrypted(static_cast<size_t>(length.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), &encrypted[0], static_cast<DWORD>(encrypted.size()), &read, nullptr) ||
        read != encrypted.size())
        throw Error("CREDENTIAL_STORAGE_INVALID", "Cannot read complete credential store");
    Sensitive decoded(Protect(encrypted, true));
    try
    {
        document_ = ParseExternalJson(decoded.value, 12);
        if (document_.size() != 4 || document_.at("format") != 1 ||
            !document_.at("credentials").is_object() ||
            document_.at("credentials").size() > MaximumCredentials || !document_.at("audit").is_array() ||
            document_.at("audit").size() > MaximumAudit || !document_.at("sequence").is_number_unsigned())
            throw Error("CREDENTIAL_STORAGE_INVALID", "Credential store format is invalid");
        for (auto it = document_.at("credentials").begin(); it != document_.at("credentials").end(); ++it)
        {
            Id(it.key());
            if (it.value().at("credentialId") != it.key())
                throw Error("CREDENTIAL_STORAGE_INVALID", "Credential identity is invalid");
            Sensitive secret(Protect(Unhex(it.value().at("ciphertext")), true));
            Remember(secret.value);
        }
    }
    catch (const Json::exception &)
    {
        throw Error("CREDENTIAL_STORAGE_INVALID", "Credential store fields are invalid");
    }
}
// 用途：尽力清除本对象脱敏字典中的敏感值；日期：2026-11-05。
CredentialVault::~CredentialVault()
{
    for (auto &value : redactions_)
        if (!value.empty())
            SecureZeroMemory(&value[0], value.size());
}
// 用途：原子提交整个加密文档并在成功后更新内存；日期：2026-11-05。
void CredentialVault::Commit(const Json &next)
{
    Sensitive serialized(next.dump());
    AtomicWrite(path_, Protect(serialized.value, false));
    document_ = next;
}
// 用途：合并重复敏感值，限制轮换总量防止脱敏字典无界增长；日期：2026-11-05。
void CredentialVault::Remember(const std::string &value)
{
    if (value.empty() || std::find(redactions_.begin(), redactions_.end(), value) != redactions_.end())
        return;
    if (redactions_.size() >= 1024)
        throw Error("CREDENTIAL_REDACTION_BUDGET",
                    "Restart after completing active work to rotate more credentials");
    redactions_.push_back(value);
    // 用途：先替换长秘密，避免短秘密与长秘密重叠时留下未遮盖的尾部；日期：2026-11-05。
    std::sort(redactions_.begin(), redactions_.end(),
              [](const std::string &a, const std::string &b) { return a.size() > b.size(); });
}
// 用途：审计不含名称、来源路径、输入和秘密，序号单调并报告截断；日期：2026-11-05。
void CredentialVault::Audit(Json &next, const std::string &operation, const std::string &id,
                            const std::string &result)
{
    next["sequence"] = next.at("sequence").get<unsigned long long>() + 1;
    auto &audit = next["audit"];
    audit.push_back({{"sequence", next.at("sequence")},
                     {"timestampMs", Now()},
                     {"operation", operation},
                     {"credentialId", id},
                     {"result", result}});
    if (audit.size() > MaximumAudit)
        audit.erase(audit.begin());
}
// 用途：提供受控凭据生命周期和有界审计，所有变更均带乐观版本；日期：2026-11-05。
Json CredentialVault::Execute(const std::string &command, const Json &arguments)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (command == "credential_list")
    {
        Json entries = Json::array();
        for (const auto &entry : document_.at("credentials"))
            entries.push_back(Metadata(entry));
        return {{"credentials", entries},
                {"protection", "DPAPI-current-user"},
                {"maximumCredentials", MaximumCredentials}};
    }
    if (command == "credential_audit")
    {
        const auto after = arguments.value("after", 0ULL);
        Json entries = Json::array();
        for (const auto &entry : document_.at("audit"))
            if (entry.at("sequence").get<unsigned long long>() > after)
                entries.push_back(entry);
        return {{"events", entries},
                {"lastSequence", document_.at("sequence")},
                {"truncatedBefore",
                 document_.at("audit").empty() ? Json(0) : document_.at("audit")[0].at("sequence")}};
    }
    auto next = document_;
    auto &entries = next["credentials"];
    std::string id;
    if (command == "credential_create")
    {
        if (entries.size() >= MaximumCredentials)
            throw Error("CREDENTIAL_CAPACITY", "Credential capacity reached");
        id = RandomId();
        std::set<std::string> origins;
        for (const auto &origin : arguments.at("allowedOrigins"))
            origins.insert(Browser::Origin(origin));
        std::set<std::string> profiles;
        for (const auto &profile : arguments.at("profileIds"))
        {
            Id(profile);
            profiles.insert(profile);
        }
        const auto purpose = arguments.at("purpose").get<std::string>();
        if (purpose == "browser" && profiles.empty())
            throw Error("CREDENTIAL_SCOPE_REQUIRED", "Browser credentials require at least one profile");
        const auto expires = arguments.value("expiresAtMs", 0LL);
        if (expires && expires <= Now())
            throw Error("CREDENTIAL_EXPIRED", "Credential expiry must be in the future");
        Sensitive secret(arguments.at("value").get<std::string>());
        Remember(secret.value);
        entries[id] = {{"credentialId", id},     {"name", arguments.at("name")},
                       {"purpose", purpose},     {"allowedOrigins", origins},
                       {"profileIds", profiles}, {"expiresAtMs", expires},
                       {"revision", 1},          {"ciphertext", Hex(Protect(secret.value, false))}};
    }
    else
    {
        id = arguments.at("credentialId").get<std::string>();
        Id(id);
        if (!entries.contains(id))
            throw Error("CREDENTIAL_NOT_FOUND", "Credential is not owned by this vault");
        if (entries.at(id).at("revision") != arguments.at("expectedRevision"))
            throw Error("REVISION_CONFLICT", "Refresh credential metadata before changing it");
        if (command == "credential_delete")
        {
            entries.erase(id);
            Audit(next, "delete", id, "committed");
            Commit(next);
            return {{"credentialId", id}, {"deleted", true}};
        }
        if (command != "credential_rotate")
            throw Error("UNSUPPORTED_COMMAND", "Credential operation is not registered");
        Sensitive secret(arguments.at("value").get<std::string>());
        Remember(secret.value);
        entries[id]["ciphertext"] = Hex(Protect(secret.value, false));
        entries[id]["revision"] = entries[id].at("revision").get<long long>() + 1;
        if (arguments.contains("expiresAtMs"))
        {
            const auto expires = arguments.at("expiresAtMs").get<long long>();
            if (expires && expires <= Now())
                throw Error("CREDENTIAL_EXPIRED", "Credential expiry must be in the future");
            entries[id]["expiresAtMs"] = expires;
        }
    }
    const auto result = Metadata(entries.at(id));
    Audit(next, command == "credential_create" ? "create" : "rotate", id, "committed");
    Commit(next);
    return result;
}
// 用途：凭据仅在显式用途与实际页面来源匹配时解密，审计提交失败时不派发动作；日期：2026-11-05。
Json CredentialVault::Use(const std::string &id, const std::string &profile, const std::string &origin,
                          const std::string &purpose,
                          const std::function<Json(const std::string &)> &operation)
{
    Id(id);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto &entries = document_.at("credentials");
    if (!entries.contains(id))
        throw Error("CREDENTIAL_NOT_FOUND", "Credential is not owned by this vault");
    const auto entry = entries.at(id);
    const auto normalized = Browser::Origin(origin);
    const auto &origins = entry.at("allowedOrigins");
    const auto &profiles = entry.at("profileIds");
    const auto expires = entry.at("expiresAtMs").get<long long>();
    const bool accepted =
        entry.at("purpose") == purpose &&
        std::find(origins.begin(), origins.end(), normalized) != origins.end() &&
        (purpose != "browser" || std::find(profiles.begin(), profiles.end(), profile) != profiles.end()) &&
        (!expires || expires > Now());
    auto next = document_;
    Audit(next, "use", id, accepted ? "authorized" : "denied");
    Commit(next);
    if (!accepted)
        throw Error(expires && expires <= Now() ? "CREDENTIAL_EXPIRED" : "CREDENTIAL_SCOPE_DENIED",
                    "Credential is expired or does not grant this origin, profile and purpose");
    Sensitive secret(Protect(Unhex(entry.at("ciphertext")), true));
    Remember(secret.value);
    // 使用时持锁，确保撤销/轮换返回后旧值不会开始新动作；回调不能递归访问凭据库。
    return operation(secret.value);
}
// 用途：在统一服务出口脱敏结构化结果，秘密从不通过查询接口返回；日期：2026-11-05。
Json CredentialVault::Redact(Json value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ScrubJson(std::move(value), redactions_);
}
// 用途：声明严格的凭据契约，CLI/MCP/HTTP和桌面共用；日期：2026-11-05。
void CredentialVault::Register(Json &tools)
{
    const Json id{{"type", "string"}, {"minLength", 32}, {"maxLength", 32}};
    const Json value{{"type", "string"}, {"minLength", 1}, {"maxLength", 65536}};
    const Json expiry{{"type", "integer"}, {"minimum", 0}, {"maximum", 32503680000000LL}};
    const Json revision{{"type", "integer"}, {"minimum", 1}};
    // 用途：登记一个只允许显式字段的凭据操作；日期：2026-11-05。
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
    add("credential_create",
        "Store a scoped secret protected by Windows current-user DPAPI; never returns its value",
        {{"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
         {"value", value},
         {"purpose", {{"type", "string"}, {"enum", Json::array({"browser", "model"})}}},
         {"allowedOrigins",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 32},
           {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 8192}}}}},
         {"profileIds", {{"type", "array"}, {"minItems", 0}, {"maxItems", 32}, {"items", id}}},
         {"expiresAtMs", expiry}},
        Json::array({"name", "value", "purpose", "allowedOrigins", "profileIds"}));
    add("credential_list", "List credential metadata only; no plaintext or ciphertext", Json::object(),
        Json::array());
    add("credential_rotate",
        "Rotate a secret using optimistic revision control; existing scope remains fixed",
        {{"credentialId", id}, {"expectedRevision", revision}, {"value", value}, {"expiresAtMs", expiry}},
        Json::array({"credentialId", "expectedRevision", "value"}));
    add("credential_delete", "Delete a credential using optimistic revision control",
        {{"credentialId", id}, {"expectedRevision", revision}},
        Json::array({"credentialId", "expectedRevision"}));
    add("credential_audit",
        "Read bounded secret-free credential access audit with monotonically increasing sequence",
        {{"after", {{"type", "integer"}, {"minimum", 0}}}}, Json::array());
}
} // namespace awb
