#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace awb
{
using Json = nlohmann::json;

class Error : public std::runtime_error
{
  public:
    // 用途：保留可跨CLI/MCP/UI传递的结构化错误码；日期：2026-11-03。
    Error(std::string code, const std::string &message, bool outcome_unknown = false);
    std::string code;
    bool outcome_unknown;
};

class Handle
{
  public:
    // 用途：接管唯一Windows句柄；日期：2026-11-03。
    explicit Handle(HANDLE value = nullptr);
    // 用途：释放唯一所有的Windows句柄；日期：2026-11-03。
    ~Handle();
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    // 用途：转移句柄所有权；日期：2026-11-03。
    Handle(Handle &&other) noexcept;
    // 用途：替换并转移句柄所有权；日期：2026-11-03。
    Handle &operator=(Handle &&other) noexcept;
    // 用途：读取借用句柄，不转移所有权；日期：2026-11-03。
    HANDLE get() const;
    // 用途：释放旧句柄并接管新值；日期：2026-11-03。
    void reset(HANDLE value = nullptr);

  private:
    HANDLE value_;
};

class BrowserDataPath
{
  public:
    // 用途：为长Profile创建受控短路径别名，保持浏览器数据位于原目录；日期：2026-11-04。
    explicit BrowserDataPath(const std::wstring &canonical);
    // 用途：释放别名/目标句柄，仅删除目录连接本身且不递归删除目标；日期：2026-11-04。
    ~BrowserDataPath();
    BrowserDataPath(const BrowserDataPath &) = delete;
    BrowserDataPath &operator=(const BrowserDataPath &) = delete;
    // 用途：返回供受管浏览器使用的已验证短路径；日期：2026-11-04。
    const std::wstring &get() const;

  private:
    std::wstring path_;
    bool alias_ = false;
    Handle alias_handle_;
    Handle target_handle_;
};

// 用途：严格UTF-8转UTF-16，不接受非法输入；日期：2026-11-03。
std::wstring Wide(const std::string &text);
// 用途：严格按Unicode码点计数，保持JSON Schema长度语义；日期：2026-11-03。
size_t UnicodeLength(const std::string &text);
// 用途：严格UTF-16转UTF-8，不损失中文；日期：2026-11-03。
std::string Utf8(const std::wstring &text);
// 用途：生成系统随机标识；日期：2026-11-03。
std::string RandomId();
// 用途：按Windows命令行规则引用单个参数，禁止shell拼接；日期：2026-11-03。
std::wstring QuoteArgument(const std::wstring &argument);
// 用途：取得规范绝对路径并拒绝存在的重解析点；日期：2026-11-03。
std::wstring SafeAbsolute(const std::wstring &path);
// 用途：创建仅当前用户及SYSTEM可访问的受管根目录；日期：2026-11-03。
void PrivateDirectory(const std::wstring &path);
// 用途：仅清理受管根下指定32位环境目录，持有目录句柄并拒绝重解析点/硬链接；日期：2026-11-04。
void RemoveProfileDirectory(const std::wstring &root, const std::string &id);
// 用途：在同目录写临时文件后原子替换，失败保留旧文件；日期：2026-11-03。
void AtomicWrite(const std::wstring &path, const std::string &data);
// 用途：有界读取文件，拒绝超预算内容；日期：2026-11-03。
std::string ReadFileBounded(const std::wstring &path, size_t limit);
// 用途：有界解析外部JSON，在构建深层对象前拒绝嵌套超限和重复字段；日期：2026-11-04。
Json ParseExternalJson(const std::string &text, int maximum_depth = 64);
// 用途：读取当前程序目录，便携包不依赖工作目录或原开发机器路径；日期：2026-11-04。
std::wstring ExecutableDirectory();
// 用途：仅发现本机已安装受支持浏览器，不下载或修改浏览器；日期：2026-11-03。
std::wstring DiscoverBrowser();
// 用途：默认将运行数据放入当前用户LocalAppData；日期：2026-11-03。
std::wstring DefaultRoot();
// 用途：将异常转成无秘密的结构化结果；日期：2026-11-03。
Json ErrorResult(const std::exception &error);
} // namespace awb
