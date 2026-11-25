#include "awb/flow_document.hpp"
#include <algorithm>
#include <cmath>

namespace awb
{
namespace
{
const std::set<std::string> NodeTypes = {
    "navigate", "fill",           "click",  "select",    "assertText",  "condition",
    "loop",     "call",           "end",    "set",       "readText",    "foreach",
    "upload",   "downloadConfig", "assert", "readTexts", "downloadWait"};
// 用途：集中识别需使用DSL1.1的节点，新增、属性替换和粘贴保持版本一致；日期：2026-11-05。
bool ExtendedNode(const std::string &type)
{
    return type == "set" || type == "readText" || type == "foreach" || type == "upload" ||
           type == "downloadConfig" || type == "downloadWait" || type == "assert" || type == "readTexts";
}
// 用途：产生与业务无关的稳定节点ID，复制时不会继承旧身份；日期：2026-11-04。
std::string NewNodeId()
{
    return "n_" + RandomId();
}
// 用途：校验节点集合的有限结构，以便编辑器安全枚举不完整草稿；日期：2026-11-04。
bool EditableFlow(const Json &flow)
{
    if (!flow.is_object() || !flow.contains("steps") || !flow.at("steps").is_array() ||
        flow.at("steps").size() > 256)
        return false;
    std::set<std::string> ids;
    for (const auto &step : flow.at("steps"))
    {
        if (!step.is_object() || !step.contains("id") || !step.at("id").is_string() ||
            !step.contains("type") || !step.at("type").is_string())
            return false;
        const auto id = step.at("id").get<std::string>();
        if (id.empty() || id.size() > 64 || !ids.insert(id).second ||
            !NodeTypes.count(step.at("type").get<std::string>()))
            return false;
    }
    return true;
}
} // namespace

// 用途：提供合法的单end初始定义，来源由用户在属性中明确配置；日期：2026-11-04。
FlowDocument::FlowDocument()
{
    Import({{"schemaVersion", "1.0"},
            {"name", "可视化新流程"},
            {"version", 1},
            {"inputSchema",
             {{"type", "object"},
              {"properties", Json::object()},
              {"required", Json::array()},
              {"additionalProperties", false}}},
            {"policy",
             {{"allowedOrigins", Json::array({"http://127.0.0.1:3000"})},
              {"maxSteps", 1024},
              {"maxDurationMs", 300000}}},
            {"entry", "done"},
            {"steps", Json::array({{{"id", "done"}, {"type", "end"}}})}});
}
// 用途：保留原定义全部字段，清空仅属于上一文档的布局及历史；日期：2026-11-04。
void FlowDocument::Import(const Json &definition)
{
    if (!definition.is_object() || definition.dump().size() > 256 * 1024)
        throw Error("FLOW_IMPORT_INVALID", "Flow definition must be an object within 256 KiB");
    definition_ = definition;
    views_ = Json::object();
    flow_.clear();
    undo_.clear();
    redo_.clear();
    saved_path_.clear();
    revision_ = 0;
    DetectReadOnly();
    EnsureViews();
}
// 用途：只读返回无视图内容的执行定义；日期：2026-11-04。
const Json &FlowDocument::Definition() const
{
    return definition_;
}
// 用途：报告未知版本或节点造成的只读状态；日期：2026-11-04。
bool FlowDocument::ReadOnly() const
{
    return read_only_;
}
// 用途：枚举现存流程，畸形未来数据保持只读且不展开；日期：2026-11-04。
std::vector<std::string> FlowDocument::Flows() const
{
    std::vector<std::string> names = {""};
    if (definition_.contains("subflows") && definition_.at("subflows").is_object())
        for (auto it = definition_.at("subflows").begin(); it != definition_.at("subflows").end(); ++it)
            names.push_back(it.key());
    return names;
}
// 用途：验证当前选中的流程确实存在；日期：2026-11-04。
void FlowDocument::SelectFlow(const std::string &name)
{
    const auto names = Flows();
    if (std::find(names.begin(), names.end(), name) == names.end())
        throw Error("FLOW_NOT_FOUND", "The selected subflow does not exist");
    flow_ = name;
}
// 用途：读取当前视图流程名；日期：2026-11-04。
const std::string &FlowDocument::CurrentFlow() const
{
    return flow_;
}
// 用途：提供可编辑的当前流程对象；日期：2026-11-04。
Json &FlowDocument::Flow()
{
    return flow_.empty() ? definition_ : definition_.at("subflows").at(flow_);
}
// 用途：提供当前流程对象的只读引用；日期：2026-11-04。
const Json &FlowDocument::Flow() const
{
    return flow_.empty() ? definition_ : definition_.at("subflows").at(flow_);
}
// 用途：对未知结构返回空视图，同时保留原始文件内容；日期：2026-11-04。
const Json &FlowDocument::Steps() const
{
    static const Json empty = Json::array();
    return Flow().is_object() && Flow().contains("steps") && Flow().at("steps").is_array()
               ? Flow().at("steps")
               : empty;
}
// 用途：按ID查询节点并对悬空选择给出明确错误；日期：2026-11-04。
Json FlowDocument::Step(const std::string &id) const
{
    for (const auto &step : Steps())
        if (step.is_object() && step.value("id", "") == id)
            return step;
    throw Error("FLOW_NODE_NOT_FOUND", "Selected node no longer exists");
}
// 用途：安全读取有限数值坐标，损坏视图不传播无穷值；日期：2026-11-04。
FlowPoint FlowDocument::Position(const std::string &id) const
{
    const auto key = flow_.empty() ? "$main" : flow_;
    if (!views_.contains(key) || !views_.at(key).contains(id))
        return {40, 40};
    const auto &value = views_.at(key).at(id);
    if (!value.is_object() || !value.contains("x") || !value.contains("y") || !value.at("x").is_number() ||
        !value.at("y").is_number())
        return {40, 40};
    const float x = value.at("x").get<float>(), y = value.at("y").get<float>();
    return std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 100000 && std::abs(y) <= 100000
               ? FlowPoint{x, y}
               : FlowPoint{40, 40};
}
// 用途：统一屏幕像素、DPI、缩放和平移的逆变换；日期：2026-11-04。
FlowPoint FlowDocument::World(float x, float y, float dpi, float zoom, FlowPoint pan)
{
    if (!std::isfinite(dpi) || !std::isfinite(zoom) || dpi < 48 || dpi > 768 || zoom < 0.1f || zoom > 4)
        throw Error("FLOW_VIEW_INVALID", "View transform is outside its bounded range");
    return {(x * 96 / dpi - pan.x) / zoom, (y * 96 / dpi - pan.y) / zoom};
}
// 用途：后绘制节点优先命中，节点尺寸以逻辑像素定义；日期：2026-11-04。
std::string FlowDocument::Hit(FlowPoint point) const
{
    for (auto it = Steps().rbegin(); it != Steps().rend(); ++it)
    {
        if (!it->is_object() || !it->contains("id") || !it->at("id").is_string())
            continue;
        const auto id = it->at("id").get<std::string>();
        const auto p = Position(id);
        if (point.x >= p.x && point.x <= p.x + 180 && point.y >= p.y && point.y <= p.y + 64)
            return id;
    }
    return "";
}
// 用途：记录有界撤销状态，未来版本不允许修改；日期：2026-11-04。
void FlowDocument::Remember()
{
    if (read_only_)
        throw Error("FLOW_READ_ONLY", "Unknown flow version or node is preserved read-only");
    undo_.push_back({{"definition", definition_}, {"views", views_}, {"flow", flow_}});
    redo_.clear();
    TrimHistory();
}
// 用途：撤销与重做共享同一预算，优先丢弃离当前最远的历史；日期：2026-11-04。
void FlowDocument::TrimHistory()
{
    size_t bytes = 0;
    for (const auto &item : undo_)
        bytes += item.dump().size();
    for (const auto &item : redo_)
        bytes += item.dump().size();
    while (undo_.size() + redo_.size() > 100 || bytes > 8 * 1024 * 1024)
    {
        auto &chain = undo_.empty() ? redo_ : undo_;
        bytes -= chain.front().dump().size();
        chain.pop_front();
    }
}
// 用途：只为已知结构创建布局，不删除任何执行字段；日期：2026-11-04。
void FlowDocument::EnsureViews()
{
    const auto previous = flow_;
    for (const auto &name : Flows())
    {
        flow_ = name;
        const auto key = name.empty() ? "$main" : name;
        size_t index = 0;
        for (const auto &step : Steps())
        {
            if (step.is_object() && step.contains("id") && step.at("id").is_string())
            {
                const auto id = step.at("id").get<std::string>();
                if (!views_.contains(key) || !views_.at(key).contains(id))
                    views_[key][id] = {{"x", 40 + (index % 4) * 230}, {"y", 40 + (index / 4) * 110}};
            }
            ++index;
        }
    }
    flow_ = previous;
}
// 用途：识别未知节点或结构，避免无损导入后被当作可执行草稿；日期：2026-11-04。
void FlowDocument::DetectReadOnly()
{
    read_only_ = !definition_.contains("schemaVersion") ||
                 (definition_.at("schemaVersion") != "1.0" && definition_.at("schemaVersion") != "1.1") ||
                 !EditableFlow(definition_);
    if (definition_.contains("subflows"))
    {
        if (!definition_.at("subflows").is_object() || definition_.at("subflows").size() > 32)
            read_only_ = true;
        else
            for (const auto &subflow : definition_.at("subflows"))
                read_only_ = read_only_ || !EditableFlow(subflow);
    }
}
// 用途：创建需完善属性的动作及文件/数组节点，自动采用正确DSL版本；日期：2026-11-05。
std::string FlowDocument::Add(const std::string &type, FlowPoint position)
{
    if (!NodeTypes.count(type) || Steps().size() >= 256)
        throw Error("FLOW_NODE_INVALID", "Unknown node type or draft node budget exceeded");
    Remember();
    const auto id = NewNodeId();
    Json step{{"id", id}, {"type", type}};
    if (ExtendedNode(type))
    {
        definition_["schemaVersion"] = "1.1";
        if (!definition_.contains("variables"))
            definition_["variables"] = Json::object();
    }
    if (type == "set")
    {
        step["variable"] = "value";
        step["value"] = "";
    }
    if (type == "readText" || type == "readTexts")
    {
        step["variable"] = "value";
        step["selector"] = "";
        if (type == "readTexts")
            step["maxItems"] = 32;
    }
    if (type == "upload")
    {
        step["selector"] = "";
        step["path"] = {{"input", "/uploadPath"}};
    }
    if (type == "downloadConfig")
        step["directory"] = {{"input", "/downloadDirectory"}};
    if (type == "downloadWait")
    {
        step["fileName"] = "report.csv";
        step["timeoutMs"] = 10000;
    }
    if (type == "assert")
        step["assertion"] = {{"type", "urlContains"}, {"expected", "/"}};
    if (type == "foreach")
    {
        step["items"] = {{"input", "/items"}};
        step["item"] = "item";
        step["maxIterations"] = 32;
        step["flow"] = "";
    }
    if (type == "navigate")
        step["url"] = "http://127.0.0.1:3000/";
    if (type == "fill" || type == "click" || type == "select" || type == "assertText")
        step["selector"] = "";
    if (type == "fill")
        step["value"] = {{"input", "/value"}};
    if (type == "select")
        step["values"] = Json::array();
    if (type == "assertText")
    {
        step["expected"] = "";
        step["timeoutMs"] = 5000;
    }
    if (type == "click")
        step["postcondition"] = {{"selector", ""}, {"expected", ""}, {"timeoutMs", 5000}};
    if (type == "condition")
    {
        step["when"] = {{"op", "eq"}, {"left", true}, {"right", true}};
        step["then"] = "";
        step["else"] = "";
    }
    else if (type == "loop")
    {
        step["count"] = 1;
        step["maxIterations"] = 1;
        step["flow"] = "";
    }
    else if (type == "call")
        step["flow"] = "";
    if (type != "condition" && type != "end")
        step["next"] = "";
    Flow()["steps"].push_back(step);
    views_[flow_.empty() ? "$main" : flow_][id] = {{"x", position.x}, {"y", position.y}};
    return id;
}
// 用途：替换节点显式属性并升级扩展DSL，节点ID保持不变；日期：2026-11-05。
void FlowDocument::Update(const std::string &id, const Json &step)
{
    Step(id);
    if (!step.is_object() || step.value("id", "") != id || !step.contains("type") ||
        !step.at("type").is_string() || !NodeTypes.count(step.at("type").get<std::string>()) ||
        step.dump().size() > 65536)
        throw Error("FLOW_NODE_INVALID", "Keep node ID and use a supported bounded node object");
    Remember();
    for (auto &item : Flow()["steps"])
        if (item.at("id") == id)
        {
            item = step;
            if (ExtendedNode(step.at("type")) || step.contains("onError"))
            {
                definition_["schemaVersion"] = "1.1";
                if (!definition_.contains("variables"))
                    definition_["variables"] = Json::object();
            }
            return;
        }
}
// 用途：连接普通端口及只读错误分支，不把图回边转成无界循环；日期：2026-11-05。
void FlowDocument::Connect(const std::string &source, const std::string &port, const std::string &target)
{
    auto step = Step(source);
    Step(target);
    const auto type = step.at("type").get<std::string>();
    if (port == "onError")
    {
        if (definition_.at("schemaVersion") != "1.1" ||
            (type != "set" && type != "readText" && type != "readTexts" && type != "assert" &&
             type != "assertText") ||
            !step.contains("onError"))
            throw Error("FLOW_PORT_INVALID", "Configure explicit onError codes before connecting its output");
        step["onError"]["next"] = target;
        Update(source, step);
        return;
    }
    if ((type == "condition" && port != "then" && port != "else") ||
        (type != "condition" && (port != "next" || type == "end")))
        throw Error("FLOW_PORT_INVALID", "The selected node does not expose that output port");
    step[port] = target;
    Update(source, step);
}
// 用途：只替换顶层元数据，避免属性编辑重置历史、布局和磁盘revision；日期：2026-11-04。
void FlowDocument::Metadata(const Json &metadata)
{
    if (!metadata.is_object() || metadata.contains("steps") || metadata.contains("subflows") ||
        !metadata.contains("schemaVersion") ||
        (metadata.at("schemaVersion") != "1.0" && metadata.at("schemaVersion") != "1.1") ||
        metadata.dump().size() > 128 * 1024)
        throw Error("FLOW_PROPERTY_INVALID",
                    "Metadata must be a bounded supported DSL object without node arrays");
    Remember();
    auto replacement = metadata;
    replacement["steps"] = definition_.at("steps");
    if (definition_.contains("subflows"))
        replacement["subflows"] = definition_.at("subflows");
    definition_ = replacement;
}
// 用途：检查子流程名称与数量，创建新的独立稳定节点；日期：2026-11-04。
void FlowDocument::AddFlow(const std::string &name)
{
    if (name.empty() || name.size() > 64 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos ||
        (definition_.contains("subflows") &&
         (definition_.at("subflows").contains(name) || definition_.at("subflows").size() >= 32)))
        throw Error("FLOW_NAME_INVALID", "Use a unique bounded ASCII subflow name");
    Remember();
    const auto id = NewNodeId();
    definition_["subflows"][name] = {{"entry", id}, {"steps", Json::array({{{"id", id}, {"type", "end"}}})}};
    flow_ = name;
    EnsureViews();
}
// 用途：入口选择与其他图编辑一样可以撤销，不重新导入整份定义；日期：2026-11-04。
void FlowDocument::Entry(const std::string &id)
{
    Step(id);
    Remember();
    Flow()["entry"] = id;
}
// 用途：一次手势移动整组选区，布局与执行定义严格分离；日期：2026-11-04。
void FlowDocument::Move(const std::set<std::string> &ids, float dx, float dy)
{
    if (ids.empty() || (!dx && !dy))
        return;
    if (!std::isfinite(dx) || !std::isfinite(dy))
        throw Error("FLOW_VIEW_INVALID", "Non-finite movement rejected");
    for (const auto &id : ids)
    {
        const auto p = Position(id);
        Step(id);
        if (std::abs(p.x + dx) > 100000 || std::abs(p.y + dy) > 100000)
            throw Error("FLOW_VIEW_INVALID", "Canvas coordinate budget exceeded");
    }
    Remember();
    for (const auto &id : ids)
    {
        const auto p = Position(id);
        views_[flow_.empty() ? "$main" : flow_][id] = {{"x", p.x + dx}, {"y", p.y + dy}};
    }
}
// 用途：删除选区节点，后续统一图校验显示所有悬空边；日期：2026-11-04。
void FlowDocument::Remove(const std::set<std::string> &ids)
{
    if (ids.empty())
        return;
    Remember();
    auto &steps = Flow()["steps"];
    steps.erase(std::remove_if(steps.begin(), steps.end(),
                               // 用途：只删除明确选定ID；日期：2026-11-04。
                               [&](const Json &step)
                               { return ids.count(step.at("id").get<std::string>()) != 0; }),
                steps.end());
}
// 用途：复制选区并映射内部边，外部目标保留为可见引用；日期：2026-11-04。
std::set<std::string> FlowDocument::Duplicate(const std::set<std::string> &ids)
{
    if (ids.empty())
        return {};
    if (Steps().size() + ids.size() > 256)
        throw Error("FLOW_NODE_INVALID", "Draft node budget exceeded");
    std::map<std::string, std::string> mapping;
    std::vector<Json> copies;
    for (const auto &id : ids)
    {
        copies.push_back(Step(id));
        mapping[id] = NewNodeId();
    }
    Remember();
    std::set<std::string> result;
    for (auto copy : copies)
    {
        const auto old_id = copy.at("id").get<std::string>();
        const auto id = mapping.at(old_id);
        copy["id"] = id;
        for (const auto *port : {"next", "then", "else"})
            if (copy.contains(port) && copy.at(port).is_string() && mapping.count(copy.at(port)))
                copy[port] = mapping.at(copy.at(port));
        if (copy.contains("onError") && copy.at("onError").is_object() &&
            copy.at("onError").contains("next") && copy.at("onError").at("next").is_string() &&
            mapping.count(copy.at("onError").at("next")))
            copy["onError"]["next"] = mapping.at(copy.at("onError").at("next"));
        Flow()["steps"].push_back(copy);
        const auto p = Position(old_id);
        views_[flow_.empty() ? "$main" : flow_][id] = {{"x", p.x + 24}, {"y", p.y + 24}};
        result.insert(id);
    }
    return result;
}
// 用途：撤销一步并把当前完整状态交给重做链；日期：2026-11-04。
Json FlowDocument::CopySelection(const std::set<std::string> &ids) const
{
    if (read_only_ || ids.empty())
        throw Error("FLOW_SELECTION_REQUIRED", "Select editable nodes before copying");
    Json nodes = Json::array();
    for (const auto &id : ids)
    {
        const auto point = Position(id);
        nodes.push_back({{"step", Step(id)}, {"position", {{"x", point.x}, {"y", point.y}}}});
    }
    Json result{{"selectionVersion", "1.0"}, {"nodes", nodes}};
    if (result.dump().size() > 256 * 1024)
        throw Error("FLOW_CLIPBOARD_TOO_LARGE", "Selection exceeds 256 KiB");
    return result;
}
// 用途：校验选区后一次提交历史，粘贴扩展节点时升级DSL并初始化变量表；日期：2026-11-05。
std::set<std::string> FlowDocument::PasteSelection(const Json &selection)
{
    if (!selection.is_object() || selection.value("selectionVersion", "") != "1.0" ||
        !selection.contains("nodes") || !selection.at("nodes").is_array() || selection.at("nodes").empty() ||
        selection.at("nodes").size() + Steps().size() > 256 || selection.dump().size() > 256 * 1024)
        throw Error("FLOW_CLIPBOARD_INVALID", "Invalid or oversized node selection");
    Json flow{{"steps", Json::array()}};
    std::map<std::string, std::string> mapping;
    for (const auto &node : selection.at("nodes"))
    {
        if (!node.is_object() || !node.contains("step") || !node.contains("position") ||
            !node.at("position").is_object())
            throw Error("FLOW_CLIPBOARD_INVALID", "Missing step or position");
        const auto &p = node.at("position");
        for (const auto *axis : {"x", "y"})
            if (!p.contains(axis) || !p.at(axis).is_number() || !std::isfinite(p.at(axis).get<double>()) ||
                std::abs(p.at(axis).get<double>()) > 99900)
                throw Error("FLOW_CLIPBOARD_INVALID", "Invalid node coordinates");
        flow["steps"].push_back(node.at("step"));
    }
    if (!EditableFlow(flow))
        throw Error("FLOW_CLIPBOARD_INVALID", "Unsupported or duplicate node identity");
    for (const auto &step : flow.at("steps"))
        mapping[step.at("id").get<std::string>()] = NewNodeId();
    Remember();
    std::set<std::string> ids;
    for (const auto &node : selection.at("nodes"))
    {
        auto step = node.at("step");
        const auto id = mapping.at(step.at("id").get<std::string>());
        step["id"] = id;
        if (step.contains("onError") && step.at("onError").is_object() &&
            step.at("onError").contains("next") && step.at("onError").at("next").is_string())
        {
            const auto old = step.at("onError").at("next").get<std::string>();
            step["onError"]["next"] = mapping.count(old) ? mapping.at(old) : "";
        }
        for (const auto *port : {"next", "then", "else"})
            if (step.contains(port) && step.at(port).is_string())
            {
                const auto old = step.at(port).get<std::string>();
                step[port] = mapping.count(old) ? mapping.at(old) : "";
            }
        Flow()["steps"].push_back(step);
        views_[flow_.empty() ? "$main" : flow_][id] = {{"x", node.at("position").at("x").get<float>() + 24},
                                                       {"y", node.at("position").at("y").get<float>() + 24}};
        ids.insert(id);
    }
    for (const auto &step : Flow().at("steps"))
        if (step.contains("onError") || ExtendedNode(step.at("type")))
        {
            definition_["schemaVersion"] = "1.1";
            if (!definition_.contains("variables"))
                definition_["variables"] = Json::object();
        }
    return ids;
}
// 用途：撤销一步并把当前完整状态交给重做链；日期：2026-11-04。
bool FlowDocument::Undo()
{
    if (undo_.empty())
        return false;
    redo_.push_back({{"definition", definition_}, {"views", views_}, {"flow", flow_}});
    definition_ = undo_.back().at("definition");
    views_ = undo_.back().at("views");
    flow_ = undo_.back().value("flow", "");
    undo_.pop_back();
    TrimHistory();
    return true;
}
// 用途：重做一步并保留相应撤销状态；日期：2026-11-04。
bool FlowDocument::Redo()
{
    if (redo_.empty())
        return false;
    undo_.push_back({{"definition", definition_}, {"views", views_}, {"flow", flow_}});
    definition_ = redo_.back().at("definition");
    views_ = redo_.back().at("views");
    flow_ = redo_.back().value("flow", "");
    redo_.pop_back();
    TrimHistory();
    return true;
}
// 用途：展示当前可撤销数，不假称无限历史；日期：2026-11-04。
size_t FlowDocument::UndoCount() const
{
    return undo_.size();
}
// 用途：持有同路径独占锁后比较revision，原子写入以保护另一编辑器的修改；日期：2026-11-04。
void FlowDocument::Save(const std::wstring &path)
{
    const auto absolute = SafeAbsolute(path);
    Handle lock(CreateFileW((absolute + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (lock.get() == INVALID_HANDLE_VALUE)
        throw Error("FLOW_SAVE_BUSY", "Another editor is saving this draft");
    const bool exists = GetFileAttributesW(absolute.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (exists)
    {
        const auto disk = ParseExternalJson(ReadFileBounded(absolute, 1024 * 1024), 48);
        if (absolute != saved_path_ || !disk.is_object() || disk.value("draftVersion", "") != "1.0" ||
            disk.value("revision", 0ULL) != revision_)
            throw Error("FLOW_REVISION_CONFLICT", "Draft changed or Save As would overwrite another file");
    }
    else if (absolute == saved_path_ && revision_)
        throw Error("FLOW_REVISION_CONFLICT", "Saved draft was removed; use a new Save As path");
    const auto revision = revision_ + 1;
    const Json envelope{
        {"draftVersion", "1.0"}, {"revision", revision}, {"definition", definition_}, {"views", views_}};
    const auto bytes = envelope.dump(2) + "\n";
    if (bytes.size() > 1024 * 1024)
        throw Error("FLOW_DRAFT_TOO_LARGE", "Draft exceeds 1 MiB");
    AtomicWrite(absolute, bytes);
    saved_path_ = absolute;
    revision_ = revision;
}
// 用途：先解析并校验视图容器再替换当前文档，保留文件编码原样；日期：2026-11-04。
void FlowDocument::Load(const std::wstring &path)
{
    const auto absolute = SafeAbsolute(path);
    const auto data = ParseExternalJson(ReadFileBounded(absolute, 1024 * 1024), 48);
    FlowDocument candidate;
    if (data.is_object() && data.contains("draftVersion"))
    {
        if (data.at("draftVersion") != "1.0" || !data.contains("revision") ||
            !data.at("revision").is_number_unsigned() || data.at("revision").get<unsigned long long>() == 0 ||
            !data.contains("views") || !data.at("views").is_object())
            throw Error("FLOW_DRAFT_INVALID", "Unsupported or malformed draft envelope");
        candidate.Import(data.at("definition"));
        candidate.views_ = data.at("views");
        candidate.EnsureViews();
        candidate.saved_path_ = absolute;
        candidate.revision_ = data.at("revision").get<unsigned long long>();
    }
    else
        candidate.Import(data);
    *this = std::move(candidate);
}
// 用途：恢复数据只保存已应用的模型与布局，不写入执行态或运行输入；日期：2026-11-04。
Json FlowDocument::RecoverySnapshot() const
{
    return {{"recoveryVersion", "1.0"}, {"definition", definition_}, {"views", views_}, {"flow", flow_}};
}
// 用途：恢复文件独立于原文件，写失败时不损坏上一份原子快照；日期：2026-11-04。
void FlowDocument::SaveRecovery(const std::wstring &path) const
{
    const auto bytes = RecoverySnapshot().dump(2) + "\n";
    if (bytes.size() > 1024 * 1024)
        throw Error("FLOW_DRAFT_TOO_LARGE", "Recovery snapshot exceeds 1 MiB");
    AtomicWrite(SafeAbsolute(path), bytes);
}
// 用途：恢复失败保持当前文档，恢复成功必须由用户另存到新路径；日期：2026-11-04。
void FlowDocument::LoadRecovery(const std::wstring &path)
{
    const auto data = ParseExternalJson(ReadFileBounded(SafeAbsolute(path), 1024 * 1024), 48);
    if (!data.is_object() || data.value("recoveryVersion", "") != "1.0" || !data.contains("definition") ||
        !data.contains("views") || !data.at("views").is_object() || !data.contains("flow") ||
        !data.at("flow").is_string())
        throw Error("FLOW_RECOVERY_INVALID", "Unsupported or malformed recovery snapshot");
    FlowDocument candidate;
    candidate.Import(data.at("definition"));
    candidate.views_ = data.at("views");
    candidate.EnsureViews();
    candidate.SelectFlow(data.at("flow").get<std::string>());
    *this = std::move(candidate);
}
} // namespace awb
