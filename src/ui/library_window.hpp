#pragma once
#include "awb/service.hpp"

namespace desktop
{
constexpr UINT LibraryChangedMessage = WM_APP + 81;
// 用途：打开原生环境库表单，绑定服务并在真实修改后通知主列表刷新；日期：2026-11-05。
void OpenLibraryManager(HWND owner, awb::Service *service, const std::string &selected_profile);
} // namespace desktop
