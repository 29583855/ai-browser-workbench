#pragma once
#include "awb/credentials.hpp"

namespace awb
{
class ModelProvider
{
  public:
    // 用途：载入本地模型配置和持久调用预算，不保留提示词、输出或API密钥；日期：2026-11-05。
    ModelProvider(const std::wstring &root, CredentialVault &vault);
    // 用途：配置兼容接口、查询状态或生成经校验的待审阅流程，绝不直接执行；日期：2026-11-05。
    Json Execute(const std::string &command, const Json &arguments);
    // 用途：向唯一工具注册表追加实际模型能力；日期：2026-11-05。
    static void Register(Json &tools);

  private:
    // 用途：原子提交配置、预算预留和无正文调用状态；日期：2026-11-05。
    void Save(const Json &next);
    std::wstring path_;
    CredentialVault &vault_;
    Json state_;
    std::mutex mutex_;
};
} // namespace awb
