#pragma once
#include "awb/platform.hpp"
#include <memory>

namespace awb
{
class Catalog
{
  public:
    // 用途：打开受管SQLite索引，核验身份、完整性并事务迁移；日期：2026-11-03。
    explicit Catalog(const std::wstring &path);
    // 用途：释放数据库连接，析构过程不抛异常；日期：2026-11-03。
    ~Catalog();
    Catalog(const Catalog &) = delete;
    Catalog &operator=(const Catalog &) = delete;
    // 用途：读取并校验持久化环境元数据，不读取浏览器Cookie；日期：2026-11-03。
    Json Profiles(bool details = false);
    // 用途：按ID读取含状态、版本和时间的单条环境；日期：2026-11-04。
    Json Profile(const std::string &id);
    // 用途：比较版本后事务更新配置或生命周期状态，引用环境禁止停用；日期：2026-11-04。
    Json ChangeProfile(const std::string &id, long long revision, const Json &patch,
                       const std::string &state);
    // 用途：仅删除已进入清理状态且没有引用的环境记录；日期：2026-11-04。
    void FinishPurge(const std::string &id, long long revision);
    // 用途：登记或解除真实资源引用，供流程存储与删除保护共同使用；日期：2026-11-04。
    void SetProfileReference(const std::string &id, const std::string &owner, bool present);
    // 用途：在单一事务内写入环境及全部允许来源；日期：2026-11-03。
    void InsertProfile(const Json &metadata);
    // 用途：验证整批配置后在同一事务创建，失败时不保留部分环境；日期：2026-11-05。
    void InsertProfiles(const Json &metadata);
    // 用途：管理本地分组及版本化配置模板，不保存Cookie或用户凭据；日期：2026-11-05。
    Json Library(const std::string &kind, const std::string &operation, const Json &arguments);
    // 用途：持久保存批次派发边界、去重回执和重启后的未知结果，禁止自动重放；日期：2026-11-05。
    Json Batches(const std::string &operation, const Json &arguments = Json::object());
    // 用途：规范化可复制配置并拒绝身份、凭据及未知字段；日期：2026-11-05。
    static Json Configuration(const Json &configuration);
    // 用途：保留旧清单和只读副本，幂等导入旧POC数据；日期：2026-11-03。
    void ImportLegacy(const std::wstring &manifest);
    // 用途：用SQLite在线备份API生成不覆盖已有文件的元数据快照；日期：2026-11-03。
    void Backup(const std::wstring &destination);
    // 用途：验证快照后恢复到新文件，失败不覆盖原数据库；日期：2026-11-03。
    static void Restore(const std::wstring &backup, const std::wstring &destination);
    // 用途：提交结果不明后拒绝继续读写，要求重启核对持久状态；日期：2026-11-03。
    void CheckHealthy();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace awb
