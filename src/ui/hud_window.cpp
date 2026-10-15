// SPDX-License-Identifier: MIT
#include "ui/hud_window.h"

#include <algorithm>
#include <cstdio>

#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

namespace wpm::ui {
namespace {

constexpr wchar_t kClassName[] = L"winperfmon.HudWindow";
constexpr UINT_PTR kTimerId = 1;
constexpr int kPaddingDp = 9;
constexpr int kTitleGapDp = 3;
constexpr int kColumnGapDp = 14;
constexpr int kUnitGapDp = 3;      // 数值与单位之间
constexpr int kMinWidthDp = 180;
constexpr int kMaxWidthDp = 640;

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
    lineHeight_ = static_cast<int>(size * 1.28);  // 紧凑：面板贴边时不占地方
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

void HudWindow::SetFreePosition(int x, int y) {
    if (hwnd_ == nullptr) return;
    freePosition_ = true;
    SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void HudWindow::ReportState() {
    if (!onState_ || hwnd_ == nullptr) return;
    RECT r{};
    if (!GetWindowRect(hwnd_, &r)) return;
    HudState st;
    st.x = r.left;
    st.y = r.top;
    st.opacity = style_.opacity;
    st.freePosition = freePosition_;
    onState_(st);
}

void HudWindow::ApplyPosition() {
    freePosition_ = false;  // 预设角落是显式选择，覆盖用户拖动过的位置
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

    const int padding = Scale(kPaddingDp);
    const int gap = Scale(style_.itemGapDp);
    const int groupGap = Scale(style_.groupGapDp);
    const int unitGap = Scale(kUnitGapDp);
    const std::size_t count = model_.rows.size();
    const int labelLine = style_.showLabels ? lineHeight_ : 0;  // 横铺时标签那行的行高

    // ── 测量：每项都要知道标签/数值/单位各占多宽 ──────────────────────────
    HDC screen = GetDC(hwnd_);
    HDC dc = CreateCompatibleDC(screen);
    const auto widthOf = [&](HFONT font, const std::wstring& text) {
        if (text.empty()) return 0;
        SelectObject(dc, font);
        SIZE s{};
        if (!GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &s)) return 0;
        return static_cast<int>(s.cx);
    };

    std::vector<int> labelW(count, 0);
    std::vector<int> valueW(count, 0);
    std::vector<int> unitW(count, 0);
    int labelMax = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const HudRow& row = model_.rows[i];
        labelW[i] = widthOf(fontLabel_, row.label);
        valueW[i] = widthOf(fontValue_, row.value);
        unitW[i] = (style_.showUnits && !row.unit.empty()) ? widthOf(fontSmall_, row.unit) : 0;
        labelMax = std::max(labelMax, labelW[i]);
    }
    const int titleW = std::max(widthOf(fontValue_, model_.title), widthOf(fontSmall_, model_.subtitle));

    cells_.assign(count, Cell{});
    int bodyHeight = 0;
    int bodyWidth = 0;

    if (!style_.horizontal) {
        // ── 纵铺：每项一行；标签列、数值列、单位列各自成列（单位对齐才好扫读）──
        labelWidth_ = labelMax;
        valueColumnW_ = 0;
        unitColumnW_ = 0;
        for (std::size_t i = 0; i < count; ++i) {
            valueColumnW_ = std::max(valueColumnW_, valueW[i]);
            unitColumnW_ = std::max(unitColumnW_, unitW[i]);
        }
        int y = 0;
        for (std::size_t i = 0; i < count; ++i) {
            // 纵铺同样尊重 itemGapDp（同段相邻项的行距）、groupGapDp（换段时的行距）：
            // 之前纵铺只用 lineHeight，itemGapDp 被静默忽略，于是「换段的空隙」看起来和其他行距完全不同。
            if (i > 0) y += model_.rows[i].newGroup ? groupGap : gap;
            cells_[i].y = y;
            cells_[i].valueW = valueW[i];
            cells_[i].unitW = unitW[i];
            cells_[i].labelW = labelW[i];
            y += lineHeight_;
        }
        bodyHeight = y;
        // 宽度取高水位并量化到 16 px：数值位数会变（59% ↔ 100%），逐轮改窗口大小会让 DWM 重建重定向表面
        const int rowW = labelWidth_ + gap + valueColumnW_ +
                         (unitColumnW_ > 0 ? Scale(kUnitGapDp) + unitColumnW_ : 0);
        contentWidth_ = std::max(contentWidth_, rowW);
        bodyWidth = std::max(titleW, contentWidth_);
    } else {
        // ── 横铺：每项一列（标签在上、数值+单位在下），可换行 ──────────────
        labelWidth_ = 0;
        const int maxBody = std::max(Scale(style_.maxWidthDp) - padding * 2, Scale(kMinWidthDp));
        int x = 0;
        int y = 0;
        int lineWidth = 0;
        int lines = count > 0 ? 1 : 0;
        for (std::size_t i = 0; i < count; ++i) {
            const int cellW = std::max(labelW[i], valueW[i] + (unitW[i] > 0 ? unitGap + unitW[i] : 0));
            const int step = cellW + (i > 0 && model_.rows[i].newGroup ? groupGap : (i > 0 ? gap : 0));
            if (i > 0 && style_.wrap && x + step > maxBody) {
                lineWidth = std::max(lineWidth, x - gap);
                x = 0;
                y += labelLine + lineHeight_;
                ++lines;
                cells_[i].x = 0;
            } else {
                x += step;
                cells_[i].x = x - cellW;
            }
            cells_[i].y = y;
            cells_[i].w = cellW;
            cells_[i].valueW = valueW[i];
            cells_[i].unitW = unitW[i];
            cells_[i].labelW = labelW[i];
            lineWidth = std::max(lineWidth, cells_[i].x + cellW);
        }
        bodyHeight = lines * (labelLine + lineHeight_);
        bodyWidth = std::max(titleW, lineWidth);
        // 对齐：横铺时按整行宽度在面板内平移（行宽要等全部排完才知道）
        if (style_.align != 0 && bodyWidth > 0) {
            std::vector<std::pair<int, int>> lineW;  // y → 该行实际宽度
            for (const Cell& c : cells_) {
                auto it = std::find_if(lineW.begin(), lineW.end(),
                                       [&](const std::pair<int, int>& e) { return e.first == c.y; });
                if (it == lineW.end())
                    lineW.emplace_back(c.y, c.x + c.w);
                else
                    it->second = std::max(it->second, c.x + c.w);
            }
            for (Cell& c : cells_) {
                auto it = std::find_if(lineW.begin(), lineW.end(),
                                       [&](const std::pair<int, int>& e) { return e.first == c.y; });
                if (it == lineW.end()) continue;
                const int slack = std::max(bodyWidth - it->second - padding, 0);
                c.x += style_.align == 1 ? slack / 2 : slack;
            }
        }
    }
    DeleteDC(dc);
    ReleaseDC(hwnd_, screen);

    // ── 面板尺寸（标题 + 主体 + 页脚）────────────────────────────────────
    const int titleBlock = lineHeight_ + (model_.subtitle.empty() ? 0 : lineHeight_) + Scale(kTitleGapDp);
    const int footerBlock = model_.footer.empty() ? 0 : lineHeight_ + Scale(kTitleGapDp);
    panelHeight_ = padding * 2 + titleBlock + bodyHeight + footerBlock;

    const int hardMax = style_.horizontal ? std::max(Scale(style_.maxWidthDp), Scale(kMaxWidthDp))
                                          : Scale(kMaxWidthDp);
    const int clampedW = std::clamp(bodyWidth + padding * 2, Scale(kMinWidthDp), hardMax);
    const int quantized = (clampedW + 15) & ~15;
    if (!style_.horizontal || quantized != panelWidth_) panelWidth_ = quantized;

    // 主体起点在标题之后；把 cells_ 平移到窗口坐标
    const int bodyTop = padding + titleBlock;
    for (std::size_t i = 0; i < count; ++i) {
        cells_[i].x += padding;
        cells_[i].y += bodyTop;
        cells_[i].w = style_.horizontal ? cells_[i].w : panelWidth_ - padding * 2;
    }

    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    const int curW = cur.right - cur.left;
    const int curH = cur.bottom - cur.top;
    // 只认 16 px 级别的宽度变化（高度只在行数/换行变化时才变），避免逐轮改大小
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

    // 指标项：位置由 Refresh 的布局引擎给出（cells_ 与 model_.rows 一一对应），
    // 这样纵铺/横铺/换行/分组间距的算法只有一份，Paint 只负责画。
    for (std::size_t i = 0; i < model_.rows.size() && i < cells_.size(); ++i) {
        const HudRow& row = model_.rows[i];
        const Cell& c = cells_[i];
        const unsigned labelColor = row.available ? style_.label : style_.muted;
        const unsigned valueColor = !row.available ? style_.muted
                                    : row.colorOverride != 0 ? row.colorOverride
                                    : row.emphasis ? style_.accent
                                                   : style_.value;
        if (!style_.horizontal) {
            // 纵铺：标签左对齐、数值右对齐、单位在数值右侧（暗色）
            SelectObject(mem, fontLabel_);
            SetTextColor(mem, ToColorRef(labelColor));
            RECT labelRect{padding, c.y, padding + labelWidth_, c.y + lineHeight_};
            DrawTextW(mem, row.label.c_str(), -1, &labelRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            // 纵铺：标签列左对齐、数值列右对齐（小数点成列）、单位列固定起点（单位成列）
            const int valueColX = padding + labelWidth_ + Scale(style_.itemGapDp);
            const int valueX = valueColX + std::max(valueColumnW_ - c.valueW, 0);
            SelectObject(mem, fontValue_);
            SetTextColor(mem, ToColorRef(valueColor));
            RECT valueRect{valueX, c.y, valueX + c.valueW + 2, c.y + lineHeight_};
            DrawTextW(mem, row.value.c_str(), -1, &valueRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            if (c.unitW > 0) {
                const int unitX = valueColX + valueColumnW_ + Scale(kUnitGapDp);
                SelectObject(mem, fontSmall_);
                SetTextColor(mem, ToColorRef(style_.unit));
                RECT unitRect{unitX, c.y, unitX + c.unitW + 2, c.y + lineHeight_};
                DrawTextW(mem, row.unit.c_str(), -1, &unitRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
        } else {
            // 横铺：标签在上（小字暗色）、数值在下（大字亮色）、单位紧贴数值右侧
            int cy = c.y;
            if (style_.showLabels) {
                SelectObject(mem, fontSmall_);
                SetTextColor(mem, ToColorRef(labelColor));
                RECT labelRect{c.x, cy, c.x + c.w, cy + lineHeight_};
                DrawTextW(mem, row.label.c_str(), -1, &labelRect,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                cy += lineHeight_;
            }
            SelectObject(mem, fontValue_);
            SetTextColor(mem, ToColorRef(valueColor));
            RECT valueRect{c.x, cy, c.x + c.w, cy + lineHeight_};
            DrawTextW(mem, row.value.c_str(), -1, &valueRect,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if (c.unitW > 0) {
                const int unitX = c.x + c.valueW + Scale(kUnitGapDp);
                SelectObject(mem, fontSmall_);
                SetTextColor(mem, ToColorRef(style_.unit));
                RECT unitRect{unitX, cy, unitX + c.unitW + 2, cy + lineHeight_};
                DrawTextW(mem, row.unit.c_str(), -1, &unitRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            }
        }
    }

    if (!model_.footer.empty()) {
        // 页脚钉在底部：布局引擎已经把它算进高度，不依赖上面累加的 y
        const int footerY = h - padding - lineHeight_;
        SelectObject(mem, fontSmall_);
        SetTextColor(mem, ToColorRef(style_.unit));
        RECT footerRect{padding, footerY, w - padding, footerY + lineHeight_};
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
                freePosition_ = true;  // 拖过就不再回角落
                ReportState();
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
            ReportState();
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
