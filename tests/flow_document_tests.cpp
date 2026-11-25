#include "awb/flow_document.hpp"
#include <cmath>
#include <iostream>

namespace
{
// 用途：对草稿实际状态进行断言，失败时明确终止本轮测试；日期：2026-11-04。
void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
// 用途：核对预期错误码，避免把其他异常当正确拒绝；日期：2026-11-04。
template <typename Function> void Expect(const char *code, Function operation)
{
    try
    {
        operation();
    }
    catch (const awb::Error &error)
    {
        Require(error.code == code, "Unexpected flow error code");
        return;
    }
    throw std::runtime_error("Expected flow rejection was not raised");
}
} // namespace

// 用途：测试草稿语义、布局、历史、图校验及磁盘revision，不替代浏览器录制验收；日期：2026-11-04。
void RunFlowDocumentTests()
{
    awb::FlowDocument document;
    const auto original = document.Definition();
    // 用途：核对新增文件/数组/断言节点的编辑、版本提升、跨文档粘贴和撤销；日期：2026-11-05。
    for (const auto *type : {"upload", "downloadConfig", "downloadWait", "assert", "readTexts"})
    {
        awb::FlowDocument extended;
        const auto node = extended.Add(type, {20, 40});
        Require(extended.Definition().at("schemaVersion") == "1.1" && !extended.ReadOnly(),
                "Extended node did not upgrade editable DSL");
        awb::FlowDocument copied;
        const auto selection = copied.PasteSelection(extended.CopySelection({node}));
        Require(selection.size() == 1 && copied.Definition().at("schemaVersion") == "1.1" &&
                    copied.Definition().contains("variables") && !copied.ReadOnly(),
                "Extended clipboard lost node or DSL version");
        Require(copied.Undo() && copied.Definition().at("schemaVersion") == "1.0",
                "Extended paste did not undo atomically");
    }
    // 用途：错误边复制/跨文档粘贴必须重建内部引用并保留DSL版本；日期：2026-11-05。
    auto routed = original;
    routed["schemaVersion"] = "1.1";
    routed["entry"] = "observe";
    routed["steps"] = awb::Json::array(
        {{{"id", "observe"},
          {"type", "assertText"},
          {"selector", "#ready"},
          {"expected", "ready"},
          {"timeoutMs", 50},
          {"next", "done"},
          {"onError", {{"codes", awb::Json::array({"ELEMENT_NOT_FOUND"})}, {"next", "done"}}}},
         {{"id", "done"}, {"type", "end"}}});
    awb::FlowDocument errors;
    errors.Import(routed);
    const auto clones = errors.Duplicate({"observe", "done"});
    for (const auto &id : clones)
        if (errors.Step(id).contains("onError"))
            Require(clones.count(errors.Step(id).at("onError").at("next")) != 0,
                    "Duplicate error edge retained old identity");
    awb::FlowDocument pasted_errors;
    const auto pasted_error_ids = pasted_errors.PasteSelection(errors.CopySelection({"observe", "done"}));
    Require(pasted_errors.Definition().at("schemaVersion") == "1.1",
            "New DSL node paste did not upgrade version");
    for (const auto &id : pasted_error_ids)
        if (pasted_errors.Step(id).contains("onError"))
            Require(pasted_error_ids.count(pasted_errors.Step(id).at("onError").at("next")) != 0,
                    "Paste error edge retained old identity");
    errors.Connect("observe", "onError", "observe");
    Require(!awb::ValidateWorkflow(errors.Definition()).at("valid").get<bool>(),
            "Error-edge cycle was accepted");
    Require(awb::ValidateWorkflow(original).at("valid"), "Initial draft invalid");
    for (const auto *type :
         {"navigate", "fill", "click", "select", "assertText", "condition", "loop", "call"})
    {
        awb::FlowDocument sample;
        const auto id = sample.Add(type, {300, 40});
        auto node = sample.Step(id);
        if (node.contains("next"))
            node["next"] = "done";
        if (node.contains("selector"))
            node["selector"] = "#user";
        if (node.at("type") == "click")
            node["postcondition"] = {{"selector", "#status"}, {"expected", "ready"}, {"timeoutMs", 5000}};
        if (node.at("type") == "condition")
        {
            node["then"] = "done";
            node["else"] = "done";
        }
        if (node.at("type") == "loop" || node.at("type") == "call")
        {
            sample.AddFlow("child");
            sample.SelectFlow("");
            node["flow"] = "child";
        }
        sample.Update(id, node);
        auto metadata = sample.Definition();
        metadata.erase("steps");
        metadata.erase("subflows");
        metadata["inputSchema"]["properties"]["value"] = {{"type", "string"}, {"maxLength", 4096}};
        metadata["inputSchema"]["required"] = awb::Json::array({"value"});
        sample.Metadata(metadata);
        sample.Entry(id);
        const auto validation = awb::ValidateWorkflow(sample.Definition());
        if (!validation.at("valid").get<bool>())
            throw std::runtime_error(std::string("Node template invalid: ") + type + " " + validation.dump());
    }
    document.Move({"done"}, 20, 30);
    Require(document.Definition() == original, "Layout changed executable definition");
    Require(document.Position("done").x == 60 && document.Position("done").y == 70, "Move failed");
    Require(document.Undo() && document.Position("done").x == 40, "Layout undo failed");
    Require(document.Redo() && document.Position("done").x == 60, "Layout redo failed");
    for (const float dpi : {96.0f, 144.0f, 192.0f})
        for (const float zoom : {0.1f, 0.5f, 1.0f, 2.0f, 4.0f})
        {
            const awb::FlowPoint pan{17, 31};
            const auto point = awb::FlowDocument::World((80 * zoom + pan.x) * dpi / 96,
                                                        (90 * zoom + pan.y) * dpi / 96, dpi, zoom, pan);
            Require(std::abs(point.x - 80) < 0.01f && document.Hit(point) == "done", "DPI hit test failed");
        }
    // 用途：无效变换必须明确拒绝；日期：2026-11-04。
    Expect("FLOW_VIEW_INVALID", [] { awb::FlowDocument::World(10, 10, 96, 0, {}); });
    const auto navigate = document.Add("navigate", {350, 100});
    auto step = document.Step(navigate);
    step["next"] = "done";
    document.Update(navigate, step);
    auto definition = document.Definition();
    definition["entry"] = navigate;
    document.Import(definition);
    Require(awb::ValidateWorkflow(document.Definition()).at("valid"), "Expected valid definition");
    document.Connect(navigate, "next", navigate);
    const bool cycle_rejected = !awb::ValidateWorkflow(document.Definition()).at("valid").get<bool>();
    Require(cycle_rejected, "Graph cycle was executable");
    document.Undo();
    const auto copies = document.Duplicate({navigate, "done"});
    Require(copies.size() == 2, "Copy did not allocate unique IDs");
    for (const auto &id : copies)
    {
        const auto copy = document.Step(id);
        if (copy.at("type") == "navigate")
            Require(copies.count(copy.at("next").get<std::string>()) == 1, "Copy shared old internal edge");
    }
    document.Remove(copies);
    Require(awb::ValidateWorkflow(document.Definition()).at("valid"), "Expected valid definition");
    document.Undo();
    Require(document.Steps().size() == 4, "Delete undo failed");
    document.Redo();
    Require(document.Steps().size() == 2, "Delete redo failed");
    for (int i = 0; i < 105; ++i)
        document.Move({"done"}, 1, 0);
    Require(document.UndoCount() == 100, "History budget not enforced");
    document.Undo();
    document.Move({"done"}, 2, 0);
    Require(!document.Redo(), "New edit retained stale redo branch");
    awb::FlowDocument large_history;
    auto large_metadata = large_history.Definition();
    large_metadata.erase("steps");
    large_metadata["name"] = std::string(126000, 'x');
    large_history.Metadata(large_metadata);
    for (int i = 0; i < 75; ++i)
        large_history.Move({"done"}, 1, 0);
    Require(large_history.UndoCount() > 0 && large_history.UndoCount() < 67,
            "History byte budget was not enforced");
    size_t undo_count = 0, redo_count = 0;
    while (large_history.Undo())
        ++undo_count;
    while (large_history.Redo())
        ++redo_count;
    Require(undo_count == redo_count && redo_count < 67, "Undo/redo lost shared bounded history");
    auto metadata = document.Definition();
    metadata.erase("steps");
    metadata["name"] = "中文属性";
    const auto old_position = document.Position("done");
    document.Metadata(metadata);
    Require(document.Position("done").x == old_position.x, "Metadata reset node layout");
    document.Undo();
    Require(document.Definition().at("name") != "中文属性", "Metadata not undoable");
    document.Redo();
    document.AddFlow("child");
    Require(document.CurrentFlow() == "child" && document.Steps().size() == 1, "Subflow creation failed");
    document.Undo();
    Require(document.CurrentFlow().empty() && document.Flows().size() == 1,
            "Subflow undo left dangling view");
    document.Redo();
    Require(document.CurrentFlow() == "child", "Subflow redo lost selection");
    document.Undo();
    auto future = document.Definition();
    future["schemaVersion"] = "9.0";
    future["futureField"] = {{"preserve", true}};
    awb::FlowDocument unknown;
    unknown.Import(future);
    Require(unknown.ReadOnly() && unknown.Definition() == future, "Future definition was lost");
    // 用途：未来版本禁止任何布局或业务修改；日期：2026-11-04。
    Expect("FLOW_READ_ONLY", [&] { unknown.Move({"done"}, 1, 1); });
    auto unknown_node = original;
    unknown_node["steps"][0]["type"] = "future-node";
    unknown.Import(unknown_node);
    Require(unknown.ReadOnly() && unknown.Definition() == unknown_node, "Unknown node was discarded");
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const auto path = std::wstring(temp) + L"awb-flow-" + awb::Wide(awb::RandomId()) + L".awbdraft";
    document.Save(path);
    awb::FlowDocument other;
    other.Load(path);
    Require(other.Definition() == document.Definition() &&
                other.Position("done").x == document.Position("done").x,
            "Save/load did not preserve definition and layout");
    other.Move({"done"}, 11, 12);
    const auto clipboard = document.CopySelection({navigate, "done"});
    awb::FlowDocument destination;
    const auto pasted = destination.PasteSelection(clipboard);
    Require(pasted.size() == 2 && destination.Steps().size() == 3, "Cross-document paste lost nodes");
    for (const auto &id : pasted)
    {
        const auto node = destination.Step(id);
        Require(id != navigate && id != "done", "Clipboard reused an old identity");
        if (node.at("type") == "navigate")
            Require(pasted.count(node.at("next").get<std::string>()) == 1, "Clipboard edge was not remapped");
    }
    Require(destination.Undo() && destination.Steps().size() == 1 && destination.Redo(),
            "Clipboard paste is not one undoable command");
    const auto external = destination.PasteSelection(document.CopySelection({navigate}));
    Require(destination.Step(*external.begin()).at("next") == "", "Clipboard retained an external edge");
    auto bad_selection = clipboard;
    bad_selection["nodes"][1]["step"]["id"] = bad_selection["nodes"][0]["step"]["id"];
    const auto before_paste = destination.Definition();
    const auto history_before = destination.UndoCount();
    // 用途：重复ID和非法坐标不能产生部分粘贴或新的历史；日期：2026-11-04。
    Expect("FLOW_CLIPBOARD_INVALID", [&] { destination.PasteSelection(bad_selection); });
    bad_selection = clipboard;
    bad_selection["nodes"][0]["position"]["x"] = 1000000;
    Expect("FLOW_CLIPBOARD_INVALID", [&] { destination.PasteSelection(bad_selection); });
    Require(destination.Definition() == before_paste && destination.UndoCount() == history_before,
            "Invalid clipboard partially changed document");
    other.AddFlow("recover_child");
    other.Move({other.Steps().front().at("id").get<std::string>()}, 13, 17);
    other.SaveRecovery(path + L".awbrecovery");
    awb::FlowDocument recovered;
    recovered.LoadRecovery(path + L".awbrecovery");
    Require(recovered.RecoverySnapshot() == other.RecoverySnapshot(), "Recovery lost model, layout or flow");
    // 用途：恢复文件不授权覆盖原草稿，必须另存；日期：2026-11-04。
    Expect("FLOW_REVISION_CONFLICT", [&] { recovered.Save(path); });
    auto bad_recovery = recovered.RecoverySnapshot();
    bad_recovery["recoveryVersion"] = "99.0";
    awb::AtomicWrite(path + L".badrecovery", bad_recovery.dump());
    const auto before_recovery = recovered.RecoverySnapshot();
    Expect("FLOW_RECOVERY_INVALID", [&] { recovered.LoadRecovery(path + L".badrecovery"); });
    Require(recovered.RecoverySnapshot() == before_recovery, "Bad recovery replaced current draft");
    other.SelectFlow("");
    other.Save(path);
    const auto newer_bytes = awb::ReadFileBounded(path, 1024 * 1024);
    // 用途：旧revision不能覆盖另一个编辑器的新草稿；日期：2026-11-04。
    Expect("FLOW_REVISION_CONFLICT", [&] { document.Save(path); });
    Require(awb::ReadFileBounded(path, 1024 * 1024) == newer_bytes, "Stale editor changed newer draft");
    const auto other_before = other.Definition();
    awb::AtomicWrite(path + L".bad", "{\"draftVersion\":\"9.0\"}\n");
    // 用途：坏草稿不能破坏当前文档；日期：2026-11-04。
    Expect("FLOW_DRAFT_INVALID", [&] { other.Load(path + L".bad"); });
    Require(other.Definition() == other_before, "Failed load changed current document");
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".lock").c_str());
    DeleteFileW((path + L".bad").c_str());
    DeleteFileW((path + L".awbrecovery").c_str());
    DeleteFileW((path + L".badrecovery").c_str());
    std::cout << "PASS: flow document, layout/hash separation, 15 DPI/zoom transforms, graph gate, copy IDs, "
                 "100 history, read-only compatibility, durable revision conflict\n";
}
