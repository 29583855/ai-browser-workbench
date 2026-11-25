#pragma once
#include <string>

namespace awb
{
// 用途：独占绑定回环HTTP端点，使用私有令牌和统一服务执行有界请求；日期：2026-11-04。
int RunLocalApi(const std::wstring &root, const std::wstring &browser, unsigned short port);
} // namespace awb
