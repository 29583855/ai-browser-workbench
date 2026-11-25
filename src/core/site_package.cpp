#include "awb/site_package.hpp"
#include "awb/browser.hpp"
#include "awb/workflow.hpp"
#include <algorithm>
#include <bcrypt.h>
#include <iomanip>
#include <set>
#include <sstream>

namespace awb
{
namespace
{
// 用途：返回固定包错误，不回显非法文件路径或业务内容；日期：2026-11-05。
void Require(bool accepted, const std::string &message)
{
    if (!accepted)
        throw Error("SITE_PACKAGE_INVALID", message);
}
// 用途：校验数据对象的封闭字段，拒绝脚本、附件及未解释字段；日期：2026-11-05。
void Fields(const Json &value, const std::set<std::string> &allowed, const std::set<std::string> &required)
{
    Require(value.is_object(), "Package field must be an object");
    for (const auto &key : required)
        Require(value.contains(key), "Package field is required");
    for (auto it = value.begin(); it != value.end(); ++it)
        Require(allowed.count(it.key()) != 0, "Unknown package field is not supported");
}
// 用途：严格限制有界文本和UTF8，禁止NUL；日期：2026-11-05。
std::string Text(const Json &value, size_t maximum)
{
    Require(value.is_string(), "Package text field must be a string");
    const auto text = value.get<std::string>();
    Require(!text.empty() && UnicodeLength(text) <= maximum && text.find('\0') == std::string::npos,
            "Package text is outside bounds");
    return text;
}
// 用途：限制稳定包/流程名称为ASCII数据标识，禁止路径穿越；日期：2026-11-05。
std::string Name(const Json &value)
{
    const auto text = Text(value, 64);
    Require(text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                std::string::npos,
            "Package identifier contains unsupported characters");
    return text;
}
// 用途：计算规范JSON摘要用于不可变版本核对，不把路径或秘密混入哈希输入；日期：2026-11-05。
std::string Hash(const Json &value)
{
    const auto bytes = value.dump();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("SITE_PACKAGE_HASH_FAILED", "Cannot initialize package hashing");
    unsigned char hash[32]{};
    const auto result =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(bytes.data())),
                   static_cast<ULONG>(bytes.size()), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0)
        throw Error("SITE_PACKAGE_HASH_FAILED", "Cannot hash package");
    std::ostringstream output;
    for (const auto byte : hash)
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    return output.str();
}
// 用途：限制包路径为明确URL路由，拒绝编码穿越、查询、片段和本地文件路径；日期：2026-11-05。
void Route(const std::string &path)
{
    Require(!path.empty() && path.front() == '/' && path.size() <= 512 &&
                path.find("..") == std::string::npos &&
                path.find_first_of("\\%?#\r\n") == std::string::npos && path.find("//") == std::string::npos,
            "Route must be a bounded absolute URL path without traversal or encoded separators");
}
// 用途：阻止站点包携带本机凭据引用，导出只能保留参数化输入；日期：2026-11-05。
void NoSecrets(const Json &value)
{
    if (value.is_object())
        Require(!value.contains("secretRef") && !value.contains("cookie") && !value.contains("storageState"),
                "Packages cannot contain local secrets or browser session state");
    if (value.is_structured())
        for (const auto &child : value)
            NoSecrets(child);
}
// 用途：核对整个包、能力、路由和每个工作流的摘要，导入本身绝不执行；日期：2026-11-05。
std::string Validate(const Json &package)
{
    try
    {
        Require(package.dump().size() <= 1024 * 1024, "Package exceeds one MiB");
        Fields(package,
               {"schemaVersion", "packageId", "name", "version", "origin", "routePaths",
                "requiredCapabilities", "locators", "workflows"},
               {"schemaVersion", "packageId", "name", "version", "origin", "routePaths",
                "requiredCapabilities", "locators", "workflows"});
        Require(package.at("schemaVersion") == "1.0", "Unsupported site package version");
        const auto id = Name(package.at("packageId"));
        Text(package.at("name"), 128);
        Require(package.at("version").is_number_integer() && package.at("version") >= 1 &&
                    package.at("version") <= 1000000,
                "Package version must be a positive bounded integer");
        const auto origin = Browser::Origin(Text(package.at("origin"), 8192));
        Require(package.at("origin") == origin, "Package origin must be normalized including its port");
        Require(package.at("routePaths").is_array() && !package.at("routePaths").empty() &&
                    package.at("routePaths").size() <= 32,
                "Package requires one to 32 routes");
        std::set<std::string> routes;
        for (const auto &value : package.at("routePaths"))
        {
            const auto route = Text(value, 512);
            Route(route);
            Require(routes.insert(route).second, "Package routes are duplicated");
        }
        const std::set<std::string> supported{
            "navigate", "fill",    "click", "select", "assertText",     "readText",        "condition",
            "loop",     "foreach", "call",  "set",    "typedVariables", "uniqueCssLocator"};
        Require(package.at("requiredCapabilities").is_array() &&
                    package.at("requiredCapabilities").size() <= supported.size(),
                "Package capability budget exceeded");
        std::set<std::string> capabilities;
        for (const auto &value : package.at("requiredCapabilities"))
        {
            const auto capability = Text(value, 64);
            Require(supported.count(capability) && capabilities.insert(capability).second,
                    "Package capability is unsupported or duplicated");
        }
        const auto &locators = package.at("locators");
        Require(locators.is_object() && locators.size() <= 128, "Locator budget exceeded");
        for (auto it = locators.begin(); it != locators.end(); ++it)
        {
            Name(it.key());
            Fields(it.value(), {"selector", "role", "name", "routePath"}, {"selector", "routePath"});
            Text(it.value().at("selector"), 2048);
            Require(routes.count(Text(it.value().at("routePath"), 512)) != 0,
                    "Locator route is not declared");
            if (it.value().contains("role"))
                Text(it.value().at("role"), 64);
            if (it.value().contains("name"))
                Text(it.value().at("name"), 256);
        }
        Require(package.at("workflows").is_array() && !package.at("workflows").empty() &&
                    package.at("workflows").size() <= 16,
                "Package requires one to 16 workflows");
        std::set<std::string> names;
        for (const auto &workflow : package.at("workflows"))
        {
            Fields(workflow, {"name", "definition", "sha256"}, {"name", "definition", "sha256"});
            Require(names.insert(Name(workflow.at("name"))).second, "Workflow name is duplicated");
            NoSecrets(workflow.at("definition"));
            const auto validation = ValidateWorkflow(workflow.at("definition"));
            Require(validation.at("valid").get<bool>() && workflow.at("sha256") == validation.at("sha256"),
                    "Package workflow failed validation or hash verification");
            std::set<std::string> workflow_origins;
            for (const auto &value : workflow.at("definition").at("policy").at("allowedOrigins"))
                workflow_origins.insert(Browser::Origin(value));
            Require(workflow_origins == std::set<std::string>{origin},
                    "Package workflow origin differs from package origin");
            for (const auto &required : validation.at("requiredCapabilities"))
                Require(capabilities.count(required.get<std::string>()) != 0,
                        "Workflow capability is absent from package declaration");
            // 用途：逐主/子流程核对静态导航路由，防止包声明与实际动作范围不同；日期：2026-11-05。
            const auto check_steps = [&](const Json &steps)
            {
                for (const auto &step : steps)
                    if (step.at("type") == "navigate")
                    {
                        const auto url = Wide(step.at("url"));
                        URL_COMPONENTS parts{};
                        parts.dwStructSize = sizeof(parts);
                        parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
                        Require(WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) &&
                                    !parts.dwExtraInfoLength,
                                "Navigation route contains unsupported components");
                        const auto path = parts.dwUrlPathLength
                                              ? Utf8(std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength))
                                              : "/";
                        Route(path);
                        Require(routes.count(path) != 0, "Navigation route is not declared");
                    }
            };
            check_steps(workflow.at("definition").at("steps"));
            if (workflow.at("definition").contains("subflows"))
                for (const auto &subflow : workflow.at("definition").at("subflows"))
                    check_steps(subflow.at("steps"));
        }
        return id;
    }
    catch (const Json::exception &)
    {
        throw Error("SITE_PACKAGE_INVALID", "Package fields have invalid types or are missing");
    }
}
// 用途：对较大差异给出类型/大小/摘要，完整内容仍通过精确版本导出读取；日期：2026-11-05。
Json Brief(const Json &value)
{
    if (value.is_structured() || (value.is_string() && value.get_ref<const std::string &>().size() > 512))
        return {
            {"type", value.type_name()}, {"serializedBytes", value.dump().size()}, {"sha256", Hash(value)}};
    return value;
}
// 用途：对规范数据生成有界路径差异，不解压附件或解析本地文件名；日期：2026-11-05。
void Difference(const Json &before, const Json &after, const std::string &path, Json &changes)
{
    if (before == after || changes.size() >= 256)
        return;
    if (before.is_object() && after.is_object())
    {
        std::set<std::string> keys;
        for (auto it = before.begin(); it != before.end(); ++it)
            keys.insert(it.key());
        for (auto it = after.begin(); it != after.end(); ++it)
            keys.insert(it.key());
        for (const auto &key : keys)
            Difference(before.contains(key) ? before.at(key) : Json(),
                       after.contains(key) ? after.at(key) : Json(), path + "/" + key, changes);
        return;
    }
    changes.push_back(
        {{"path", path.empty() ? "/" : path}, {"before", Brief(before)}, {"after", Brief(after)}});
}
} // namespace
// 用途：核对受管知识包存储格式、数量、全部版本和摘要；日期：2026-11-05。
SitePackages::SitePackages(const std::wstring &root) : path_(SafeAbsolute(root + L"\\site-packages.json"))
{
    if (GetFileAttributesW((L"\\\\?\\" + path_).c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    try
    {
        const auto document = ParseExternalJson(ReadFileBounded(path_, 8 * 1024 * 1024), 40);
        Fields(document, {"format", "packages"}, {"format", "packages"});
        Require(document.at("format") == 1 && document.at("packages").is_object() &&
                    document.at("packages").size() <= 64,
                "Package store format or capacity is invalid");
        for (auto it = document.at("packages").begin(); it != document.at("packages").end(); ++it)
        {
            Name(it.key());
            Require(it.value().is_array() && !it.value().empty() && it.value().size() <= 16,
                    "Version budget exceeded");
            long long previous = 0;
            for (const auto &version : it.value())
            {
                Fields(version, {"package", "sha256"}, {"package", "sha256"});
                Require(Validate(version.at("package")) == it.key() &&
                            version.at("sha256") == Hash(version.at("package")) &&
                            version.at("package").at("version").get<long long>() > previous,
                        "Package store integrity failed");
                previous = version.at("package").at("version");
            }
        }
        packages_ = document.at("packages");
    }
    catch (const std::exception &)
    {
        throw Error("SITE_PACKAGE_STORAGE_INVALID", "Stored site package failed integrity verification");
    }
}
// 用途：以先预览差异、再CAS导入的方式保存不可变站点版本；日期：2026-11-05。
Json SitePackages::Execute(const std::string &command, const Json &arguments)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (command == "site_package_list")
    {
        Json list = Json::array();
        for (auto it = packages_.begin(); it != packages_.end(); ++it)
            for (const auto &entry : it.value())
                list.push_back({{"packageId", it.key()},
                                {"version", entry.at("package").at("version")},
                                {"name", entry.at("package").at("name")},
                                {"sha256", entry.at("sha256")}});
        return {{"packages", list}};
    }
    if (command == "site_package_export")
    {
        const auto id = Name(arguments.at("packageId"));
        if (packages_.contains(id))
            for (const auto &entry : packages_.at(id))
                if (entry.at("package").at("version") == arguments.at("version"))
                    return entry;
        throw Error("SITE_PACKAGE_NOT_FOUND", "Requested package version is not installed");
    }
    const auto &package = arguments.at("package");
    const auto id = Validate(package);
    const auto sha = Hash(package);
    const auto existing = packages_.contains(id) ? packages_.at(id).back() : Json();
    const auto latest = existing.is_null() ? 0LL : existing.at("package").at("version").get<long long>();
    Json changes = Json::array();
    Difference(existing.is_null() ? Json::object() : existing.at("package"), package, "", changes);
    if (command == "site_package_preview")
        return {{"valid", true},           {"packageId", id},    {"sha256", sha},
                {"latestVersion", latest}, {"changes", changes}, {"changesTruncated", changes.size() >= 256},
                {"willExecute", false}};
    if (arguments.at("expectedLatestVersion") != latest)
        throw Error("REVISION_CONFLICT", "Refresh import preview before installing a package version");
    if (!existing.is_null() && existing.at("sha256") == sha)
        return {{"packageId", id}, {"version", latest}, {"sha256", sha}, {"existing", true}};
    if (package.at("version") <= latest)
        throw Error("SITE_PACKAGE_VERSION_CONFLICT",
                    "Published package versions are immutable and increasing");
    if ((!packages_.contains(id) && packages_.size() >= 64) ||
        (packages_.contains(id) && packages_.at(id).size() >= 16))
        throw Error("SITE_PACKAGE_CAPACITY", "Site package or version capacity reached");
    auto next = packages_;
    if (!next.contains(id))
        next[id] = Json::array();
    next[id].push_back({{"package", package}, {"sha256", sha}});
    const auto serialized = Json{{"format", 1}, {"packages", next}}.dump(2) + "\n";
    if (serialized.size() > 8 * 1024 * 1024)
        throw Error("SITE_PACKAGE_CAPACITY", "Site package storage exceeds eight MiB");
    AtomicWrite(path_, serialized);
    packages_ = next;
    return {{"packageId", id}, {"version", package.at("version")}, {"sha256", sha}, {"existing", false}};
}
// 用途：生成知识包工具契约，内部仍逐字段严格验证包本身；日期：2026-11-05。
void SitePackages::Register(Json &tools)
{
    const Json document{{"type", "object"},
                        {"properties", Json::object()},
                        {"required", Json::array()},
                        {"additionalProperties", true}};
    // 用途：登记数据包操作而不增加任意文件读写接口；日期：2026-11-05。
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
    add("site_package_preview",
        "Validate data-only site package, capabilities, routes, workflow hashes and review import "
        "differences",
        {{"package", document}}, Json::array({"package"}));
    add("site_package_import",
        "Install an immutable validated site package version using optimistic version control; does not run "
        "workflows",
        {{"package", document},
         {"expectedLatestVersion", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000000}}}},
        Json::array({"package", "expectedLatestVersion"}));
    add("site_package_list", "List installed site package versions and hashes", Json::object(),
        Json::array());
    add("site_package_export", "Export one exact installed data-only package version and its hash",
        {{"packageId", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
         {"version", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000000}}}},
        Json::array({"packageId", "version"}));
}
} // namespace awb
