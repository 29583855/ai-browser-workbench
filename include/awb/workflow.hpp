#pragma once
#include "awb/platform.hpp"
#include <functional>
#include <memory>

namespace awb
{
// 用途：校验受限流程方言及可选输入，返回无业务值的定位错误；日期：2026-11-04。
Json ValidateWorkflow(const Json &definition, const Json *input = nullptr);

class WorkflowEngine
{
  public:
    using Dispatch = std::function<Json(const std::string &, const Json &)>;
    // 用途：在私有根中管理不可变发布版本及无秘密运行检查点；日期：2026-11-04。
    explicit WorkflowEngine(const std::wstring &root);
    // 用途：停止后续派发并等待本对象工作线程退出；日期：2026-11-04。
    ~WorkflowEngine();
    WorkflowEngine(const WorkflowEngine &) = delete;
    WorkflowEngine &operator=(const WorkflowEngine &) = delete;
    // 用途：校验后发布不可变内容，以摘要识别版本且不覆盖旧版本；日期：2026-11-04。
    Json Publish(const Json &definition);
    // 用途：有界列出发布版本，不读取或回显运行输入；日期：2026-11-04。
    Json List();
    // 用途：按受管版本标识读取并重新校验不可变定义；日期：2026-11-04。
    Json Definition(const std::string &id);
    // 用途：启动异步顺序/条件/有界循环/子流程任务，断开调用连接不取消；日期：2026-11-04。
    Json Start(const std::string &id, const Json &input, const std::string &profile, Dispatch dispatch,
               const Json &debug = Json::object());
    // 用途：配置精确流程/节点断点并只在安全暂停边界检查脱敏变量；日期：2026-11-05。
    Json Debug(const std::string &id, const Json &options);
    // 用途：仅返回步骤元数据与实际状态，不包含输入值、租约或页面正文；日期：2026-11-04。
    Json Status(const std::string &id);
    // 用途：列出运行摘要以便客户端响应丢失后找回runId，不回显输入；日期：2026-11-04。
    Json Runs();
    // 用途：在步骤边界暂停/恢复/取消，未知结果与重启记录不能继续；日期：2026-11-04。
    Json Control(const std::string &id, const std::string &operation);
    // 用途：阻止其他入口在运行中插入页面动作，允许所属执行线程；日期：2026-11-04。
    bool OwnsProfile(const std::string &id);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace awb
