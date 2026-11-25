#pragma once
#include "awb/platform.hpp"
#include <functional>
#include <mutex>

namespace awb
{
class CredentialVault
{
  public:
    // 用途：加载当前Windows用户绑定的DPAPI凭据库，损坏或异机副本明确拒绝；日期：2026-11-05。
    explicit CredentialVault(const std::wstring &root);
    // 用途：释放运行期脱敏副本并尽力清零本对象持有的明文；日期：2026-11-05。
    ~CredentialVault();
    // 用途：执行创建、轮换、删除、元数据查询和有界审计，不提供明文读取接口；日期：2026-11-05。
    Json Execute(const std::string &command, const Json &arguments);
    // 用途：核对用途、来源、环境、有效期后审计并仅向同步动作提供临时明文；日期：2026-11-05。
    Json Use(const std::string &id, const std::string &profile, const std::string &origin,
             const std::string &purpose, const std::function<Json(const std::string &)> &operation);
    // 用途：递归清除返回结构中的凭据及已轮换旧值，二进制截图由浏览器单独阻断；日期：2026-11-05。
    Json Redact(Json value);
    // 用途：给契约注册表追加真实凭据工具，保持全部传输共用；日期：2026-11-05。
    static void Register(Json &tools);

  private:
    // 用途：先加密原子提交再替换内存索引，失败时保持旧库；日期：2026-11-05。
    void Commit(const Json &next);
    // 用途：记住需要脱敏的值，限制总量并在析构时清零；日期：2026-11-05。
    void Remember(const std::string &value);
    // 用途：追加只含操作、凭据ID、时间及状态的有界审计；日期：2026-11-05。
    void Audit(Json &next, const std::string &operation, const std::string &id, const std::string &result);
    std::wstring path_;
    Json document_;
    std::vector<std::string> redactions_;
    std::mutex mutex_;
};
} // namespace awb
