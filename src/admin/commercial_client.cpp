#include "awb/commercial_client.hpp"
#include <algorithm>
#include <wincrypt.h>

namespace awb
{
namespace
{
// 用途：用当前用户DPAPI加解密连接文件，异机复制必须重新登录；日期：2026-11-05。
std::string Protect(const std::string &data, bool decrypt)
{
    DATA_BLOB input{static_cast<DWORD>(data.size()),
                    reinterpret_cast<BYTE *>(const_cast<char *>(data.data()))};
    DATA_BLOB output{};
    const BOOL success = decrypt ? CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                                      CRYPTPROTECT_UI_FORBIDDEN, &output)
                                 : CryptProtectData(&input, L"AWB commercial connection", nullptr, nullptr,
                                                    nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!success)
        throw Error("ACCOUNT_STORAGE", "Cannot protect/unprotect account connection for this Windows user");
    const std::string result(reinterpret_cast<const char *>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
} // namespace
// 用途：读取独立加密连接文件，未登录时本地免费开发模式继续可用；日期：2026-11-05。
CommercialClient::CommercialClient(const std::wstring &root)
    : path_(SafeAbsolute(root + L"\\commercial.dpapi")),
      state_({{"connected", false}, {"slots", Json::object()}})
{
    if (GetFileAttributesW(path_.c_str()) != INVALID_FILE_ATTRIBUTES)
        state_ = ParseExternalJson(Protect(ReadFileBounded(path_, 1024 * 1024), true));
}
// 用途：原子持久化连接，避免断电后丢失待核对的运行槽；日期：2026-11-05。
void CommercialClient::Save()
{
    AtomicWrite(path_, Protect(state_.dump(), false));
}
// 用途：按保存的组织发送请求，调用者不能通过参数切换到其他租户；日期：2026-11-05。
Json CommercialClient::Request(const std::string &action, Json arguments)
{
    if (!state_.value("connected", false))
        throw Error("ACCOUNT_REQUIRED", "Connect a control-plane account first");
    arguments["tenantId"] = state_.at("tenantId");
    return ControlRequest(state_.at("url"), action, arguments, state_.at("token"));
}
// 用途：在无遗留运行槽时登录并绑定设备，失败保留原连接；日期：2026-11-05。
Json CommercialClient::Connect(const Json &arguments)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto url = arguments.at("url").get<std::string>();
    const auto session =
        arguments.contains("code")
            ? ControlRequest(url, "client.code.exchange", {{"code", arguments.at("code")}}, "")
            : ControlRequest(url, "login",
                             {{"email", arguments.at("email")}, {"password", arguments.at("password")}}, "");
    const auto tenant = arguments.value("tenantId", session.at("tenantId").get<std::string>());
    const auto device_id = state_.value("localDeviceId", RandomId());
    if (!state_.at("slots").empty())
    {
        const auto expected = tenant + ":" + session.at("userId").get<std::string>() + ":" + device_id;
        if (state_.value("url", "") != url || state_.value("deviceId", "") != expected)
            throw Error("ACCOUNT_BUSY",
                        "Tracked slots only allow reauthentication to the same account/device");
        state_["token"] = session.at("token");
        Save();
        return {{"connected", true}, {"reauthenticated", true}, {"tenantId", tenant}, {"deviceId", expected}};
    }
    const auto device = ControlRequest(url, "device.register",
                                       {{"tenantId", tenant},
                                        {"deviceId", device_id},
                                        {"name", arguments.value("deviceName", "Workbench device")}},
                                       session.at("token"));
    state_ = {{"connected", true},
              {"url", url},
              {"token", session.at("token")},
              {"tenantId", tenant},
              {"deviceId", device.at("id")},
              {"localDeviceId", device_id},
              {"slots", Json::object()}};
    Save();
    return {{"connected", true},
            {"tenantId", tenant},
            {"deviceId", device.at("id")},
            {"sessionExpiresAt", session.at("expiresAt")}};
}
// 用途：先服务端注销再删除本地连接，遗留准入不得通过退出绕开；日期：2026-11-05。
Json CommercialClient::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.at("slots").empty())
        throw Error("ACCOUNT_BUSY", "Tracked browsers must be stopped before disconnecting");
    if (state_.value("connected", false))
        Request("logout", Json::object());
    state_ = {{"connected", false}, {"slots", Json::object()}};
    Save();
    return {{"connected", false}};
}
// 用途：返回连接元数据或服务器实时快照，不返回会话令牌；日期：2026-11-05。
Json CommercialClient::Status(bool remote)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (remote && state_.value("connected", false))
        return Request("snapshot", Json::object());
    return {{"connected", state_.value("connected", false)},
            {"tenantId", state_.value("tenantId", "")},
            {"url", state_.value("url", "")},
            {"trackedSlots", state_.at("slots").size()},
            {"mode", state_.value("connected", false) ? "managed-commercial" : "local-free-development"}};
}
// 用途：限定客户端客户门户动作，全部权限仍由服务端再次验证；日期：2026-11-05。
Json CommercialClient::Call(const std::string &action, const Json &arguments)
{
    static const std::vector<std::string> allowed{"identity",
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
                                                  "client.code.create"};
    if (std::find(allowed.begin(), allowed.end(), action) == allowed.end())
        throw Error("INVALID_ARGUMENTS", "Customer action is not registered");
    std::lock_guard<std::mutex> lock(mutex_);
    return Request(action, arguments);
}
// 用途：先保存启动意图，再进行幂等准入，异常不能丢失已占用配额；日期：2026-11-05。
void CommercialClient::Start(const std::string &profile)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.value("connected", false))
        return;
    if (state_.at("slots").contains(profile))
        throw Error("COMMERCIAL_RECONCILIATION",
                    "Stop/reconcile the previous admission before restarting this profile");
    const auto environment =
        Request("environment.reserve", {{"deviceId", state_.at("deviceId")}, {"profileId", profile}});
    state_["slots"][profile] = {{"key", RandomId()}, {"environmentId", environment.at("id")}};
    Save();
    try
    {
        const auto slot =
            Request("slot.acquire", {{"environmentId", environment.at("id")},
                                     {"idempotencyKey", state_.at("slots").at(profile).at("key")}});
        state_["slots"][profile]["id"] = slot.at("id");
        Save();
    }
    catch (const Error &error)
    {
        if (!error.outcome_unknown)
        {
            state_["slots"].erase(profile);
            Save();
        }
        throw;
    }
}
// 用途：验证已登记运行槽；网络、登录或权限失效时拒绝继续网页操作；日期：2026-11-05。
void CommercialClient::Check(const std::string &profile)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.value("connected", false))
        return;
    if (!state_.at("slots").contains(profile) || !state_.at("slots").at(profile).contains("id"))
        throw Error("COMMERCIAL_RECONCILIATION", "Browser has no confirmed control-plane admission");
    Request("slot.heartbeat", {{"id", state_.at("slots").at(profile).at("id")}});
}
// 用途：重查未决启动结果并在确认停止后释放槽；服务不可用时保留本地恢复记录；日期：2026-11-05。
void CommercialClient::Stop(const std::string &profile)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.value("connected", false) || !state_.at("slots").contains(profile))
        return;
    auto slot = state_.at("slots").at(profile);
    if (!slot.contains("id"))
    {
        const auto recovered = Request("slot.lookup", {{"idempotencyKey", slot.at("key")}});
        if (!recovered.at("found").get<bool>())
        {
            state_["slots"].erase(profile);
            Save();
            return;
        }
        slot["id"] = recovered.at("slot").at("id");
    }
    Request("slot.release", {{"id", slot.at("id")}, {"confirmStopped", slot.at("id")}});
    state_["slots"].erase(profile);
    Save();
}
} // namespace awb
