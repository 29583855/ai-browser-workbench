#include "awb/browser.hpp"

namespace awb
{
// 用途：使用浏览器原生InspectMode截获点击，避免网页事件先触发后才阻止；日期：2026-11-04。
Json Browser::PickerStart()
{
    if (recording_active_)
        throw Error("RECORDING_ACTIVE", "Stop recording before selecting an element");
    if (picker_active_)
        throw Error("PICKER_ACTIVE", "Read or stop the current picker first");
    const auto document = Document();
    Call("Overlay.enable");
    Call("Overlay.setInspectMode",
         {{"mode", "searchForNode"},
          {"highlightConfig",
           {{"showInfo", false}, {"contentColor", {{"r", 36}, {"g", 99}, {"b", 235}, {"a", 0.18}}}}}});
    picker_active_ = true;
    picker_node_ = 0;
    picker_page_ = active_page_;
    picker_loader_ = document.value("loaderId", "");
    return {{"state", "picking"},
            {"pageId", picker_page_},
            {"pageActionsDispatched", 0},
            {"scope", "main-document-only"}};
}
// 用途：先泵取选取事件再重新核对文档和节点，随后关闭拦截覆盖层；日期：2026-11-04。
Json Browser::PickerRead()
{
    if (!picker_active_)
        throw Error("PICKER_NOT_ACTIVE", "Start element selection first");
    const auto document = Document();
    if (active_page_ != picker_page_ || document.value("loaderId", "") != picker_loader_)
    {
        PickerStop();
        throw Error("PICKER_DOCUMENT_CHANGED", "Document changed; select again in the current document");
    }
    if (!picker_node_)
        return {{"state", "picking"}, {"pageId", picker_page_}, {"pageActionsDispatched", 0}};
    const int backend = picker_node_;
    PickerStop();
    const auto object = Call("DOM.resolveNode", {{"backendNodeId", backend}}).at("object");
    if (!object.contains("objectId"))
        throw Error("PICKER_ELEMENT_GONE", "Selected element was removed before inspection");
    // 仅生成定位描述，不读取输入值、页面正文或Cookie。
    const auto result = Call("Runtime.callFunctionOn", {{"objectId", object.at("objectId")},
                                                        {"functionDeclaration", R"JS(function() {
            if (!(this instanceof Element) || this.getRootNode() !== document) return {supported:false};
            const candidates = [];
            if (this.id && this.id.length <= 128) candidates.push('#' + CSS.escape(this.id));
            const testId = this.getAttribute('data-testid');
            if (testId && testId.length <= 128) candidates.push('[data-testid=' + JSON.stringify(testId) + ']');
            let node = this;
            const path = [];
            while (node && path.length < 32) {
                let index = 1;
                for (let previous = node.previousElementSibling; previous; previous = previous.previousElementSibling)
                    if (previous.localName === node.localName) index++;
                path.unshift(node.localName + ':nth-of-type(' + index + ')');
                if (node === document.documentElement) break;
                node = node.parentElement;
            }
            candidates.push(path.join('>'));
            return {supported:true,candidates:candidates.filter(s => s.length <= 2048 && document.querySelectorAll(s).length === 1)};
        })JS"},
                                                        {"returnByValue", true}});
    Call("Runtime.releaseObject", {{"objectId", object.at("objectId")}});
    if (result.contains("exceptionDetails") || !result.at("result").contains("value"))
        throw Error("PICKER_ELEMENT_GONE", "Selected element is no longer readable");
    const auto value = result.at("result").at("value");
    if (!value.value("supported", false) || value.at("candidates").empty())
        throw Error("PICKER_UNSUPPORTED_SCOPE", "Select a supported main-document element");
    const auto selector = value.at("candidates").front().get<std::string>();
    const auto inspected = Inspect(selector, false);
    return {{"state", "picked"},
            {"pageId", picker_page_},
            {"selector", selector},
            {"candidates", value.at("candidates")},
            {"inspection", inspected},
            {"pageActionsDispatched", 0},
            {"scope", "main-document-only"}};
}
// 用途：退出拦截模式并清除节点状态，不对业务页面重放选取手势；日期：2026-11-04。
Json Browser::PickerStop()
{
    if (picker_active_)
        Call("Overlay.setInspectMode", {{"mode", "none"}, {"highlightConfig", Json::object()}});
    picker_active_ = false;
    picker_node_ = 0;
    return {{"state", "stopped"}, {"pageActionsDispatched", 0}, {"replayedClicks", 0}};
}
} // namespace awb
