#pragma once
#include "awb/control_plane.hpp"
#include <mutex>

namespace awb
{
class CommercialClient
{
  public:
    // 用途：加载当前Windows用户DPAPI保护的后台连接与未决准入记录；日期：2026-11-05。
    explicit CommercialClient(const std::wstring &root);
    // 用途：连接后台、验证组织成员身份并注册本机设备；日期：2026-11-05。
    Json Connect(const Json &arguments);
    // 用途：无运行槽时吊销会话并清除后台连接；日期：2026-11-05。
    Json Disconnect();
    // 用途：读取无秘密连接状态或当前组织真实授权；日期：2026-11-05。
    Json Status(bool remote);
    // 用途：调用客户购买、席位与账务动作，不允许调用平台运营接口；日期：2026-11-05。
    Json Call(const std::string &action, const Json &arguments);
    // 用途：持久保存幂等启动意图后取得全局环境与并发准入；日期：2026-11-05。
    void Start(const std::string &profile);
    // 用途：在网页动作前验证当前槽、账号、设备与租户授权；日期：2026-11-05。
    void Check(const std::string &profile);
    // 用途：仅在浏览器确认停止后结算用量并释放并发；日期：2026-11-05。
    void Stop(const std::string &profile);

  private:
    // 用途：发送当前组织的已认证请求，连接凭据不进入结果；日期：2026-11-05。
    Json Request(const std::string &action, Json arguments);
    // 用途：DPAPI加密并原子保存当前会话与持久启动意图；日期：2026-11-05。
    void Save();
    std::wstring path_;
    Json state_;
    std::mutex mutex_;
};
} // namespace awb
