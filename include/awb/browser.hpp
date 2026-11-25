#pragma once
#include "awb/platform.hpp"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <winhttp.h>

namespace awb
{
class InternetHandle
{
  public:
    // 用途：接管WinHTTP句柄；日期：2026-11-03。
    explicit InternetHandle(HINTERNET value = nullptr);
    // 用途：关闭WinHTTP句柄；日期：2026-11-03。
    ~InternetHandle();
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
    // 用途：读取借用WinHTTP句柄；日期：2026-11-03。
    HINTERNET get() const;
    // 用途：替换WinHTTP句柄；日期：2026-11-03。
    void reset(HINTERNET value = nullptr);

  private:
    HINTERNET value_;
};

class Browser
{
  public:
    // 用途：启动独立受管进程树，绑定私有Profile目录与准入域；日期：2026-11-03。
    Browser(const std::wstring &executable, const std::wstring &directory, bool headless,
            const std::vector<std::string> &origins, const std::string &proxy_server = "",
            const Json &runtime_settings = Json::object());
    // 用途：只终止本对象Job中的受管进程，禁止误杀普通浏览器；日期：2026-11-03。
    ~Browser();
    Browser(const Browser &) = delete;
    Browser &operator=(const Browser &) = delete;
    // 用途：取得受管进程归属和浏览器握手事实，不暴露CDP地址；日期：2026-11-03。
    Json Status() const;
    // 用途：跳转允许域并等待文档就绪，超时不自动重发；日期：2026-11-03。
    Json Navigate(const std::string &url);
    // 用途：读取AX语义树，生成绑定文档的有限元素引用；日期：2026-11-03。
    Json Snapshot();
    // 用途：在当前主文档唯一匹配CSS，生成可验证引用；日期：2026-11-03。
    Json Locate(const std::string &selector);
    // 用途：探查唯一元素、返回无字段值的动作草稿，并按需高亮该节点；日期：2026-11-04。
    Json Inspect(const std::string &selector, bool highlight);
    // 用途：只读发现主文档可见功能候选，返回唯一CSS与语义近似而不自动点击；日期：2026-11-05。
    Json Discover(const std::string &query, const std::string &role, size_t limit);
    // 用途：只读预览点击、填写或选择的当前条件，不派发网页动作；日期：2026-11-04。
    Json Preview(const std::string &reference, const std::string &action);
    // 用途：清除本实例的调试高亮，不改网页样式或业务数据；日期：2026-11-04。
    Json ClearHighlight();
    // 用途：按值唯一选择原生单选或多选控件并验证事件后结果，不返回选项值；日期：2026-11-04。
    Json Select(const std::string &reference, const std::vector<std::string> &values);
    // 用途：将引用对应输入控件赋值并触发标准事件，禁止记录值；日期：2026-11-03。
    Json Fill(const std::string &reference, const std::string &value);
    // 用途：按当前实际来源核对SecretRef并在秘密进入页面前阻断此实例后续截图；日期：2026-11-05。
    Json FillSecret(const std::string &reference, const std::string &value, const std::string &origin);
    // 用途：读取当前真实文档来源，供凭据库实施来源绑定；日期：2026-11-05。
    std::string CurrentOrigin();
    // 用途：唯一引用解析后通过CDP输入事件点击，已发送失败不自动重试；日期：2026-11-03。
    Json Click(const std::string &reference);
    // 用途：读取受限元素文本，用于业务断言；日期：2026-11-03。
    Json Text(const std::string &reference);
    // 用途：有界等待唯一控件满足文本断言，不重发网页业务；日期：2026-11-03。
    Json WaitText(const std::string &selector, const std::string &expected, int timeout_ms);
    // 用途：抓取PNG，返回编码数据供受管证据存储；日期：2026-11-03。
    Json Screenshot();
    // 用途：按有界标量及字符串数组Schema读取主文档公开数据，返回逐字段错误；日期：2026-11-05。
    Json Extract(const Json &schema, const Json &bindings);
    // 用途：将受控本地文件绑定到唯一文件输入控件，返回文件元数据而不读取文件正文；日期：2026-11-05。
    Json Upload(const std::string &reference, const std::wstring &path);
    // 用途：为当前页面设置受控下载目录并返回规范路径，禁止目录重解析点；日期：2026-11-05。
    Json ConfigureDownloads(const std::wstring &directory);
    // 用途：等待唯一匹配下载的完成事件并核对实际普通文件，超时不重发下载；日期：2026-11-05。
    Json WaitDownload(const std::string &file_name, int timeout_ms);
    // 用途：执行URL、文本、数量和控件状态断言，失败只返回结构化观测结果；日期：2026-11-05。
    Json Assert(const Json &assertion);
    // 用途：发布流程前严格校验断言定义，不连接浏览器或执行网页动作；日期：2026-11-05。
    static void ValidateAssertion(const Json &assertion);
    // 用途：预检整批动作后顺序执行，未知结果强制停批且不自动重放；日期：2026-11-04。
    Json Batch(const Json &actions, bool stop_on_error);
    // 用途：列出本实例已管理页面并显式收纳允许来源的子弹窗；日期：2026-11-04。
    Json Pages();
    // 用途：在本受管进程中新建独立页面会话并应用区域设置；日期：2026-11-04。
    Json OpenPage(const std::string &url);
    // 用途：切换当前页面并清除旧文档引用，禁止跨页面盲用引用；日期：2026-11-04。
    Json SwitchPage(const std::string &id);
    // 用途：仅关闭本实例登记的页面，保留其他页面与浏览器；日期：2026-11-04。
    Json ClosePage(const std::string &id);
    // 用途：按实例游标轮询有界脱敏事件，明确报告丢失和重置；日期：2026-11-04。
    Json Events(unsigned long long after, const std::string &epoch, size_t limit);
    // 用途：显式重建本受管实例连接及仍存活页面会话，不重放任何网页动作；日期：2026-11-04。
    Json Reconnect();
    // 用途：在网页动作前启用有界诊断元数据收集，路径仅按显式白名单展示；日期：2026-11-04。
    Json DiagnosticsStart(const std::vector<std::string> &routes);
    // 用途：按实例游标读取Console/异常/请求元数据，报告溢出及关联不确定性；日期：2026-11-04。
    Json DiagnosticsRead(unsigned long long after, const std::string &epoch, size_t limit);
    // 用途：停止接收新诊断并保留已有内存记录供查询，不改变网页请求；日期：2026-11-04。
    Json DiagnosticsStop();
    // 用途：为当前允许主文档预注册参数化录制器，绝不读取输入值；日期：2026-11-04。
    Json RecordingStart();
    // 用途：读取有界录制事件及待补全DSL草稿，不伪造断言；日期：2026-11-04。
    Json RecordingRead();
    // 用途：暂停/恢复/停止录制并解绑新文档脚本，保留已有证据；日期：2026-11-04。
    Json RecordingControl(const std::string &operation);
    // 用途：报告是否存在未停止录制，供共享入口排斥回放或页面切换；日期：2026-11-04。
    bool RecordingActive() const;
    // 用途：启用Chromium原生元素拾取覆盖层，使选取点击不进入网页业务；日期：2026-11-04。
    Json PickerStart();
    // 用途：读取唯一主文档选中元素及稳定CSS候选，拒绝过期文档；日期：2026-11-04。
    Json PickerRead();
    // 用途：退出原生拾取模式，不补发任何被拦截的点击；日期：2026-11-04。
    Json PickerStop();
    // 用途：在一个真实动作窗口中关联诊断，上下文仅含不敏感的运行及步骤标识；日期：2026-11-04。
    Json Diagnose(const Json &context, const std::function<Json()> &operation);
    // 用途：请求浏览器关闭后等待进程退出，必要时只终止本Job；日期：2026-11-03。
    Json Stop();
    // 用途：设置派发门，原子校验租约与发送；只供同一应用服务调用；日期：2026-11-03。
    void SetDispatchGate(std::function<void(const std::function<void()> &)> gate);
    // 用途：规范化HTTP(S)来源，拒绝凭据/文件/脚本地址；日期：2026-11-03。
    static std::string Origin(const std::string &url);
    // 用途：准入无认证HTTP/SOCKS5代理端点，禁止凭据及路径混入；日期：2026-11-04。
    static void ValidateProxyServer(const std::string &proxy_server);
    // 用途：验证UA、语言列表和时区的配置形状，实际时区由浏览器确认；日期：2026-11-04。
    static void ValidateRuntimeSettings(const Json &settings);
    // 用途：核对持久进程记录、PID创建时间及镜像，未知归属只阻断而不误杀；日期：2026-11-04。
    static Json InspectRecovery(const std::wstring &directory);

  private:
    // 用途：同步CDP请求，关联ID并缓存有界事件，不自动重试写入；日期：2026-11-03。
    Json Call(const std::string &method, const Json &parameters = Json::object(), bool page = true,
              bool controlled = false);
    // 用途：为当前已连接会话启用基础域并应用环境区域配置；日期：2026-11-04。
    void InitializePage();
    // 用途：绑定本进程目标为本地页面ID，失败仍保留可核对关闭的记录；日期：2026-11-04。
    std::string AttachPage(const std::string &target);
    // 用途：只缓存固定元数据事件，不保留网络正文/URL查询/控制台秘密；日期：2026-11-04。
    void RecordEvent(const Json &message);
    // 用途：只接收本实例隔离世界绑定的录制元数据，过滤所有未知字段；日期：2026-11-04。
    void RecordRecording(const Json &message);
    // 用途：处理已管理会话的诊断事件，不保留正文、标头、查询参数和控制台内容；日期：2026-11-04。
    void RecordDiagnostic(const Json &message);
    // 用途：追加固定预算诊断记录并维护丢失计数；日期：2026-11-04。
    void PushDiagnostic(Json event);
    // 用途：回收已关闭会话的上下文和未决关联，未完成请求只计丢失不伪造结果；日期：2026-11-04。
    void ForgetDiagnosticSession(const std::string &session);
    // 用途：从自有发现文件连接回环端点，禁止外部调用者指定原生地址；日期：2026-11-04。
    void Connect(const std::string &discovery, bool controlled);
    // 用途：检查当前页面真实来源及文档版本；日期：2026-11-03。
    Json Document();
    // 用途：拒绝过期/移除/跨文档引用并取得临时Runtime对象；日期：2026-11-03。
    std::string Resolve(const std::string &reference);
    // 用途：读取控件类型、几何和可操作条件，不读取输入内容或页面正文；日期：2026-11-04。
    Json InspectReference(const std::string &reference);
    // 用途：切换或导航前清理旧页面调试覆盖层，失效会话只清除本地记录；日期：2026-11-04。
    void HideHighlight();
    // 用途：登记绑定loaderId的引用，不复用旧文档ID；日期：2026-11-03。
    std::string Reference(int backend_node, const std::string &loader);
    // 用途：校验页面来源，未获授重定向不允许继续动作；日期：2026-11-03。
    void CheckOrigin(const std::string &url) const;

    struct NodeReference
    {
        int backend_node;
        std::string loader;
    };
    std::unique_ptr<BrowserDataPath> storage_path_;
    Handle job_;
    Handle process_;
    DWORD process_id_ = 0;
    FILETIME process_created_{};
    std::wstring executable_;
    InternetHandle http_;
    InternetHandle connection_;
    InternetHandle websocket_;
    std::string session_;
    Json version_;
    Json runtime_report_;
    Json runtime_settings_;
    struct PageSession
    {
        std::string target;
        std::string session;
        Json runtime;
    };
    std::map<std::string, PageSession> pages_;
    std::string active_page_;
    bool highlighted_ = false;
    bool sensitive_capture_blocked_ = false;
    std::wstring download_directory_;
    std::map<std::string, Json> downloads_;
    bool download_events_lost_ = false;
    std::deque<Json> events_;
    unsigned long long event_sequence_ = 0;
    bool diagnostics_active_ = false;
    bool diagnostics_suspended_ = false;
    std::string diagnostics_epoch_ = RandomId();
    unsigned long long diagnostics_sequence_ = 0;
    unsigned long long diagnostics_dropped_ = 0;
    unsigned long long diagnostics_pending_dropped_ = 0;
    unsigned long long diagnostics_scope_dropped_ = 0;
    std::deque<Json> diagnostics_;
    std::map<std::string, Json> diagnostic_requests_;
    std::map<std::string, bool> diagnostic_contexts_;
    std::vector<std::string> diagnostic_routes_;
    Json diagnostic_context_ = Json::object();
    std::wstring ownership_file_;
    std::wstring directory_;
    int request_sequence_ = 0;
    size_t ref_sequence_ = 0;
    const std::string reference_epoch_ = RandomId();
    std::map<std::string, NodeReference> references_;
    std::vector<std::string> origins_;
    bool stopped_ = false;
    std::function<void(const std::function<void()> &)> dispatch_gate_;
    bool recording_active_ = false;
    bool recording_paused_ = false;
    bool recording_incomplete_ = false;
    std::string recording_id_;
    std::string recording_page_;
    std::string recording_session_;
    std::string recording_script_;
    std::string recording_binding_;
    std::string recording_world_;
    std::deque<Json> recording_events_;
    size_t recording_bytes_ = 0;
    unsigned long long recording_sequence_ = 0;
    std::set<std::string> recording_documents_;
    std::set<std::string> recording_issues_;
    std::map<std::string, unsigned long long> recording_document_sequences_;
    bool picker_active_ = false;
    int picker_node_ = 0;
    std::string picker_page_;
    std::string picker_loader_;
};
} // namespace awb
