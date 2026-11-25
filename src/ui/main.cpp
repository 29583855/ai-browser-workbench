#include "account_window.hpp"
#include "automation_windows.hpp"
#include "awb/service.hpp"
#include "flow/flow_editor.hpp"
#include "library_window.hpp"
#include "theme.hpp"
#include <algorithm>
#include <commctrl.h>
#include <iostream>
#include <memory>
#include <shellapi.h>
#include <thread>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' "       \
                        "version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

namespace
{
constexpr UINT ResultMessage = WM_APP + 1;
constexpr int CreateButton = 101;
constexpr int StartButton = 102;
constexpr int StopButton = 103;
constexpr int NavigateButton = 104;
constexpr int SnapshotButton = 105;
constexpr int RevokeButton = 106;
constexpr int LocateButton = 107;
constexpr int FillButton = 108;
constexpr int ClickButton = 109;
constexpr int TextButton = 110;
constexpr int ResumeButton = 111;
constexpr int UpdateButton = 112;
constexpr int ArchiveButton = 113;
constexpr int TrashButton = 114;
constexpr int RestoreButton = 115;
constexpr int PurgeButton = 116;
constexpr int PagesButton = 117;
constexpr int OpenPageButton = 118;
constexpr int SwitchPageButton = 119;
constexpr int ClosePageButton = 120;
constexpr int EventsButton = 121;
constexpr int ReconnectButton = 122;
constexpr int InspectButton = 123;
constexpr int ClearHighlightButton = 124;
constexpr int PreviewButton = 125;
constexpr int SelectButton = 126;
constexpr int ValidateWorkflowButton = 127;
constexpr int PublishWorkflowButton = 128;
constexpr int RunWorkflowButton = 129;
constexpr int WorkflowStatusButton = 130;
constexpr int PauseWorkflowButton = 131;
constexpr int ResumeWorkflowButton = 132;
constexpr int CancelWorkflowButton = 133;
constexpr int StartDiagnosticsButton = 134;
constexpr int ReadDiagnosticsButton = 135;
constexpr int StopDiagnosticsButton = 136;
constexpr int ExtractButton = 137;
constexpr int BatchButton = 138;
constexpr int FlowEditorMenu = 901;
constexpr int RecorderStartMenu = 902;
constexpr int RecorderReadMenu = 903;
constexpr int RecorderPauseMenu = 904;
constexpr int RecorderResumeMenu = 905;
constexpr int RecorderStopMenu = 906;
constexpr int RecorderDraftMenu = 907;
constexpr int PickerStartMenu = 908;
constexpr int PickerReadMenu = 909;
constexpr int PickerStopMenu = 910;
constexpr int CredentialsMenu = 911;
constexpr int DebuggerMenu = 912;
constexpr int DebugNextMenu = 913;
constexpr int DiscoverMenu = 914;
constexpr int RepairMenu = 915;
constexpr int DoctorMenu = 916;
constexpr int LibraryManagerMenu = 917;
constexpr int AccountMenu = 918;
constexpr int FirstAssetMenu = 920;
constexpr int FirstLibraryMenu = 940;
const char *LibraryCommands[] = {"admin_summary",
                                 "admin_policy",
                                 "commerce_quote",
                                 "profile_group_list",
                                 "profile_group_create",
                                 "profile_group_update",
                                 "profile_group_delete",
                                 "profile_template_list",
                                 "profile_template_create",
                                 "profile_template_update",
                                 "profile_template_delete",
                                 "profile_template_instantiate",
                                 "profile_export",
                                 "profile_import",
                                 "profile_batch",
                                 "commerce_entitlements",
                                 "profile_template_history",
                                 "profile_template_rollback",
                                 "profile_batch_list",
                                 "profile_batch_get"};
const wchar_t *LibraryLabels[] = {L"本地后台概览",
                                  L"免费政策与实现范围",
                                  L"从JSON预览报价",
                                  L"查看环境分组",
                                  L"用名称框新建当前环境分组",
                                  L"从JSON更新分组",
                                  L"从JSON删除分组（保留环境）",
                                  L"查看版本化模板",
                                  L"用名称框保存当前环境为模板",
                                  L"从JSON更新模板",
                                  L"从JSON删除模板（保留实例）",
                                  L"从JSON实例化指定模板版本",
                                  L"导出当前环境配置",
                                  L"从JSON导入配置（创建新环境）",
                                  L"从JSON执行环境批操作",
                                  L"查看本地免费权益",
                                  L"从JSON查看模板历史",
                                  L"从JSON回滚模板（创建新版本）",
                                  L"查看持久批次记录",
                                  L"从JSON查看指定批次"};

struct WindowState
{
    desktop::Theme theme;
    std::unique_ptr<awb::Service> service;
    std::thread worker;
    HWND profiles = nullptr;
    HWND name = nullptr;
    HWND url = nullptr;
    HWND output = nullptr;
    HWND selector = nullptr;
    HWND value = nullptr;
    HWND pages = nullptr;
    std::vector<std::string> page_ids;
    std::string page_profile;
    std::vector<std::string> ids;
    std::map<std::string, awb::Json> leases;
    std::map<std::string, awb::Json> metadata;
    std::map<std::string, std::string> workflow_ids;
    std::map<std::string, std::string> run_ids;
    std::map<std::string, awb::Json> recorded_drafts;
    std::string busy_profile;
    std::string pending_stop;
    unsigned long long control_epoch = 0;
    bool busy = false;
    bool smoke = false;
    bool debug_next = false;
    std::wstring recovery_directory;
};

// 用途：将画布发布结果绑定到打开时的环境，避免切换选择后误运行其他环境；日期：2026-11-04。
void ShowFlowEditor(HWND window, WindowState &state, const awb::Json *definition = nullptr,
                    const std::string *bound_profile = nullptr)
{
    const auto selected = SendMessageW(state.profiles, LB_GETCURSEL, 0, 0);
    const auto profile = bound_profile ? *bound_profile
                         : selected != LB_ERR && static_cast<size_t>(selected) < state.ids.size()
                             ? state.ids[static_cast<size_t>(selected)]
                             : "";
    // 用途：同一UI线程回填不可变版本ID，原发布内容和其他环境映射保持独立；日期：2026-11-04。
    desktop::OpenFlowEditor(
        window, state.service.get(), definition,
        [window, &state, profile](const awb::Json &published)
        {
            if (!profile.empty())
                state.workflow_ids[profile] = published.at("workflowId");
            const awb::Json result{
                {"ok", true}, {"command", "workflow_publish"}, {"profileId", profile}, {"result", published}};
            SetWindowTextW(state.output, awb::Wide(result.dump(2)).c_str());
        },
        state.recovery_directory);
}

// 用途：只展示当前环境缓存的本地页面ID和活动标记，选择不自动派发动作；日期：2026-11-04。
void RenderPages(WindowState &state, const std::string &active = "")
{
    if (!state.pages)
        return;
    SendMessageW(state.pages, CB_RESETCONTENT, 0, 0);
    size_t selected = 0;
    for (size_t index = 0; index < state.page_ids.size(); ++index)
    {
        const auto &id = state.page_ids[index];
        const auto label = awb::Wide(id.substr(0, 8) + (id == active ? " (当前页面)" : ""));
        SendMessageW(state.pages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (id == active)
            selected = index;
    }
    if (!state.page_ids.empty())
        SendMessageW(state.pages, CB_SETCURSEL, selected, 0);
}

// 用途：读取Win32控件UTF-16内容，保留中文；日期：2026-11-03。
std::wstring ControlText(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &result[0], length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}
// 用途：创建带中文主题字体及原生键盘行为的控件；日期：2026-11-04。
HWND Control(HWND parent, const wchar_t *kind, const wchar_t *text, DWORD style, int x, int y, int width,
             int height, int id = 0)
{
    auto *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(parent, GWLP_USERDATA));
    const bool interactive = std::wstring(kind) != L"STATIC";
    if (std::wstring(kind) == L"BUTTON")
        style = (style & ~static_cast<DWORD>(BS_TYPEMASK)) | BS_OWNERDRAW;
    const auto control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
                                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         GetModuleHandleW(nullptr), nullptr);
    if (!control)
        throw awb::Error("UI_CREATE_FAILED", "Cannot create native control");
    if (interactive)
        SetWindowLongPtrW(control, GWL_STYLE, GetWindowLongPtrW(control, GWL_STYLE) | WS_TABSTOP);
    if (state)
        state->theme.Style(control);
    if (std::wstring(kind) == L"EDIT")
        SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    return control;
}
// 用途：从服务刷新列表并保留当前或指定新建环境，避免后续动作操作其他环境；日期：2026-11-04。
void Refresh(WindowState &state, const std::string &preferred_id = "")
{
    auto selected_id = preferred_id;
    const auto previous = SendMessageW(state.profiles, LB_GETCURSEL, 0, 0);
    if (selected_id.empty() && previous != LB_ERR && static_cast<size_t>(previous) < state.ids.size())
        selected_id = state.ids[static_cast<size_t>(previous)];
    SendMessageW(state.profiles, LB_RESETCONTENT, 0, 0);
    state.ids.clear();
    state.metadata.clear();
    size_t selected_index = 0;
    const auto catalog = state.service->Execute("profile_list", {{"state", "all"}});
    for (const auto &profile : catalog.at("profiles"))
    {
        const auto lifecycle = profile.value("state", "active");
        const auto state_label = lifecycle == "archived"  ? "已归档"
                                 : lifecycle == "trashed" ? "回收站"
                                 : lifecycle == "purging" ? "删除中"
                                                          : "可用";
        const auto label = awb::Wide(profile.at("name").get<std::string>() + "  [" +
                                     profile.at("id").get<std::string>().substr(0, 8) + "] " + state_label);
        SendMessageW(state.profiles, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (profile.at("id").get<std::string>() == selected_id)
            selected_index = state.ids.size();
        state.ids.push_back(profile.at("id"));
        state.metadata[profile.at("id").get<std::string>()] = profile;
    }
    state.theme.EnvironmentCount(state.ids.size());
    if (!state.ids.empty())
    {
        SendMessageW(state.profiles, LB_SETCURSEL, selected_index, 0);
        SetWindowTextW(state.name,
                       awb::Wide(state.metadata.at(state.ids[selected_index]).at("name")).c_str());
        if (state.page_profile != state.ids[selected_index])
        {
            state.page_ids.clear();
            state.page_profile.clear();
            RenderPages(state);
        }
    }
}
// 用途：在单工作线程执行真实命令，保持窗口消息循环响应；日期：2026-11-03。
void RunCommand(HWND window, WindowState &state, const std::string &command, awb::Json arguments,
                const std::string &context_profile = "")
{
    if (state.busy)
        return;
    if (state.worker.joinable())
        state.worker.join();
    state.busy = true;
    state.theme.Activity(1);
    const auto profile_id = arguments.value("profileId", context_profile);
    state.busy_profile = profile_id;
    const auto epoch = state.control_epoch;
    SetWindowTextW(state.output, L"正在执行，请查看返回结果；关闭窗口前会等待当前操作退出。");
    // 用途：只在线程中访问服务，结果通过窗口消息转交UI线程；日期：2026-11-03。
    state.worker = std::thread(
        [window, &state, command, arguments, epoch, profile_id]()
        {
            auto result = std::make_unique<awb::Json>();
            try
            {
                awb::Json answer;
                if (command == "ui_fill" || command == "ui_click" || command == "ui_text" ||
                    command == "ui_preview" || command == "ui_select")
                {
                    awb::Json page{{"profileId", arguments.at("profileId")},
                                   {"lease", arguments.at("lease")},
                                   {"selector", arguments.at("selector")}};
                    const auto located = state.service->Execute("page_locate", page);
                    page.erase("selector");
                    page["ref"] = located.at("ref");
                    if (command == "ui_fill")
                        page["value"] = arguments.at("value");
                    if (command == "ui_preview")
                        page["action"] = "click";
                    if (command == "ui_select")
                        page["values"] = awb::Json::array({arguments.at("value")});
                    answer = state.service->Execute(command == "ui_fill"      ? "page_fill"
                                                    : command == "ui_click"   ? "page_click"
                                                    : command == "ui_preview" ? "page_preview"
                                                    : command == "ui_select"  ? "page_select"
                                                                              : "page_text",
                                                    page);
                }
                else
                    answer = state.service->Execute(command, arguments);
                *result = {{"ok", true},
                           {"result", answer},
                           {"command", command},
                           {"profileId", profile_id},
                           {"controlEpoch", epoch}};
            }
            catch (const std::exception &error)
            {
                *result = awb::ErrorResult(error);
                (*result)["command"] = command;
                (*result)["profileId"] = profile_id;
                (*result)["controlEpoch"] = epoch;
            }
            if (PostMessageW(window, ResultMessage, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
}
// 用途：管理真实控件、分组模板及本地后台菜单，异步结果回到UI线程；日期：2026-11-05。
LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try
    {
        if (message == WM_CREATE)
        {
            state = static_cast<WindowState *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->theme.Initialize(window);
            auto &theme = state->theme;
            theme.title = Control(window, L"STATIC", L"浏览器工作台", SS_NOPREFIX, 0, 0, 0, 0);
            theme.subtitle =
                Control(window, L"STATIC", L"管理独立环境，调试网页与自动化流程", SS_NOPREFIX, 0, 0, 0, 0);
            theme.status = Control(window, L"STATIC", L"准备就绪", SS_CENTER, 0, 0, 0, 0);
            theme.count = Control(window, L"STATIC", L"0 个环境", SS_NOPREFIX, 0, 0, 0, 0);
            theme.library_label = Control(window, L"STATIC", L"环境库", SS_NOPREFIX, 0, 0, 0, 0);
            state->profiles =
                Control(window, L"LISTBOX", L"",
                        LBS_NOTIFY | LBS_HASSTRINGS | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                        0, 0, 0, 0);
            Control(window, L"STATIC", L"环境与导航", SS_NOPREFIX, 0, 0, 0, 0, 601);
            Control(window, L"STATIC", L"环境名称", SS_NOPREFIX, 0, 0, 0, 0, 602);
            state->name =
                Control(window, L"EDIT", L"开发测试环境", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, 201);
            Control(window, L"STATIC", L"页面地址", SS_NOPREFIX, 0, 0, 0, 0, 603);
            state->url = Control(window, L"EDIT", L"http://127.0.0.1:3000/login", WS_BORDER | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, 202);
            theme.selector_label = Control(window, L"STATIC", L"CSS 选择器", SS_NOPREFIX, 0, 0, 0, 0);
            state->selector =
                Control(window, L"EDIT", L"#username", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, 203);
            theme.value_label = Control(window, L"STATIC", L"填写 / 选项值", SS_NOPREFIX, 0, 0, 0, 0);
            state->value =
                Control(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, 0, 0, 0, 0, 204);
            theme.page_label = Control(window, L"STATIC", L"当前页面", SS_NOPREFIX, 0, 0, 0, 0);
            state->pages = Control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0);
            const wchar_t *button_names[] = {
                L"新建环境",     L"启动",        L"停止",     L"打开地址", L"语义快照", L"接管/撤销",
                L"定位",         L"填写",        L"点击",     L"读取文本", L"取控制",   L"保存名称",
                L"归档",         L"回收站",      L"恢复",     L"永久删除", L"页面列表", L"新页面",
                L"切换",         L"关闭选中页",  L"页面事件", L"显式重连", L"探查高亮", L"清除高亮",
                L"预览点击",     L"选择选项",    L"校验流程", L"发布流程", L"运行流程", L"流程状态",
                L"暂停流程",     L"恢复流程",    L"取消流程", L"开始诊断", L"读取诊断", L"停止诊断",
                L"按Schema提取", L"执行动作批次"};
            for (int index = 0; index < 38; ++index)
                Control(window, L"BUTTON", button_names[index], BS_PUSHBUTTON, 0, 0, 0, 0, 101 + index);
            const wchar_t *groups[] = {L"网页操作", L"页面与诊断", L"流程与数据", L"环境管理"};
            for (int index = 0; index < desktop::GroupCount; ++index)
                Control(window, L"BUTTON", groups[index], BS_PUSHBUTTON, 0, 0, 0, 0,
                        desktop::FirstGroupButton + index);
            theme.hint = Control(window, L"STATIC", L"", SS_NOPREFIX, 0, 0, 0, 0);
            theme.result_hint = Control(window, L"STATIC", L"返回详情 · 可选择复制", SS_RIGHT, 0, 0, 0, 0);
            theme.result_label = Control(window, L"STATIC", L"操作结果", SS_NOPREFIX, 0, 0, 0, 0);
            state->output =
                Control(window, L"EDIT",
                        L"欢迎使用浏览器工作台。\r\n\r\n1. 新建或选择左侧环境\r\n2. "
                        L"启动浏览器并打开页面地址\r\n3. 定位元素，填写或点击，再检查操作结果",
                        ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL, 0, 0, 0, 0, 205);
            theme.name = state->name;
            theme.url = state->url;
            theme.selector = state->selector;
            theme.value = state->value;
            theme.pages = state->pages;
            theme.output = state->output;
            theme.profiles = state->profiles;
            SendMessageW(state->name, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"输入环境名称"));
            SendMessageW(state->selector, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"CSS 选择器或 JSON 文件完整路径"));
            SendMessageW(state->value, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"隐藏输入，提交后清空"));
            theme.Layout();
            Refresh(*state);
            return 0;
        }
        if (state && message == WM_PAINT)
        {
            state->theme.Paint();
            return 0;
        }
        if (state && message == WM_ERASEBKGND)
            return 1;
        if (state && message == WM_SIZE && wparam != SIZE_MINIMIZED)
        {
            state->theme.Layout();
            return 0;
        }
        if (state && message == WM_DPICHANGED)
        {
            state->theme.DpiChanged(HIWORD(wparam));
            const auto *bounds = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        if (state && message == WM_GETMINMAXINFO)
        {
            auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
            limits->ptMinTrackSize = {state->theme.Scale(800), state->theme.Scale(580)};
            return 0;
        }
        if (state && (message == WM_VSCROLL || message == WM_HSCROLL))
        {
            state->theme.Scroll(message == WM_VSCROLL ? SB_VERT : SB_HORZ, LOWORD(wparam));
            return 0;
        }
        if (state && message == WM_MOUSEWHEEL)
        {
            state->theme.Scroll((GetKeyState(VK_SHIFT) & 0x8000) ? SB_HORZ : SB_VERT, 0,
                                GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 72);
            return 0;
        }
        if (state && message == WM_MEASUREITEM)
        {
            auto *item = reinterpret_cast<MEASUREITEMSTRUCT *>(lparam);
            if (item->CtlType == ODT_LISTBOX)
            {
                item->itemHeight = state->theme.Scale(58);
                return TRUE;
            }
        }
        if (state && message == WM_DRAWITEM && state->theme.Draw(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam)))
            return TRUE;
        if (state &&
            (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX))
            return reinterpret_cast<LRESULT>(state->theme.ControlColor(
                reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam), message == WM_CTLCOLOREDIT));
        if (state && message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED &&
            LOWORD(wparam) >= desktop::FirstGroupButton &&
            LOWORD(wparam) < desktop::FirstGroupButton + desktop::GroupCount)
        {
            state->theme.group = LOWORD(wparam) - desktop::FirstGroupButton;
            state->theme.Layout();
            for (int index = 0; index < desktop::GroupCount; ++index)
                InvalidateRect(GetDlgItem(window, desktop::FirstGroupButton + index), nullptr, TRUE);
            return 0;
        }
        if (message == desktop::LibraryChangedMessage && state)
        {
            // 用途：同步独立环境库表单的真实修改，失效桌面缓存而不释放服务租约；日期：2026-11-05。
            if (wparam)
            {
                state->leases.clear();
                ++state->control_epoch;
            }
            Refresh(*state);
            return 0;
        }
        if (message == ResultMessage && state)
        {
            std::unique_ptr<awb::Json> result(reinterpret_cast<awb::Json *>(lparam));
            state->busy = false;
            if (state->worker.joinable())
                state->worker.join();
            const bool success = result->value("ok", false);
            if (success && result->value("command", "") == "browser_start" &&
                result->value("controlEpoch", 0ULL) == state->control_epoch)
                state->leases[result->at("profileId").get<std::string>()] = state->service->Execute(
                    "lease_acquire",
                    {{"profileId", result->at("profileId")}, {"owner", "desktop"}, {"ttlMs", 300000}});
            if (success && result->value("command", "") == "profile_create")
                Refresh(*state, result->at("result").at("id").get<std::string>());
            else if (success && (result->value("command", "") == "profile_import" ||
                                 result->value("command", "") == "profile_template_instantiate"))
                Refresh(*state, result->at("result").at("profiles").at(0).at("id").get<std::string>());
            else if (result->value("command", "").find("profile_") == 0)
            {
                const auto changed_id = result->value("profileId", "");
                if (success)
                    state->leases.erase(changed_id);
                Refresh(*state, changed_id);
            }
            if (success && result->value("command", "") == "lease_acquire" &&
                result->value("controlEpoch", 0ULL) == state->control_epoch)
                state->leases[result->at("profileId").get<std::string>()] = result->at("result");
            if (success && result->value("command", "") == "workflow_publish")
                state->workflow_ids[result->at("profileId").get<std::string>()] =
                    result->at("result").at("workflowId");
            if (success && result->value("command", "") == "workflow_run")
            {
                state->run_ids[result->at("profileId").get<std::string>()] = result->at("result").at("runId");
                if (state->debug_next)
                {
                    state->debug_next = false;
                    CheckMenuItem(GetMenu(window), DebugNextMenu, MF_BYCOMMAND | MF_UNCHECKED);
                    desktop::OpenWorkflowDebugger(window, state->service.get(),
                                                  result->at("result").at("runId"));
                }
            }
            if (success && result->value("command", "").find("recorder_") == 0 &&
                result->at("result").contains("draft") && result->at("result").at("draft").is_object())
                state->recorded_drafts[result->at("profileId").get<std::string>()] =
                    result->at("result").at("draft");
            if (success && (result->value("command", "") == "model_generate_workflow" ||
                            result->value("command", "") == "workflow_repair_draft"))
            {
                const auto bound = result->value("profileId", "");
                ShowFlowEditor(window, *state, &result->at("result").at("draft"), &bound);
            }
            if (success && result->value("command", "") == "recorder_start")
                state->recorded_drafts.erase(result->at("profileId").get<std::string>());
            const auto changed_profile = result->value("profileId", "");
            if (success && result->value("command", "") == "page_pick_read" &&
                result->at("result").value("state", "") == "picked")
                SetWindowTextW(state->selector, awb::Wide(result->at("result").at("selector")).c_str());
            const auto selected = SendMessageW(state->profiles, LB_GETCURSEL, 0, 0);
            if (success && selected != LB_ERR && static_cast<size_t>(selected) < state->ids.size() &&
                state->ids[static_cast<size_t>(selected)] == changed_profile)
            {
                const auto command = result->value("command", "");
                const auto &answer = result->at("result");
                if (command == "browser_start" || command == "page_list")
                {
                    state->page_ids.clear();
                    state->page_profile = changed_profile;
                    if (command == "page_list")
                        for (const auto &page : answer.at("pages"))
                            state->page_ids.push_back(page.at("pageId"));
                    else if (!answer.value("activePageId", "").empty())
                        state->page_ids.push_back(answer.at("activePageId"));
                }
                if (command == "page_open")
                {
                    if (state->page_profile != changed_profile)
                        state->page_ids.clear();
                    state->page_profile = changed_profile;
                    state->page_ids.push_back(answer.at("pageId"));
                }
                if (command == "page_close")
                {
                    const auto id = answer.at("pageId").get<std::string>();
                    state->page_ids.erase(std::remove(state->page_ids.begin(), state->page_ids.end(), id),
                                          state->page_ids.end());
                }
                if (command == "browser_stop")
                {
                    state->page_ids.clear();
                    state->page_profile.clear();
                }
                if (command == "browser_start" || command == "browser_stop" || command == "page_list" ||
                    command == "page_open" || command == "page_switch" || command == "page_close" ||
                    command == "browser_reconnect")
                    RenderPages(*state, answer.value("activePageId", answer.value("pageId", "")));
            }
            // 用途：先恢复按钮，再发布完成结果，避免调用方看到完成后点击仍被禁用；日期：2026-11-04。
            state->theme.Activity(success ? 2 : 3);
            SetWindowTextW(state->output, awb::Wide(result->dump(2)).c_str());
            if (!state->pending_stop.empty())
            {
                const auto stopping = state->pending_stop;
                state->pending_stop.clear();
                RunCommand(window, *state, "browser_stop", {{"profileId", stopping}});
            }
            return 0;
        }
        if (message == WM_COMMAND && state && LOWORD(wparam) == 0 && HIWORD(wparam) == LBN_SELCHANGE &&
            reinterpret_cast<HWND>(lparam) == state->profiles)
        {
            const auto selected = SendMessageW(state->profiles, LB_GETCURSEL, 0, 0);
            if (selected != LB_ERR && static_cast<size_t>(selected) < state->ids.size())
            {
                SetWindowTextW(
                    state->name,
                    awb::Wide(state->metadata.at(state->ids[static_cast<size_t>(selected)]).at("name"))
                        .c_str());
                state->page_ids.clear();
                state->page_profile.clear();
                RenderPages(*state);
            }
            return 0;
        }
        if (message == WM_COMMAND && state && HIWORD(wparam) == BN_CLICKED)
        {
            const int button = LOWORD(wparam);
            if (button == FlowEditorMenu)
            {
                ShowFlowEditor(window, *state);
                return 0;
            }
            if (state->busy && button != RevokeButton && button != StopButton)
                return 0;
            if (button == DoctorMenu)
            {
                RunCommand(window, *state, "doctor", awb::Json::object());
                return 0;
            }
            if (button == AccountMenu)
            {
                if (!state->busy)
                    desktop::OpenAccountWindow(window, state->service.get());
                return 0;
            }
            if (button == LibraryManagerMenu)
            {
                const auto selected = SendMessageW(state->profiles, LB_GETCURSEL, 0, 0);
                const auto id = selected != LB_ERR && static_cast<size_t>(selected) < state->ids.size()
                                    ? state->ids[static_cast<size_t>(selected)]
                                    : "";
                desktop::OpenLibraryManager(window, state->service.get(), id);
                return 0;
            }
            if (button >= FirstLibraryMenu &&
                button < FirstLibraryMenu + static_cast<int>(_countof(LibraryCommands)))
            {
                const int action = button - FirstLibraryMenu;
                awb::Json arguments = awb::Json::object();
                if (action == 4 || action == 8 || action == 12)
                {
                    const auto selected = SendMessageW(state->profiles, LB_GETCURSEL, 0, 0);
                    if (selected == LB_ERR || static_cast<size_t>(selected) >= state->ids.size())
                        throw awb::Error("PROFILE_NOT_SELECTED", "请先选择要分组、保存模板或导出的环境");
                    const auto &id = state->ids[static_cast<size_t>(selected)];
                    if (action == 8)
                    {
                        const auto exported = state->service->Execute(
                            "profile_export", {{"profileIds", awb::Json::array({id})}});
                        arguments = {{"name", awb::Utf8(ControlText(state->name))},
                                     {"configuration", exported.at("configurations").at(0)}};
                    }
                    else
                    {
                        arguments["profileIds"] = awb::Json::array({id});
                        if (action == 4)
                            arguments["name"] = awb::Utf8(ControlText(state->name));
                    }
                }
                else if (action == 2 || action == 5 || action == 6 || (action >= 9 && action <= 11) ||
                         action == 13 || action == 14 || action == 16 || action == 17 || action == 19)
                {
                    arguments = awb::ParseExternalJson(
                        awb::ReadFileBounded(ControlText(state->selector), 1024 * 1024), 40);
                    if (action == 13 && arguments.is_object() && arguments.contains("format"))
                        arguments = {{"bundle", arguments}};
                }
                RunCommand(window, *state, LibraryCommands[action], arguments);
                return 0;
            }
            if (button >= FirstAssetMenu && button < FirstAssetMenu + 7)
            {
                const char *commands[] = {
                    "model_status",         "model_configure",     "model_generate_workflow",
                    "site_package_preview", "site_package_import", "site_package_list",
                    "site_package_export"};
                awb::Json arguments = awb::Json::object();
                if (button != FirstAssetMenu && button != FirstAssetMenu + 5)
                    arguments = awb::ParseExternalJson(
                        awb::ReadFileBounded(ControlText(state->selector), 1024 * 1024), 40);
                const auto selected = SendMessageW(state->profiles, LB_GETCURSEL, 0, 0);
                const auto context = selected != LB_ERR && static_cast<size_t>(selected) < state->ids.size()
                                         ? state->ids[static_cast<size_t>(selected)]
                                         : "";
                RunCommand(window, *state, commands[button - FirstAssetMenu], arguments, context);
                return 0;
            }
            if (button == CreateButton)
            {
                RunCommand(window, *state, "profile_create",
                           {{"name", awb::Utf8(ControlText(state->name))},
                            {"allowedOrigins",
                             awb::Json::array({awb::Browser::Origin(awb::Utf8(ControlText(state->url)))})}});
                return 0;
            }
            const auto index = SendMessageW(state->profiles, LB_GETCURSEL, 0, 0);
            if (index == LB_ERR || static_cast<size_t>(index) >= state->ids.size())
                throw awb::Error("PROFILE_NOT_SELECTED", "请先创建并选择环境");
            const auto id = state->busy && !state->busy_profile.empty()
                                ? state->busy_profile
                                : state->ids[static_cast<size_t>(index)];
            if (button == CredentialsMenu)
            {
                desktop::OpenCredentialManager(window, state->service.get(), id,
                                               state->metadata.at(id).at("allowedOrigins"));
                return 0;
            }
            if (button == DebuggerMenu)
            {
                if (!state->run_ids.count(id))
                    throw awb::Error("WORKFLOW_RUN_NOT_FOUND", "当前环境还没有从界面启动的流程");
                desktop::OpenWorkflowDebugger(window, state->service.get(), state->run_ids.at(id));
                return 0;
            }
            if (button == DebugNextMenu)
            {
                state->debug_next = !state->debug_next;
                CheckMenuItem(GetMenu(window), DebugNextMenu,
                              MF_BYCOMMAND | (state->debug_next ? MF_CHECKED : MF_UNCHECKED));
                return 0;
            }
            awb::Json arguments{{"profileId", id}};
            if (button == DiscoverMenu || button == RepairMenu)
            {
                if (!state->leases.count(id))
                    throw awb::Error("LEASE_REQUIRED", "先取得当前环境控制权");
                if (button == RepairMenu)
                    arguments.update(awb::ParseExternalJson(
                        awb::ReadFileBounded(ControlText(state->selector), 256 * 1024), 40));
                else
                    arguments["query"] = awb::Utf8(ControlText(state->selector));
                arguments["profileId"] = id;
                arguments["lease"] = state->leases.at(id);
                RunCommand(window, *state, button == DiscoverMenu ? "page_discover" : "workflow_repair_draft",
                           arguments);
                return 0;
            }
            if (button >= PickerStartMenu && button <= PickerStopMenu)
            {
                if (button != PickerStopMenu)
                {
                    if (!state->leases.count(id))
                        throw awb::Error("LEASE_REQUIRED", "先取得控制权再选择元素；停止拾取不需要租约");
                    arguments["lease"] = state->leases.at(id);
                }
                RunCommand(window, *state,
                           button == PickerStartMenu  ? "page_pick_start"
                           : button == PickerReadMenu ? "page_pick_read"
                                                      : "page_pick_stop",
                           arguments);
                return 0;
            }
            if (button >= RecorderStartMenu && button <= RecorderDraftMenu)
            {
                if (button == RecorderDraftMenu)
                {
                    if (!state->recorded_drafts.count(id))
                        throw awb::Error("RECORDING_NOT_FOUND", "先读取或停止当前环境录制，再打开草稿");
                    ShowFlowEditor(window, *state, &state->recorded_drafts.at(id));
                    return 0;
                }
                if (!state->leases.count(id))
                    throw awb::Error("LEASE_REQUIRED", "请先取得当前环境控制权");
                arguments["lease"] = state->leases.at(id);
                const auto command = button == RecorderStartMenu  ? "recorder_start"
                                     : button == RecorderReadMenu ? "recorder_read"
                                                                  : "recorder_control";
                if (command == std::string("recorder_control"))
                    arguments["operation"] = button == RecorderPauseMenu    ? "pause"
                                             : button == RecorderResumeMenu ? "resume"
                                                                            : "stop";
                RunCommand(window, *state, command, arguments);
                return 0;
            }
            if (button == ExtractButton || button == BatchButton)
            {
                if (!state->leases.count(id))
                    throw awb::Error("LEASE_REQUIRED", "请先取得当前环境控制租约");
                const auto content = awb::ReadFileBounded(ControlText(state->selector), 256 * 1024);
                const auto definition = awb::ParseExternalJson(content, 16);
                if (!definition.is_object() || definition.contains("profileId") ||
                    definition.contains("lease"))
                    throw awb::Error("INVALID_ARGUMENTS", "JSON文件只包含提取定义或批次定义");
                arguments.update(definition);
                arguments["lease"] = state->leases.at(id);
                RunCommand(window, *state, button == ExtractButton ? "page_extract" : "page_batch",
                           arguments);
                return 0;
            }
            if (button >= StartDiagnosticsButton && button <= StopDiagnosticsButton)
            {
                if (!state->leases.count(id))
                    throw awb::Error("LEASE_REQUIRED", "请先取得当前环境控制租约");
                arguments["lease"] = state->leases.at(id);
                RunCommand(window, *state,
                           button == StartDiagnosticsButton  ? "browser_diagnostics_start"
                           : button == ReadDiagnosticsButton ? "browser_diagnostics_read"
                                                             : "browser_diagnostics_stop",
                           arguments);
                return 0;
            }
            if (button >= ValidateWorkflowButton && button <= CancelWorkflowButton)
            {
                std::string workflow_command;
                if (button == ValidateWorkflowButton || button == PublishWorkflowButton)
                {
                    const auto source = awb::ReadFileBounded(ControlText(state->selector), 256 * 1024);
                    // 用途：限制用户导入流程文件解析深度，保持源文件字节不变；日期：2026-11-04。
                    const auto definition = awb::ParseExternalJson(source, 32);
                    arguments = {{"definition", definition}};
                    workflow_command =
                        button == ValidateWorkflowButton ? "workflow_validate" : "workflow_publish";
                }
                else if (button == RunWorkflowButton)
                {
                    if (!state->workflow_ids.count(id) || !state->leases.count(id))
                        throw awb::Error("WORKFLOW_NOT_SELECTED", "先为当前环境发布流程并取得控制权");
                    auto text = awb::Utf8(ControlText(state->value));
                    SetWindowTextW(state->value, L"");
                    arguments["workflowId"] = state->workflow_ids.at(id);
                    arguments["lease"] = state->leases.at(id);
                    // 用途：输入仅驻留内存且执行后清空隐藏控件，不写入发布定义；日期：2026-11-04。
                    arguments["input"] = awb::ParseExternalJson(text.empty() ? "{}" : text, 16);
                    if (state->debug_next)
                        arguments["debug"] = {{"pauseOnStart", true}};
                    workflow_command = "workflow_run";
                }
                else
                {
                    if (!state->run_ids.count(id))
                        throw awb::Error("WORKFLOW_RUN_NOT_FOUND", "当前环境还没有从界面启动的流程");
                    arguments = {{"runId", state->run_ids.at(id)}};
                    workflow_command =
                        button == WorkflowStatusButton ? "workflow_status" : "workflow_control";
                    if (button != WorkflowStatusButton)
                        arguments["operation"] = button == PauseWorkflowButton    ? "pause"
                                                 : button == ResumeWorkflowButton ? "resume"
                                                                                  : "cancel";
                }
                // 发布/运行归属只供界面关联，不进入工具协议的额外参数。
                RunCommand(window, *state, workflow_command, arguments, id);
                return 0;
            }
            if (button >= UpdateButton && button <= PurgeButton)
            {
                arguments["expectedRevision"] = state->metadata.at(id).at("revision");
                const auto command = button == UpdateButton    ? "profile_update"
                                     : button == ArchiveButton ? "profile_archive"
                                     : button == TrashButton   ? "profile_trash"
                                     : button == RestoreButton ? "profile_restore"
                                                               : "profile_purge";
                if (button == UpdateButton)
                    arguments["name"] = awb::Utf8(ControlText(state->name));
                if (button == PurgeButton)
                {
                    const auto prompt =
                        L"永久删除环境及全部本地登录数据，无法撤销。\n环境ID：" + awb::Wide(id);
                    if (MessageBoxW(window, prompt.c_str(), L"确认永久删除",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                        return 0;
                    arguments["confirmProfileId"] = id;
                }
                RunCommand(window, *state, command, arguments);
                return 0;
            }
            if (button == RevokeButton || button == StopButton)
            {
                ++state->control_epoch;
                state->leases.erase(id);
                const auto revoked = state->service->Execute("lease_revoke", arguments);
                SetWindowTextW(state->output, awb::Wide(revoked.dump(2)).c_str());
                if (button == StopButton)
                {
                    if (state->busy)
                        state->pending_stop = id;
                    else
                        RunCommand(window, *state, "browser_stop", arguments);
                }
                return 0;
            }
            std::string command;
            if (button >= PagesButton && button <= ReconnectButton)
            {
                const auto held = state->leases.find(id);
                if (held == state->leases.end())
                    throw awb::Error("LEASE_REQUIRED", "请先取得当前环境控制租约");
                arguments["lease"] = held->second;
                command = button == PagesButton        ? "page_list"
                          : button == OpenPageButton   ? "page_open"
                          : button == SwitchPageButton ? "page_switch"
                          : button == ClosePageButton  ? "page_close"
                          : button == EventsButton     ? "browser_events"
                                                       : "browser_reconnect";
                if (button == OpenPageButton)
                    arguments["url"] = awb::Utf8(ControlText(state->url));
                if (button == SwitchPageButton || button == ClosePageButton)
                {
                    const auto page = SendMessageW(state->pages, CB_GETCURSEL, 0, 0);
                    if (state->page_profile != id || page == CB_ERR ||
                        static_cast<size_t>(page) >= state->page_ids.size())
                        throw awb::Error("PAGE_NOT_SELECTED", "先刷新页面列表并选择当前环境的页面");
                    arguments["pageId"] = state->page_ids[static_cast<size_t>(page)];
                }
                RunCommand(window, *state, command, arguments);
                return 0;
            }
            if (button == StartButton)
            {
                command = "browser_start";
                arguments["headless"] = false;
            }
            else if (button == ResumeButton)
            {
                command = "lease_acquire";
                arguments["owner"] = "desktop";
                arguments["ttlMs"] = 300000;
            }
            else if (button == NavigateButton || button == SnapshotButton || button == LocateButton ||
                     button == FillButton || button == ClickButton || button == TextButton ||
                     button == InspectButton || button == ClearHighlightButton || button == PreviewButton ||
                     button == SelectButton)
            {
                const auto held = state->leases.find(id);
                if (held == state->leases.end())
                    throw awb::Error("LEASE_REQUIRED", "请先启动当前环境并取得控制租约");
                arguments["lease"] = held->second;
                command = button == NavigateButton         ? "browser_navigate"
                          : button == SnapshotButton       ? "page_snapshot"
                          : button == LocateButton         ? "page_locate"
                          : button == FillButton           ? "ui_fill"
                          : button == ClickButton          ? "ui_click"
                          : button == InspectButton        ? "page_inspect"
                          : button == ClearHighlightButton ? "page_clear_highlight"
                          : button == PreviewButton        ? "ui_preview"
                          : button == SelectButton         ? "ui_select"
                                                           : "ui_text";
                if (button == NavigateButton)
                    arguments["url"] = awb::Utf8(ControlText(state->url));
                else if (button != SnapshotButton && button != ClearHighlightButton)
                    arguments["selector"] = awb::Utf8(ControlText(state->selector));
                if (button == InspectButton)
                    arguments["highlight"] = true;
                if (button == FillButton || button == SelectButton)
                {
                    arguments["value"] = awb::Utf8(ControlText(state->value));
                    SetWindowTextW(state->value, L"");
                }
            }
            if (!command.empty())
                RunCommand(window, *state, command, arguments);
            return 0;
        }
        if (message == WM_CLOSE && state)
        {
            if (state->busy)
            {
                SetWindowTextW(state->output,
                               L"当前操作尚未返回，请等待完成后关闭；受管浏览器随工作台退出。");
                return 0;
            }
            for (auto popup = GetWindow(window, GW_HWNDFIRST); popup; popup = GetWindow(popup, GW_HWNDNEXT))
            {
                wchar_t type[64]{};
                GetClassNameW(popup, type, 64);
                if (GetWindow(popup, GW_OWNER) == window && std::wstring(type) == L"AWB.AccountPanel")
                {
                    SetWindowTextW(state->output, L"请先关闭后台账号窗口，再退出工作台。");
                    return 0;
                }
                if (GetWindow(popup, GW_OWNER) == window &&
                    (std::wstring(type) == L"AwbFlowEditor" || std::wstring(type) == L"AWB.Library.Panel") &&
                    !IsWindowEnabled(popup))
                {
                    SetWindowTextW(state->output, L"请先关闭子窗口的文件选择或确认对话框，再退出工作台。");
                    return 0;
                }
            }
            desktop::SaveFlowEditorsForShutdown(window);
        }
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
    }
    catch (const std::exception &error)
    {
        std::cerr << awb::ErrorResult(error).dump() << std::endl;
        if (state && state->output)
        {
            SetWindowTextW(state->output, awb::Wide(awb::ErrorResult(error).dump(2)).c_str());
            state->theme.Activity(3);
        }
        if (message == WM_CREATE)
            return -1;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

// 用途：创建可运行原生壳，smoke模式验证真实控件后退出；日期：2026-11-03。
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        WindowState state;
        std::wstring root = awb::DefaultRoot();
        std::wstring browser;
        std::wstring flow_report;
        for (int index = 1; index < argc; ++index)
        {
            if (std::wstring(argv[index]) == L"--smoke-test")
                state.smoke = true;
            else if (std::wstring(argv[index]) == L"--root" && index + 1 < argc)
                root = argv[++index];
            else if (std::wstring(argv[index]) == L"--browser" && index + 1 < argc)
                browser = argv[++index];
            else if (std::wstring(argv[index]) == L"--flow-self-test" && index + 1 < argc)
                flow_report = argv[++index];
            else
                throw awb::Error("INVALID_ARGUMENTS", "Unsupported window argument");
        }
        LocalFree(argv);
        if (!flow_report.empty())
            return desktop::TestFlowEditor(flow_report);
        if (state.smoke)
        {
            wchar_t temporary[MAX_PATH]{};
            GetTempPathW(MAX_PATH, temporary);
            root = std::wstring(temporary) + L"awb-shell-" + awb::Wide(awb::RandomId());
        }
        state.service = std::make_unique<awb::Service>(root, browser);
        state.recovery_directory = awb::SafeAbsolute(root) + L"\\flow-drafts";
        WNDCLASSW type{};
        type.lpfnWndProc = WindowProcedure;
        type.hInstance = instance;
        type.lpszClassName = L"AIBrowserWorkbench";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = nullptr;
        if (!RegisterClassW(&type))
            throw awb::Error("UI_CREATE_FAILED", "Cannot register window class");
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const auto window =
            CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"AI 浏览器工作台",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            std::min(1280L, work.right - work.left - 32),
                            std::min(920L, work.bottom - work.top - 32), nullptr, nullptr, instance, &state);
        if (!window)
            throw awb::Error("UI_CREATE_FAILED", "Cannot create native shell");
        const auto menu = CreateMenu();
        AppendMenuW(menu, MF_STRING, FlowEditorMenu, L"可视化流程编辑器");
        const auto recording_menu = CreatePopupMenu();
        const wchar_t *recording_labels[] = {L"开始录制当前主页面", L"读取录制与缺项", L"暂停录制",
                                             L"恢复录制",           L"停止并生成草稿", L"打开录制草稿画布"};
        for (int index = 0; index < 6; ++index)
            AppendMenuW(recording_menu, MF_STRING, RecorderStartMenu + index, recording_labels[index]);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recording_menu), L"网页录制（预览）");
        const auto picker_menu = CreatePopupMenu();
        AppendMenuW(picker_menu, MF_STRING, PickerStartMenu, L"开始鼠标拾取");
        AppendMenuW(picker_menu, MF_STRING, PickerReadMenu, L"读取所选元素");
        AppendMenuW(picker_menu, MF_STRING, PickerStopMenu, L"退出拾取（不补发点击）");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(picker_menu), L"元素拾取（预览）");
        const auto automation_menu = CreatePopupMenu();
        AppendMenuW(automation_menu, MF_STRING, CredentialsMenu, L"管理当前环境凭据");
        AppendMenuW(automation_menu, MF_STRING, DebugNextMenu, L"下次流程从首节点暂停调试");
        AppendMenuW(automation_menu, MF_STRING, DebuggerMenu, L"打开当前运行调试器");
        AppendMenuW(automation_menu, MF_STRING, DiscoverMenu, L"查找功能（选择器框输入名称）");
        AppendMenuW(automation_menu, MF_STRING, RepairMenu, L"从JSON文件生成定位修复草稿");
        AppendMenuW(automation_menu, MF_STRING, DoctorMenu, L"只读状态诊断");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(automation_menu), L"凭据与调试");
        const auto assets_menu = CreatePopupMenu();
        const wchar_t *asset_labels[] = {L"查看模型配置与预算",     L"从JSON文件配置模型",
                                         L"从JSON文件生成流程草稿", L"从JSON文件预览知识包差异",
                                         L"从JSON文件导入知识包",   L"列出知识包版本",
                                         L"从JSON文件指定导出版本"};
        for (int index = 0; index < 7; ++index)
            AppendMenuW(assets_menu, MF_STRING, FirstAssetMenu + index, asset_labels[index]);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(assets_menu), L"模型与知识包");
        const auto library_menu = CreatePopupMenu();
        AppendMenuW(library_menu, MF_STRING, AccountMenu, L"后台账号与授权（表单）");
        AppendMenuW(library_menu, MF_STRING, LibraryManagerMenu, L"环境库管理（表单）");
        for (size_t index = 0; index < _countof(LibraryCommands); ++index)
            AppendMenuW(library_menu, MF_STRING, FirstLibraryMenu + index, LibraryLabels[index]);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(library_menu), L"环境库与本地后台");
        SetMenu(window, menu);
        if (state.smoke)
        {
            const bool controls_ready = state.profiles && state.name && state.url && state.output &&
                                        state.selector && state.value && GetDlgItem(window, RevokeButton) &&
                                        GetDlgItem(window, FillButton) && GetDlgItem(window, ClickButton) &&
                                        GetDlgItem(window, ResumeButton);
            const auto profile = state.service->Execute(
                "profile_create",
                {{"name", "急停控件验证"}, {"allowedOrigins", awb::Json::array({"http://127.0.0.1:3000"})}});
            const auto profile_id = profile.at("id").get<std::string>();
            state.service->Execute(
                "lease_acquire",
                {{"profileId", profile_id}, {"owner", "before-ui-revoke"}, {"ttlMs", 300000}});
            Refresh(state);
            state.busy = true;
            state.busy_profile = profile_id;
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(RevokeButton, BN_CLICKED), 0);
            const auto after = state.service->Execute(
                "lease_acquire",
                {{"profileId", profile_id}, {"owner", "after-ui-revoke"}, {"ttlMs", 300000}});
            const bool revoke_ready = after.at("owner") == "after-ui-revoke";
            state.busy = false;
            DestroyWindow(window);
            state.service.reset();
            DeleteFileW((root + L"\\.owner.lock").c_str());
            DeleteFileW((root + L"\\workbench.sqlite3").c_str());
            RemoveDirectoryW((root + L"\\" + awb::Wide(profile_id)).c_str());
            RemoveDirectoryW(root.c_str());
            return controls_ready && revoke_ready ? 0 : 2;
        }
        ShowWindow(window, show);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (!IsDialogMessageW(GetAncestor(message.hwnd, GA_ROOT), &message))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (state.worker.joinable())
            state.worker.join();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << awb::ErrorResult(error).dump() << std::endl;
        MessageBoxW(nullptr, awb::Wide(error.what()).c_str(), L"工作台启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}
