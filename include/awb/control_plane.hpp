#pragma once
#include "awb/platform.hpp"
#include <memory>

namespace awb
{
class ControlPlane
{
  public:
    // 用途：打开独立控制面数据库并检查事务与迁移版本；日期：2026-11-05。
    explicit ControlPlane(const std::wstring &root);
    // 用途：关闭控制面数据库与独占目录句柄；日期：2026-11-05。
    ~ControlPlane();
    ControlPlane(const ControlPlane &) = delete;
    ControlPlane &operator=(const ControlPlane &) = delete;
    // 用途：执行经过身份、租户和角色检查的后台业务事务；日期：2026-11-05。
    Json Execute(const std::string &action, const Json &body, const std::string &token = "");
    // 用途：对网关原始正文验签、去重并原子入账，禁止客户端伪造支付结果；日期：2026-11-05。
    // 用途：验证官方通知原文并提交幂等账本事务；日期：2026-11-05。
    Json OfficialWebhook(const std::string &channel, const std::string &body,
                         const std::map<std::string, std::string> &headers);
    Json Webhook(const std::string &body, const std::string &timestamp, const std::string &signature);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// 用途：启动独立回环管理服务及中文管理门户；日期：2026-11-05。
int RunAdmin(const std::wstring &root, unsigned short port);
// 用途：通过有界HTTP请求调用后台，默认仅允许回环明文或正式HTTPS；日期：2026-11-05。
Json ControlRequest(const std::string &url, const std::string &action, const Json &body,
                    const std::string &token, const std::string &client_certificate = "");
} // namespace awb
