#pragma once
#include "awb/service.hpp"
namespace desktop
{
// 用途：打开原生后台账号表单，密码掩码输入且不保存到文本日志；日期：2026-11-05。
void OpenAccountWindow(HWND owner, awb::Service *service);
} // namespace desktop
