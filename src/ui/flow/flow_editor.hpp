#pragma once
#include "awb/service.hpp"

namespace desktop
{
// 用途：显示复用现有服务校验/发布入口的原生流程编辑器；日期：2026-11-04。
HWND OpenFlowEditor(HWND owner, awb::Service *service, const awb::Json *definition = nullptr,
                    const std::function<void(const awb::Json &)> &published = {},
                    const std::wstring &recovery_directory = L"");
// 用途：销毁主窗口前保存其全部画布，失败抛出并保留所有编辑窗口供重试；日期：2026-11-04。
void SaveFlowEditorsForShutdown(HWND owner);
// 用途：运行真实Direct2D窗口交互及200节点性能检查并保存报告；日期：2026-11-04。
int TestFlowEditor(const std::wstring &report_path);
} // namespace desktop
