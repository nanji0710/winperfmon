// SPDX-License-Identifier: MIT
#include "ui/hud_window.h"

#include <algorithm>
#include <cstdio>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

namespace wpm::ui {
namespace {

constexpr wchar_t kClassName[] = L"winperfmon.HudWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr int kPaddingDp = 14;
constexpr int kTitleGapDp = 6;
constexpr int kColumnGapDp = 16;
constexpr int kMinWidthDp = 240;
constexpr int kMaxWidthDp = 520;

COLORREF ToColorRef(unsigned argb) {
    return RGB(static_cast<BYTE>((argb >> 16) & 0xFF), static_cast<BYTE>((argb >> 8) & 0xFF),
               static_cast<BYTE>(argb & 0xFF));
}

/// DWM 圆角偏好（Win11 22000+）。动态取，避免为一个小圆角去链接 dwmapi。
void TryRoundCorners(HWND hwnd, int preference) {
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) return;
    auto fn = reinterpret_cast<DwmSetWindowAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (fn != nullptr) {
        const DWORD attr = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
        fn(hwnd, attr, &preference, sizeof(preference));
    }
    FreeLibrary(dwm);
}

}  // namespace

void HudWindow::EnablePerMonitorDpi() {
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return;
    auto fn = reinterpret_cast<SetCtxFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (fn != nullptr) {
        const HANDLE kPerMonitorV2 = reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4));
        fn(kPerMonitorV2);
    }
}

HudWindow::HudWindow() = default;

HudWindow::~HudWindow() {
    if (fontLabel_ != nullptr) DeleteObject(fontLabel_);
    if (fontValue_ != nullptr) DeleteObject(fontValue_);
    if (fontSmall_ != nullptr) DeleteObject(fontSmall_);
}

int HudWindow::Scale(int dp) const {
    return MulDiv(dp, static_cast<int>(dpi_), 96);
}

void HudWindow::RebuildFonts() {
    if (fontLabel_ != nullptr) DeleteObject(fontLabel_);
    if (fontValue_ != nullptr) DeleteObject(fontValue_);
    if (fontSmall_ != nullptr) DeleteObject(fontSmall_);
    const int size = Scale(style_.baseSizeDp);
    // 用系统 UI 字体族：中文标签与英文数值都能正确显示，且不引入字体依赖。
    fontLabel_ = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    fontValue_ = CreateFontW(-size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    fontSmall_ = CreateFontW(-std::max(Scale(style_.baseSizeDp - 2), Scale(10)), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    lineHeight_ = static_cast<int>(size * 1.6);
}

bool HudWindow::Create(const HudStyle& style, std::string* error) {
    style_ = style;
    topmost_ = style.topmost;

    const wchar_t* kProbeClass = kClassName;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HudWindow::WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kProbeClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error != nullptr) *error = "注册窗口类失败（GetLastError=" + std::to_string(GetLastError()) + "）";
        return false;
    }

    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (topmost_) exStyle |= WS_EX_TOPMOST;
    // WS_POPUP + 无边缘：HUD 的形态。拖动由 WM_LBUTTONDOWN 自己处理
    // （不返回 HTCAPTION，这样右键还能收到自己的菜单消息）。
    hwnd_ = CreateWindowExW(exStyle, kProbeClass, L"winperfmon", WS_POPUP, 0, 0, 320, 200,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        if (error != nullptr) *error = "创建窗口失败（GetLastError=" + std::to_string(GetLastError()) + "）";
        return false;
    }

    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (fn != nullptr) {
            const UINT d = fn(hwnd_);
            if (d >= 72 && d <= 480) dpi_ = d;
        }
    }
    RebuildFonts();

    const BYTE alpha = static_cast<BYTE>(
        std::clamp(static_cast<int>(style_.opacity * 255.0f + 0.5f), 40, 255));
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
    TryRoundCorners(hwnd_, 2);  // DWMWCP_ROUND

    Refresh();          // 先采一次，拿到行数才好定尺寸
    ApplyPosition();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, kTimerId, intervalMs_, nullptr);
    return true;
}

void HudWindow::SetIntervalMs(unsigned ms) {
    intervalMs_ = std::clamp(ms, 100u, 10000u);
    if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kTimerId);
        SetTimer(hwnd_, kTimerId, intervalMs_, nullptr);
    }
}

void HudWindow::SetTopmost(bool on) {
    topmost_ = on;
    if (hwnd_ == nullptr) return;
    SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

int HudWindow::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK HudWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HudWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<HudWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HudWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->WndProc(hwnd, msg, wp, lp);
}

void HudWindow::ApplyPosition() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int margin = Scale(style_.marginDp);
    const int w = panelWidth_;
    const int h = panelHeight_;
    int x = work.left + margin;
    int y = work.top + margin;
    if (style_.corner == 1 || style_.corner == 3) x = work.right - margin - w;
    if (style_.corner == 2 || style_.corner == 3) y = work.bottom - margin - h;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void HudWindow::Refresh() {
    if (builder_) model_ = builder_();

    HDC screen = GetDC(hwnd_);
    HDC dc = CreateCompatibleDC(screen);
    if (fontLabel_ != nullptr) SelectObject(dc, fontLabel_);

    // 量出标签列宽与数值列宽，再算面板尺寸
    int labelMax = 0;
    int valueMax = 0;
    for (const auto& row : model_.rows) {
        SIZE s{};
        SelectObject(dc, fontLabel_);
        if (GetTextExtentPoint32W(dc, row.label.c_str(), static_cast<int>(row.label.size()), &s))
            labelMax = std::max(labelMax, static_cast<int>(s.cx));
        SelectObject(dc, fontValue_);
        if (GetTextExtentPoint32W(dc, row.value.c_str(), static_cast<int>(row.value.size()), &s))
            valueMax = std::max(valueMax, static_cast<int>(s.cx));
        if (style_.showUnits && !row.unit.empty()) {
            SelectObject(dc, fontLabel_);
            if (GetTextExtentPoint32W(dc, row.unit.c_str(), static_cast<int>(row.unit.size()), &s))
                valueMax = std::max(valueMax, static_cast<int>(s.cx));
        }
    }
    // 标题/副标题/页脚也要量：长 CPU 名与统计行不该被截断（宁可面板宽一点）
    SIZE s{};
    SelectObject(dc, fontValue_);
    if (GetTextExtentPoint32W(dc, model_.title.c_str(), static_cast<int>(model_.title.size()), &s))
        valueMax = std::max(valueMax, static_cast<int>(s.cx));
    if (!model_.subtitle.empty()) {
        SelectObject(dc, fontSmall_);
        if (GetTextExtentPoint32W(dc, model_.subtitle.c_str(), static_cast<int>(model_.subtitle.size()), &s))
            valueMax = std::max(valueMax, static_cast<int>(s.cx));
    }
    if (!model_.footer.empty()) {
        SelectObject(dc, fontSmall_);
        if (GetTextExtentPoint32W(dc, model_.footer.c_str(), static_cast<int>(model_.footer.size()), &s))
            valueMax = std::max(valueMax, static_cast<int>(s.cx));
    }
    DeleteDC(dc);
    ReleaseDC(hwnd_, screen);

    labelWidth_ = labelMax;
    const int padding = Scale(kPaddingDp);
    const int gap = Scale(kColumnGapDp);
    const int titleW = Scale(210);
    // 宽度取高水位并量化到 16 px：刷新时数值/单位位数会变（59% ↔ 100%），若每轮都跟着改窗口大小，
    // DWM 会不断重建重定向表面，工作集随刷新次数线性上涨（实测 14 MB → 30 MB / 45 s）。
    contentWidth_ = std::max(contentWidth_, valueMax);
    const int wanted = padding * 2 + labelWidth_ + gap + contentWidth_;
    const int clampedW = std::clamp(std::max(wanted, titleW), Scale(kMinWidthDp), Scale(kMaxWidthDp));
    panelWidth_ = (clampedW + 15) & ~15;

    const int titleLines = (model_.subtitle.empty() ? 1 : 2) + (model_.footer.empty() ? 0 : 1);
    panelHeight_ = padding * 2 + lineHeight_ * (titleLines + static_cast<int>(model_.rows.size())) +
                   Scale(kTitleGapDp) * 2;

    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    const int curW = cur.right - cur.left;
    const int curH = cur.bottom - cur.top;
    // 只认 16 px 级别的宽度变化（高度只在行数变化时才变），避免逐轮改大小
    if (std::abs(curW - panelWidth_) >= 16 || curH != panelHeight_) {
        SetWindowPos(hwnd_, nullptr, 0, 0, panelWidth_, panelHeight_,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void HudWindow::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int w = client.right;
    const int h = client.bottom;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(ToColorRef(style_.bg));
    FillRect(mem, &client, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    const int padding = Scale(kPaddingDp);
    int y = padding;

    // 标题 + 副标题
    SelectObject(mem, fontValue_);
    SetTextColor(mem, ToColorRef(style_.value));
    RECT titleRect{padding, y, w - padding, y + lineHeight_};
    DrawTextW(mem, model_.title.c_str(), -1, &titleRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += lineHeight_;
    if (!model_.subtitle.empty()) {
        SelectObject(mem, fontSmall_);
        SetTextColor(mem, ToColorRef(style_.unit));
        RECT subRect{padding, y, w - padding, y + lineHeight_};
        DrawTextW(mem, model_.subtitle.c_str(), -1, &subRect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        y += lineHeight_;
    }
    y += Scale(kTitleGapDp);

    // 指标行：标签左对齐、数值右对齐、单位在数值右侧但用暗色
    for (const auto& row : model_.rows) {
        const unsigned labelColor = row.available ? style_.label : style_.muted;
        const unsigned valueColor = !row.available ? style_.muted
                                    : row.emphasis ? style_.accent
                                                   : style_.value;
        SelectObject(mem, fontLabel_);
        SetTextColor(mem, ToColorRef(labelColor));
        RECT labelRect{padding, y, padding + labelWidth_, y + lineHeight_};
        DrawTextW(mem, row.label.c_str(), -1, &labelRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        SelectObject(mem, fontValue_);
        SIZE vs{};
        GetTextExtentPoint32W(mem, row.value.c_str(), static_cast<int>(row.value.size()), &vs);
        int unitW = 0;
        if (style_.showUnits && !row.unit.empty()) {
            SelectObject(mem, fontSmall_);
            SIZE us{};
            GetTextExtentPoint32W(mem, row.unit.c_str(), static_cast<int>(row.unit.size()), &us);
            unitW = static_cast<int>(us.cx) + Scale(4);
        }
        const int valueRight = w - padding;
        const int valueX = valueRight - unitW - static_cast<int>(vs.cx);
        SelectObject(mem, fontValue_);
        SetTextColor(mem, ToColorRef(valueColor));
        RECT valueRect{valueX, y, valueX + vs.cx + 2, y + lineHeight_};
        DrawTextW(mem, row.value.c_str(), -1, &valueRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        if (unitW > 0) {
            SelectObject(mem, fontSmall_);
            SetTextColor(mem, ToColorRef(style_.unit));
            RECT unitRect{valueRight - unitW + Scale(4), y, valueRight, y + lineHeight_};
            DrawTextW(mem, row.unit.c_str(), -1, &unitRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        y += lineHeight_;
    }

    if (!model_.footer.empty()) {
        y += Scale(kTitleGapDp);
        SelectObject(mem, fontSmall_);
        SetTextColor(mem, ToColorRef(style_.unit));
        RECT footerRect{padding, y, w - padding, y + lineHeight_};
        DrawTextW(mem, model_.footer.c_str(), -1, &footerRect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // 1 px 边框：HUD 贴在浅色桌面上时靠它划定边界
    HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(style_.border));
    HPEN oldPen = static_cast<HPEN>(SelectObject(mem, pen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(mem, GetStockObject(NULL_BRUSH)));
    Rectangle(mem, 0, 0, w, h);
    SelectObject(mem, oldBrush);
    SelectObject(mem, oldPen);
    DeleteObject(pen);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

void HudWindow::ShowContextMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING | (topmost_ ? MF_CHECKED : 0), 1, L"总在最前");
    AppendMenuW(menu, MF_STRING, 2, L"回到右上角");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"退出");
    SetForegroundWindow(hwnd_);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0,
                                   hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 1) {
        SetTopmost(!topmost_);
    } else if (cmd == 2) {
        ApplyPosition();
    } else if (cmd == 3) {
        DestroyWindow(hwnd_);
    }
}

LRESULT HudWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // WM_NCCREATE 阶段 hwnd_ 还没赋值，默认分支必须用参数里的 hwnd（否则 CreateWindowEx 直接失败）
    hwnd_ = hwnd;
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerId) Refresh();
            return 0;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 双缓冲自己画全窗口，不需要系统擦背景（否则闪）
        case WM_LBUTTONDOWN: {
            dragging_ = true;
            dragOrigin_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT r{};
            GetWindowRect(hwnd_, &r);
            dragRect_ = r;
            SetCapture(hwnd_);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_) {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                const int dx = pt.x - dragOrigin_.x;
                const int dy = pt.y - dragOrigin_.y;
                SetWindowPos(hwnd_, nullptr, dragRect_.left + dx, dragRect_.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &pt);
            ShowContextMenu(pt);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd_);
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮调不透明度：粘在游戏画面边上时最常需要微调的就是这个
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            style_.opacity = std::clamp(style_.opacity + (delta > 0 ? 0.05f : -0.05f), 0.2f, 1.0f);
            SetLayeredWindowAttributes(hwnd_, 0,
                                       static_cast<BYTE>(style_.opacity * 255.0f + 0.5f), LWA_ALPHA);
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            RebuildFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Refresh();
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerId);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace wpm::ui
