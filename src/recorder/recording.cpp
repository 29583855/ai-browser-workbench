#include "awb/browser.hpp"
#include "awb/flow_document.hpp"
#include <algorithm>

namespace awb
{
namespace
{
// 用途：在隔离世界观察主文档手势，输入字段只记录参数槽而从不读取值；日期：2026-11-04。
const char *RecorderSource = R"JS((config => {
    if (window !== window.top || !config.origins.includes(location.origin)) return;
    if (globalThis[config.key]) return;
    const controller = new AbortController();
    const pending = new Set();
    const doc = crypto.randomUUID();
    let sequence = 0, paused = false, stopped = false, lastSubmitter = null, lastClick = 0;
    // 用途：只输出结构化有限元数据，绝不序列化DOM或输入值；日期：2026-11-04。
    function emit(kind, target, extra = {}) {
        if (stopped || paused || sequence > 1000) return;
        if (sequence === 1000) kind = 'gap';
        let selector = '';
        if (target instanceof Element) {
            let node = target;
            const path = [];
            while (node && path.length < 32) {
                const tag = node.localName;
                let index = 1;
                for (let prev = node.previousElementSibling; prev; prev = prev.previousElementSibling)
                    if (prev.localName === tag) index++;
                path.unshift(tag + ':nth-of-type(' + index + ')');
                if (node === document.documentElement) break;
                node = node.parentElement;
            }
            selector = path.join('>');
            if (target.getRootNode() !== document || selector.length > 2048 ||
                document.querySelectorAll(selector).length !== 1) {
                kind = 'gap'; selector = '';
            }
        }
        globalThis[config.binding](JSON.stringify({sequence: ++sequence, document: doc, kind,
            selector, sensitive: !!extra.sensitive, origin: location.origin}));
    }
    // 用途：合并一次输入区间，敏感判断先于输出且不读取实际输入内容；日期：2026-11-04。
    function flush() {
        for (const target of pending) {
            const kind = target instanceof HTMLSelectElement ? (target.multiple ? 'unsupported' : 'select') :
                target instanceof HTMLInputElement && ['checkbox','radio','file'].includes(target.type) ? 'unsupported' :
                target.isContentEditable ? 'unsupported' : 'fill';
            const sensitive = target instanceof HTMLInputElement &&
                (target.type === 'password' || /pass|token|secret|otp|code/i.test(target.name + target.autocomplete));
            emit(kind, target, {sensitive});
        }
        pending.clear();
    }
    // 用途：统一注册可整体解绑的捕获监听，不改变表单的默认业务行为；日期：2026-11-04。
    function on(type, handler) { window.addEventListener(type, handler, {capture: true, signal: controller.signal}); }
    // 用途：只标记已变更控件，不保存按键字符或composition数据；日期：2026-11-04。
    function input(event) {
        if (!paused && event.target instanceof Element) pending.add(event.target);
    }
    on('input', input);
    on('change', input);
    on('focusout', flush);
    // 用途：在真实点击前结算填写；观察动作绝不再次派发点击；日期：2026-11-04。
    on('click', event => {
        if (paused) return;
        flush();
        const target = event.target instanceof Element ? event.target.closest('button,a,input,[role="button"]') : null;
        if (!target || target.matches('textarea,select,input:not([type=submit]):not([type=button])')) return;
        lastSubmitter = target; lastClick = performance.now();
        emit('click', target);
    });
    // 用途：Enter和脚本表单提交与同次点击合并；无可回放提交器则报告缺能力；日期：2026-11-04。
    on('submit', event => {
        if (paused) return;
        flush();
        const target = event.submitter;
        if (target && target === lastSubmitter && performance.now() - lastClick < 250) return;
        if (target) emit('click', target); else emit('unsupported', event.target);
        lastSubmitter = null;
    });
    // 用途：快速导航前结算已有输入，不阻止导航也不写页面；日期：2026-11-04。
    on('pagehide', flush);
    // 用途：主文档出现复杂容器时显示缺能力，不静默略过不可观察区域；日期：2026-11-04。
    function complex() {
        if (document.querySelector('iframe,frame')) emit('unsupported', null);
    }
    if (document.readyState === 'loading') on('DOMContentLoaded', complex); else complex();
    globalThis[config.key] = {
        // 用途：暂停先结算输入，恢复不补录暂停期间发生的动作；日期：2026-11-04。
        control(operation) {
            if (operation === 'pause') { flush(); paused = true; }
            else if (operation === 'resume') { pending.clear(); paused = false; }
            else if (operation === 'stop') { flush(); stopped = true; controller.abort(); pending.clear(); delete globalThis[config.key]; }
            return {document: doc, paused, stopped};
        }
    };
    emit('document', null);
})( )JS";
} // namespace

// 用途：预注册隔离世界和新文档监听，当前主页面之外不声称已录制；日期：2026-11-04。
Json Browser::RecordingStart()
{
    if (recording_active_)
        throw Error("RECORDING_ACTIVE", "Stop the current recording before starting another");
    if (picker_active_)
        throw Error("PICKER_ACTIVE", "Stop element selection before recording");
    const auto document = Document();
    recording_id_ = RandomId();
    recording_world_ = "awb-recorder-" + recording_id_;
    recording_binding_ = "awb_record_" + recording_id_;
    recording_page_ = active_page_;
    recording_session_ = session_;
    recording_events_.clear();
    recording_documents_.clear();
    recording_issues_.clear();
    recording_document_sequences_.clear();
    recording_sequence_ = 0;
    recording_bytes_ = 0;
    recording_incomplete_ = false;
    recording_paused_ = false;
    Json allowed = Json::array();
    for (const auto &origin : origins_)
    {
        // 浏览器location.origin省略默认端口，使用URL对象完成相同规范化。
        allowed.push_back(origin);
    }
    const Json config{{"key", recording_world_}, {"binding", recording_binding_}, {"origins", allowed}};
    // 使用URL规范化把显式默认端口转换为浏览器的origin形式。
    const auto bootstrap = "(()=>{const c=" + config.dump() +
                           ";c.origins=c.origins.map(o=>new URL(o).origin);return " +
                           std::string(RecorderSource) + "c)})()";
    recording_active_ = true;
    try
    {
        Call("Runtime.addBinding",
             {{"name", recording_binding_}, {"executionContextName", recording_world_}});
        recording_script_ = Call("Page.addScriptToEvaluateOnNewDocument",
                                 {{"source", bootstrap}, {"worldName", recording_world_}})
                                .at("identifier");
        const auto context = Call("Page.createIsolatedWorld",
                                  {{"frameId", document.at("id")}, {"worldName", recording_world_}})
                                 .at("executionContextId");
        const auto result = Call(
            "Runtime.evaluate", {{"expression", bootstrap}, {"contextId", context}, {"returnByValue", true}});
        if (result.contains("exceptionDetails"))
            throw Error("RECORDING_INJECTION_FAILED", "Recorder listener could not initialize");
    }
    catch (...)
    {
        recording_incomplete_ = true;
        // 保留active状态以便调用者显式停止并解绑已成功注册的资源。
        throw;
    }
    return {{"recordingId", recording_id_},
            {"pageId", recording_page_},
            {"state", "recording"},
            {"scope", "main-document-only"},
            {"fieldValuesCaptured", false}};
}
// 用途：严格过滤CDP绑定元数据并限制事件数量和字节，不允许秘密字段入队；日期：2026-11-04。
void Browser::RecordRecording(const Json &message)
{
    if (!recording_active_ || message.value("sessionId", "") != recording_session_)
        return;
    if (message.value("method", "") == "Target.detachedFromTarget")
        recording_incomplete_ = true;
    if (message.value("method", "") != "Runtime.bindingCalled")
        return;
    const auto parameters = message.value("params", Json::object());
    if (parameters.value("name", "") != recording_binding_)
        return;
    try
    {
        const auto payload = parameters.value("payload", "");
        if (payload.size() > 4096)
            throw Error("RECORDING_EVENT_INVALID", "Event exceeds bounded metadata");
        const auto value = ParseExternalJson(payload, 4);
        const auto kind = value.at("kind").get<std::string>();
        const auto doc = value.at("document").get<std::string>();
        const auto selector = value.at("selector").get<std::string>();
        const auto origin = Origin(value.at("origin").get<std::string>());
        const auto local_sequence = value.at("sequence").get<unsigned long long>();
        if (!value.is_object() || value.size() != 6 || doc.size() > 64 || selector.size() > 2048 ||
            std::find(origins_.begin(), origins_.end(), origin) == origins_.end() ||
            (kind != "document" && kind != "fill" && kind != "click" && kind != "select" &&
             kind != "unsupported" && kind != "gap"))
            throw Error("RECORDING_EVENT_INVALID", "Unexpected recorder metadata");
        if (local_sequence != recording_document_sequences_[doc] + 1)
            recording_incomplete_ = true;
        recording_document_sequences_[doc] = local_sequence;
        if (kind == "document")
        {
            if (!recording_documents_.insert(doc).second)
                return;
            if (recording_paused_)
                recording_incomplete_ = true;
        }
        if (recording_paused_)
            return;
        if (kind == "unsupported" || kind == "gap")
        {
            recording_incomplete_ = true;
            recording_issues_.insert(kind == "unsupported" ? "unsupported-control" : "source-event-gap");
        }
        Json event{{"sequence", ++recording_sequence_},
                   {"document", doc},
                   {"kind", kind},
                   {"selector", selector},
                   {"sensitive", value.at("sensitive").get<bool>()},
                   {"pageId", recording_page_},
                   {"origin", origin}};
        const auto bytes = event.dump().size();
        if (recording_events_.size() >= 1000 || recording_bytes_ + bytes > 8 * 1024 * 1024)
        {
            recording_incomplete_ = true;
            recording_issues_.insert("event-budget-exceeded");
            return;
        }
        recording_events_.push_back(event);
        recording_bytes_ += bytes;
    }
    catch (...)
    {
        recording_incomplete_ = true;
    }
}
// 用途：只读泵取已到达事件，生成仍需输入及断言的草稿，不自动发布；日期：2026-11-04。
Json Browser::RecordingRead()
{
    if (recording_id_.empty())
        throw Error("RECORDING_NOT_FOUND", "Start a recording first");
    if (recording_active_)
    {
        if (session_ != recording_session_ || active_page_ != recording_page_)
            throw Error("RECORDING_PAGE_MISMATCH", "Return to the recording page before reading or stopping");
        try
        {
            const auto frame = Document();
            const auto context = Call("Page.createIsolatedWorld",
                                      {{"frameId", frame.at("id")}, {"worldName", recording_world_}})
                                     .at("executionContextId");
            const auto state =
                Call("Runtime.evaluate",
                     {{"expression", "Boolean(globalThis[" + Json(recording_world_).dump() + "])"},
                      {"contextId", context},
                      {"returnByValue", true}});
            if (state.contains("exceptionDetails") || !state.at("result").value("value", false))
                recording_incomplete_ = true;
        }
        catch (...)
        {
            recording_incomplete_ = true;
            throw;
        }
    }
    FlowDocument document;
    auto definition = document.Definition();
    definition["name"] = "录制草稿";
    definition["policy"]["allowedOrigins"] = origins_;
    definition["steps"] =
        Json::array({{{"id", "open_recording_url"}, {"type", "navigate"}, {"url", ""}, {"next", "done"}}});
    Json gaps =
        Json::array({{{"nodeId", "open_recording_url"}, {"kind", "initial-url-literal"}, {"field", "url"}}});
    size_t index = 0;
    for (const auto &event : recording_events_)
    {
        const auto kind = event.at("kind").get<std::string>();
        if (kind != "fill" && kind != "select" && kind != "click")
            continue;
        const auto id = "recorded_" + std::to_string(++index);
        Json step{{"id", id}, {"type", kind}, {"selector", event.at("selector")}, {"next", ""}};
        if (kind == "fill" || kind == "select")
        {
            const auto slot = "input_" + std::to_string(index);
            definition["inputSchema"]["properties"][slot] = {{"type", "string"},
                                                             {"maxLength", kind == "select" ? 4096 : 65536}};
            definition["inputSchema"]["required"].push_back(slot);
            if (kind == "fill")
                step["value"] = {{"input", "/" + slot}};
            else
                step["values"] = Json::array({{{"input", "/" + slot}}});
            gaps.push_back({{"nodeId", id},
                            {"kind", event.at("sensitive") == true ? "credential-binding" : "input-binding"},
                            {"slot", slot}});
        }
        else
        {
            step["postcondition"] = {{"selector", ""}, {"expected", ""}, {"timeoutMs", 5000}};
            gaps.push_back({{"nodeId", id}, {"kind", "postcondition-required"}});
        }
        if (!definition["steps"].empty())
            definition["steps"].back()["next"] = id;
        definition["steps"].push_back(step);
    }
    if (!definition["steps"].empty())
        definition["steps"].back()["next"] = "done";
    definition["entry"] = "open_recording_url";
    definition["steps"].push_back({{"id", "done"}, {"type", "end"}});
    if (recording_documents_.size() > 1)
        gaps.push_back({{"kind", "cross-document-review-required"}});
    const bool draft_budget = definition.at("steps").size() <= 128 && definition.dump().size() <= 256 * 1024;
    if (!draft_budget)
        gaps.push_back({{"kind", "draft-budget-exceeded-split-required"}});
    return {{"recordingId", recording_id_},
            {"state", recording_active_ ? recording_paused_ ? "paused" : "recording" : "stopped"},
            {"complete", !recording_incomplete_},
            {"scope", "main-document-only"},
            {"events", recording_events_},
            {"draft", draft_budget ? definition : Json(nullptr)},
            {"captureIssues", recording_issues_},
            {"draftReadyForReplay", false},
            {"requiredConfiguration", gaps},
            {"pageActionsDispatched", 0},
            {"fieldValuesCaptured", false},
            {"note", "Navigation, multi-select, frames, Shadow and complex controls require review; draft is "
                     "not executable until validated"}};
}
// 用途：通过隔离世界结算和解绑监听，停止不派发页面业务且不自动回放；日期：2026-11-04。
Json Browser::RecordingControl(const std::string &operation)
{
    if (!recording_active_)
        throw Error("RECORDING_NOT_ACTIVE", "No active recording to control");
    if (operation != "pause" && operation != "resume" && operation != "stop")
        throw Error("INVALID_ARGUMENTS", "Use pause, resume or stop");
    if (active_page_ != recording_page_ || session_ != recording_session_)
        throw Error("RECORDING_PAGE_MISMATCH", "Return to the recording page before controlling its session");
    const auto frame = Document();
    const auto context =
        Call("Page.createIsolatedWorld", {{"frameId", frame.at("id")}, {"worldName", recording_world_}})
            .at("executionContextId");
    const auto expression =
        "globalThis[" + Json(recording_world_).dump() + "]?.control(" + Json(operation).dump() + ")";
    const auto result = Call("Runtime.evaluate",
                             {{"expression", expression}, {"contextId", context}, {"returnByValue", true}});
    if (result.contains("exceptionDetails") || !result.at("result").contains("value"))
        recording_incomplete_ = true;
    if (operation == "stop")
    {
        if (!recording_script_.empty())
            Call("Page.removeScriptToEvaluateOnNewDocument", {{"identifier", recording_script_}});
        Call("Runtime.removeBinding", {{"name", recording_binding_}});
        recording_active_ = false;
        recording_paused_ = false;
        recording_script_.clear();
    }
    else
        recording_paused_ = operation == "pause";
    return RecordingRead();
}
// 用途：不通过CDP读取录制状态，避免排斥检查本身产生页面动作；日期：2026-11-04。
bool Browser::RecordingActive() const
{
    return recording_active_;
}
} // namespace awb
