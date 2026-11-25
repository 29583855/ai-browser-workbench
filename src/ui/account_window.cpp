#include "account_window.hpp"
#include "theme.hpp"
#include <shellapi.h>
#include <thread>

namespace desktop
{
namespace
{
constexpr UINT Completed = WM_APP + 85;
constexpr int Server = 1401, Email = 1402, Password = 1403, Tenant = 1404;
constexpr int Login = 1410, Refresh = 1411, Logout = 1412, Portal = 1413, Summary = 1420, Detail = 1421;
struct AccountPanel
{
    Theme theme;
    awb::Service *service = nullptr;
    HWND window = nullptr;
    std::thread worker;
    awb::Json result;
    bool busy = false;
};
// 用途：从账号表单有界读取Unicode文本；日期：2026-11-05。
std::string Read(HWND window, int id)
{
    const auto control = GetDlgItem(window, id);
    const auto length = GetWindowTextLengthW(control);
    if (length > 2048)
        throw awb::Error("INVALID_ARGUMENTS", "账号字段过长");
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &value[0], length + 1);
    value.resize(static_cast<size_t>(length));
    return awb::Utf8(value);
}
// 用途：创建有主题与键盘顺序的账号控件，设计尺寸按当前DPI缩放；日期：2026-11-05。
void Child(AccountPanel &panel, const wchar_t *kind, const wchar_t *label, int id, int x, int y, int width,
           int height, DWORD style = 0)
{
    const auto control = CreateWindowExW(
        0, kind, label, WS_CHILD | WS_VISIBLE | style, panel.theme.Scale(x), panel.theme.Scale(y),
        panel.theme.Scale(width), panel.theme.Scale(height), panel.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!control)
        throw awb::Error("ACCOUNT_UI", "无法创建账号控件");
    panel.theme.Style(control);
    if (std::wstring(kind) == L"EDIT")
        SendMessageW(control, EM_SETLIMITTEXT, id == Detail ? 65536 : 2048, 0);
}
// 用途：设置状态并在后台请求期间禁用所有重复提交入口；日期：2026-11-05。
void Busy(AccountPanel &panel, bool value)
{
    panel.busy = value;
    for (int id : {Server, Email, Password, Tenant, Login, Refresh, Logout, Portal})
        EnableWindow(GetDlgItem(panel.window, id), !value);
    SetWindowTextW(GetDlgItem(panel.window, Summary), value ? L"正在连接后台，请稍候……" : L"操作已返回");
}
// 用途：异步执行同一服务契约，不把口令写入界面结果或持久表单；日期：2026-11-05。
void Run(AccountPanel &panel, const std::string &command, awb::Json arguments)
{
    if (panel.worker.joinable())
        panel.worker.join();
    Busy(panel, true);
    // 用途：工作线程只访问服务和结果，所有原生控件更新回到UI线程；日期：2026-11-05。
    panel.worker = std::thread(
        [&panel, command, arguments]
        {
            try
            {
                panel.result = {{"ok", true}, {"result", panel.service->Execute(command, arguments)}};
            }
            catch (const std::exception &error)
            {
                panel.result = awb::ErrorResult(error);
            }
            PostMessageW(panel.window, Completed, 0, 0);
        });
}
// 用途：处理账号表单、异步回执和忙碌关闭保护；日期：2026-11-05。
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *panel = reinterpret_cast<AccountPanel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        panel = static_cast<AccountPanel *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        panel->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    if (!panel)
        return DefWindowProcW(window, message, wparam, lparam);
    try
    {
        if (message == WM_CREATE)
        {
            panel->theme.Initialize(window);
            Child(*panel, L"STATIC", L"后台账号与商业授权", 1450, 24, 18, 720, 28);
            Child(*panel, L"STATIC",
                  L"在管理门户购买、续费和分配席位；连接后浏览器按组织统一核算环境与并发。", 1451, 24, 54,
                  720, 42);
            Child(*panel, L"STATIC", L"后台地址", 1452, 24, 108, 90, 24);
            Child(*panel, L"EDIT", L"http://127.0.0.1:17861", Server, 122, 104, 600, 30,
                  WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
            Child(*panel, L"STATIC", L"邮箱 / sso", 1453, 24, 154, 90, 24);
            Child(*panel, L"EDIT", L"", Email, 122, 150, 600, 30, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
            Child(*panel, L"STATIC", L"密码 / 连接码", 1454, 24, 200, 90, 24);
            Child(*panel, L"EDIT", L"", Password, 122, 196, 600, 30,
                  WS_TABSTOP | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL);
            Child(*panel, L"STATIC", L"组织ID", 1455, 24, 246, 96, 24);
            Child(*panel, L"EDIT", L"", Tenant, 122, 242, 600, 30, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL);
            Child(*panel, L"BUTTON", L"登录 / 重新认证", Login, 24, 292, 170, 34, WS_TABSTOP);
            Child(*panel, L"BUTTON", L"查询实时权益", Refresh, 204, 292, 150, 34, WS_TABSTOP);
            Child(*panel, L"BUTTON", L"打开管理门户", Portal, 364, 292, 150, 34, WS_TABSTOP);
            Child(*panel, L"BUTTON", L"退出后台账号", Logout, 524, 292, 170, 34, WS_TABSTOP);
            Child(*panel, L"STATIC", L"连接信息由当前Windows用户的DPAPI加密保存。", Summary, 24, 342, 700,
                  28);
            Child(*panel, L"EDIT", L"", Detail, 24, 380, 700, 210,
                  WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY);
            Run(*panel, "account_status", awb::Json::object());
            return 0;
        }
        if (message == Completed)
        {
            if (panel->worker.joinable())
                panel->worker.join();
            Busy(*panel, false);
            auto text = panel->result.dump(2);
            std::wstring native;
            for (const auto c : awb::Wide(text))
            {
                if (c == L'\n')
                    native += L'\r';
                native += c;
            }
            SetWindowTextW(GetDlgItem(window, Detail), native.c_str());
            SetWindowTextW(GetDlgItem(window, Summary), panel->result.value("ok", false)
                                                            ? L"操作完成；购买和席位分配请打开管理门户。"
                                                            : L"操作未完成，详细原因见下方。未自动重试。");
            if (panel->result.value("ok", false) && panel->result.at("result").contains("entitlement"))
            {
                const auto entitlement = panel->result.at("result").at("entitlement");
                const auto summary =
                    L"环境 " + std::to_wstring(entitlement.at("environmentLimit").get<int>()) +
                    L"  |  并发 " + std::to_wstring(entitlement.at("concurrentLimit").get<int>()) +
                    L"  |  席位 " + std::to_wstring(entitlement.at("seatLimit").get<int>()) +
                    L"  |  预付秒数 " +
                    std::to_wstring(entitlement.at("timeBalanceSeconds").get<long long>());
                SetWindowTextW(GetDlgItem(window, Summary), summary.c_str());
            }
            if (panel->result.value("ok", false) && panel->result.at("result").contains("url") &&
                !panel->result.at("result").at("url").get<std::string>().empty())
                SetWindowTextW(GetDlgItem(window, Server),
                               awb::Wide(panel->result.at("result").at("url")).c_str());
            return 0;
        }
        if (message == WM_COMMAND && !panel->busy)
        {
            const auto id = LOWORD(wparam);
            if (id == IDCANCEL)
            {
                SendMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            if (id == Login)
            {
                awb::Json arguments{{"url", Read(window, Server)},
                                    {"email", Read(window, Email)},
                                    {"password", Read(window, Password)}};
                const auto tenant = Read(window, Tenant);
                if (!tenant.empty())
                    arguments["tenantId"] = tenant;
                SetWindowTextW(GetDlgItem(window, Password), L"");
                if (arguments.at("email") == "sso")
                    Run(*panel, "account_connect_code",
                        {{"url", arguments.at("url")}, {"code", arguments.at("password")}});
                else
                    Run(*panel, "account_connect", arguments);
            }
            else if (id == Refresh)
                Run(*panel, "account_status", {{"remote", true}});
            else if (id == Logout)
                Run(*panel, "account_disconnect", awb::Json::object());
            else if (id == Portal)
            {
                const auto url = Read(window, Server);
                if (url.find("https://") != 0 && url.find("http://127.0.0.1:") != 0)
                    throw awb::Error("INVALID_ARGUMENTS", "管理门户仅允许HTTPS或回环HTTP地址");
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", awb::Wide(url).c_str(), nullptr,
                                                            nullptr, SW_SHOWNORMAL)) <= 32)
                    throw awb::Error("ACCOUNT_UI", "无法打开系统默认浏览器");
            }
            return 0;
        }
        if (message == WM_CLOSE)
        {
            if (!panel->busy)
            {
                DestroyWindow(window);
                PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
            }
            return 0;
        }
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
        {
            SetTextColor(reinterpret_cast<HDC>(wparam), Ink);
            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
    }
    catch (const std::exception &error)
    {
        MessageBoxW(window, awb::Wide(error.what()).c_str(), L"后台账号", MB_OK | MB_ICONERROR);
        if (message == WM_CREATE)
            return -1;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
// 用途：运行有所有者保护的模态账号窗口，关闭前等待服务请求结束；日期：2026-11-05。
void OpenAccountWindow(HWND owner, awb::Service *service)
{
    AccountPanel panel;
    panel.service = service;
    WNDCLASSW type{};
    type.lpfnWndProc = Procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"AWB.AccountPanel";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&type);
    const int dpi = static_cast<int>(GetDpiForWindow(owner));
    const auto window =
        CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, type.lpszClassName, L"后台账号与授权",
                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                        MulDiv(770, dpi, 96), MulDiv(650, dpi, 96), owner, nullptr, type.hInstance, &panel);
    if (!window)
        throw awb::Error("ACCOUNT_UI", "无法打开后台账号窗口");
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (IsWindow(window))
    {
        const auto received = GetMessageW(&message, nullptr, 0, 0);
        if (received <= 0)
        {
            if (received == 0)
                PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (panel.worker.joinable())
        panel.worker.join();
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}
} // namespace desktop
