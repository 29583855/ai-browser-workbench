#include "flow_editor.hpp"
#include "awb/flow_document.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <cstring>
#include <d2d1.h>
#include <dwrite.h>
#include <set>
#include <windowsx.h>
#include <wrl/client.h>

namespace desktop
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr int CanvasId = 801;
constexpr int NodeListId = 802;
constexpr int PropertyId = 803;
constexpr int OutputId = 804;
constexpr int FlowListId = 805;
constexpr int PortId = 806;
constexpr int FirstButton = 810;
constexpr int ButtonCount = 20;
const wchar_t *ButtonLabels[] = {L"新建",       L"导入",     L"保存草稿", L"另存草稿", L"导出JSON",
                                 L"撤销",       L"重做",     L"复制选区", L"删除选区", L"应用属性",
                                 L"流程属性",   L"校验",     L"发布",     L"设为入口", L"连接到…",
                                 L"新建子流程", L"恢复草稿", L"复制",     L"粘贴",     L"剪切"};
const char *Types[] = {"navigate", "fill",           "click",  "select",    "assertText",  "condition",
                       "loop",     "call",           "end",    "set",       "readText",    "foreach",
                       "upload",   "downloadConfig", "assert", "readTexts", "downloadWait"};
const wchar_t *TypeLabels[] = {L"打开页面",     L"填写输入",    L"点击/断言", L"选择选项", L"文本断言",
                               L"条件分支",     L"有界循环",    L"子流程",    L"结束",     L"变量赋值",
                               L"读取文本",     L"列表循环",    L"上传文件",  L"下载目录", L"页面断言",
                               L"读取文本列表", L"等待下载完成"};

struct Editor
{
    awb::FlowDocument document;
    awb::Service *service = nullptr;
    std::function<void(const awb::Json &)> published;
    HWND window = nullptr, canvas = nullptr, properties = nullptr, output = nullptr, node_list = nullptr;
    HWND flow_list = nullptr, port = nullptr;
    HFONT font = nullptr;
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<IDWriteTextFormat> text_format;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    std::set<std::string> selected;
    std::string connection_source;
    bool flow_properties = false;
    std::wstring path;
    std::wstring recovery_directory, recovery_path;
    std::string recovery_snapshot;
    bool discard_recovery = false;
    float zoom = 1;
    awb::FlowPoint pan{};
    awb::FlowPoint start{}, current{}, delta{};
    bool dragging = false, boxing = false, panning = false, palette_drag = false;
    bool test_mode = false;
    int palette_index = -1;
    double last_paint_ms = 0;
    size_t paint_count = 0;

    // 用途：释放只由编辑器持有的字体，COM对象自动释放；日期：2026-11-04。
    ~Editor()
    {
        if (font)
            DeleteObject(font);
    }
};

// 用途：将失败的图形API转成可见错误，禁止空画布伪装渲染成功；日期：2026-11-04。
void Check(HRESULT result)
{
    if (FAILED(result))
        throw awb::Error("FLOW_RENDER_FAILED", "Direct2D/DirectWrite operation failed");
}
// 用途：创建标准控件并沿用中文字体及原生IME支持；日期：2026-11-04。
HWND Child(Editor &editor, const wchar_t *kind, const wchar_t *label, DWORD style, int id)
{
    auto control = CreateWindowExW(0, kind, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, editor.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (!control)
        throw awb::Error("FLOW_UI_FAILED", "Cannot create editor control");
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(editor.font), FALSE);
    return control;
}
// 用途：读取有限编辑框文本，保留中文输入和换行；日期：2026-11-04。
std::wstring Text(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length > 1024 * 1024)
        throw awb::Error("FLOW_PROPERTY_TOO_LARGE", "Editor input exceeds 1 MiB");
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, &text[0], length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}
// 用途：在结果区显示实际校验或错误，绝不把画布动画当执行成功；日期：2026-11-04。
void Result(Editor &editor, const awb::Json &result)
{
    SetWindowTextW(editor.output, awb::Wide(result.dump(2)).c_str());
}
// 用途：刷新节点或流程属性，不修改JSON中的任何业务字段；日期：2026-11-04。
void Properties(Editor &editor)
{
    awb::Json value;
    std::set<std::string> live;
    for (const auto &step : editor.document.Steps())
        if (step.is_object() && step.contains("id") && step.at("id").is_string())
            live.insert(step.at("id").get<std::string>());
    for (auto it = editor.selected.begin(); it != editor.selected.end();)
        if (!live.count(*it))
            it = editor.selected.erase(it);
        else
            ++it;
    if (editor.flow_properties)
    {
        value = editor.document.Definition();
        value.erase("steps");
        value.erase("subflows");
    }
    else if (!editor.selected.empty())
        value = editor.document.Step(*editor.selected.begin());
    else
        value = awb::Json::object();
    SetWindowTextW(editor.properties, awb::Wide(value.dump(2)).c_str());
    InvalidateRect(editor.canvas, nullptr, FALSE);
}
// 用途：刷新主流程与子流程下拉项，使用真实流程名称；日期：2026-11-04。
void FlowNames(Editor &editor)
{
    SendMessageW(editor.flow_list, CB_RESETCONTENT, 0, 0);
    int index = 0, selected = 0;
    for (const auto &name : editor.document.Flows())
    {
        const auto label = name.empty() ? L"主流程" : awb::Wide(name);
        SendMessageW(editor.flow_list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (name == editor.document.CurrentFlow())
            selected = index;
        ++index;
    }
    SendMessageW(editor.flow_list, CB_SETCURSEL, selected, 0);
}
// 用途：按照DPI布局工具条、节点库、画布和原生属性区；日期：2026-11-04。
void Layout(Editor &editor)
{
    RECT bounds{};
    GetClientRect(editor.window, &bounds);
    const float dpi = static_cast<float>(GetDpiForWindow(editor.window)) / 96;
    const int width = static_cast<int>(bounds.right / dpi), height = static_cast<int>(bounds.bottom / dpi);
    // 用途：将逻辑布局换算成当前窗口像素；日期：2026-11-04。
    const auto place = [dpi](HWND child, int x, int y, int w, int h)
    {
        MoveWindow(child, static_cast<int>(x * dpi), static_cast<int>(y * dpi),
                   std::max(1, static_cast<int>(w * dpi)), std::max(1, static_cast<int>(h * dpi)), TRUE);
    };
    for (int i = 0; i < ButtonCount; ++i)
        place(GetDlgItem(editor.window, FirstButton + i), 8 + (i % 8) * 102, 8 + (i / 8) * 34, 98, 30);
    place(editor.flow_list, 8, 116, 160, 300);
    place(editor.port, 180, 116, 130, 200);
    place(editor.node_list, 8, 152, 160, height - 160);
    place(editor.canvas, 178, 152, width - 506, height - 160);
    place(editor.properties, width - 318, 152, 310, std::max(120, height - 376));
    place(editor.output, width - 318, height - 216, 310, 208);
    if (editor.target)
    {
        RECT canvas{};
        GetClientRect(editor.canvas, &canvas);
        Check(editor.target->Resize(D2D1::SizeU(canvas.right, canvas.bottom)));
        editor.target->SetDpi(dpi * 96, dpi * 96);
    }
    InvalidateRect(editor.canvas, nullptr, FALSE);
}
// 用途：创建可恢复的Direct2D设备资源，显示器变化时按当前DPI绘制；日期：2026-11-04。
void Device(Editor &editor)
{
    if (editor.target)
        return;
    RECT rect{};
    GetClientRect(editor.canvas, &rect);
    Check(editor.factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(editor.canvas, D2D1::SizeU(rect.right, rect.bottom)),
        &editor.target));
    const auto dpi = static_cast<float>(GetDpiForWindow(editor.canvas));
    editor.target->SetDpi(dpi, dpi);
    Check(editor.target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &editor.brush));
}
// 用途：取得拖拽预览坐标，直到松手才提交一次历史命令；日期：2026-11-04。
awb::FlowPoint Position(Editor &editor, const std::string &id)
{
    auto point = editor.document.Position(id);
    if (editor.dragging && editor.selected.count(id))
    {
        point.x += editor.delta.x;
        point.y += editor.delta.y;
    }
    return point;
}
// 用途：绘制节点、端口和结构化连线，渲染失败释放设备资源后重建；日期：2026-11-04。
void Paint(Editor &editor)
{
    const auto started = std::chrono::steady_clock::now();
    PAINTSTRUCT paint{};
    BeginPaint(editor.canvas, &paint);
    try
    {
        Device(editor);
        auto *target = editor.target.Get();
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->Clear(D2D1::ColorF(0xf1f5fa));
        target->SetTransform(D2D1::Matrix3x2F::Scale(editor.zoom, editor.zoom) *
                             D2D1::Matrix3x2F::Translation(editor.pan.x, editor.pan.y));
        for (const auto &step : editor.document.Steps())
        {
            if (!step.is_object() || !step.contains("id") || !step.at("id").is_string())
                continue;
            const auto id = step.at("id").get<std::string>();
            const auto p = Position(editor, id);
            for (const auto *port : {"next", "then", "else", "onError"})
            {
                const auto route = step.value("onError", awb::Json::object());
                const auto destination_value =
                    std::string(port) == "onError"
                        ? (route.is_object() ? route.value("next", awb::Json()) : awb::Json())
                        : step.value(port, awb::Json());
                if (!destination_value.is_string())
                    continue;
                const auto destination = destination_value.get<std::string>();
                bool found = false;
                for (const auto &candidate : editor.document.Steps())
                    if (candidate.is_object() && candidate.value("id", "") == destination)
                        found = true;
                if (!found)
                    continue;
                const auto q = Position(editor, destination);
                editor.brush->SetColor(D2D1::ColorF(std::string(port) == "onError" ? 0xc13659
                                                    : std::string(port) == "else"  ? 0xc1693b
                                                                                   : 0x7183a2));
                target->DrawLine(D2D1::Point2F(p.x + 180, p.y + 32), D2D1::Point2F(q.x, q.y + 32),
                                 editor.brush.Get(), 2);
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(q.x, q.y + 32), 4, 4), editor.brush.Get());
            }
        }
        for (const auto &step : editor.document.Steps())
        {
            if (!step.is_object() || !step.contains("id") || !step.at("id").is_string())
                continue;
            const auto id = step.at("id").get<std::string>();
            const auto p = Position(editor, id);
            const auto rect = D2D1::RoundedRect(D2D1::RectF(p.x, p.y, p.x + 180, p.y + 64), 8, 8);
            editor.brush->SetColor(D2D1::ColorF(0xffffff));
            target->FillRoundedRectangle(rect, editor.brush.Get());
            editor.brush->SetColor(D2D1::ColorF(editor.selected.count(id) ? 0x2463eb : 0xa7b6cd));
            target->DrawRoundedRectangle(rect, editor.brush.Get(), editor.selected.count(id) ? 3.0f : 1.0f);
            const auto label = awb::Wide(step.value("type", "unknown") + "\n" + id.substr(0, 18));
            editor.brush->SetColor(D2D1::ColorF(0x182b49));
            target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), editor.text_format.Get(),
                              D2D1::RectF(p.x + 10, p.y + 8, p.x + 172, p.y + 60), editor.brush.Get());
        }
        if (editor.boxing)
        {
            editor.brush->SetColor(D2D1::ColorF(0x2463eb));
            target->DrawRectangle(D2D1::RectF(std::min(editor.start.x, editor.current.x),
                                              std::min(editor.start.y, editor.current.y),
                                              std::max(editor.start.x, editor.current.x),
                                              std::max(editor.start.y, editor.current.y)),
                                  editor.brush.Get());
        }
        const auto result = target->EndDraw();
        if (result == D2DERR_RECREATE_TARGET)
        {
            editor.brush.Reset();
            editor.target.Reset();
            InvalidateRect(editor.canvas, nullptr, FALSE);
        }
        else
            Check(result);
        ++editor.paint_count;
        editor.last_paint_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    catch (...)
    {
        EndPaint(editor.canvas, &paint);
        throw;
    }
    EndPaint(editor.canvas, &paint);
}
// 用途：读取画布消息坐标并使用同一DPI逆变换；日期：2026-11-04。
awb::FlowPoint Point(Editor &editor, LPARAM packed)
{
    return awb::FlowDocument::World(
        static_cast<float>(GET_X_LPARAM(packed)), static_cast<float>(GET_Y_LPARAM(packed)),
        static_cast<float>(GetDpiForWindow(editor.canvas)), editor.zoom, editor.pan);
}
// 用途：结束拖拽或失去捕获时取消未提交预览，避免粘住鼠标；日期：2026-11-04。
void CancelGesture(Editor &editor)
{
    editor.dragging = editor.boxing = editor.panning = editor.palette_drag = false;
    editor.delta = {};
    InvalidateRect(editor.canvas, nullptr, FALSE);
}
// 用途：处理节点选择、拖拽、框选、平移、连接和画布键盘命令；日期：2026-11-04。
LRESULT CALLBACK CanvasProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        editor = static_cast<Editor *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    if (!editor)
        return DefWindowProcW(window, message, wparam, lparam);
    try
    {
        if (message == WM_PAINT)
        {
            Paint(*editor);
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_GETDLGCODE)
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN)
        {
            SetFocus(window);
            SetCapture(window);
            editor->start = editor->current = Point(*editor, lparam);
            if (message == WM_MBUTTONDOWN)
                editor->panning = true;
            else
            {
                const auto id = editor->document.Hit(editor->start);
                if (!editor->connection_source.empty() && !id.empty())
                {
                    const int port = static_cast<int>(SendMessageW(editor->port, CB_GETCURSEL, 0, 0));
                    editor->document.Connect(editor->connection_source,
                                             port == 1   ? "then"
                                             : port == 2 ? "else"
                                             : port == 3 ? "onError"
                                                         : "next",
                                             id);
                    editor->connection_source.clear();
                }
                else if (!id.empty())
                {
                    if (!(wparam & MK_CONTROL) && !editor->selected.count(id))
                        editor->selected.clear();
                    if ((wparam & MK_CONTROL) && editor->selected.count(id))
                        editor->selected.erase(id);
                    else
                        editor->selected.insert(id);
                    editor->dragging = !editor->document.ReadOnly();
                }
                else
                {
                    if (!(wparam & MK_CONTROL))
                        editor->selected.clear();
                    editor->boxing = true;
                }
                editor->flow_properties = false;
                Properties(*editor);
            }
            return 0;
        }
        if (message == WM_MOUSEMOVE && GetCapture() == window)
        {
            editor->current = Point(*editor, lparam);
            editor->delta = {editor->current.x - editor->start.x, editor->current.y - editor->start.y};
            if (editor->panning)
            {
                editor->pan.x += editor->delta.x * editor->zoom;
                editor->pan.y += editor->delta.y * editor->zoom;
                editor->current = editor->start;
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_LBUTTONUP || message == WM_MBUTTONUP)
        {
            if (editor->dragging)
                editor->document.Move(editor->selected, editor->delta.x, editor->delta.y);
            if (editor->boxing)
                for (const auto &step : editor->document.Steps())
                {
                    const auto id = step.at("id").get<std::string>();
                    const auto p = editor->document.Position(id);
                    if (p.x >= std::min(editor->start.x, editor->current.x) &&
                        p.y >= std::min(editor->start.y, editor->current.y) &&
                        p.x + 180 <= std::max(editor->start.x, editor->current.x) &&
                        p.y + 64 <= std::max(editor->start.y, editor->current.y))
                        editor->selected.insert(id);
                }
            CancelGesture(*editor);
            ReleaseCapture();
            Properties(*editor);
            return 0;
        }
        if (message == WM_CAPTURECHANGED)
            CancelGesture(*editor);
        if (message == WM_MOUSEWHEEL)
        {
            POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window, &screen);
            const auto before = awb::FlowDocument::World(
                static_cast<float>(screen.x), static_cast<float>(screen.y),
                static_cast<float>(GetDpiForWindow(window)), editor->zoom, editor->pan);
            editor->zoom = std::max(
                0.1f, std::min(4.0f, editor->zoom * (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1.1f : 1 / 1.1f)));
            const auto after = awb::FlowDocument::World(
                static_cast<float>(screen.x), static_cast<float>(screen.y),
                static_cast<float>(GetDpiForWindow(window)), editor->zoom, editor->pan);
            editor->pan.x += (after.x - before.x) * editor->zoom;
            editor->pan.y += (after.y - before.y) * editor->zoom;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (message == WM_KEYDOWN)
        {
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wparam == VK_ESCAPE)
            {
                editor->connection_source.clear();
                CancelGesture(*editor);
                ReleaseCapture();
            }
            else if (wparam == VK_DELETE)
            {
                editor->document.Remove(editor->selected);
                editor->selected.clear();
            }
            else if (control && wparam == 'Z')
                editor->document.Undo();
            else if (control && wparam == 'Y')
                editor->document.Redo();
            else if (control && wparam == 'D')
                editor->selected = editor->document.Duplicate(editor->selected);
            else if (control && (wparam == 'C' || wparam == 'V' || wparam == 'X'))
            {
                SendMessageW(editor->window, WM_COMMAND,
                             FirstButton + (wparam == 'C'   ? 17
                                            : wparam == 'V' ? 18
                                                            : 19),
                             0);
                return 0;
            }
            else if (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN)
            {
                if (editor->selected.empty() && !editor->document.Steps().empty())
                    editor->selected.insert(editor->document.Steps().front().at("id"));
                else
                    editor->document.Move(editor->selected,
                                          wparam == VK_LEFT    ? -10.0f
                                          : wparam == VK_RIGHT ? 10.0f
                                                               : 0,
                                          wparam == VK_UP     ? -10.0f
                                          : wparam == VK_DOWN ? 10.0f
                                                              : 0);
            }
            Properties(*editor);
            return 0;
        }
    }
    catch (const std::exception &error)
    {
        CancelGesture(*editor);
        ReleaseCapture();
        Result(*editor, awb::ErrorResult(error));
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
// 用途：使用专用剪贴板格式传递有限节点数据，关闭剪贴板后才修改模型；日期：2026-11-04。
void Clipboard(Editor &editor, bool paste, bool cut = false)
{
    const UINT format = RegisterClipboardFormatW(L"AI.BrowserWorkbench.FlowSelection.1");
    bool opened = false;
    // 用途：仅等待系统剪贴板短暂竞争，尚未修改模型或发布内容；最多等待120毫秒；日期：2026-11-05。
    for (int attempt = 0; format && attempt < 5; ++attempt)
    {
        if (OpenClipboard(editor.window))
        {
            opened = true;
            break;
        }
        if (attempt < 4)
            Sleep(30);
    }
    if (!opened)
        throw awb::Error("FLOW_CLIPBOARD_BUSY",
                         "Cannot open clipboard; retry after the other application closes it");
    try
    {
        if (paste)
        {
            const auto memory = GetClipboardData(format);
            const auto size = memory ? GlobalSize(memory) : 0;
            if (!size || size > 256 * 1024 + 1)
                throw awb::Error("FLOW_CLIPBOARD_INVALID",
                                 "Clipboard has no supported bounded node selection");
            const auto *bytes = static_cast<const char *>(GlobalLock(memory));
            if (!bytes)
                throw awb::Error("FLOW_CLIPBOARD_INVALID", "Cannot read node selection");
            const void *end = std::memchr(bytes, 0, size);
            const std::string data(bytes, end ? static_cast<const char *>(end) - bytes : size);
            GlobalUnlock(memory);
            CloseClipboard();
            editor.selected = editor.document.PasteSelection(awb::ParseExternalJson(data, 32));
            return;
        }
        const auto data = editor.document.CopySelection(editor.selected).dump();
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, data.size() + 1);
        if (!memory)
            throw awb::Error("FLOW_CLIPBOARD_FAILED", "Cannot allocate clipboard memory");
        auto *bytes = static_cast<char *>(GlobalLock(memory));
        if (!bytes)
        {
            GlobalFree(memory);
            throw awb::Error("FLOW_CLIPBOARD_FAILED", "Cannot lock clipboard memory");
        }
        std::memcpy(bytes, data.c_str(), data.size() + 1);
        GlobalUnlock(memory);
        if (!EmptyClipboard() || !SetClipboardData(format, memory))
        {
            GlobalFree(memory);
            throw awb::Error("FLOW_CLIPBOARD_FAILED", "Cannot publish node selection to clipboard");
        }
        CloseClipboard();
        if (cut)
        {
            editor.document.Remove(editor.selected);
            editor.selected.clear();
        }
    }
    catch (...)
    {
        CloseClipboard();
        throw;
    }
}
// 用途：独立保存已应用草稿并限制恢复文件数量，不覆盖手动文件；日期：2026-11-04。
void Autosave(Editor &editor)
{
    if (editor.recovery_path.empty() || editor.discard_recovery)
        return;
    const auto snapshot = editor.document.RecoverySnapshot().dump();
    if (snapshot == editor.recovery_snapshot &&
        GetFileAttributesW(editor.recovery_path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    WIN32_FIND_DATAW found{};
    const auto search = FindFirstFileW((editor.recovery_directory + L"\\*.awbrecovery").c_str(), &found);
    size_t count = 0;
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            ++count;
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    if (count >= 50 && GetFileAttributesW(editor.recovery_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        throw awb::Error("FLOW_RECOVERY_FULL",
                         "50 recovery files retained; save or remove reviewed recovery files first");
    editor.document.SaveRecovery(editor.recovery_path);
    editor.recovery_snapshot = snapshot;
    SetWindowTextW(editor.window, L"可视化流程 · 已自动保存（仅已应用属性，5秒间隔）");
}
// 用途：弹出系统文件选择器，只接受用户明确选定的本地文件；日期：2026-11-04。
std::wstring File(Editor &editor, bool save, bool recovery = false)
{
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = editor.window;
    dialog.lpstrFilter = recovery ? L"恢复文件 (*.awbrecovery)\0*.awbrecovery\0"
                                  : L"流程文件 (*.json;*.awbdraft)\0*.json;*.awbdraft\0所有文件\0*.*\0";
    dialog.lpstrInitialDir =
        recovery && !editor.recovery_directory.empty() ? editor.recovery_directory.c_str() : nullptr;
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    dialog.lpstrDefExt = save ? L"awbdraft" : L"json";
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)))
        return L"";
    return path;
}
// 用途：处理编辑动作，校验和发布始终复用同一执行契约；日期：2026-11-04。
void Command(Editor &editor, int command)
{
    if (command == 0)
    {
        if (!editor.test_mode &&
            MessageBoxW(editor.window, L"新建将替换当前草稿，确认已保存？", L"新建流程", MB_OKCANCEL) != IDOK)
            return;
        editor.document = awb::FlowDocument();
        editor.path.clear();
        editor.selected.clear();
        FlowNames(editor);
    }
    else if (command == 1)
    {
        const auto path = File(editor, false);
        if (path.empty())
            return;
        editor.document.Load(path);
        editor.path = path;
        editor.selected.clear();
        FlowNames(editor);
    }
    else if (command == 2 || command == 3 || command == 4)
    {
        const auto path = command == 2 && !editor.path.empty() ? editor.path : File(editor, true);
        if (path.empty())
            return;
        if (command == 4)
        {
            const auto validation = awb::ValidateWorkflow(editor.document.Definition());
            if (!validation.at("valid").get<bool>())
            {
                Result(editor, validation);
                return;
            }
            awb::AtomicWrite(awb::SafeAbsolute(path), editor.document.Definition().dump(2) + "\n");
        }
        else
        {
            editor.document.Save(path);
            editor.path = path;
        }
        Result(editor, {{"saved", true}, {"executableExport", command == 4}});
    }
    else if (command == 5 || command == 6)
    {
        command == 5 ? editor.document.Undo() : editor.document.Redo();
        editor.selected.clear();
        FlowNames(editor);
    }
    else if (command == 7)
        editor.selected = editor.document.Duplicate(editor.selected);
    else if (command == 8)
    {
        editor.document.Remove(editor.selected);
        editor.selected.clear();
    }
    else if (command == 9)
    {
        const auto value = awb::ParseExternalJson(awb::Utf8(Text(editor.properties)), 32);
        if (editor.flow_properties)
        {
            editor.document.Metadata(value);
        }
        else if (editor.selected.size() == 1)
            editor.document.Update(*editor.selected.begin(), value);
        else
            throw awb::Error("FLOW_SELECTION_REQUIRED", "Select exactly one node before applying properties");
    }
    else if (command == 10)
        editor.flow_properties = true;
    else if (command == 11 || command == 12)
    {
        const auto &definition = editor.document.Definition();
        if (command == 11)
            Result(editor, editor.service
                               ? editor.service->Execute("workflow_validate", {{"definition", definition}})
                               : awb::ValidateWorkflow(definition));
        else
        {
            if (!editor.service)
                throw awb::Error("FLOW_SERVICE_REQUIRED", "Publishing requires the workbench service");
            const auto published = editor.service->Execute("workflow_publish", {{"definition", definition}});
            Result(editor, published);
            if (editor.published)
                editor.published(published);
        }
    }
    else if (command == 13)
    {
        if (editor.selected.size() != 1)
            throw awb::Error("FLOW_SELECTION_REQUIRED", "Select one entry node");
        editor.document.Entry(*editor.selected.begin());
    }
    else if (command == 14)
    {
        if (editor.selected.size() != 1)
            throw awb::Error("FLOW_SELECTION_REQUIRED", "Select source then click destination on canvas");
        editor.connection_source = *editor.selected.begin();
        Result(editor, {{"state", "select-destination"}, {"cancel", "Escape"}});
    }
    else if (command == 15)
    {
        editor.document.AddFlow("flow_" + awb::RandomId().substr(0, 8));
        editor.selected.clear();
        FlowNames(editor);
    }
    else if (command == 16)
    {
        const auto path = File(editor, false, true);
        if (path.empty())
            return;
        editor.document.LoadRecovery(path);
        editor.path.clear();
        editor.selected.clear();
        editor.flow_properties = false;
        FlowNames(editor);
        Result(editor, {{"recovered", true}, {"saveAsRequired", true}, {"source", awb::Utf8(path)}});
    }
    else if (command >= 17 && command <= 19)
        Clipboard(editor, command == 18, command == 19);
    Properties(editor);
}
// 用途：处理节点库的真实拖放手势，在画布松手才插入节点；日期：2026-11-04。
LRESULT CALLBACK PaletteProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                                  DWORD_PTR context)
{
    auto &editor = *reinterpret_cast<Editor *>(context);
    if (message == WM_KEYDOWN && wparam == VK_RETURN)
    {
        SendMessageW(editor.window, WM_COMMAND, MAKEWPARAM(NodeListId, LBN_DBLCLK),
                     reinterpret_cast<LPARAM>(window));
        SetFocus(editor.canvas);
        return 0;
    }
    if (message == WM_LBUTTONDOWN)
    {
        const auto hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, lparam);
        editor.palette_index = HIWORD(hit) ? -1 : LOWORD(hit);
        editor.palette_drag =
            editor.palette_index >= 0 && editor.palette_index < static_cast<int>(_countof(Types));
        if (editor.palette_drag)
        {
            SendMessageW(window, LB_SETCURSEL, editor.palette_index, 0);
            SetFocus(window);
            SetCapture(window);
        }
        return 0;
    }
    if (message == WM_LBUTTONUP && editor.palette_drag)
    {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(window, editor.canvas, &point, 1);
        RECT bounds{};
        GetClientRect(editor.canvas, &bounds);
        try
        {
            if (PtInRect(&bounds, point))
            {
                const auto position = awb::FlowDocument::World(
                    static_cast<float>(point.x), static_cast<float>(point.y),
                    static_cast<float>(GetDpiForWindow(editor.canvas)), editor.zoom, editor.pan);
                editor.selected = {editor.document.Add(Types[editor.palette_index], position)};
                editor.flow_properties = false;
                Properties(editor);
            }
        }
        catch (const std::exception &error)
        {
            Result(editor, awb::ErrorResult(error));
        }
        editor.palette_drag = false;
        ReleaseCapture();
        return 0;
    }
    if (message == WM_CAPTURECHANGED)
        editor.palette_drag = false;
    return DefSubclassProc(window, message, wparam, lparam);
}
// 用途：管理编辑器窗口、标准属性控件、DPI和输入焦点；日期：2026-11-04。
LRESULT CALLBACK EditorProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        editor = static_cast<Editor *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        editor->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    if (!editor)
        return DefWindowProcW(window, message, wparam, lparam);
    try
    {
        if (message == WM_CREATE)
        {
            editor->font = CreateFontW(-MulDiv(14, GetDpiForWindow(window), 96), 0, 0, 0, FW_NORMAL, FALSE,
                                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, editor->factory.GetAddressOf()));
            Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown **>(editor->text_factory.GetAddressOf())));
            Check(editor->text_factory->CreateTextFormat(
                L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14, L"zh-CN", &editor->text_format));
            for (int i = 0; i < ButtonCount; ++i)
                Child(*editor, L"BUTTON", ButtonLabels[i], WS_TABSTOP | BS_PUSHBUTTON, FirstButton + i);
            editor->node_list = Child(*editor, L"LISTBOX", L"",
                                      WS_TABSTOP | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, NodeListId);
            for (const auto *label : TypeLabels)
                SendMessageW(editor->node_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SetWindowSubclass(editor->node_list, PaletteProcedure, 1, reinterpret_cast<DWORD_PTR>(editor));
            editor->properties =
                Child(*editor, L"EDIT", L"",
                      WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                      PropertyId);
            SendMessageW(editor->properties, EM_SETLIMITTEXT, 256 * 1024, 0);
            editor->output =
                Child(*editor, L"EDIT",
                      L"拖入节点；中键平移，滚轮缩放，Ctrl+选取，空白处框选。\r\n节点属性使用DSL1.0 "
                      L"JSON。草稿校验通过后才能导出或发布。",
                      WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY, OutputId);
            editor->flow_list =
                Child(*editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, FlowListId);
            editor->port = Child(*editor, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, PortId);
            for (const auto *label :
                 {L"next 顺序", L"then 成立", L"else 不成立", L"onError 已配置的错误分支"})
                SendMessageW(editor->port, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(editor->port, CB_SETCURSEL, 0, 0);
            editor->canvas = CreateWindowExW(WS_EX_CLIENTEDGE, L"AwbFlowCanvas", L"流程画布",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(CanvasId)),
                                             GetModuleHandleW(nullptr), editor);
            if (!editor->canvas)
                throw awb::Error("FLOW_UI_FAILED", "Cannot create Direct2D canvas");
            FlowNames(*editor);
            Layout(*editor);
            if (!editor->recovery_path.empty() && !SetTimer(window, 1, 5000, nullptr))
                throw awb::Error("FLOW_AUTOSAVE_FAILED", "Cannot start recovery timer");
            return 0;
        }
        if (message == WM_SIZE && wparam != SIZE_MINIMIZED && editor->canvas)
        {
            Layout(*editor);
            return 0;
        }
        if (message == WM_DPICHANGED)
        {
            const auto *bounds = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
                         bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(*editor);
            return 0;
        }
        if (message == WM_GETMINMAXINFO)
        {
            const auto dpi = GetDpiForWindow(window);
            auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
            limits->ptMinTrackSize = {MulDiv(1000, dpi, 96), MulDiv(640, dpi, 96)};
            return 0;
        }
        if (message == WM_TIMER && wparam == 1)
        {
            Autosave(*editor);
            return 0;
        }
        if (message == WM_COMMAND && LOWORD(wparam) >= FirstButton &&
            LOWORD(wparam) < FirstButton + ButtonCount)
        {
            Command(*editor, LOWORD(wparam) - FirstButton);
            return 0;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == FlowListId && HIWORD(wparam) == CBN_SELCHANGE)
        {
            const auto index = static_cast<size_t>(SendMessageW(editor->flow_list, CB_GETCURSEL, 0, 0));
            editor->document.SelectFlow(editor->document.Flows().at(index));
            editor->selected.clear();
            editor->connection_source.clear();
            Properties(*editor);
            return 0;
        }
        if (message == WM_COMMAND && LOWORD(wparam) == NodeListId && HIWORD(wparam) == LBN_DBLCLK)
        {
            const auto index = SendMessageW(editor->node_list, LB_GETCURSEL, 0, 0);
            if (index >= 0 && index < static_cast<int>(_countof(Types)))
                editor->selected = {editor->document.Add(Types[index], {40, 40})};
            editor->flow_properties = false;
            Properties(*editor);
            return 0;
        }
        if (message == WM_CLOSE)
        {
            if (!editor->test_mode &&
                MessageBoxW(window, L"确认已手动保存，或放弃本窗口草稿？关闭将删除本窗口自动恢复文件。",
                            L"流程编辑器", MB_OKCANCEL) != IDOK)
                return 0;
            editor->discard_recovery = true;
        }
        if (message == WM_DESTROY)
        {
            KillTimer(window, 1);
            if (editor->discard_recovery)
                DeleteFileW(editor->recovery_path.c_str());
            else
                Autosave(*editor);
        }
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete editor;
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }
    catch (const std::exception &error)
    {
        if (editor->output)
            Result(*editor, awb::ErrorResult(error));
        if (message == WM_CREATE)
            return -1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
// 用途：注册一次原生窗口类型，不引入Qt或网页宿主依赖；日期：2026-11-04。
void Register()
{
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    type.lpfnWndProc = CanvasProcedure;
    type.lpszClassName = L"AwbFlowCanvas";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw awb::Error("FLOW_UI_FAILED", "Cannot register canvas window");
    type.lpfnWndProc = EditorProcedure;
    type.lpszClassName = L"AwbFlowEditor";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw awb::Error("FLOW_UI_FAILED", "Cannot register editor window");
}
} // namespace

// 用途：在窗口仍可操作时预存全部草稿，任何写入错误均阻止主窗口退出；日期：2026-11-04。
void SaveFlowEditorsForShutdown(HWND owner)
{
    for (auto window = GetWindow(owner, GW_HWNDFIRST); window; window = GetWindow(window, GW_HWNDNEXT))
    {
        wchar_t type[64]{};
        GetClassNameW(window, type, 64);
        if (GetWindow(window, GW_OWNER) != owner || std::wstring(type) != L"AwbFlowEditor")
            continue;
        auto *editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (editor)
            Autosave(*editor);
    }
}

// 用途：创建非模态编辑窗口，与主工作台共享唯一服务实例；日期：2026-11-04。
HWND OpenFlowEditor(HWND owner, awb::Service *service, const awb::Json *definition,
                    const std::function<void(const awb::Json &)> &published,
                    const std::wstring &recovery_directory)
{
    Register();
    auto *editor = new Editor();
    editor->service = service;
    editor->published = published;
    if (definition)
        editor->document.Import(*definition);
    if (!recovery_directory.empty())
    {
        editor->recovery_directory = awb::SafeAbsolute(recovery_directory);
        awb::PrivateDirectory(editor->recovery_directory);
        editor->recovery_path =
            editor->recovery_directory + L"\\" + awb::Wide(awb::RandomId()) + L".awbrecovery";
    }
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, L"AwbFlowEditor", L"可视化流程 · DSL1.0",
                                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                        1220, 820, owner, nullptr, GetModuleHandleW(nullptr), editor);
    if (!window)
        throw awb::Error("FLOW_UI_FAILED", "Cannot open flow editor");
    ShowWindow(window, SW_SHOWNORMAL);
    return window;
}
// 用途：对真实画布消息和渲染性能取证，不把模型单测冒充UI验收；日期：2026-11-04。
int TestFlowEditor(const std::wstring &report_path)
{
    awb::Json report{{"date", "2026-11-04"}, {"result", "failed"}, {"checks", awb::Json::array()}};
    HWND window = nullptr;
    try
    {
        window = OpenFlowEditor(nullptr, nullptr);
        auto &editor = *reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        editor.test_mode = true;
        // 用途：失败即保留报告并阻止任务完成；日期：2026-11-04。
        const auto require = [&report](bool condition, const char *name)
        {
            if (!condition)
                throw awb::Error("FLOW_TEST_FAILED", name);
            report["checks"].push_back(name);
        };
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        const auto canvas = editor.canvas;
        const auto before = editor.document.Definition();
        const auto dpi = static_cast<float>(GetDpiForWindow(canvas));
        // 用途：构造按真实DPI换算的鼠标消息坐标；日期：2026-11-04。
        const auto packed = [dpi](int x, int y)
        { return MAKELPARAM(static_cast<int>(x * dpi / 96), static_cast<int>(y * dpi / 96)); };
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, packed(60, 60));
        SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, packed(100, 100));
        SendMessageW(canvas, WM_LBUTTONUP, 0, packed(100, 100));
        require(editor.document.Position("done").x == 80 && editor.document.Position("done").y == 80,
                "native-mouse-drag-commits-layout");
        require(editor.document.Definition() == before, "layout-keeps-executable-definition-unchanged");
        SendMessageW(canvas, WM_KEYDOWN, VK_RIGHT, 0);
        require(editor.document.Position("done").x == 90, "native-arrow-key-moves-selected-node");
        editor.document.Undo();
        editor.document.Redo();
        require(editor.document.Position("done").x == 90, "undo-redo-restores-layout");
        editor.selected.clear();
        SendMessageW(canvas, WM_LBUTTONDOWN, MK_LBUTTON, packed(70, 60));
        SendMessageW(canvas, WM_MOUSEMOVE, MK_LBUTTON, packed(300, 200));
        SendMessageW(canvas, WM_LBUTTONUP, 0, packed(300, 200));
        require(editor.selected.count("done") == 1, "native-box-selection");
        RECT palette_rect{};
        SendMessageW(editor.node_list, LB_GETITEMRECT, 8, reinterpret_cast<LPARAM>(&palette_rect));
        SendMessageW(editor.node_list, WM_LBUTTONDOWN, MK_LBUTTON,
                     MAKELPARAM(10, (palette_rect.top + palette_rect.bottom) / 2));
        POINT drop{static_cast<LONG>(350 * dpi / 96), static_cast<LONG>(200 * dpi / 96)};
        MapWindowPoints(canvas, editor.node_list, &drop, 1);
        SendMessageW(editor.node_list, WM_LBUTTONUP, 0, MAKELPARAM(drop.x, drop.y));
        require(editor.document.Steps().size() == 2, "native-palette-drag-drop");
        editor.document.Undo();
        editor.selected.clear();
        POINT wheel{static_cast<LONG>(200 * dpi / 96), static_cast<LONG>(150 * dpi / 96)};
        ClientToScreen(canvas, &wheel);
        SendMessageW(canvas, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(wheel.x, wheel.y));
        require(editor.zoom > 1, "native-mouse-wheel-zoom");
        editor.zoom = 1;
        editor.pan = {};
        SendMessageW(canvas, WM_MBUTTONDOWN, MK_MBUTTON, packed(300, 200));
        SendMessageW(canvas, WM_MOUSEMOVE, MK_MBUTTON, packed(330, 240));
        SendMessageW(canvas, WM_MBUTTONUP, 0, packed(330, 240));
        require(std::abs(editor.pan.x - 30) < 0.1f && std::abs(editor.pan.y - 40) < 0.1f,
                "native-middle-button-pan");
        editor.pan = {};
        const std::wstring chinese = L"{\"name\":\"中文输入与属性\"}";
        SetWindowTextW(editor.properties, chinese.c_str());
        require(Text(editor.properties) == chinese, "unicode-native-property-edit-roundtrip");
        SendMessageW(editor.node_list, LB_SETCURSEL, 8, 0);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(NodeListId, LBN_DBLCLK),
                     reinterpret_cast<LPARAM>(editor.node_list));
        require(editor.document.Steps().size() == 2, "native-palette-inserts-node");
        const auto id = editor.document.Steps().back().at("id").get<std::string>();
        editor.document.Update(
            id, {{"id", id}, {"type", "navigate"}, {"url", "http://127.0.0.1:3000/"}, {"next", "done"}});
        editor.document.Connect(id, "next", id);
        const bool rejected = !awb::ValidateWorkflow(editor.document.Definition()).at("valid").get<bool>();
        require(rejected, "invalid-cycle-cannot-pass-shared-validator");
        awb::Json definition = before;
        definition["steps"] = awb::Json::array();
        for (int i = 0; i < 200; ++i)
            definition["steps"].push_back({{"id", "p" + std::to_string(i)}, {"type", "end"}});
        const auto load_start = std::chrono::steady_clock::now();
        editor.document.Import(definition);
        const auto load_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start).count();
        report["load200NodesMs"] = load_ms;
        require(load_ms < 2000, "200-node-draft-opens-within-two-seconds");
        report["paintSamplesMs"] = awb::Json::array();
        std::vector<double> times;
        for (const auto sample_dpi : {96.0f, 144.0f})
        {
            Device(editor);
            editor.target->SetDpi(sample_dpi, sample_dpi);
            for (int i = 0; i < 30; ++i)
            {
                InvalidateRect(canvas, nullptr, FALSE);
                UpdateWindow(canvas);
                times.push_back(editor.last_paint_ms);
                report["paintSamplesMs"].push_back({{"dpi", sample_dpi}, {"ms", editor.last_paint_ms}});
            }
            for (const auto zoom : {0.5f, 1.0f, 2.0f})
            {
                const auto p = awb::FlowDocument::World(60 * zoom * sample_dpi / 96,
                                                        60 * zoom * sample_dpi / 96, sample_dpi, zoom, {});
                require(editor.document.Hit(p) == "p0", "dpi-and-zoom-hit-test");
            }
        }
        std::sort(times.begin(), times.end());
        report["paintP95Ms"] = times.at(static_cast<size_t>(times.size() * 0.95));
        report["actualWindowDpi"] = dpi;
        report["paintCount"] = editor.paint_count;
        report["limits"] =
            "Synthetic 100/150% render targets; real second monitor and physical IME remain unverified";
        require(report.at("paintP95Ms").get<double>() <= 50, "200-node-paint-p95-within-50ms");
        report["result"] = "passed";
    }
    catch (const std::exception &error)
    {
        report["error"] = awb::ErrorResult(error);
    }
    if (window)
        DestroyWindow(window);
    awb::AtomicWrite(awb::SafeAbsolute(report_path), report.dump(2) + "\n");
    return report.at("result") == "passed" ? 0 : 1;
}
} // namespace desktop
