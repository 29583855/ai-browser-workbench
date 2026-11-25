#include "awb/service.hpp"
#include <atomic>
#include <iostream>
#include <shellapi.h>
#include <sstream>
#include <thread>

// 用途：执行专项草稿模型的独立行为测试；日期：2026-11-04。
void RunFlowDocumentTests();

// 用途：独立断言失败时输出可定位说明；日期：2026-11-03。
static void Require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}
// 用途：核对失败码，防止测试把任意异常都算成功；日期：2026-11-03。
template <typename Function> static void Expect(const std::string &code, Function operation)
{
    try
    {
        operation();
    }
    catch (const awb::Error &error)
    {
        Require(error.code == code, "Expected " + code + ", got " + error.code);
        return;
    }
    throw std::runtime_error("Missing expected rejection: " + code);
}
// 用途：验证中文、命令行、租约竞争/撤销、路径、免费政策和真实持久索引；日期：2026-11-05。
int main()
{
    try
    {
        Require(__cplusplus == 201402L, "C++14 mode required");
        Require(_MSC_VER == 1929, "v142 19.29 required");
        RunFlowDocumentTests();
        Require(awb::Utf8(awb::Wide("中文路径/注册")) == "中文路径/注册", "Unicode roundtrip failed");
        Require(awb::UnicodeLength("中文😀") == 3, "JSON Schema length must count Unicode code points");
        const auto unknown = awb::ErrorResult(awb::Error("ACTION_OUTCOME_UNKNOWN", "submitted", true));
        Require(unknown.at("error").at("outcomeUnknown") &&
                    !unknown.at("error").at("automaticRetryAllowed").get<bool>(),
                "Unknown business outcome was marked safe to retry");
        // 用途：确认非法UTF-8不会被悄悄替换；日期：2026-11-03。
        Expect("INVALID_UTF8", [] { awb::Wide(std::string("\xc3\x28", 2)); });
        const std::vector<std::wstring> arguments = {L"a b", L"", L"quoted\"value", L"trailing\\",
                                                     L"中文路径\\a b\\"};
        std::wstring command = L"test.exe";
        for (const auto &argument : arguments)
            command += L" " + awb::QuoteArgument(argument);
        int count = 0;
        auto parsed = CommandLineToArgvW(command.c_str(), &count);
        Require(parsed && count == static_cast<int>(arguments.size()) + 1, "Argument count mismatch");
        for (size_t index = 0; index < arguments.size(); ++index)
            Require(parsed[index + 1] == arguments[index], "Command line escaping failed");
        LocalFree(parsed);
        Require(awb::Browser::Origin("https://Example.com/path") == "https://example.com:443",
                "Origin normalization failed");
        // 用途：拒绝非Web协议和URL口令；日期：2026-11-03。
        Expect("ORIGIN_DENIED", [] { awb::Browser::Origin("file:///C:/Windows/win.ini"); });
        // 用途：拒绝带用户凭据的目标，避免泄露；日期：2026-11-03。
        Expect("ORIGIN_DENIED", [] { awb::Browser::Origin("https://user:password@example.com"); });
        awb::LeaseGate gate;
        const auto lease = gate.Acquire("writer", 30000);
        // 用途：第二持有者在旧租约有效时必须失败；日期：2026-11-03。
        Expect("LEASE_BUSY", [&] { gate.Acquire("second", 30000); });
        std::atomic<int> dispatched{0};
        // 用途：并发发送每次都经过同一派发锁；日期：2026-11-03。
        std::thread writer(
            [&]
            {
                for (int index = 0; index < 10000; ++index)
                {
                    try
                    {
                        // 用途：模拟真正发送发生的原子位置；日期：2026-11-03。
                        gate.Dispatch(lease, [&] { ++dispatched; });
                    }
                    catch (const awb::Error &error)
                    {
                        if (error.code != "LEASE_REVOKED")
                            std::terminate();
                        break;
                    }
                }
            });
        gate.Revoke();
        const int at_revoke = dispatched.load();
        writer.join();
        Require(dispatched == at_revoke, "Old writer sent after revocation returned");
        // 用途：确认旧租约不能重新通过门禁；日期：2026-11-03。
        Expect("LEASE_REVOKED", [&] { gate.Dispatch(lease, [] {}); });
        const auto next = gate.Acquire("human", 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        // 用途：验证真实单调时钟过期；日期：2026-11-03。
        Expect("LEASE_REVOKED", [&] { gate.Dispatch(next, [] {}); });
        std::istringstream too_large(std::string(1024 * 1024 + 1, 'x'));
        std::string line;
        // 用途：无换行超大消息也必须有界拒绝；日期：2026-11-03。
        Expect("INPUT_TOO_LARGE", [&] { awb::ReadProtocolLine(too_large, line); });
        std::istringstream carriage("{\"value\":\"a\rb\"}\r\n{\"value\":2}\n");
        Require(awb::ReadProtocolLine(carriage, line) && line == "{\"value\":\"a\rb\"}",
                "Protocol silently changed an internal carriage return");
        Require(!awb::Json::accept(line), "Raw JSON control character was accepted");
        Require(awb::ReadProtocolLine(carriage, line) && awb::Json::parse(line).at("value") == 2,
                "CRLF or following message was corrupted");
        wchar_t temporary[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temporary);
        const auto root = std::wstring(temporary) + L"awb-test-" + awb::Wide(awb::RandomId());
        std::string profile_id;
        {
            awb::Service service(root);
            // 用途：迁移版本及历史/批次能力声明必须与真实数据库和工具入口一致；日期：2026-11-05。
            const auto capabilities = service.Execute("capabilities", awb::Json::object());
            Require(capabilities.at("catalogSchemaVersion") == 7 &&
                        capabilities.at("profileTemplateHistory").get<bool>() &&
                        capabilities.at("durableProfileBatchReceipts").get<bool>() &&
                        capabilities.at("profileBatchPaginationAndRetention").get<bool>(),
                    "Catalog version or durable library capability declaration drifted");
            // 用途：第二服务不得同时打开同一数据根；日期：2026-11-03。
            Expect("ROOT_IN_USE", [&] { awb::Service second(root); });
            // 用途：拒绝未登记参数，避免静默忽略；日期：2026-11-03。
            Expect("INVALID_ARGUMENTS", [&] { service.Execute("profile_list", {{"ignored", true}}); });
            // 用途：未实现接口明确拒绝；日期：2026-11-03。
            Expect("UNSUPPORTED_COMMAND", [&] { service.Execute("purchase", awb::Json::object()); });
            // 用途：核对本地后台免费档、并发和付费占位报价契约；日期：2026-11-05。
            const auto policy = service.Execute("admin_policy", awb::Json::object());
            Require(policy.at("freeEnvironmentLimit") == 10 && policy.at("freeConcurrentLimit") == 10 &&
                        policy.at("freeProxy").get<bool>(),
                    "Local commerce policy mismatch");
            const auto free_quote = service.Execute("commerce_quote", {{"environmentCount", 2}});
            Require(free_quote.at("classification") == "free" && free_quote.at("amountMinor") == 0,
                    "Free commerce quote mismatch");
            const auto proxy_quote = service.Execute(
                "commerce_quote", {{"environmentCount", 10}, {"concurrentCount", 10}, {"proxy", true}});
            Require(proxy_quote.at("classification") == "free" && !proxy_quote.at("paymentRequired"),
                    "Latest free proxy and ten-environment policy mismatch");
            const auto paid_quote = service.Execute(
                "commerce_quote", {{"environmentCount", 11}, {"concurrentCount", 3}, {"proxy", true}});
            Require(paid_quote.at("classification") == "payment_required" && paid_quote.at("paymentRequired"),
                    "Paid commerce placeholder quote mismatch");
            const auto slot_quote =
                service.Execute("commerce_quote", {{"environmentCount", 10}, {"concurrentCount", 11}});
            Require(slot_quote.at("classification") == "payment_required", "Concurrency boundary mismatch");
            std::string long_name;
            for (int index = 0; index < 128; ++index)
                long_name += "中";
            // 用途：多字节中文按Schema码点上限通过，超限在落盘前拒绝；日期：2026-11-03。
            Expect("INVALID_ARGUMENTS",
                   [&]
                   {
                       service.Execute("profile_create",
                                       {{"name", long_name + "文"},
                                        {"allowedOrigins", awb::Json::array({"http://127.0.0.1:3000"})}});
                   });
            const auto created = service.Execute(
                "profile_create",
                {{"name", long_name}, {"allowedOrigins", awb::Json::array({"http://127.0.0.1:3000"})}});
            profile_id = created.at("id");
            Require(service.Execute("profile_list", awb::Json::object()).at("profiles").size() == 1,
                    "Profile not saved");
        }
        {
            awb::Service reopened(root);
            Require(reopened.Execute("profile_list", awb::Json::object()).at("profiles")[0].at("id") ==
                        profile_id,
                    "Persistent catalog changed identity");
        }
        const auto long_parent = root + L"\\" + std::wstring(110, L'长');
        const auto long_child = long_parent + L"\\" + std::wstring(110, L'深');
        awb::PrivateDirectory(long_parent);
        awb::PrivateDirectory(long_child);
        const auto long_file = long_child + L"\\awb-process-owner.json";
        Require(long_file.size() > MAX_PATH, "Long-path fixture did not exceed traditional limit");
        awb::AtomicWrite(long_file, "first");
        awb::AtomicWrite(long_file, "持久归属文件");
        Require(awb::ReadFileBounded(long_file, 1024) == "持久归属文件",
                "Long-path atomic replacement/read lost bytes");
        std::wstring alias_path;
        {
            awb::BrowserDataPath alias(long_child);
            alias_path = alias.get();
            Require(alias_path.size() <= 100 && alias_path != long_child,
                    "Long profile did not receive a bounded storage alias");
            awb::Handle via_alias(CreateFileW((alias_path + L"\\awb-process-owner.json").c_str(),
                                              GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                                              nullptr));
            Require(via_alias.get() != INVALID_HANDLE_VALUE, "Alias cannot read canonical data");
            char content[128]{};
            DWORD read = 0;
            Require(ReadFile(via_alias.get(), content, sizeof(content), &read, nullptr) &&
                        std::string(content, read) == "持久归属文件",
                    "Alias points to different data");
            Require(!RemoveDirectoryW(alias_path.c_str()) &&
                        !MoveFileExW((L"\\\\?\\" + long_child).c_str(),
                                     (L"\\\\?\\" + long_child + L"-replaced").c_str(), 0),
                    "Pinned alias or target was replaceable");
            awb::Handle write_link(CreateFileW(
                alias_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            Require(write_link.get() == INVALID_HANDLE_VALUE,
                    "Another writer can modify the pinned junction");
            // 用途：普通路径校验仍拒绝别名，不放宽用户输入的重解析边界；日期：2026-11-04。
            Expect("PATH_REPARSE_POINT", [&] { awb::SafeAbsolute(alias_path); });
        }
        Require(GetFileAttributesW(alias_path.c_str()) == INVALID_FILE_ATTRIBUTES &&
                    awb::ReadFileBounded(long_file, 1024) == "持久归属文件",
                "Alias cleanup deleted target data or retained a normal link");
        Require(CreateDirectoryW(alias_path.c_str(), nullptr) != FALSE, "Cannot inject alias conflict");
        // 用途：遇到非本程序验证的普通目录，拒绝采用且保留冲突对象；日期：2026-11-04。
        Expect("BROWSER_STORAGE_ALIAS_CONFLICT", [&] { awb::BrowserDataPath conflict(long_child); });
        Require(RemoveDirectoryW(alias_path.c_str()) != FALSE,
                "Conflict handling deleted or changed an unrelated directory");
        Require(DeleteFileW((L"\\\\?\\" + long_file).c_str()) &&
                    RemoveDirectoryW((L"\\\\?\\" + long_child).c_str()) &&
                    RemoveDirectoryW((L"\\\\?\\" + long_parent).c_str()),
                "Long-path test cleanup failed");
        const auto legacy_root = root + L"-legacy";
        awb::PrivateDirectory(legacy_root);
        const auto legacy_file = legacy_root + L"\\workbench-poc.json";
        const auto legacy =
            awb::Json({{"schemaVersion", 1},
                       {"mode", "development-poc"},
                       {"profiles",
                        awb::Json::array({{{"id", profile_id},
                                           {"name", "旧版迁移环境"},
                                           {"allowedOrigins", awb::Json::array({"https://example.com"})}}})}})
                .dump();
        awb::AtomicWrite(legacy_file, legacy);
        {
            awb::Service migrated(legacy_root);
            Require(migrated.Execute("profile_list", awb::Json::object()).at("profiles")[0].at("id") ==
                        profile_id,
                    "Service did not automatically import legacy catalog");
            migrated.Execute(
                "profile_create",
                {{"name", "迁移后的新环境"}, {"allowedOrigins", awb::Json::array({"https://example.com"})}});
        }
        {
            awb::Service reopened(legacy_root);
            const auto profiles = reopened.Execute("profile_list", awb::Json::object()).at("profiles");
            Require(profiles.size() == 2 && awb::ReadFileBounded(legacy_file, 1024 * 1024) == legacy,
                    "Reopen reimported stale JSON or altered preserved bytes");
            for (const auto &profile : profiles)
                RemoveDirectoryW((legacy_root + L"\\" + awb::Wide(profile.at("id"))).c_str());
        }
        DeleteFileW((legacy_root + L"\\workbench.sqlite3").c_str());
        DeleteFileW((legacy_root + L"\\.owner.lock").c_str());
        DeleteFileW(legacy_file.c_str());
        SetFileAttributesW((legacy_file + L".pre-sqlite.bak").c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW((legacy_file + L".pre-sqlite.bak").c_str());
        Require(RemoveDirectoryW(legacy_root.c_str()) != FALSE, "Legacy test cleanup failed");
        DeleteFileW((root + L"\\workbench.sqlite3").c_str());
        DeleteFileW((root + L"\\.owner.lock").c_str());
        RemoveDirectoryW((root + L"\\" + awb::Wide(profile_id)).c_str());
        RemoveDirectoryW(root.c_str());
        std::cout << "PASS: C++14/v142, Unicode, quoting, origins, lease race/expiry, bounded protocol, root "
                     "lock, persistence\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
