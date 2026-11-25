#pragma once
#include "awb/platform.hpp"
#include <mutex>

namespace awb
{
class SitePackages
{
  public:
    // 用途：载入只含数据的站点知识包，校验所有不可变版本及流程摘要；日期：2026-11-05。
    explicit SitePackages(const std::wstring &root);
    // 用途：预览导入差异、提交版本、查询及导出，不安装脚本或访问外部文件；日期：2026-11-05。
    Json Execute(const std::string &command, const Json &arguments);
    // 用途：登记共享站点知识包契约；日期：2026-11-05。
    static void Register(Json &tools);

  private:
    std::wstring path_;
    Json packages_ = Json::object();
    std::mutex mutex_;
};
} // namespace awb
