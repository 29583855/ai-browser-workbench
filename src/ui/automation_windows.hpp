#pragma once
#include "awb/service.hpp"

namespace desktop
{
// 用途：打开绑定所选环境的原生凭据管理窗口，秘密输入框始终掩码；日期：2026-11-05。
void OpenCredentialManager(HWND owner, awb::Service *service, const std::string &profile,
                           const awb::Json &origins);
// 用途：打开固定运行的断点/单步/脱敏变量调试窗口，切换环境不改变绑定；日期：2026-11-05。
void OpenWorkflowDebugger(HWND owner, awb::Service *service, const std::string &run);
} // namespace desktop
