#pragma once
#include "awb/browser.hpp"
#include "awb/catalog.hpp"
#include "awb/commercial_client.hpp"
#include "awb/credentials.hpp"
#include "awb/model_provider.hpp"
#include "awb/site_package.hpp"
#include "awb/workflow.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace awb
{
class LeaseGate
{
  public:
    // 用途：取得单写租约，已有有效owner时拒绝竞争；日期：2026-11-03。
    Json Acquire(const std::string &owner, int ttl_ms);
    // 用途：原子增加代数并撤销旧持有者；日期：2026-11-03。
    Json Revoke();
    // 用途：仅内部远程执行器在原有效租约上续期，不改变身份和代数；日期：2026-11-05。
    void Renew(const Json &lease, int ttl_ms);
    // 用途：在校验锁内完成发送，撤销确认后禁止旧代再次发送；日期：2026-11-03。
    void Dispatch(const Json &lease, const std::function<void()> &send);

  private:
    std::mutex mutex_;
    std::string owner_;
    std::string token_;
    unsigned long long generation_ = 0;
    std::chrono::steady_clock::time_point expires_{};
};

class Service
{
  public:
    // 用途：打开独占技术验证数据目录，不冒充正式中央授权服务；日期：2026-11-03。
    explicit Service(const std::wstring &root, const std::wstring &browser = L"");
    // 用途：先排空流程工作线程，再释放环境与数据库；日期：2026-11-04。
    ~Service();
    // 用途：所有UI/CLI/MCP调用共用入口，完整校验参数/范围/租约；日期：2026-11-03。
    Json Execute(const std::string &command, const Json &arguments);
    // 用途：从同一注册表生成MCP工具schema；日期：2026-11-03。
    static Json Tools();
    // 用途：如实返回实际能力及尚未实现能力，不虚报后台/四协议支持；日期：2026-11-03。
    static Json Capabilities();
    // 用途：节点启动前设置远程租约发送门；每条实际CDP消息都重新核验；日期：2026-11-05。
    void SetRemoteDispatchGate(std::function<void(const std::function<void()> &)> gate);
    // 用途：只在远程中央围栏仍有效时续租本地控制权；日期：2026-11-05。
    void RenewRemoteLease(const std::string &profile, const Json &lease);

  private:
    // 用途：在统一脱敏出口内部派发真实业务，不向传输层暴露临时秘密；日期：2026-11-05。
    Json ExecuteInternal(const std::string &command, const Json &arguments);
    struct Profile
    {
        Json metadata;
        std::unique_ptr<Browser> browser;
        LeaseGate gate;
        std::mutex mutex;
        bool removed = false;
    };
    // 用途：严格按不透明ID查所属环境，不接受任意数据目录；日期：2026-11-03。
    std::shared_ptr<Profile> Find(const std::string &id);
    // 用途：整批规范化配置并事务创建全新环境，失败不复用原环境身份或数据；日期：2026-11-05。
    Json CreateConfigurations(const Json &configurations);
    // 用途：延迟创建流程存储，持有独立互斥锁避免并发初始化；日期：2026-11-04。
    WorkflowEngine &Workflows();
    // 用途：在环境锁内重新定位并派发一个流程步骤，每条CDP消息仍验证租约；日期：2026-11-04。
    Json WorkflowStep(const std::shared_ptr<Profile> &profile, const Json &lease, const std::string &type,
                      const Json &arguments);
    // 用途：延迟启动后台授权巡检线程，避免免费本地操作额外等待；日期：2026-11-05。
    void StartCommercialWatch();
    std::function<void(const std::function<void()> &)> remote_gate_;
    std::mutex commercial_watch_mutex_;
    std::wstring root_;
    std::wstring executable_;
    Handle root_lock_;
    std::unique_ptr<Catalog> catalog_;
    std::unique_ptr<CredentialVault> credentials_;
    std::unique_ptr<ModelProvider> model_;
    std::unique_ptr<SitePackages> site_packages_;
    std::unique_ptr<CommercialClient> commercial_;
    std::atomic<bool> commercial_stop_{false};
    std::thread commercial_watch_;
    std::mutex registry_mutex_;
    std::map<std::string, std::shared_ptr<Profile>> profiles_;
    std::mutex workflows_mutex_;
    std::unique_ptr<WorkflowEngine> workflows_;
};

// 用途：验证协议请求体预算及UTF-8，读取一行消息；日期：2026-11-03。
bool ReadProtocolLine(std::istream &stream, std::string &line);
} // namespace awb
