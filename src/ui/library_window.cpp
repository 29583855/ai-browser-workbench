#include "library_window.hpp"
#include "theme.hpp"
#include <algorithm>
#include <commdlg.h>
#include <memory>
#include <sstream>
#include <thread>

namespace desktop
{
namespace
{
constexpr int FirstTab = 1200, Listing = 1210, Name = 1211, ProfileName = 1212, Origins = 1213;
constexpr int Proxy = 1214, UserAgent = 1215, Timezone = 1216, Languages = 1217, Members = 1218;
constexpr int Versions = 1219, Detail = 1220, Scope = 1221, Confirm = 1222, Status = 1223;
constexpr int Fresh = 1240, Create = 1241, Save = 1242, Refresh = 1243, Remove = 1244;
constexpr int Archive = 1245, Trash = 1246, Restore = 1247, Rollback = 1248, Instantiate = 1249;
constexpr int Export = 1250, Import = 1251, Next = 1252;
constexpr UINT Completed = WM_APP + 82;
struct Panel
{
    Theme theme;
    awb::Service *service = nullptr;
    HWND window = nullptr, owner = nullptr;
    awb::Json rows = awb::Json::array(), profiles = awb::Json::array(), history = awb::Json::array();
    awb::Json current, result;
    std::thread worker;
    std::string selected, next_cursor, before;
    int mode = 0, scroll_x = 0, scroll_y = 0;
    bool busy = false, owned = false;
};
// 用途：读取有限的Unicode输入，不截断后静默提交配置；日期：2026-11-05。
std::string Read(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    if (length > 65536)
        throw awb::Error("INVALID_ARGUMENTS", "表单文本超过65536字符，请缩短输入");
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return awb::Utf8(text);
}
// 用途：统一设置可复制的原生控件文本，保留中文字段；日期：2026-11-05。
void Put(Panel &panel, int id, const std::string &text)
{
    const auto value = awb::Wide(text);
    std::wstring native;
    for (size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] == L'\n' && (index == 0 || value[index - 1] != L'\r'))
            native += L'\r';
        native += value[index];
    }
    SetWindowTextW(GetDlgItem(panel.window, id), native.c_str());
}
// 用途：展示真实命令回执，较大结果明确提示改用API导出而不假称完整；日期：2026-11-05。
void Display(Panel &panel, const awb::Json &value)
{
    auto text = value.dump(2);
    if (text.size() > 65536)
        text = awb::Json{{"displayTruncated", true}, {"message", "结果超过面板预算，请通过API保存完整结果"}}
                   .dump(2);
    Put(panel, Detail, text);
}
// 用途：创建带主题、可访问名称与键盘焦点的原生控件；日期：2026-11-05。
HWND Child(Panel &panel, const wchar_t *kind, const wchar_t *label, DWORD style, int id)
{
    const auto control = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                         panel.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         GetModuleHandleW(nullptr), nullptr);
    if (!control)
        throw awb::Error("LIBRARY_UI_FAILED", "无法创建环境库控件");
    panel.theme.Style(control);
    if (std::wstring(kind) == L"EDIT")
        SendMessageW(control, EM_SETLIMITTEXT, 65536, 0);
    return control;
}
// 用途：切换模式时同时隐藏和禁用无关控件，避免Tab进入隐藏字段；日期：2026-11-05。
void Visible(Panel &panel, int id, bool visible)
{
    const auto control = GetDlgItem(panel.window, id);
    ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    EnableWindow(control, visible && !panel.busy);
}
// 用途：按DPI布局并提供双向滚动，较小屏幕仍可访问全部表单和操作；日期：2026-11-05。
void Layout(Panel &panel)
{
    RECT client{};
    GetClientRect(panel.window, &client);
    const int dpi = static_cast<int>(GetDpiForWindow(panel.window));
    const int viewport_width = MulDiv(client.right, 96, dpi),
              viewport_height = MulDiv(client.bottom, 96, dpi);
    const int width = (std::max)(950, viewport_width), height = (std::max)(940, viewport_height);
    panel.theme.group = panel.mode;
    panel.scroll_x = (std::min)(panel.scroll_x, width - viewport_width);
    panel.scroll_y = (std::min)(panel.scroll_y, height - viewport_height);
    SCROLLINFO horizontal{sizeof(SCROLLINFO),
                          SIF_RANGE | SIF_PAGE | SIF_POS,
                          0,
                          width - 1,
                          static_cast<UINT>(viewport_width),
                          panel.scroll_x,
                          0};
    SCROLLINFO vertical{sizeof(SCROLLINFO),
                        SIF_RANGE | SIF_PAGE | SIF_POS,
                        0,
                        height - 1,
                        static_cast<UINT>(viewport_height),
                        panel.scroll_y,
                        0};
    SetScrollInfo(panel.window, SB_HORZ, &horizontal, TRUE);
    SetScrollInfo(panel.window, SB_VERT, &vertical, TRUE);
    // 用途：把设计坐标转换为当前DPI及滚动位置；日期：2026-11-05。
    const auto place = [&](int id, int x, int y, int w, int h)
    {
        MoveWindow(GetDlgItem(panel.window, id), panel.theme.Scale(x - panel.scroll_x),
                   panel.theme.Scale(y - panel.scroll_y), panel.theme.Scale(w), panel.theme.Scale(h), TRUE);
    };
    place(1260, 20, 14, width - 40, 28);
    place(1261, 20, 48, width - 40, 34);
    for (int index = 0; index < 4; ++index)
        place(FirstTab + index, 20 + index * 142, 88, 132, 32);
    place(Refresh, 20, 132, 118, 32);
    place(Fresh, 148, 132, 118, 32);
    place(Listing, 20, 174, 270, height - 234);
    const int right = 314, available = width - right - 20;
    place(1262, right, 132, available, 20);
    place(Name, right, 156, available, 30);
    place(1263, right, 196, available, 20);
    place(ProfileName, right, 220, available, 30);
    place(1264, right, 260, available, 20);
    place(Origins, right, 284, available, 76);
    place(1265, right, 370, available, 20);
    place(Proxy, right, 394, available, 30);
    place(1266, right, 434, available, 20);
    place(UserAgent, right, 458, available, 30);
    place(1267, right, 498, available / 2 - 10, 20);
    place(Timezone, right, 522, available / 2 - 10, 30);
    place(1268, right + available / 2, 498, available / 2, 20);
    place(Languages, right + available / 2, 522, available / 2, 30);
    place(Members, right, 220, available, 332);
    place(1269, right, 562, 100, 24);
    place(Versions, right + 104, 558, available - 104, 220);
    place(Scope, right, 156, 200, 180);
    place(Next, right + 212, 154, 130, 32);
    const int actions = 598;
    place(Create, right, actions, 132, 32);
    place(Save, right + 142, actions, 132, 32);
    place(Remove, right, actions + 40, 132, 32);
    place(Archive, right + 142, actions + 40, 132, 32);
    place(Trash, right + 284, actions + 40, 132, 32);
    place(Restore, right + 426, actions + 40, 132, 32);
    place(Rollback, right, actions, 132, 32);
    place(Instantiate, right + 142, actions, 132, 32);
    place(Export, right, actions + 80, 132, 32);
    place(Import, right + 142, actions + 80, 132, 32);
    const int extra = panel.mode == 0 ? 40 : 0;
    place(Confirm, right, actions + 78 + extra, available, 26);
    place(Detail, right, panel.mode == 3 ? 204 : actions + 110 + extra, available,
          panel.mode == 3 ? 374 : height - actions - 160 - extra);
    place(Status, 20, height - 40, width - 40, 26);
    const bool configured = panel.mode == 0 || panel.mode == 2;
    for (int id : {Name, 1262})
        Visible(panel, id, panel.mode != 3);
    Visible(panel, ProfileName, panel.mode == 2);
    Visible(panel, 1263, panel.mode == 1 || panel.mode == 2);
    Put(panel, 1263, panel.mode == 1 ? "分组成员（Ctrl 或 Shift 多选）" : "配置环境名称 / 新实例名称");
    // 环境模式不占用模板专用名称行，保持固定字段位置以便键盘和辅助工具使用。
    for (int id : {Origins, Proxy, UserAgent, Timezone, Languages, 1264, 1265, 1266, 1267, 1268})
        Visible(panel, id, configured);
    for (int id : {Members})
        Visible(panel, id, panel.mode == 1);
    for (int id : {Versions, 1269, Rollback, Instantiate})
        Visible(panel, id, panel.mode == 2);
    for (int id : {Scope, Next})
        Visible(panel, id, panel.mode == 3);
    for (int id : {Create, Save, Fresh, Remove})
        Visible(panel, id, panel.mode != 3);
    Visible(panel, Archive, panel.mode == 0 || panel.mode == 3);
    for (int id : {Trash, Restore, Export, Import})
        Visible(panel, id, panel.mode == 0);
    // 保存和模板回滚使用独立位置，避免两个可见按钮重叠。
    if (panel.mode == 2)
    {
        place(Create, right + 284, actions, 132, 32);
        place(Save, right + 426, actions, 132, 32);
    }
    for (int index = 0; index < 4; ++index)
    {
        EnableWindow(GetDlgItem(panel.window, FirstTab + index), !panel.busy);
        InvalidateRect(GetDlgItem(panel.window, FirstTab + index), nullptr, TRUE);
    }
    EnableWindow(GetDlgItem(panel.window, Listing), !panel.busy);
    EnableWindow(GetDlgItem(panel.window, Refresh), !panel.busy);
    EnableWindow(GetDlgItem(panel.window, Confirm), !panel.busy);
    EnableWindow(GetDlgItem(panel.window, Next), !panel.busy && !panel.next_cursor.empty());
    Put(panel, Archive, panel.mode == 3 ? "归档批次详情" : "归档环境");
    Put(panel, Remove, panel.mode == 0 ? "永久删除环境" : "删除所选资源");
}
// 用途：解析逐行来源或逗号分隔语言，空行不成为无效数组元素；日期：2026-11-05。
awb::Json Lines(std::string text, bool comma = false)
{
    if (comma)
        std::replace(text.begin(), text.end(), ',', '\n');
    awb::Json values = awb::Json::array();
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
    {
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            continue;
        values.push_back(line.substr(first, line.find_last_not_of(" \t\r") - first + 1));
    }
    return values;
}
// 用途：将有界配置数组显示为可编辑文本，不修改数组原顺序；日期：2026-11-05。
std::string Join(const awb::Json &values, const std::string &separator)
{
    std::string text;
    for (const auto &value : values)
    {
        if (!text.empty())
            text += separator;
        text += value.get<std::string>();
    }
    return text;
}
// 用途：把当前资源的固定revision与配置加载到表单，用户选择历史不暗改当前配置；日期：2026-11-05。
void Fields(Panel &panel, const awb::Json &resource)
{
    panel.current = resource;
    panel.selected = resource.value(panel.mode == 3 ? "batchId" : "id", "");
    SendMessageW(GetDlgItem(panel.window, Confirm), BM_SETCHECK, BST_UNCHECKED, 0);
    if (panel.mode == 3)
    {
        Display(panel, resource);
        return;
    }
    Put(panel, Name, resource.value("name", ""));
    if (panel.mode == 0 || panel.mode == 2)
    {
        const auto configuration =
            panel.mode == 2 ? resource.value("configuration", awb::Json::object()) : resource;
        const auto runtime = configuration.value("runtimeSettings", awb::Json::object());
        Put(panel, ProfileName, configuration.value("name", "模板环境"));
        Put(panel, Origins, Join(configuration.value("allowedOrigins", awb::Json::array()), "\r\n"));
        Put(panel, Proxy, configuration.value("proxyServer", ""));
        Put(panel, UserAgent, runtime.value("userAgent", ""));
        Put(panel, Timezone, runtime.value("timezone", ""));
        Put(panel, Languages, Join(runtime.value("languages", awb::Json::array()), ", "));
    }
    if (panel.mode == 1)
    {
        SendMessageW(GetDlgItem(panel.window, Members), LB_SETSEL, FALSE, -1);
        const auto members = resource.value("profileIds", awb::Json::array());
        for (size_t index = 0; index < panel.profiles.size(); ++index)
            if (std::find(members.begin(), members.end(), panel.profiles[index].at("id")) != members.end())
                SendMessageW(GetDlgItem(panel.window, Members), LB_SETSEL, TRUE, static_cast<LPARAM>(index));
    }
    panel.history = awb::Json::array();
    SendMessageW(GetDlgItem(panel.window, Versions), CB_RESETCONTENT, 0, 0);
    if (panel.mode == 2 && !panel.selected.empty())
    {
        panel.history =
            panel.service->Execute("profile_template_history", {{"id", panel.selected}}).at("items");
        for (const auto &entry : panel.history)
        {
            const auto label = "版本 " + std::to_string(entry.at("revision").get<long long>()) + " · " +
                               entry.at("name").get<std::string>();
            SendMessageW(GetDlgItem(panel.window, Versions), CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(awb::Wide(label).c_str()));
        }
        SendMessageW(GetDlgItem(panel.window, Versions), CB_SETCURSEL, 0, 0);
    }
    Display(panel, {{"event", "selected"}, {"resource", resource}});
}
// 用途：从服务读取真实列表和分页游标，资源ID保持稳定且不靠标签猜测身份；日期：2026-11-05。
void Reload(Panel &panel, const std::string &selected = "")
{
    panel.profiles = panel.service->Execute("profile_list", {{"state", "all"}}).at("profiles");
    SendMessageW(GetDlgItem(panel.window, Members), LB_RESETCONTENT, 0, 0);
    for (const auto &profile : panel.profiles)
    {
        const auto label = profile.at("name").get<std::string>() + " · " +
                           profile.at("state").get<std::string>() + " · " +
                           profile.at("id").get<std::string>().substr(0, 8);
        SendMessageW(GetDlgItem(panel.window, Members), LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(awb::Wide(label).c_str()));
    }
    panel.next_cursor.clear();
    if (panel.mode == 0)
        panel.rows = panel.profiles;
    else if (panel.mode == 1 || panel.mode == 2)
        panel.rows = panel.service
                         ->Execute(panel.mode == 1 ? "profile_group_list" : "profile_template_list",
                                   awb::Json::object())
                         .at("items");
    else
    {
        const auto scope = SendMessageW(GetDlgItem(panel.window, Scope), CB_GETCURSEL, 0, 0);
        awb::Json arguments{{"scope", scope == 1 ? "archived" : (scope == 2 ? "all" : "active")},
                            {"limit", 30}};
        if (!panel.before.empty())
            arguments["before"] = panel.before;
        const auto result = panel.service->Execute("profile_batch_list", arguments);
        panel.rows = result.at("items");
        if (!result.at("nextCursor").is_null())
            panel.next_cursor = result.at("nextCursor");
    }
    SendMessageW(GetDlgItem(panel.window, Listing), LB_RESETCONTENT, 0, 0);
    int select = 0;
    for (size_t index = 0; index < panel.rows.size(); ++index)
    {
        const auto &entry = panel.rows[index];
        const std::string id = entry.at(panel.mode == 3 ? "batchId" : "id");
        const auto label = entry.value("name", entry.value("state", "")) + " · " + id.substr(0, 8);
        SendMessageW(GetDlgItem(panel.window, Listing), LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(awb::Wide(label).c_str()));
        if (id == selected)
            select = static_cast<int>(index);
    }
    if (!panel.rows.empty())
    {
        SendMessageW(GetDlgItem(panel.window, Listing), LB_SETCURSEL, select, 0);
        Fields(panel, panel.rows[static_cast<size_t>(select)]);
    }
    else
        Fields(panel, awb::Json::object());
    Put(panel, Status,
        "已加载 " + std::to_string(panel.rows.size()) +
            " 项；修改按当前revision提交，冲突时请先保留输入再刷新。");
    Layout(panel);
}
// 用途：从结构化表单生成白名单配置，空字段可清除旧覆盖而不继承隐藏值；日期：2026-11-05。
awb::Json Configuration(Panel &panel)
{
    awb::Json configuration{{"name", Read(GetDlgItem(panel.window, panel.mode == 2 ? ProfileName : Name))},
                            {"allowedOrigins", Lines(Read(GetDlgItem(panel.window, Origins)))}};
    const auto proxy = Read(GetDlgItem(panel.window, Proxy));
    if (!proxy.empty())
        configuration["proxyServer"] = proxy;
    awb::Json runtime = awb::Json::object();
    const auto agent = Read(GetDlgItem(panel.window, UserAgent)),
               zone = Read(GetDlgItem(panel.window, Timezone));
    const auto languages = Lines(Read(GetDlgItem(panel.window, Languages)), true);
    if (!agent.empty())
        runtime["userAgent"] = agent;
    if (!zone.empty())
        runtime["timezone"] = zone;
    if (!languages.empty())
        runtime["languages"] = languages;
    configuration["runtimeSettings"] = runtime;
    return configuration;
}
// 用途：在工作线程执行共享命令，UI不自动重试写入，销毁时等待线程结束；日期：2026-11-05。
void Submit(Panel &panel, const std::string &command, awb::Json arguments, const std::wstring &file = L"")
{
    panel.busy = true;
    Layout(panel);
    Put(panel, Status, "正在执行 " + command + "，请等待实际回执；不会自动重试。");
    try
    {
        panel.worker = std::thread(
            [&panel, command, arguments, file]
            {
                try
                {
                    auto result = panel.service->Execute(command, arguments);
                    if (!file.empty())
                    {
                        awb::AtomicWrite(file, result.dump(2) + "\n");
                        result["savedTo"] = awb::Utf8(file);
                    }
                    panel.result = {{"command", command}, {"result", result}, {"completed", true}};
                }
                catch (const std::exception &error)
                {
                    panel.result = awb::ErrorResult(error);
                    panel.result["command"] = command;
                    panel.result["completed"] = true;
                }
                PostMessageW(panel.window, Completed, 0, 0);
            });
    }
    catch (...)
    {
        panel.busy = false;
        Layout(panel);
        throw;
    }
}
// 用途：使用系统文件选择器读取或保存配置包，取消选择不触发动作；日期：2026-11-05。
std::wstring ChooseFile(Panel &panel, bool save)
{
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = panel.window;
    dialog.lpstrFilter = L"JSON 配置包\0*.json\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = _countof(path);
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))
        return path;
    if (CommDlgExtendedError())
        throw awb::Error("LIBRARY_FILE_DIALOG_FAILED", "无法打开配置文件选择器");
    return L"";
}
// 用途：把明确按钮意图映射到版本受控命令，不使用任意JSON作为配置表单；日期：2026-11-05。
void Action(Panel &panel, int action)
{
    if (action == Fresh)
    {
        Fields(panel,
               {{"name", panel.mode == 0 ? "新环境" : (panel.mode == 1 ? "新分组" : "新模板")},
                {"allowedOrigins", awb::Json::array({"http://127.0.0.1:3000"})},
                {"configuration",
                 {{"name", "模板环境"}, {"allowedOrigins", awb::Json::array({"http://127.0.0.1:3000"})}}}});
        SendMessageW(GetDlgItem(panel.window, Listing), LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
        return;
    }
    if (action == Refresh || action == Next)
    {
        panel.before = action == Next ? panel.next_cursor : "";
        Reload(panel, action == Next ? "" : panel.selected);
        return;
    }
    if (action == Import)
    {
        const auto file = ChooseFile(panel, false);
        if (file.empty())
            return;
        const auto value = awb::ParseExternalJson(awb::ReadFileBounded(file, 1024 * 1024), 40);
        Submit(panel, "profile_import", value.contains("format") ? awb::Json{{"bundle", value}} : value);
        return;
    }
    if (action != Create && panel.selected.empty())
        throw awb::Error("LIBRARY_NOT_SELECTED", "请先选择资源；创建新资源请按新建草稿");
    if (action == Export)
    {
        const auto file = ChooseFile(panel, true);
        if (!file.empty())
            Submit(panel, "profile_export", {{"profileIds", awb::Json::array({panel.selected})}}, file);
        return;
    }
    if (action == Remove || (panel.mode == 3 && action == Archive))
        if (SendMessageW(GetDlgItem(panel.window, Confirm), BM_GETCHECK, 0, 0) != BST_CHECKED)
            throw awb::Error("LIBRARY_CONFIRMATION_REQUIRED", "请先勾选确认删除资源或归档批次详情");
    if (panel.mode == 3)
    {
        Submit(panel, "profile_batch_archive",
               {{"batchId", panel.selected}, {"confirmBatchId", panel.selected}});
        return;
    }
    if (panel.mode == 0)
    {
        if (action == Create || action == Save)
        {
            auto arguments = Configuration(panel);
            if (action == Save)
            {
                arguments["profileId"] = panel.selected;
                arguments["expectedRevision"] = panel.current.at("revision");
                arguments["proxyServer"] = arguments.value("proxyServer", "");
            }
            Submit(panel, action == Create ? "profile_create" : "profile_update", arguments);
        }
        else
        {
            awb::Json arguments{{"profileId", panel.selected},
                                {"expectedRevision", panel.current.at("revision")}};
            if (action == Remove)
                arguments["confirmProfileId"] = panel.selected;
            Submit(panel,
                   action == Archive
                       ? "profile_archive"
                       : (action == Trash ? "profile_trash"
                                          : (action == Restore ? "profile_restore" : "profile_purge")),
                   arguments);
        }
        return;
    }
    const auto prefix = panel.mode == 1 ? "profile_group_" : "profile_template_";
    awb::Json arguments{{"id", panel.selected}};
    if (action != Create)
        arguments["expectedRevision"] = panel.current.at("revision");
    if (action == Instantiate)
        arguments["name"] = Read(GetDlgItem(panel.window, ProfileName));
    else if (action == Rollback)
    {
        const auto version = SendMessageW(GetDlgItem(panel.window, Versions), CB_GETCURSEL, 0, 0);
        if (version == CB_ERR || static_cast<size_t>(version) >= panel.history.size())
            throw awb::Error("TEMPLATE_VERSION_NOT_FOUND", "请先选择要回滚的历史版本");
        arguments["sourceRevision"] = panel.history[static_cast<size_t>(version)].at("revision");
    }
    else if (action == Create || action == Save)
    {
        arguments["name"] = Read(GetDlgItem(panel.window, Name));
        if (panel.mode == 1)
        {
            arguments["profileIds"] = awb::Json::array();
            for (size_t index = 0; index < panel.profiles.size(); ++index)
                if (SendMessageW(GetDlgItem(panel.window, Members), LB_GETSEL, index, 0) > 0)
                    arguments["profileIds"].push_back(panel.profiles[index].at("id"));
        }
        else
            arguments["configuration"] = Configuration(panel);
        if (action == Create)
            arguments.erase("id");
    }
    Submit(
        panel,
        std::string(prefix) +
            (action == Create
                 ? "create"
                 : (action == Save
                        ? "update"
                        : (action == Remove ? "delete" : (action == Rollback ? "rollback" : "instantiate")))),
        arguments);
}
// 用途：管理环境库窗口、异步回执、版本冲突和销毁时的线程寿命；日期：2026-11-05。
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
        if (message == WM_CREATE)
        {
            panel->theme.Initialize(window);
            Child(*panel, L"STATIC", L"环境库管理", 0, 1260);
            Child(*panel, L"STATIC",
                  L"结构化配置与版本控制。分组和模板不包含Cookie、凭据或原环境身份；批次归档保留去重摘要。",
                  0, 1261);
            const wchar_t *tabs[] = {L"环境配置", L"环境分组", L"版本模板", L"批次记录"};
            for (int index = 0; index < 4; ++index)
                Child(*panel, L"BUTTON", tabs[index], BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                      FirstTab + index);
            Child(*panel, L"LISTBOX", L"资源列表", WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, Listing);
            const wchar_t *labels[] = {L"资源名称",
                                       L"配置环境名称 / 新实例名称",
                                       L"允许来源（每行一个，至少一个）",
                                       L"代理（空白清除；当前支持无认证HTTP/SOCKS5）",
                                       L"User-Agent（空白使用浏览器默认）",
                                       L"时区（如Asia/Shanghai）",
                                       L"语言（逗号或换行分隔）",
                                       L"历史版本"};
            for (int index = 0; index < 8; ++index)
                Child(*panel, L"STATIC", labels[index], 0, 1262 + index);
            for (int id : {Name, ProfileName, Proxy, UserAgent, Timezone, Languages})
                Child(*panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, id);
            Child(*panel, L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
                  Origins);
            Child(*panel, L"LISTBOX", L"分组成员（Ctrl或Shift多选）",
                  WS_BORDER | LBS_EXTENDEDSEL | WS_VSCROLL | WS_TABSTOP, Members);
            Child(*panel, L"COMBOBOX", L"模板历史", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, Versions);
            Child(*panel, L"EDIT", L"",
                  WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, Detail);
            Child(*panel, L"COMBOBOX", L"批次范围", CBS_DROPDOWNLIST | WS_TABSTOP, Scope);
            for (const auto *label : {L"未归档批次", L"已归档摘要", L"全部批次"})
                SendMessageW(GetDlgItem(window, Scope), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(GetDlgItem(window, Scope), CB_SETCURSEL, 0, 0);
            Child(*panel, L"BUTTON", L"我确认删除所选资源 / 归档批次详情（详情不可恢复）",
                  BS_AUTOCHECKBOX | WS_TABSTOP, Confirm);
            Child(*panel, L"STATIC", L"准备就绪", 0, Status);
            const wchar_t *actions[] = {L"新建草稿",     L"创建",         L"保存修改",   L"刷新 / 第一页",
                                        L"删除所选资源", L"归档环境",     L"移入回收站", L"恢复环境",
                                        L"回滚成新版本", L"创建环境实例", L"导出配置包", L"导入配置包",
                                        L"下一页"};
            for (int index = 0; index < 13; ++index)
                Child(*panel, L"BUTTON", actions[index], BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                      Fresh + index);
            Reload(*panel, panel->selected);
            return 0;
        }
        if (message == Completed)
        {
            if (panel->worker.joinable())
                panel->worker.join();
            panel->busy = false;
            auto result = panel->result;
            if (!result.contains("error"))
            {
                const std::string command = result.at("command");
                const auto &value = result.at("result");
                auto selected = value.value("id", panel->selected);
                if (panel->mode == 0 && value.contains("profiles") && !value.at("profiles").empty())
                    selected = value.at("profiles")[0].at("id");
                try
                {
                    Reload(*panel, selected);
                }
                catch (const std::exception &error)
                {
                    result["refreshError"] = awb::ErrorResult(error);
                }
                const bool profiles_changed = command == "profile_create" || command == "profile_update" ||
                                              command == "profile_import" ||
                                              command == "profile_template_instantiate" ||
                                              command == "profile_archive" || command == "profile_trash" ||
                                              command == "profile_restore" || command == "profile_purge";
                PostMessageW(panel->owner, LibraryChangedMessage, profiles_changed ? 1 : 0, 0);
            }
            Layout(*panel);
            Display(*panel, result);
            Put(*panel, Status,
                result.contains("error")
                    ? "操作未完成；已保留输入。版本冲突时请先复制输入，再刷新重试。"
                    : (result.contains("refreshError")
                           ? "写入已完成，但刷新失败；请核对回执并手动刷新，不要重复提交。"
                           : "操作已完成，请核对实际回执。"));
            return 0;
        }
        if (message == WM_COMMAND && !panel->busy)
        {
            const auto id = LOWORD(wparam);
            if (id >= FirstTab && id < FirstTab + 4)
            {
                panel->mode = id - FirstTab;
                panel->before.clear();
                Reload(*panel);
            }
            else if (id >= Fresh && id <= Next)
                Action(*panel, id);
            else if (id == Listing && HIWORD(wparam) == LBN_SELCHANGE)
            {
                const auto index = SendMessageW(GetDlgItem(window, Listing), LB_GETCURSEL, 0, 0);
                if (index != LB_ERR && static_cast<size_t>(index) < panel->rows.size())
                    Fields(*panel, panel->rows[static_cast<size_t>(index)]);
            }
            else if (id == Scope && HIWORD(wparam) == CBN_SELCHANGE)
            {
                panel->before.clear();
                Reload(*panel);
            }
            return 0;
        }
        if (message == WM_SIZE)
        {
            Layout(*panel);
            return 0;
        }
        if (message == WM_VSCROLL || message == WM_HSCROLL || message == WM_MOUSEWHEEL)
        {
            const bool vertical = message != WM_HSCROLL;
            auto &position = vertical ? panel->scroll_y : panel->scroll_x;
            SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL};
            GetScrollInfo(window, vertical ? SB_VERT : SB_HORZ, &info);
            const auto code = LOWORD(wparam);
            if (message == WM_MOUSEWHEEL)
                position -= GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 72;
            else if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION)
                position = info.nTrackPos;
            else if (code == SB_TOP)
                position = 0;
            else if (code == SB_BOTTOM)
                position = info.nMax;
            else if (code == SB_LINEUP || code == SB_PAGEUP)
                position -= code == SB_LINEUP ? 32 : static_cast<int>(info.nPage);
            else if (code == SB_LINEDOWN || code == SB_PAGEDOWN)
                position += code == SB_LINEDOWN ? 32 : static_cast<int>(info.nPage);
            position = (std::max)(0, position);
            Layout(*panel);
            return 0;
        }
        if (message == WM_DPICHANGED)
        {
            panel->theme.DpiChanged(HIWORD(wparam));
            const auto *bounds = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(*panel);
            return 0;
        }
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT)
            return reinterpret_cast<LRESULT>(panel->theme.ControlColor(
                reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam), message == WM_CTLCOLOREDIT));
        if (message == WM_DRAWITEM &&
            panel->theme.Draw(*reinterpret_cast<DRAWITEMSTRUCT *>(lparam), FirstTab))
            return TRUE;
        if (message == WM_CLOSE)
        {
            if (panel->busy)
                Put(*panel, Status, "当前请求尚未确认，请等待回执后关闭，避免误判执行结果。");
            else
                DestroyWindow(window);
            return 0;
        }
        if (message == WM_NCDESTROY)
        {
            if (panel->worker.joinable())
                panel->worker.join();
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
        Put(*panel, Status, "操作未完成；未自动重试，请核对错误和输入。");
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
// 用途：创建独立原生管理窗口，窗口持有状态但不转移服务所有权；日期：2026-11-05。
void OpenLibraryManager(HWND owner, awb::Service *service, const std::string &selected_profile)
{
    WNDCLASSW kind{};
    kind.hInstance = GetModuleHandleW(nullptr);
    kind.lpszClassName = L"AWB.Library.Panel";
    kind.lpfnWndProc = Procedure;
    kind.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    kind.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassW(&kind) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw awb::Error("LIBRARY_UI_FAILED", "无法注册环境库窗口");
    auto panel = std::make_unique<Panel>();
    panel->service = service;
    panel->owner = owner;
    panel->selected = selected_profile;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const auto window = CreateWindowExW(
        WS_EX_CONTROLPARENT, kind.lpszClassName, L"AI Browser Workbench · 环境库管理",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL, CW_USEDEFAULT, CW_USEDEFAULT,
        (std::min)(1140L, work.right - work.left - 40), (std::min)(940L, work.bottom - work.top - 40), owner,
        nullptr, kind.hInstance, panel.get());
    if (!window)
        throw awb::Error("LIBRARY_UI_FAILED", "无法创建环境库窗口");
    panel->owned = true;
    panel.release();
    ShowWindow(window, SW_SHOW);
}
} // namespace desktop
