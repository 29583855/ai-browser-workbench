#pragma once
#include <algorithm>
#include <commctrl.h>
#include <set>
#include <string>
#include <vector>
#include <windows.h>

namespace desktop
{
constexpr int FirstGroupButton = 501;
constexpr int GroupCount = 4;
constexpr COLORREF Canvas = RGB(244, 247, 251);
constexpr COLORREF Surface = RGB(255, 255, 255);
constexpr COLORREF Ink = RGB(27, 40, 65);
constexpr COLORREF Muted = RGB(100, 116, 139);
constexpr COLORREF Accent = RGB(37, 99, 235);
constexpr COLORREF Line = RGB(219, 227, 238);
constexpr COLORREF Tint = RGB(235, 242, 255);

class Theme
{
  public:
    HWND window = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND count = nullptr;
    HWND library_label = nullptr;
    HWND status = nullptr;
    HWND selector_label = nullptr;
    HWND value_label = nullptr;
    HWND page_label = nullptr;
    HWND hint = nullptr;
    HWND result_label = nullptr;
    HWND result_hint = nullptr;
    HWND name = nullptr;
    HWND url = nullptr;
    HWND selector = nullptr;
    HWND value = nullptr;
    HWND pages = nullptr;
    HWND output = nullptr;
    HWND profiles = nullptr;
    int group = 0;

    // 用途：释放主题自有GDI资源，系统颜色画刷不由本对象释放；日期：2026-11-04。
    ~Theme()
    {
        ReleaseFonts();
        if (surface_)
            DeleteObject(surface_);
        if (canvas_)
            DeleteObject(canvas_);
    }

    // 用途：初始化实际窗口DPI和高对比度配色，不改变系统设置；日期：2026-11-04。
    void Initialize(HWND target)
    {
        window = target;
        dpi_ = GetDpiForWindow(window);
        HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW), 0, nullptr};
        high_contrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                         (contrast.dwFlags & HCF_HIGHCONTRASTON);
        surface_ = CreateSolidBrush(Color(Surface, COLOR_WINDOW));
        canvas_ = CreateSolidBrush(Color(Canvas, COLOR_WINDOW));
        CreateFonts();
    }

    // 用途：将设计尺寸按当前窗口DPI换算为物理像素；日期：2026-11-04。
    int Scale(int design_units) const
    {
        return MulDiv(design_units, static_cast<int>(dpi_), 96);
    }

    // 用途：为新增原生控件设置字体、键盘顺序和按钮悬停绘制；日期：2026-11-04。
    void Style(HWND control)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(body_), FALSE);
        wchar_t kind[32]{};
        GetClassNameW(control, kind, 32);
        if (std::wstring(kind) == L"Button")
            SetWindowSubclass(control, ButtonProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    // 用途：根据真实命令状态更新提示和忙碌按钮，撤销和停止始终可用；日期：2026-11-04。
    void Activity(int next_status)
    {
        activity_ = next_status;
        SetWindowTextW(status, next_status == 1   ? L"正在执行"
                               : next_status == 2 ? L"操作完成"
                               : next_status == 3 ? L"操作未完成"
                                                  : L"准备就绪");
        for (int id = 101; id <= 138; ++id)
            EnableWindow(GetDlgItem(window, id), next_status != 1 || id == 103 || id == 106);
        InvalidateRect(status, nullptr, TRUE);
    }

    // 用途：显示真实环境总数，不将本地目录数量冒充中央免费额度；日期：2026-11-04。
    void EnvironmentCount(size_t total)
    {
        const auto label = std::to_wstring(total) + L" 个环境";
        SetWindowTextW(count, label.c_str());
    }

    // 用途：重新创建DPI字体并将其应用到现有原生子控件；日期：2026-11-04。
    void DpiChanged(UINT dpi)
    {
        dpi_ = dpi;
        ReleaseFonts();
        CreateFonts();
        // 用途：仅更新本窗口子控件字体，不读取或修改业务数据；日期：2026-11-04。
        EnumChildWindows(
            window,
            [](HWND child, LPARAM context) -> BOOL
            {
                auto *theme = reinterpret_cast<Theme *>(context);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(theme->body_), FALSE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(this));
        SendMessageW(profiles, LB_SETITEMHEIGHT, 0, Scale(58));
        Layout();
    }

    // 用途：为较小窗口保留可滚动内容区，缩放和切组均不改变输入值；日期：2026-11-04。
    void Layout()
    {
        if (laying_out_ || !output)
            return;
        laying_out_ = true;
        RECT client{};
        GetClientRect(window, &client);
        const int visible_width = MulDiv(client.right, 96, static_cast<int>(dpi_));
        const int visible_height = MulDiv(client.bottom, 96, static_cast<int>(dpi_));
        width_ = std::max(1040, visible_width);
        height_ = std::max(780, visible_height);
        UpdateScroll(SB_HORZ, width_, visible_width, scroll_x_);
        UpdateScroll(SB_VERT, height_, visible_height, scroll_y_);
        const int x = 260;
        const int width = width_ - x - 28;
        Place(title, x, 24, width - 200, 34);
        Place(subtitle, x, 64, width - 200, 22);
        Place(status, width_ - 166, 34, 132, 28);
        Place(count, 24, 119, 190, 24);
        Place(library_label, 24, 92, 170, 24);
        Place(profiles, 16, 160, 208, height_ - 223);
        Place(GetDlgItem(window, 601), x + 20, 121, 180, 24);
        const int form_x = x + 20;
        Place(GetDlgItem(window, 602), form_x, 163, 70, 26);
        Place(name, form_x + 76, 158, width - 616, 34);
        int button_x = x + width - 500;
        const int top_buttons[] = {101, 102, 103, 106, 111};
        for (int id : top_buttons)
        {
            Place(GetDlgItem(window, id), button_x, 158, 88, 34);
            button_x += 96;
        }
        Place(GetDlgItem(window, 603), form_x, 213, 70, 26);
        Place(url, form_x + 76, 208, width - 240, 34);
        Place(GetDlgItem(window, 104), x + width - 130, 208, 110, 34);
        for (int index = 0; index < GroupCount; ++index)
            Place(GetDlgItem(window, FirstGroupButton + index), x + index * 136, 282, 126, 38);
        const bool inputs = group == 0 || group == 2;
        Show(selector_label, inputs);
        Show(value_label, inputs);
        Show(selector, inputs);
        Show(value, inputs);
        SetWindowTextW(selector_label, group == 2 ? L"JSON 文件" : L"CSS 选择器");
        SetWindowTextW(value_label, group == 2 ? L"流程参数" : L"填写 / 选项值");
        Place(selector_label, form_x, 359, 100, 26);
        Place(selector, form_x + 112, 354, width - 152, 34);
        Place(value_label, form_x, 407, 100, 26);
        Place(value, form_x + 112, 402, width - 152, 34);
        Show(page_label, group == 1);
        Show(pages, group == 1);
        Place(page_label, form_x, 359, 80, 26);
        Place(pages, form_x + 88, 354, width - 264, 220);
        for (int id = 105; id <= 138; ++id)
            if (id != 106 && id != 111)
                Show(GetDlgItem(window, id), false);
        const std::vector<std::vector<int>> groups = {{107, 108, 109, 110, 105, 123, 124, 125, 126},
                                                      {117, 118, 119, 120, 121, 122, 134, 135, 136},
                                                      {127, 128, 129, 130, 131, 132, 133, 137, 138},
                                                      {112, 113, 114, 115, 116}};
        const auto &buttons = groups[static_cast<size_t>(group)];
        const int gap = 10;
        const int cell = (width - 40 - gap * 4) / 5;
        size_t ordinal = 0;
        for (int id : buttons)
        {
            Show(GetDlgItem(window, id), true);
            if (group == 1 && id == 117)
                Place(GetDlgItem(window, id), x + width - 148, 354, 128, 34);
            else
            {
                const int start = group == 1 ? 410 : group == 3 ? 358 : 454;
                Place(GetDlgItem(window, id), form_x + static_cast<int>(ordinal % 5) * (cell + gap),
                      start + static_cast<int>(ordinal / 5) * 48, cell, 36);
                ++ordinal;
            }
        }
        const wchar_t *hints[] = {
            L"先定位或探查元素，再填写、点击并读取结果。填写值提交后自动清空。",
            L"地址框用于新页面；选择页面后点“切换”。需要排查问题时，先开始诊断。",
            L"上方输入 JSON 文件完整路径；流程参数使用 JSON。提取和批次共用文件框。",
            L"先停止环境，再保存名称、归档或恢复。永久删除需先移入回收站，并再次确认。"};
        SetWindowTextW(hint, hints[group]);
        Place(hint, form_x, group == 3 ? 422 : 550, width - 40, group == 3 ? 90 : 36);
        Place(result_label, form_x, 624, 180, 26);
        Place(result_hint, x + width - 270, 627, 250, 22);
        Place(output, form_x, 662, width - 40, height_ - 690);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), FALSE);
        SendMessageW(output, WM_SETFONT, reinterpret_cast<WPARAM>(code_), FALSE);
        SendMessageW(profiles, LB_SETITEMHEIGHT, 0, Scale(58));
        InvalidateRect(window, nullptr, TRUE);
        laying_out_ = false;
    }

    // 用途：处理窗口滚动条和鼠标滚轮，滚动只影响界面布局；日期：2026-11-04。
    void Scroll(int bar, int operation, int wheel = 0)
    {
        int &position = bar == SB_VERT ? scroll_y_ : scroll_x_;
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL, 0, 0, 0, 0, 0};
        GetScrollInfo(window, bar, &info);
        if (wheel)
            position -= wheel;
        else if (operation == SB_LINEUP)
            position -= 32;
        else if (operation == SB_LINEDOWN)
            position += 32;
        else if (operation == SB_PAGEUP)
            position -= static_cast<int>(info.nPage);
        else if (operation == SB_PAGEDOWN)
            position += static_cast<int>(info.nPage);
        else if (operation == SB_THUMBTRACK || operation == SB_THUMBPOSITION)
            position = info.nTrackPos;
        else if (operation == SB_TOP)
            position = 0;
        else if (operation == SB_BOTTOM)
            position = info.nMax;
        Layout();
    }

    // 用途：绘制工作区背景、环境侧栏和分组卡片，控件仍为真实Win32控件；日期：2026-11-04。
    void Paint()
    {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, canvas_);
        RECT sidebar{0, 0, Scale(240) - Scale(scroll_x_), client.bottom};
        FillRect(dc, &sidebar, surface_);
        Card(dc, 20, 24, 42, 42, Color(Accent, COLOR_HIGHLIGHT), Color(Accent, COLOR_HIGHLIGHT));
        Text(dc, L"W", 22, 26, 38, 36, heading_, Color(Surface, COLOR_HIGHLIGHTTEXT), DT_CENTER);
        Text(dc, L"WEB WORKBENCH", 75, 27, 150, 24, strong_, Color(Ink, COLOR_WINDOWTEXT));
        Text(dc, L"浏览器自动化工作台", 75, 54, 150, 22, small_, Color(Muted, COLOR_WINDOWTEXT));
        Text(dc, L"选择环境，开始工作", 24, height_ - 42, 190, 22, small_, Color(Muted, COLOR_WINDOWTEXT));
        const int width = width_ - 288;
        Card(dc, 260, 104, width, 158, Color(Surface, COLOR_WINDOW), Color(Line, COLOR_WINDOWFRAME));
        Card(dc, 260, 336, width, 260, Color(Surface, COLOR_WINDOW), Color(Line, COLOR_WINDOWFRAME));
        Card(dc, 260, 608, width, height_ - 636, Color(Surface, COLOR_WINDOW),
             Color(Line, COLOR_WINDOWFRAME));
        EndPaint(window, &paint);
    }

    // 用途：为标签和编辑框提供一致颜色，并保留高对比度系统颜色；日期：2026-11-04。
    HBRUSH ControlColor(HDC dc, HWND control, bool edit) const
    {
        SetBkMode(dc, TRANSPARENT);
        COLORREF color = Color(edit || control == output ? Ink : Muted, COLOR_WINDOWTEXT);
        if (control == title || control == result_label || control == library_label ||
            GetDlgCtrlID(control) == 601)
            color = Color(Ink, COLOR_WINDOWTEXT);
        if (control == status)
            color = Color(activity_ == 3   ? RGB(185, 28, 28)
                          : activity_ == 1 ? Accent
                                           : RGB(21, 128, 93),
                          COLOR_WINDOWTEXT);
        SetTextColor(dc, color);
        SetBkColor(dc, Color(Surface, COLOR_WINDOW));
        if (control == title || control == subtitle || control == status)
        {
            SetBkColor(dc, Color(Canvas, COLOR_WINDOW));
            return canvas_;
        }
        return surface_;
    }

    // 用途：绘制标准按钮和环境列表，保持可访问名称、焦点和系统按钮行为；日期：2026-11-04。
    bool Draw(const DRAWITEMSTRUCT &item, int first_tab = FirstGroupButton)
    {
        const auto dc = item.hDC;
        const int saved = SaveDC(dc);
        SetBkMode(dc, TRANSPARENT);
        RECT bounds = item.rcItem;
        if (item.CtlType == ODT_BUTTON)
        {
            const int id = static_cast<int>(item.CtlID);
            const bool tab = id >= first_tab && id < first_tab + GroupCount;
            const bool selected = tab && id - first_tab == group;
            const bool primary = id == 101 || id == 104 || id == 109 || id == 129;
            const bool danger = id == 106 || id == 116 || id == 133;
            const bool disabled = (item.itemState & ODS_DISABLED) != 0;
            COLORREF fill = selected ? Tint : primary ? Accent : Surface;
            COLORREF foreground = selected ? Accent : primary ? Surface : danger ? RGB(185, 28, 28) : Ink;
            if (!disabled && ((item.itemState & ODS_SELECTED) || hovered_.count(item.hwndItem)))
                fill = primary ? RGB(29, 78, 216) : RGB(237, 242, 249);
            if (disabled)
            {
                fill = Canvas;
                foreground = Muted;
            }
            const auto brush =
                CreateSolidBrush(Color(fill, selected || primary ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
            const auto pen = CreatePen(PS_SOLID, 1, Color(tab || primary ? fill : Line, COLOR_WINDOWFRAME));
            const auto previous_brush = SelectObject(dc, brush);
            const auto previous_pen = SelectObject(dc, pen);
            RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, Scale(10), Scale(10));
            SelectObject(dc, previous_pen);
            SelectObject(dc, previous_brush);
            DeleteObject(pen);
            DeleteObject(brush);
            wchar_t label[128]{};
            GetWindowTextW(item.hwndItem, label, 128);
            SetTextColor(dc, Color(foreground, selected || primary ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT));
            SelectObject(dc, selected || primary ? strong_ : body_);
            DrawTextW(dc, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (item.itemState & ODS_FOCUS)
            {
                InflateRect(&bounds, -Scale(4), -Scale(4));
                DrawFocusRect(dc, &bounds);
            }
        }
        else if (item.CtlType == ODT_LISTBOX && item.itemID != static_cast<UINT>(-1))
        {
            const bool selected = (item.itemState & ODS_SELECTED) != 0;
            const auto brush =
                CreateSolidBrush(Color(selected ? Tint : Surface, selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
            FillRect(dc, &bounds, brush);
            DeleteObject(brush);
            const auto length = SendMessageW(item.hwndItem, LB_GETTEXTLEN, item.itemID, 0);
            if (length != LB_ERR && length < 4096)
            {
                std::wstring label(static_cast<size_t>(length) + 1, L'\0');
                SendMessageW(item.hwndItem, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(&label[0]));
                label.resize(static_cast<size_t>(length));
                const auto split = label.rfind(L"  [");
                RECT name_bounds{bounds.left + Scale(12), bounds.top + Scale(8), bounds.right - Scale(8),
                                 bounds.top + Scale(30)};
                RECT meta_bounds{name_bounds.left, bounds.top + Scale(33), name_bounds.right,
                                 bounds.bottom - Scale(4)};
                SelectObject(dc, strong_);
                SetTextColor(
                    dc, Color(selected ? Accent : Ink, selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
                const auto name_text = label.substr(0, split);
                DrawTextW(dc, name_text.c_str(), -1, &name_bounds,
                          DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (split != std::wstring::npos)
                {
                    SelectObject(dc, small_);
                    SetTextColor(dc, Color(Muted, selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
                    DrawTextW(dc, label.c_str() + split + 2, -1, &meta_bounds,
                              DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
                if (item.itemState & ODS_FOCUS)
                    DrawFocusRect(dc, &bounds);
            }
        }
        else
        {
            RestoreDC(dc, saved);
            return false;
        }
        RestoreDC(dc, saved);
        return true;
    }

  private:
    UINT dpi_ = 96;
    HFONT body_ = nullptr;
    HFONT strong_ = nullptr;
    HFONT heading_ = nullptr;
    HFONT small_ = nullptr;
    HFONT code_ = nullptr;
    HBRUSH surface_ = nullptr;
    HBRUSH canvas_ = nullptr;
    bool high_contrast_ = false;
    bool laying_out_ = false;
    int width_ = 1240;
    int height_ = 860;
    int scroll_x_ = 0;
    int scroll_y_ = 0;
    int activity_ = 0;
    std::set<HWND> hovered_;

    // 用途：高对比度模式下使用系统颜色，普通模式使用工作台配色；日期：2026-11-04。
    COLORREF Color(COLORREF normal, int system) const
    {
        return high_contrast_ ? GetSysColor(system) : normal;
    }

    // 用途：释放并清空自有字体句柄，避免切换DPI时泄漏；日期：2026-11-04。
    void ReleaseFonts()
    {
        for (auto *font : {&body_, &strong_, &heading_, &small_, &code_})
        {
            if (*font)
                DeleteObject(*font);
            *font = nullptr;
        }
    }

    // 用途：创建中文界面字体与结果等宽字体，缺少字体时由Windows回退；日期：2026-11-04。
    void CreateFonts()
    {
        body_ = Font(14, FW_NORMAL);
        strong_ = Font(14, FW_SEMIBOLD);
        heading_ = Font(25, FW_SEMIBOLD);
        small_ = Font(12, FW_NORMAL);
        code_ = Font(13, FW_NORMAL, L"Consolas");
    }

    // 用途：以当前DPI创建有中文回退能力的字体；日期：2026-11-04。
    HFONT Font(int size, int weight, const wchar_t *face = L"Microsoft YaHei UI") const
    {
        return CreateFontW(-Scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    }

    // 用途：移动原生子控件并保持滚动和DPI换算一致；日期：2026-11-04。
    void Place(HWND control, int x, int y, int width, int height)
    {
        MoveWindow(control, Scale(x - scroll_x_), Scale(y - scroll_y_), Scale(width), Scale(height), TRUE);
    }

    // 用途：切换分组控件可见性，不销毁控件或清空输入；日期：2026-11-04。
    void Show(HWND control, bool visible)
    {
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    }

    // 用途：为内容范围设置标准滚动条并限制滚动位置；日期：2026-11-04。
    void UpdateScroll(int bar, int total, int visible, int &position)
    {
        position = std::max(0, std::min(position, total - visible));
        SCROLLINFO info{sizeof(SCROLLINFO),
                        SIF_RANGE | SIF_PAGE | SIF_POS,
                        0,
                        total - 1,
                        static_cast<UINT>(visible),
                        position,
                        0};
        SetScrollInfo(window, bar, &info, TRUE);
    }

    // 用途：绘制主题卡片，绘制后恢复并释放临时GDI对象；日期：2026-11-04。
    void Card(HDC dc, int x, int y, int width, int height, COLORREF fill, COLORREF border)
    {
        const auto brush = CreateSolidBrush(fill);
        const auto pen = CreatePen(PS_SOLID, 1, border);
        const auto previous_brush = SelectObject(dc, brush);
        const auto previous_pen = SelectObject(dc, pen);
        RoundRect(dc, Scale(x - scroll_x_), Scale(y - scroll_y_), Scale(x + width - scroll_x_),
                  Scale(y + height - scroll_y_), Scale(16), Scale(16));
        SelectObject(dc, previous_pen);
        SelectObject(dc, previous_brush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    // 用途：绘制不参与业务交互的品牌和侧栏说明文字；日期：2026-11-04。
    void Text(HDC dc, const wchar_t *text, int x, int y, int width, int height, HFONT font, COLORREF color,
              UINT alignment = DT_LEFT)
    {
        RECT bounds{Scale(x - scroll_x_), Scale(y - scroll_y_), Scale(x + width - scroll_x_),
                    Scale(y + height - scroll_y_)};
        const auto previous = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text, -1, &bounds,
                  alignment | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, previous);
    }

    // 用途：仅跟踪标准按钮悬停与焦点，输入仍交由原生按钮处理；日期：2026-11-04。
    static LRESULT CALLBACK ButtonProcedure(HWND control, UINT message, WPARAM wparam, LPARAM lparam,
                                            UINT_PTR subclass, DWORD_PTR context)
    {
        auto *theme = reinterpret_cast<Theme *>(context);
        if (message == WM_MOUSEMOVE && !theme->hovered_.count(control))
        {
            theme->hovered_.insert(control);
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, control, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(control, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE || message == WM_NCDESTROY)
        {
            theme->hovered_.erase(control);
            InvalidateRect(control, nullptr, FALSE);
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
            InvalidateRect(control, nullptr, FALSE);
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(control, ButtonProcedure, subclass);
        return DefSubclassProc(control, message, wparam, lparam);
    }
};
} // namespace desktop
