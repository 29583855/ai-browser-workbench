#include "automation_windows.hpp"
#include "theme.hpp"
#include <algorithm>
#include <memory>
#include <sstream>

namespace desktop
{
namespace
{
constexpr int Listing = 1101, NameField = 1102, SecretField = 1103, Detail = 1104, Points = 1105;
constexpr int FirstAction = 1120;
struct Panel
{
    Theme theme;
    awb::Service *service = nullptr;
    std::string profile, run;
    awb::Json origins, credentials = awb::Json::array();
    HWND window = nullptr;
    bool debugger = false;
    bool owned = false;
};
// 用途：创建原生控件、保持主题字体与键盘顺序；日期：2026-11-05。
HWND Child(Panel &panel, const wchar_t *kind, const wchar_t *label, DWORD style, int id)
{
    const auto control = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                         panel.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         GetModuleHandleW(nullptr), nullptr);
    if (!control)
        throw awb::Error("AUTOMATION_UI_FAILED", "Cannot create automation control");
    panel.theme.Style(control);
    return control;
}
// 用途：读取限定大小的窗口文本，支持中文且不写入日志；日期：2026-11-05。
std::string Read(HWND window)
{
    const auto length = GetWindowTextLengthW(window);
    if (length > 65536)
        throw awb::Error("INVALID_ARGUMENTS", "Control input exceeds its budget");
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, &value[0], length + 1);
    value.resize(static_cast<size_t>(length));
    return awb::Utf8(value);
}
// 用途：按当前DPI布置窗口，详情区域可滚动且控件保持可达；日期：2026-11-05。
void Layout(Panel &panel)
{
    RECT client{};
    GetClientRect(panel.window, &client);
    const auto scale = [&](int value) { return panel.theme.Scale(value); };
    const auto place = [&](int id, int x, int y, int width, int height)
    { MoveWindow(GetDlgItem(panel.window, id), scale(x), scale(y), scale(width), scale(height), TRUE); };
    const int width =
        (std::max)(760, MulDiv(client.right, 96, static_cast<int>(GetDpiForWindow(panel.window))));
    const int height =
        (std::max)(500, MulDiv(client.bottom, 96, static_cast<int>(GetDpiForWindow(panel.window))));
    place(1140, 20, 16, width - 40, 28);
    place(1141, 20, 48, width - 40, 42);
    if (panel.debugger)
    {
        place(1142, 20, 100, 190, 24);
        place(Points, 20, 128, 220, height - 210);
        for (int index = 0; index < 8; ++index)
            place(FirstAction + index, 260 + (index % 4) * 118, 100 + (index / 4) * 42, 110, 34);
        place(Detail, 260, 190, width - 280, height - 210);
    }
    else
    {
        place(Listing, 20, 100, 300, height - 172);
        place(1142, 340, 100, 160, 24);
        place(NameField, 340, 130, width - 360, 32);
        place(1143, 340, 174, 160, 24);
        place(SecretField, 340, 204, width - 360, 32);
        for (int index = 0; index < 4; ++index)
            place(FirstAction + index, 340 + (index % 2) * 160, 252 + (index / 2) * 42, 150, 34);
        place(Detail, 340, 346, width - 360, height - 366);
        place(1144, 20, height - 58, 300, 36);
    }
}
// 用途：显示共享服务的真实结果和错误，秘密输入从不拼入展示；日期：2026-11-05。
void Display(Panel &panel, const awb::Json &value)
{
    SetWindowTextW(GetDlgItem(panel.window, Detail), awb::Wide(value.dump(2)).c_str());
}
// 用途：刷新凭据元数据列表，恢复按ID选中且不读取任何秘密；日期：2026-11-05。
void Credentials(Panel &panel, const std::string &selected = "")
{
    panel.credentials = panel.service->Execute("credential_list", awb::Json::object()).at("credentials");
    const auto list = GetDlgItem(panel.window, Listing);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    size_t select = 0;
    for (size_t index = 0; index < panel.credentials.size(); ++index)
    {
        const auto &entry = panel.credentials[index];
        const auto text = entry.at("name").get<std::string>() + " [r" +
                          std::to_string(entry.at("revision").get<int>()) + "]";
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(awb::Wide(text).c_str()));
        if (entry.at("credentialId") == selected)
            select = index;
    }
    if (!panel.credentials.empty())
        SendMessageW(list, LB_SETCURSEL, select, 0);
    Display(panel, panel.credentials.empty() ? awb::Json{{"credentials", awb::Json::array()}}
                                             : panel.credentials[select]);
}
// 用途：按精确发布流程/节点标识解析断点，每行仅接受一个flow/step；日期：2026-11-05。
awb::Json Breakpoints(Panel &panel)
{
    std::istringstream lines(Read(GetDlgItem(panel.window, Points)));
    std::string line;
    awb::Json points = awb::Json::array();
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const auto separator = line.find('/');
        if (separator == std::string::npos || line.find('/', separator + 1) != std::string::npos)
            throw awb::Error("WORKFLOW_DEBUG_INVALID", "断点每行格式为 main/节点ID 或 子流程名/节点ID");
        points.push_back({{"flow", line.substr(0, separator)}, {"stepId", line.substr(separator + 1)}});
    }
    return points;
}
// 用途：读取实际运行状态，终态保留证据，暂停时才请求脱敏变量；日期：2026-11-05。
void DebugState(Panel &panel)
{
    auto status = panel.service->Execute("workflow_status", {{"runId", panel.run}});
    if (status.at("state") == "paused")
        status["debug"] = panel.service->Execute("workflow_debug",
                                                 {{"runId", panel.run}, {"options", awb::Json::object()}});
    Display(panel, status);
}
// 用途：处理凭据按钮，取值后立即清空掩码控件且保持环境/来源绑定；日期：2026-11-05。
void CredentialAction(Panel &panel, int action)
{
    if (action == 3)
    {
        Credentials(panel);
        return;
    }
    auto secret = Read(GetDlgItem(panel.window, SecretField));
    SetWindowTextW(GetDlgItem(panel.window, SecretField), L"");
    try
    {
        if (action == 0)
        {
            const auto result = panel.service->Execute("credential_create",
                                                       {{"name", Read(GetDlgItem(panel.window, NameField))},
                                                        {"value", secret},
                                                        {"purpose", "browser"},
                                                        {"allowedOrigins", panel.origins},
                                                        {"profileIds", awb::Json::array({panel.profile})}});
            Credentials(panel, result.at("credentialId"));
        }
        else
        {
            const auto selected = SendMessageW(GetDlgItem(panel.window, Listing), LB_GETCURSEL, 0, 0);
            if (selected == LB_ERR || static_cast<size_t>(selected) >= panel.credentials.size())
                throw awb::Error("CREDENTIAL_NOT_SELECTED", "请先选择凭据");
            const auto &entry = panel.credentials[static_cast<size_t>(selected)];
            awb::Json arguments{{"credentialId", entry.at("credentialId")},
                                {"expectedRevision", entry.at("revision")}};
            if (action == 1)
                arguments["value"] = secret;
            panel.service->Execute(action == 1 ? "credential_rotate" : "credential_delete", arguments);
            Credentials(panel);
        }
    }
    catch (...)
    {
        if (!secret.empty())
            SecureZeroMemory(&secret[0], secret.size());
        throw;
    }
    if (!secret.empty())
        SecureZeroMemory(&secret[0], secret.size());
}
// 用途：以固定运行ID操作调试器，不派发额外网页动作或重试已执行步骤；日期：2026-11-05。
void DebugAction(Panel &panel, int action)
{
    const char *operations[] = {"", "pause", "resume", "step", "stepOver", "stepOut", "cancel"};
    if (action == 7)
        panel.service->Execute("workflow_debug",
                               {{"runId", panel.run}, {"options", {{"breakpoints", Breakpoints(panel)}}}});
    else if (action > 0 && action < 7)
        panel.service->Execute("workflow_control", {{"runId", panel.run}, {"operation", operations[action]}});
    DebugState(panel);
}
// 用途：承载原生管理/调试窗口，销毁前解绑定时器并释放窗口专属状态；日期：2026-11-05。
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *panel = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        panel = static_cast<Panel *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        panel->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    if (!panel)
        return DefWindowProcW(window, message, wparam, lparam);
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            panel->theme.Initialize(window);
            Child(*panel, L"STATIC", panel->debugger ? L"流程运行调试" : L"安全凭据库", 0, 1140);
            Child(*panel, L"STATIC",
                  panel->debugger
                      ? L"暂停只阻止后续步骤。单步不回滚网页；未知结果必须先核对。"
                      : L"凭据使用 Windows 用户加密。创建后复制 credentialId，以 SecretRef 绑定填写节点。",
                  0, 1141);
            Child(*panel, L"STATIC", panel->debugger ? L"断点：每行 flow/stepId" : L"凭据名称", 0, 1142);
            Child(*panel, L"EDIT", L"",
                  WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, Detail);
            if (panel->debugger)
            {
                Child(*panel, L"EDIT", L"",
                      WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, Points);
                const wchar_t *labels[] = {L"刷新状态", L"暂停",     L"继续",     L"单步进入",
                                           L"单步跨过", L"单步跳出", L"取消运行", L"设置断点"};
                for (int index = 0; index < 8; ++index)
                    Child(*panel, L"BUTTON", labels[index], BS_PUSHBUTTON | WS_TABSTOP, FirstAction + index);
                DebugState(*panel);
                SetTimer(window, 1, 500, nullptr);
            }
            else
            {
                Child(*panel, L"LISTBOX", L"", WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, Listing);
                Child(*panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, NameField);
                Child(*panel, L"STATIC", L"秘密值（使用后清空）", 0, 1143);
                Child(*panel, L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP,
                      SecretField);
                SendMessageW(GetDlgItem(window, NameField), EM_SETLIMITTEXT, 128, 0);
                SendMessageW(GetDlgItem(window, SecretField), EM_SETLIMITTEXT, 65536, 0);
                Child(*panel, L"STATIC", L"列表只显示元数据；删除不会撤回网站已接收的数据。", 0, 1144);
                const wchar_t *labels[] = {L"创建凭据", L"轮换所选", L"删除所选", L"刷新列表"};
                for (int index = 0; index < 4; ++index)
                    Child(*panel, L"BUTTON", labels[index], BS_PUSHBUTTON | WS_TABSTOP, FirstAction + index);
                Credentials(*panel);
            }
            Layout(*panel);
            return 0;
        }
        case WM_SIZE:
            Layout(*panel);
            return 0;
        case WM_DPICHANGED:
        {
            panel->theme.DpiChanged(HIWORD(wparam));
            const auto *bounds = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(*panel);
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lparam)->ptMinTrackSize = {panel->theme.Scale(800),
                                                                      panel->theme.Scale(560)};
            return 0;
        case WM_COMMAND:
        {
            const auto id = LOWORD(wparam);
            if (id >= FirstAction && id < FirstAction + 8)
            {
                if (panel->debugger)
                    DebugAction(*panel, id - FirstAction);
                else
                    CredentialAction(*panel, id - FirstAction);
                return 0;
            }
            if (id == Listing && HIWORD(wparam) == LBN_SELCHANGE)
            {
                const auto selected = SendMessageW(GetDlgItem(window, Listing), LB_GETCURSEL, 0, 0);
                if (selected != LB_ERR && static_cast<size_t>(selected) < panel->credentials.size())
                    Display(*panel, panel->credentials[static_cast<size_t>(selected)]);
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (panel->debugger)
                DebugState(*panel);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            KillTimer(window, 1);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            if (panel->owned)
                delete panel;
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }
    catch (const std::exception &error)
    {
        if (message == WM_CREATE)
            return -1;
        Display(*panel, awb::ErrorResult(error));
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
// 用途：创建受主窗口拥有的独立工具窗，失败不泄露状态或服务指针；日期：2026-11-05。
void Open(HWND owner, std::unique_ptr<Panel> panel)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW kind{};
    kind.hInstance = instance;
    kind.lpszClassName = L"AWB.Automation.Panel";
    kind.lpfnWndProc = Procedure;
    kind.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    kind.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassW(&kind) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw awb::Error("AUTOMATION_UI_FAILED", "Cannot register automation window");
    // 窗口从WM_NCCREATE开始持有状态，WM_NCDESTROY统一释放。
    auto *state = panel.get();
    const auto window = CreateWindowExW(
        WS_EX_CONTROLPARENT, kind.lpszClassName,
        state->debugger ? L"AI Browser Workbench · 流程调试器" : L"AI Browser Workbench · 凭据管理",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1040, 720, owner, nullptr, instance, state);
    if (!window)
        throw awb::Error("AUTOMATION_UI_FAILED", "Cannot open automation window");
    state->owned = true;
    panel.release();
    ShowWindow(window, SW_SHOW);
}
} // namespace
// 用途：固定所选环境及来源创建凭据面板，后续切换不会误绑定；日期：2026-11-05。
void OpenCredentialManager(HWND owner, awb::Service *service, const std::string &profile,
                           const awb::Json &origins)
{
    auto panel = std::make_unique<Panel>();
    panel->service = service;
    panel->profile = profile;
    panel->origins = origins;
    Open(owner, std::move(panel));
}
// 用途：以不透明runId创建调试面板，变量仅从暂停运行查询；日期：2026-11-05。
void OpenWorkflowDebugger(HWND owner, awb::Service *service, const std::string &run)
{
    auto panel = std::make_unique<Panel>();
    panel->service = service;
    panel->run = run;
    panel->debugger = true;
    Open(owner, std::move(panel));
}
} // namespace desktop
