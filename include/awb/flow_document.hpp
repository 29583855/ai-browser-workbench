#pragma once
#include "awb/workflow.hpp"
#include <deque>
#include <set>

namespace awb
{
struct FlowPoint
{
    float x = 0;
    float y = 0;
};

class FlowDocument
{
  public:
    // 用途：创建可编辑的DSL1.0空流程，不创建浏览器控制器；日期：2026-11-04。
    FlowDocument();
    // 用途：保留完整定义并初始化独立视图，未来版本仅只读导入；日期：2026-11-04。
    void Import(const Json &definition);
    // 用途：读取完整可执行定义，布局变化不改变此内容；日期：2026-11-04。
    const Json &Definition() const;
    // 用途：读取只读标记，禁止编辑未知节点或未来版本；日期：2026-11-04。
    bool ReadOnly() const;
    // 用途：列出主流程及子流程名称，主流程使用空名称；日期：2026-11-04。
    std::vector<std::string> Flows() const;
    // 用途：切换当前流程，不修改任何执行语义；日期：2026-11-04。
    void SelectFlow(const std::string &name);
    // 用途：返回当前流程名供属性和布局定位；日期：2026-11-04。
    const std::string &CurrentFlow() const;
    // 用途：读取当前流程节点的完整原始字段；日期：2026-11-04。
    const Json &Steps() const;
    // 用途：按稳定ID查询当前流程中的节点；日期：2026-11-04。
    Json Step(const std::string &id) const;
    // 用途：读取独立布局坐标，不从行号推断节点身份；日期：2026-11-04。
    FlowPoint Position(const std::string &id) const;
    // 用途：统一把物理像素转换成画布逻辑坐标，适用缩放及DPI；日期：2026-11-04。
    static FlowPoint World(float x, float y, float dpi, float zoom, FlowPoint pan);
    // 用途：按逆绘制顺序命中节点，禁止使用错误缩放后的坐标；日期：2026-11-04。
    std::string Hit(FlowPoint point) const;
    // 用途：新增尚需配置的节点草稿，不绕过发布校验；日期：2026-11-04。
    std::string Add(const std::string &type, FlowPoint position);
    // 用途：更新稳定ID对应的属性，允许保存不完整但明确不可运行的草稿；日期：2026-11-04。
    void Update(const std::string &id, const Json &step);
    // 用途：修改顶层属性而保留节点、布局、保存路径和撤销历史；日期：2026-11-04。
    void Metadata(const Json &metadata);
    // 用途：创建有独立入口及结束节点的子流程，尚未调用时校验会提示；日期：2026-11-04。
    void AddFlow(const std::string &name);
    // 用途：修改当前流程入口并保留原版本及布局的撤销记录；日期：2026-11-04。
    void Entry(const std::string &id);
    // 用途：连接指定结构化端口，非法图交给统一执行器拒绝发布；日期：2026-11-04。
    void Connect(const std::string &source, const std::string &port, const std::string &target);
    // 用途：移动一组节点并保存一次撤销命令，不修改执行定义；日期：2026-11-04。
    void Move(const std::set<std::string> &ids, float dx, float dy);
    // 用途：删除选定节点，保留显式悬空引用供校验定位；日期：2026-11-04。
    void Remove(const std::set<std::string> &ids);
    // 用途：复制节点并重建选区内部ID关系，返回新节点选区；日期：2026-11-04。
    std::set<std::string> Duplicate(const std::set<std::string> &ids);
    // 用途：序列化选区及布局供专用剪贴板使用，不携带整个流程或运行输入；日期：2026-11-04。
    Json CopySelection(const std::set<std::string> &ids) const;
    // 用途：原子粘贴已校验选区并重建内部ID，外部连线留空待配置；日期：2026-11-04。
    std::set<std::string> PasteSelection(const Json &selection);
    // 用途：撤销最近业务或布局修改，历史最多100项及8MiB；日期：2026-11-04。
    bool Undo();
    // 用途：恢复已撤销的修改，新编辑会清空重做链；日期：2026-11-04。
    bool Redo();
    // 用途：读取已保留的历史条目数供界面提示；日期：2026-11-04。
    size_t UndoCount() const;
    // 用途：以带revision的原子草稿格式保存，并拒绝过期覆盖；日期：2026-11-04。
    void Save(const std::wstring &path);
    // 用途：加载草稿或原始流程JSON，失败时不替换当前文档；日期：2026-11-04。
    void Load(const std::wstring &path);
    // 用途：返回包含独立布局的恢复快照，不修改手动保存revision；日期：2026-11-04。
    Json RecoverySnapshot() const;
    // 用途：原子保存独立恢复文件，保留原草稿及其并发revision；日期：2026-11-04。
    void SaveRecovery(const std::wstring &path) const;
    // 用途：加载已知恢复格式且强制另存，禁止恢复文件指定覆盖路径；日期：2026-11-04。
    void LoadRecovery(const std::wstring &path);

  private:
    // 用途：取得当前流程对象供草稿修改，主流程直接引用顶层定义；日期：2026-11-04。
    Json &Flow();
    // 用途：读取当前流程对象，不生成不存在的子流程；日期：2026-11-04。
    const Json &Flow() const;
    // 用途：检查编辑权限并登记完整修改前状态；日期：2026-11-04。
    void Remember();
    // 用途：统一裁剪撤销和重做两条链，合计不超过100项和8MiB；日期：2026-11-04。
    void TrimHistory();
    // 用途：补齐缺失节点视图，不删除未知字段或旧布局；日期：2026-11-04。
    void EnsureViews();
    // 用途：检查结构是否适合编辑，未知节点保留为只读；日期：2026-11-04。
    void DetectReadOnly();
    Json definition_;
    Json views_ = Json::object();
    std::string flow_;
    bool read_only_ = false;
    std::deque<Json> undo_;
    std::deque<Json> redo_;
    std::wstring saved_path_;
    unsigned long long revision_ = 0;
};
} // namespace awb
