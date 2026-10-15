// SPDX-License-Identifier: MIT
// 极简 HUD 窗口：无边框、半透明、可拖动、置顶，纯 GDI 绘制（零第三方依赖）。
//
// 为什么是 GDI 而不是 Direct2D：这是 M0.5 的可见雏形，目标是「冷启动快、内存小、不闪烁」。
// GDI 双缓冲在这个规模（十几行文字）完全够用，且不需要 D2D/DWrite 那套运行时。
// M3 的自绘 Overlay 再决定是否换渲染后端。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace wpm::ui {

/// 一行指标：左标签、右数值、可选单位。窗口不解释状态，只负责画。
struct HudRow {
    std::wstring label;
    std::wstring value;   // 已格式化好的文本（含百分号/单位前缀）
    std::wstring unit;    // 单独画在数值右侧，可用颜色区分
    bool available = true;  // false → 用暗色画（读不到），但**仍然占一行**（不静默消失）
    bool emphasis = false;  // true → 数值用强调色
};

struct HudModel {
    std::wstring title;
    std::wstring subtitle;
    std::vector<HudRow> rows;
    std::wstring footer;
};

/// 视觉参数，全部来自用户配置（`WindowConfig` / `LayoutConfig` / `ColorsConfig` / `TextConfig`）。
struct HudStyle {
    unsigned bg = 0xE0101418;       // 0xAARRGGBB（alpha 只用于决定窗口整体不透明度）
    unsigned border = 0x40FFFFFF;
    unsigned label = 0xB3FFFFFF;
    unsigned value = 0xFFFFFFFF;
    unsigned unit = 0x99FFFFFF;
    unsigned muted = 0x80FFFFFF;    // 不可用行
    unsigned accent = 0xFF4FC3F7;
    int baseSizeDp = 14;
    float opacity = 0.90f;
    bool showUnits = true;
    bool hideMissing = false;
    int corner = 0;   // 0 左上 / 1 右上 / 2 左下 / 3 右下
    int marginDp = 12;
    bool topmost = true;
};

class HudWindow {
public:
    /// 每次刷新前调用：外部在这里采样（Collector::Step）并组装模型。
    using BuildFn = std::function<HudModel()>;

    HudWindow();
    ~HudWindow();
    HudWindow(const HudWindow&) = delete;
    HudWindow& operator=(const HudWindow&) = delete;

    /// 必须在加入消息循环前调用 `SetBuilder`；`error` 收到创建失败原因（中文）。
    bool Create(const HudStyle& style, std::string* error);
    void SetBuilder(BuildFn fn) { builder_ = std::move(fn); }
    void SetIntervalMs(unsigned ms);
    void SetTopmost(bool on);
    [[nodiscard]] bool topmost() const noexcept { return topmost_; }

    /// 消息循环，返回值即进程退出码。
    int Run();

    static void EnablePerMonitorDpi();  // 进程启动时调用一次

private:
    static LRESULT CALLBACK WndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void RebuildFonts();
    void Refresh();                       // 调 builder_、必要时改窗口尺寸、请求重绘
    void Paint();
    void ApplyPosition();                 // 按 corner/margin 摆到工作区角落
    void ShowContextMenu(POINT screenPt);
    [[nodiscard]] int Scale(int dp) const;

    HWND hwnd_ = nullptr;
    HudStyle style_;
    BuildFn builder_;
    HudModel model_;
    unsigned intervalMs_ = 500;
    bool topmost_ = true;

    HFONT fontLabel_ = nullptr;
    HFONT fontValue_ = nullptr;
    HFONT fontSmall_ = nullptr;
    unsigned dpi_ = 96;

    int lineHeight_ = 20;                 // 逻辑像素（已含行距）
    int labelWidth_ = 90;                 // 由模型实测
    int contentWidth_ = 0;                // 内容宽度高水位：只增不减，避免数值位数变化引起窗口反复改大小
    int panelWidth_ = 0;
    int panelHeight_ = 0;

    bool dragging_ = false;
    POINT dragOrigin_{};
    RECT dragRect_{};
};

}  // namespace wpm::ui
