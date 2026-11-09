// NvChangeGui - an iOS-inspired Win32 front-end for the NVAPI display core.
//
// Everything is drawn with GDI+ (no external UI framework).  The window is
// borderless, DPI aware, and uses the Windows 11 rounded-corner DWM attribute
// when available.

#include "NvChangeCore.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

using namespace Gdiplus;
using namespace nvchange;

namespace {

constexpr int kBaseWidth = 1040;
constexpr int kBaseHeight = 760;
constexpr int kIdNone = -1;
constexpr int kIdTitle = -2;
constexpr int kIdClose = 1;
constexpr int kIdMinimize = 2;
constexpr int kIdSwitchDisplay = 3;
constexpr int kIdApply = 4;
constexpr int kIdRestore = 5;
constexpr int kIdCustomModes = 6;
constexpr int kIdCustomRefresh = 7;
constexpr int kIdPresetBase = 1000;
constexpr int kIdRefreshBase = 2000;

enum class Tone {
    Neutral,
    Info,
    Success,
    Error,
};

struct Preset {
    UINT width = 0;
    UINT height = 0;
    std::wstring title;
    std::wstring subtitle;
};

struct Hit {
    int id = kIdNone;
    RECT rect{};
};

struct Layout {
    RECT client{};
    RECT dragArea{};
    RECT closeButton{};
    RECT minimizeButton{};
    RECT switchButton{};
    RECT displayCard{};
    RECT resolutionCard{};
    RECT refreshCard{};
    RECT customButton{};
    RECT customRefreshButton{};
    RECT restoreButton{};
    RECT applyButton{};
    std::vector<RECT> presets;
    std::vector<RECT> refreshes;
};

const wchar_t* UiFamily() {
    static const wchar_t* family = []() -> const wchar_t* {
        FontFamily preferred(L"Microsoft YaHei UI");
        if (preferred.IsAvailable()) {
            return L"Microsoft YaHei UI";
        }
        FontFamily fallback(L"Segoe UI");
        if (fallback.IsAvailable()) {
            return L"Segoe UI";
        }
        return L"Arial";
    }();
    return family;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::wstring FormatResolution(UINT width, UINT height) {
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%u × %u", width, height);
    return buffer;
}

std::wstring FormatHz(UINT milliHz) {
    wchar_t buffer[64]{};
    if (milliHz % 1000 == 0) {
        swprintf(buffer, 64, L"%u", milliHz / 1000);
    } else {
        swprintf(buffer, 64, L"%.3f", milliHz / 1000.0);
    }
    return buffer;
}

std::wstring FormatMode(UINT width, UINT height, UINT milliHz) {
    std::wstring result = FormatResolution(width, height);
    if (milliHz > 0) {
        result += L" @ ";
        result += FormatHz(milliHz);
        result += L" Hz";
    }
    return result;
}

int ScaleInt(int value, UINT dpi) {
    return static_cast<int>(std::lround(value * static_cast<double>(dpi) / 96.0));
}

float ScaleFloat(float value, UINT dpi) {
    return value * static_cast<float>(dpi) / 96.0f;
}

RECT MakeRect(int left, int top, int right, int bottom) {
    RECT rect{left, top, right, bottom};
    return rect;
}

RectF ToRectF(const RECT& rect) {
    return RectF(static_cast<REAL>(rect.left), static_cast<REAL>(rect.top),
                 static_cast<REAL>(rect.right - rect.left),
                 static_cast<REAL>(rect.bottom - rect.top));
}

void AddRoundedRect(GraphicsPath& path, const RectF& rect, float radius) {
    const float maxRadius = std::min(rect.Width, rect.Height) / 2.0f;
    const float r = std::max(0.0f, std::min(radius, maxRadius));
    if (r <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    const float d = r * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRounded(Graphics& graphics, const RectF& rect, float radius, const Color& color) {
    GraphicsPath path;
    AddRoundedRect(path, rect, radius);
    SolidBrush brush(color);
    graphics.FillPath(&brush, &path);
}

void StrokeRounded(Graphics& graphics, const RectF& rect, float radius, const Color& color,
                   float width) {
    GraphicsPath path;
    AddRoundedRect(path, rect, radius);
    Pen pen(color, width);
    pen.SetLineJoin(LineJoinRound);
    graphics.DrawPath(&pen, &path);
}

void DrawText(Graphics& graphics, const std::wstring& text, Font& font, const Color& color,
              const RectF& rect, StringAlignment alignment = StringAlignmentNear,
              StringAlignment lineAlignment = StringAlignmentNear, bool ellipsis = true) {
    StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(lineAlignment);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetHotkeyPrefix(HotkeyPrefixNone);
    if (ellipsis) {
        format.SetTrimming(StringTrimmingEllipsisCharacter);
    }
    SolidBrush brush(color);
    graphics.DrawString(text.c_str(), -1, &font, rect, &format, &brush);
}

void DrawText(Graphics& graphics, const std::wstring& text, Font& font, const Color& color,
              float x, float y, float width, float height,
              StringAlignment alignment = StringAlignmentNear,
              StringAlignment lineAlignment = StringAlignmentNear) {
    DrawText(graphics, text, font, color, RectF(x, y, width, height), alignment, lineAlignment);
}

void DrawMonitorGlyph(Graphics& graphics, const RectF& box, const Color& color, float stroke) {
    const float w = box.Width;
    const float h = box.Height;
    const float screenW = w * 0.62f;
    const float screenH = h * 0.44f;
    const float screenX = box.X + (w - screenW) / 2.0f;
    const float screenY = box.Y + h * 0.22f;
    RectF screen(screenX, screenY, screenW, screenH);
    GraphicsPath path;
    AddRoundedRect(path, screen, screenH * 0.18f);
    Pen pen(color, stroke);
    pen.SetLineJoin(LineJoinRound);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    graphics.DrawPath(&pen, &path);
    const float centerX = box.X + w / 2.0f;
    const float baseY = box.Y + h * 0.84f;
    graphics.DrawLine(&pen, centerX, screen.GetBottom(), centerX, baseY);
    graphics.DrawLine(&pen, centerX - w * 0.15f, baseY, centerX + w * 0.15f, baseY);
}

void DrawCheckGlyph(Graphics& graphics, const PointF& center, float size, const Color& color,
                    float stroke) {
    Pen pen(color, stroke);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    PointF a(center.X - size * 0.42f, center.Y + size * 0.02f);
    PointF b(center.X - size * 0.10f, center.Y + size * 0.32f);
    PointF c(center.X + size * 0.46f, center.Y - size * 0.34f);
    graphics.DrawLine(&pen, a, b);
    graphics.DrawLine(&pen, b, c);
}

void DrawCloseGlyph(Graphics& graphics, const RectF& rect, const Color& color, float stroke) {
    Pen pen(color, stroke);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    const float inset = rect.Width * 0.34f;
    graphics.DrawLine(&pen, rect.X + inset, rect.Y + inset, rect.GetRight() - inset,
                      rect.GetBottom() - inset);
    graphics.DrawLine(&pen, rect.GetRight() - inset, rect.Y + inset, rect.X + inset,
                      rect.GetBottom() - inset);
}

void DrawMinimizeGlyph(Graphics& graphics, const RectF& rect, const Color& color, float stroke) {
    Pen pen(color, stroke);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    const float inset = rect.Width * 0.32f;
    graphics.DrawLine(&pen, rect.X + inset, rect.Y + rect.Height * 0.58f,
                      rect.GetRight() - inset, rect.Y + rect.Height * 0.58f);
}

void DrawChevronGlyph(Graphics& graphics, const RectF& rect, const Color& color, float stroke) {
    Pen pen(color, stroke);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    const float x = rect.X + rect.Width * 0.42f;
    const float y = rect.Y + rect.Height * 0.50f;
    const float d = rect.Height * 0.20f;
    graphics.DrawLine(&pen, x, y - d, x + d, y);
    graphics.DrawLine(&pen, x + d, y, x, y + d);
}

void DrawResetGlyph(Graphics& graphics, const RectF& box, const Color& color, float stroke) {
    Pen pen(color, stroke);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    RectF arc(box.X + box.Width * 0.20f, box.Y + box.Height * 0.18f,
              box.Width * 0.60f, box.Height * 0.60f);
    graphics.DrawArc(&pen, arc, 35.0f, 285.0f);
    const float tipX = box.X + box.Width * 0.24f;
    const float tipY = box.Y + box.Height * 0.28f;
    graphics.DrawLine(&pen, tipX, tipY, tipX - box.Width * 0.06f, tipY + box.Height * 0.17f);
    graphics.DrawLine(&pen, tipX, tipY, tipX + box.Width * 0.15f, tipY - box.Height * 0.01f);
}

void DrawBlob(Graphics& graphics, const RectF& rect, const Color& centerColor) {
    GraphicsPath path;
    path.AddEllipse(rect);
    PathGradientBrush brush(&path);
    brush.SetCenterColor(centerColor);
    Color surround[] = {Color(0, centerColor.GetR(), centerColor.GetG(), centerColor.GetB())};
    int count = 1;
    brush.SetSurroundColors(surround, &count);
    graphics.FillPath(&brush, &path);
}

bool ShowCustomEditor(HWND owner, UINT dpi, std::vector<CustomMode>& modes);
bool ShowRefreshEditor(HWND owner, UINT dpi, UINT& hz);

class App {
public:
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;
    UINT dpi = 96;

    NvApi api;
    DisplayConfig config;
    bool apiReady = false;
    bool configReady = false;
    std::wstring startupError;
    std::wstring gpuName;
    std::wstring nvapiVersion;

    std::size_t pathIndex = 0;
    std::vector<CustomMode> customModes;
    std::wstring customLoadError;
    std::vector<Preset> presets;
    int selectedPreset = 0;
    std::vector<UINT> refreshOptions;
    int selectedRefresh = 0;
    NvU32 customRefreshHz = 0;
    std::wstring customRefreshLoadError;

    int hot = kIdNone;
    int pressed = kIdNone;
    bool dragging = false;
    POINT dragCursor{};
    RECT dragWindow{};

    std::wstring message;
    Tone tone = Tone::Neutral;
    Layout layout;
    std::vector<Hit> hits;

    int S(int value) const {
        return ScaleInt(value, dpi);
    }

    float SF(float value) const {
        return ScaleFloat(value, dpi);
    }

    void Initialize() {
        std::string error;
        std::string customError;
        std::string refreshError;
        if (!LoadCustomModes(customModes, customError)) {
            customLoadError = Utf8ToWide(customError);
        }
        if (!LoadCustomRefresh(customRefreshHz, refreshError)) {
            customRefreshLoadError = Utf8ToWide(refreshError);
        }
        if (!api.Load(error)) {
            startupError = Utf8ToWide(error);
            BuildPresets();
            BuildRefreshOptions();
            return;
        }
        apiReady = true;
        nvapiVersion = Utf8ToWide(api.InterfaceVersion());
        gpuName = Utf8ToWide(api.GpuName());

        if (!config.Load(api, error)) {
            startupError = Utf8ToWide(error);
            BuildPresets();
            BuildRefreshOptions();
            return;
        }
        configReady = true;
        pathIndex = config.PrimaryPath();
        BuildPresets();
        BuildRefreshOptions();
        if (!customLoadError.empty()) {
            SetMessage(L"自定义分辨率读取失败：" + customLoadError, Tone::Error);
        } else if (!customRefreshLoadError.empty()) {
            SetMessage(L"自定义刷新率读取失败：" + customRefreshLoadError, Tone::Error);
        }
    }

    const NV_DISPLAYCONFIG_PATH_INFO* CurrentPath() const {
        if (!configReady || pathIndex >= config.paths.size()) {
            return nullptr;
        }
        return &config.paths[pathIndex];
    }

    UINT CurrentWidth() const {
        const auto* path = CurrentPath();
        return path && path->sourceModeInfo ? path->sourceModeInfo[0].resolution.width : 0;
    }

    UINT CurrentHeight() const {
        const auto* path = CurrentPath();
        return path && path->sourceModeInfo ? path->sourceModeInfo[0].resolution.height : 0;
    }

    UINT CurrentRefresh() const {
        const auto* path = CurrentPath();
        if (!path || path->targetInfoCount == 0 || path->targetInfo[0].details == nullptr) {
            return 0;
        }
        return path->targetInfo[0].details->refreshRate1K;
    }

    UINT CurrentDisplayId() const {
        const auto* path = CurrentPath();
        if (!path || path->targetInfoCount == 0) {
            return 0;
        }
        return path->targetInfo[0].displayId;
    }

    UINT CurrentColorDepth() const {
        const auto* path = CurrentPath();
        return path && path->sourceModeInfo ? path->sourceModeInfo[0].resolution.colorDepth : 32;
    }

    std::size_t DisplayCount() const {
        return configReady ? config.paths.size() : 0;
    }

    void BuildPresets() {
        presets.clear();
        if (configReady) {
            presets.push_back({CurrentWidth(), CurrentHeight(),
                               FormatResolution(CurrentWidth(), CurrentHeight()), L"当前模式"});
        }

        for (const auto& mode : customModes) {
            if (presets.size() >= 8) {
                break;
            }
            if (!IsValidCustomMode(mode)) {
                continue;
            }
            bool duplicate = false;
            for (const auto& existing : presets) {
                if (existing.width == mode.width && existing.height == mode.height) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                presets.push_back({mode.width, mode.height, FormatResolution(mode.width, mode.height),
                                   L"自定义"});
            }
        }

        static const Preset common[] = {
            {1920, 1080, L"1920 × 1080", L"FHD · 16:9"},
            {1600, 900, L"1600 × 900", L"HD+ · 16:9"},
            {1366, 768, L"1366 × 768", L"HD · 16:9"},
            {1280, 720, L"1280 × 720", L"HD · 16:9"},
        };

        for (const auto& item : common) {
            if (presets.size() >= 8) {
                break;
            }
            bool duplicate = false;
            for (const auto& existing : presets) {
                if (existing.width == item.width && existing.height == item.height) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                presets.push_back(item);
            }
        }
        selectedPreset = 0;
    }

    void BuildRefreshOptions() {
        refreshOptions.clear();
        const UINT current = CurrentRefresh();
        static const UINT common[] = {60000,  75000,  100000, 120000, 144000,
                                      165000, 240000, 360000, 480000, 600000,
                                      1000000};

        const auto addOption = [this](UINT value) {
            for (UINT existing : refreshOptions) {
                if (std::abs(static_cast<long long>(existing) - value) <= 500) {
                    return;
                }
            }
            refreshOptions.push_back(value);
        };

        if (current > 0 && current <= 1000000) {
            addOption(current);
        }
        if (customRefreshHz >= 1 && customRefreshHz <= 1000) {
            addOption(static_cast<UINT>(customRefreshHz) * 1000);
        }
        for (UINT value : common) {
            addOption(value);
        }
        selectedRefresh = 0;
        if (current > 0) {
            long long best = -1;
            for (std::size_t i = 0; i < refreshOptions.size(); ++i) {
                long long diff = std::abs(static_cast<long long>(refreshOptions[i]) - current);
                if (best < 0 || diff < best) {
                    best = diff;
                    selectedRefresh = static_cast<int>(i);
                }
            }
        }
    }

    bool ReloadConfig() {
        std::string error;
        config.Clear();
        if (!config.Load(api, error)) {
            configReady = false;
            startupError = Utf8ToWide(error);
            BuildPresets();
            BuildRefreshOptions();
            return false;
        }
        configReady = true;
        if (pathIndex >= config.paths.size()) {
            pathIndex = 0;
        }
        BuildPresets();
        BuildRefreshOptions();
        return true;
    }

    UINT SelectedWidth() const {
        return presets.empty() ? 0 : presets[static_cast<std::size_t>(selectedPreset)].width;
    }

    UINT SelectedHeight() const {
        return presets.empty() ? 0 : presets[static_cast<std::size_t>(selectedPreset)].height;
    }

    UINT SelectedRefresh() const {
        if (refreshOptions.empty()) {
            return 0;
        }
        return refreshOptions[static_cast<std::size_t>(selectedRefresh)];
    }

    bool CanApply() const {
        return apiReady && configReady && !presets.empty() && !refreshOptions.empty();
    }

    bool IsDirty() const {
        if (!CanApply()) {
            return false;
        }
        if (SelectedWidth() != CurrentWidth() || SelectedHeight() != CurrentHeight()) {
            return true;
        }
        const long long diff = std::abs(static_cast<long long>(SelectedRefresh()) -
                                        static_cast<long long>(CurrentRefresh()));
        return diff > 100;
    }

    void OpenCustomEditor() {
        if (!hwnd) {
            return;
        }
        std::vector<CustomMode> slots = customModes;
        slots.resize(3);
        if (!ShowCustomEditor(hwnd, dpi, slots)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        std::string error;
        if (!SaveCustomModes(slots, error)) {
            SetMessage(L"无法保存自定义分辨率：" + Utf8ToWide(error), Tone::Error);
            return;
        }
        customModes = slots;
        customLoadError.clear();
        BuildPresets();
        // Keep the current mode selected after saving; the user can pick a
        // custom card explicitly without an accidental pending change.
        selectedPreset = 0;
        if (customModes.empty()) {
            SetMessage(L"已清除自定义分辨率", Tone::Neutral);
        } else {
            SetMessage(L"已保存 " + std::to_wstring(customModes.size()) + L" 个自定义分辨率",
                       Tone::Success);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void OpenRefreshEditor() {
        if (!hwnd) {
            return;
        }
        UINT hz = customRefreshHz;
        if (!ShowRefreshEditor(hwnd, dpi, hz)) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        std::string error;
        if (!SaveCustomRefresh(hz, error)) {
            SetMessage(L"无法保存自定义刷新率：" + Utf8ToWide(error), Tone::Error);
            return;
        }
        customRefreshHz = hz;
        customRefreshLoadError.clear();
        BuildRefreshOptions();
        for (std::size_t i = 0; i < refreshOptions.size(); ++i) {
            if (std::abs(static_cast<long long>(refreshOptions[i]) -
                         static_cast<long long>(hz) * 1000) <= 500) {
                selectedRefresh = static_cast<int>(i);
                break;
            }
        }
        SetMessage(L"已添加自定义刷新率 " + std::to_wstring(hz) + L" Hz", Tone::Success);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void SetMessage(std::wstring text, Tone newTone) {
        message = std::move(text);
        tone = newTone;
        if (hwnd) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    std::wstring StatusText() const {
        if (!message.empty()) {
            return message;
        }
        if (!apiReady) {
            return startupError.empty() ? L"NVAPI 初始化失败"
                                        : L"NVAPI 初始化失败：" + startupError;
        }
        if (!configReady) {
            return startupError.empty() ? L"未读取到显示配置"
                                        : L"未读取到显示配置：" + startupError;
        }
        if (IsDirty()) {
            return L"将应用：" + FormatMode(SelectedWidth(), SelectedHeight(), SelectedRefresh());
        }
        return L"当前：" + FormatMode(CurrentWidth(), CurrentHeight(), CurrentRefresh());
    }

    Color StatusColor() const {
        if (!message.empty()) {
            switch (tone) {
            case Tone::Success:
                return Color(255, 22, 163, 74);
            case Tone::Error:
                return Color(255, 220, 38, 38);
            case Tone::Info:
                return Color(255, 10, 132, 255);
            default:
                break;
            }
        }
        if (!apiReady || !configReady) {
            return Color(255, 220, 38, 38);
        }
        return IsDirty() ? Color(255, 10, 132, 255) : Color(255, 107, 114, 128);
    }

    void Relayout() {
        if (!hwnd) {
            return;
        }
        GetClientRect(hwnd, &layout.client);
        const int width = layout.client.right;
        const int height = layout.client.bottom;
        if (width <= 0 || height <= 0) {
            return;
        }

        const int margin = S(32);
        const int closeSize = S(36);
        layout.closeButton = MakeRect(width - margin - closeSize, S(28),
                                      width - margin, S(28) + closeSize);
        layout.minimizeButton = MakeRect(layout.closeButton.left - S(44), S(28),
                                         layout.closeButton.left - S(8), S(28) + closeSize);
        layout.dragArea = MakeRect(0, 0, width, S(100));
        layout.displayCard = MakeRect(margin, S(112), width - margin, S(236));
        layout.resolutionCard = MakeRect(margin, S(252), width - margin, S(474));
        layout.refreshCard = MakeRect(margin, S(490), width - margin, S(578));
        layout.customButton = MakeRect(layout.resolutionCard.right - S(112),
                                       layout.resolutionCard.top + S(14),
                                       layout.resolutionCard.right - S(24),
                                       layout.resolutionCard.top + S(40));
        layout.customRefreshButton = MakeRect(layout.refreshCard.right - S(112),
                                              layout.refreshCard.top + S(14),
                                              layout.refreshCard.right - S(24),
                                              layout.refreshCard.top + S(40));

        layout.applyButton = MakeRect(width - margin - S(180), height - S(94), width - margin,
                                      height - S(48));
        layout.restoreButton =
            MakeRect(layout.applyButton.left - S(12) - S(124), height - S(94),
                     layout.applyButton.left - S(12), height - S(48));

        const int cardLeft = layout.displayCard.left;
        const int cardTop = layout.displayCard.top;
        layout.switchButton = MakeRect(cardLeft + S(96), cardTop + S(70),
                                       cardLeft + S(96) + S(88), cardTop + S(70) + S(24));

        layout.presets.clear();
        const int gridLeft = layout.resolutionCard.left + S(24);
        const int gridTop = layout.resolutionCard.top + S(52);
        const int gap = S(12);
        const int tileWidth = (layout.resolutionCard.right - layout.resolutionCard.left - S(48) -
                               3 * gap) /
                              4;
        const int tileHeight = S(66);
        for (int i = 0; i < 8; ++i) {
            const int column = i % 4;
            const int row = i / 4;
            const int x = gridLeft + column * (tileWidth + gap);
            const int y = gridTop + row * (tileHeight + gap);
            layout.presets.push_back(MakeRect(x, y, x + tileWidth, y + tileHeight));
        }

        layout.refreshes.clear();
        const int refreshLeft = layout.refreshCard.left + S(24);
        const int refreshTop = layout.refreshCard.top + S(44);
        const int refreshHeight = S(32);
        const int refreshGap = S(5);
        const int chipAreaRight = layout.customRefreshButton.left - S(16);
        const int chipAreaWidth = chipAreaRight - refreshLeft;
        const int chipCount = static_cast<int>(refreshOptions.size());
        int refreshWidth = S(66);
        if (chipCount > 1) {
            refreshWidth = std::min(refreshWidth,
                                    (chipAreaWidth - (chipCount - 1) * refreshGap) / chipCount);
        }
        refreshWidth = std::max(refreshWidth, S(44));
        for (std::size_t i = 0; i < refreshOptions.size(); ++i) {
            const int x = refreshLeft + static_cast<int>(i) * (refreshWidth + refreshGap);
            layout.refreshes.push_back(
                MakeRect(x, refreshTop, x + refreshWidth, refreshTop + refreshHeight));
        }

        hits.clear();
        hits.push_back({kIdClose, layout.closeButton});
        hits.push_back({kIdMinimize, layout.minimizeButton});
        if (DisplayCount() > 1) {
            hits.push_back({kIdSwitchDisplay, layout.switchButton});
        }
        for (std::size_t i = 0; i < layout.presets.size(); ++i) {
            hits.push_back({kIdPresetBase + static_cast<int>(i), layout.presets[i]});
        }
        for (std::size_t i = 0; i < layout.refreshes.size(); ++i) {
            hits.push_back({kIdRefreshBase + static_cast<int>(i), layout.refreshes[i]});
        }
        hits.push_back({kIdCustomModes, layout.customButton});
        hits.push_back({kIdCustomRefresh, layout.customRefreshButton});
        hits.push_back({kIdApply, layout.applyButton});
        hits.push_back({kIdRestore, layout.restoreButton});
    }

    int HitTest(POINT point) const {
        for (const auto& hit : hits) {
            if (PtInRect(&hit.rect, point)) {
                return hit.id;
            }
        }
        if (PtInRect(&layout.dragArea, point)) {
            return kIdTitle;
        }
        return kIdNone;
    }

    void Activate(int id) {
        if (id == kIdClose) {
            DestroyWindow(hwnd);
            return;
        }
        if (id == kIdMinimize) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return;
        }
        if (id == kIdSwitchDisplay) {
            if (DisplayCount() > 1) {
                pathIndex = (pathIndex + 1) % config.paths.size();
                BuildPresets();
                BuildRefreshOptions();
                SetMessage(L"已切换到显示器 " + std::to_wstring(pathIndex + 1), Tone::Neutral);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return;
        }
        if (id == kIdCustomModes) {
            OpenCustomEditor();
            return;
        }
        if (id == kIdCustomRefresh) {
            OpenRefreshEditor();
            return;
        }
        if (id >= kIdPresetBase && id < kIdPresetBase + static_cast<int>(presets.size())) {
            selectedPreset = id - kIdPresetBase;
            message.clear();
            tone = Tone::Neutral;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (id >= kIdRefreshBase && id < kIdRefreshBase + static_cast<int>(refreshOptions.size())) {
            selectedRefresh = id - kIdRefreshBase;
            message.clear();
            tone = Tone::Neutral;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        if (id == kIdApply) {
            Apply();
        } else if (id == kIdRestore) {
            Restore();
        }
    }

    void Apply() {
        if (!CanApply()) {
            SetMessage(L"NVAPI 不可用，无法应用", Tone::Error);
            return;
        }
        if (!IsDirty()) {
            SetMessage(L"当前模式已经是最新", Tone::Neutral);
            return;
        }

        const UINT width = SelectedWidth();
        const UINT height = SelectedHeight();
        const UINT refresh = SelectedRefresh();
        std::string error;
        if (!SaveBackup(config, error)) {
            SetMessage(L"无法保存恢复点，已取消：" + Utf8ToWide(error), Tone::Error);
            return;
        }
        if (!config.SetMode(pathIndex, width, height, refresh)) {
            SetMessage(L"无法修改当前显示路径", Tone::Error);
            return;
        }

        SetMessage(L"正在应用 " + FormatMode(width, height, refresh) + L"…", Tone::Info);
        UpdateWindow(hwnd);

        const NvAPI_Status status = api.SetDisplayConfig(config.count, config.paths.data(), 0);
        if (status != NVAPI_OK) {
            const std::wstring detail = Utf8ToWide(api.ErrorText(status));
            ReloadConfig();
            SetMessage(L"应用失败：" + detail, Tone::Error);
            return;
        }

        Sleep(450);
        if (!ReloadConfig()) {
            SetMessage(L"模式已设置，但重新读取配置失败", Tone::Error);
            return;
        }
        SetMessage(L"已应用：" + FormatMode(CurrentWidth(), CurrentHeight(), CurrentRefresh()),
                   Tone::Success);
    }

    void Restore() {
        if (!apiReady) {
            SetMessage(L"NVAPI 不可用，无法恢复", Tone::Error);
            return;
        }
        std::vector<SavedMode> saved;
        std::string error;
        if (!ReadBackup(saved, error)) {
            SetMessage(L"恢复失败：" + Utf8ToWide(error), Tone::Error);
            return;
        }

        DisplayConfig target;
        if (!target.Load(api, error)) {
            SetMessage(L"读取当前显示配置失败：" + Utf8ToWide(error), Tone::Error);
            return;
        }

        int applied = 0;
        for (const auto& mode : saved) {
            std::size_t index = target.paths.size();
            if (mode.displayId != 0) {
                for (std::size_t i = 0; i < target.paths.size(); ++i) {
                    for (NvU32 j = 0; j < target.paths[i].targetInfoCount; ++j) {
                        if (target.paths[i].targetInfo[j].displayId == mode.displayId) {
                            index = i;
                            break;
                        }
                    }
                    if (index != target.paths.size()) {
                        break;
                    }
                }
            }
            if (index == target.paths.size() && mode.pathIndex < target.paths.size()) {
                index = mode.pathIndex;
            }
            if (index == target.paths.size()) {
                continue;
            }
            if (target.SetMode(index, mode.width, mode.height, mode.refreshMilliHz)) {
                ++applied;
            }
        }

        if (applied == 0) {
            SetMessage(L"没有可恢复的显示模式", Tone::Error);
            return;
        }

        SetMessage(L"正在恢复…", Tone::Info);
        UpdateWindow(hwnd);
        const NvAPI_Status status = api.SetDisplayConfig(target.count, target.paths.data(), 0);
        if (status != NVAPI_OK) {
            SetMessage(L"恢复失败：" + Utf8ToWide(api.ErrorText(status)), Tone::Error);
            return;
        }

        Sleep(450);
        if (!ReloadConfig()) {
            SetMessage(L"已恢复，但重新读取配置失败", Tone::Error);
            return;
        }
        SetMessage(L"已恢复上一次保存的模式", Tone::Success);
    }

    void Paint(HDC hdc) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right;
        const int height = client.bottom;
        if (width <= 0 || height <= 0) {
            return;
        }

        Bitmap buffer(width, height, PixelFormat32bppPARGB);
        if (buffer.GetLastStatus() != Ok) {
            return;
        }
        Graphics graphics(&buffer);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        DrawBackground(graphics, width, height);
        DrawHeader(graphics);
        DrawDisplayCard(graphics);
        DrawResolutionCard(graphics);
        DrawRefreshCard(graphics);
        DrawBottom(graphics, width, height);

        Graphics screen(hdc);
        screen.DrawImage(&buffer, 0, 0);
    }

private:
    void DrawBackground(Graphics& graphics, int width, int height) {
        LinearGradientBrush background(Rect(0, 0, width, height),
                                       Color(255, 248, 250, 253),
                                       Color(255, 238, 242, 249),
                                       LinearGradientModeForwardDiagonal);
        graphics.FillRectangle(&background, 0, 0, width, height);

        DrawBlob(graphics, RectF(static_cast<REAL>(width) - SF(290.0f), SF(-150.0f),
                                 SF(430.0f), SF(430.0f)),
                 Color(58, 90, 160, 255));
        DrawBlob(graphics, RectF(SF(-160.0f), static_cast<REAL>(height) - SF(250.0f),
                                 SF(380.0f), SF(380.0f)),
                 Color(42, 142, 120, 255));
        DrawBlob(graphics, RectF(static_cast<REAL>(width) * 0.46f, SF(240.0f), SF(260.0f),
                                 SF(260.0f)),
                 Color(24, 255, 120, 190));
    }

    void DrawHeader(Graphics& graphics) {
        const RectF icon(SF(32.0f), SF(30.0f), SF(44.0f), SF(44.0f));
        GraphicsPath iconPath;
        AddRoundedRect(iconPath, icon, SF(14.0f));
        LinearGradientBrush iconBrush(icon, Color(255, 64, 156, 255), Color(255, 0, 102, 255),
                                      LinearGradientModeVertical);
        graphics.FillPath(&iconBrush, &iconPath);
        DrawMonitorGlyph(graphics, RectF(icon.X + SF(9.0f), icon.Y + SF(8.0f), SF(26.0f),
                                         SF(28.0f)),
                         Color(255, 255, 255, 255), SF(1.8f));

        Font titleFont(UiFamily(), SF(26.0f), FontStyleBold, UnitPixel);
        DrawText(graphics, L"显示设置", titleFont, Color(255, 17, 24, 39),
                 SF(92.0f), SF(24.0f), SF(330.0f), SF(38.0f));

        Font subtitleFont(UiFamily(), SF(11.5f), FontStyleRegular, UnitPixel);
        std::wstring subtitle = L"通过 NVIDIA NVAPI 调整显示器模式";
        if (!gpuName.empty()) {
            subtitle += L"  ·  ";
            subtitle += gpuName;
        }
        DrawText(graphics, subtitle, subtitleFont, Color(255, 107, 114, 128),
                 SF(93.0f), SF(64.0f), SF(500.0f), SF(20.0f));

        const bool ok = apiReady && configReady;
        const RectF chip(static_cast<REAL>(layout.minimizeButton.left) - SF(120.0f), SF(37.0f),
                         SF(108.0f), SF(26.0f));
        FillRounded(graphics, chip, SF(13.0f), Color(190, 255, 255, 255));
        StrokeRounded(graphics, chip, SF(13.0f), Color(90, 15, 23, 42), SF(1.0f));
        const Color dotColor = ok ? Color(255, 34, 197, 94) : Color(255, 239, 68, 68);
        SolidBrush dotBrush(dotColor);
        graphics.FillEllipse(&dotBrush, chip.X + SF(10.0f), chip.Y + SF(9.0f), SF(7.0f),
                             SF(7.0f));
        Font chipFont(UiFamily(), SF(9.5f), FontStyleRegular, UnitPixel);
        DrawText(graphics, ok ? L"NVAPI 已连接" : L"NVAPI 异常", chipFont,
                 ok ? Color(255, 22, 101, 52) : Color(255, 153, 27, 27), chip.X + SF(23.0f),
                 chip.Y, chip.Width - SF(26.0f), chip.Height, StringAlignmentNear,
                 StringAlignmentCenter);

        DrawCircularButton(graphics, layout.closeButton, kIdClose, true);
        DrawCircularButton(graphics, layout.minimizeButton, kIdMinimize, false);
    }

    void DrawCircularButton(Graphics& graphics, const RECT& rect, int id, bool close) {
        const bool isHot = hot == id;
        const bool isPressed = pressed == id;
        const Color fill = isPressed ? (close ? Color(255, 255, 215, 215)
                                             : Color(255, 222, 227, 234))
                                     : (isHot ? (close ? Color(255, 255, 236, 236)
                                                       : Color(255, 244, 247, 251))
                                              : Color(210, 255, 255, 255));
        const Color border = close && isHot ? Color(120, 239, 68, 68)
                                            : Color(70, 15, 23, 42);
        FillRounded(graphics, ToRectF(rect), SF(18.0f), fill);
        StrokeRounded(graphics, ToRectF(rect), SF(18.0f), border, SF(1.0f));
        const Color glyph = close && isHot ? Color(255, 220, 38, 38)
                                           : Color(255, 75, 85, 99);
        if (close) {
            DrawCloseGlyph(graphics, ToRectF(rect), glyph, SF(1.8f));
        } else {
            DrawMinimizeGlyph(graphics, ToRectF(rect), glyph, SF(1.8f));
        }
    }

    void DrawDisplayCard(Graphics& graphics) {
        const RectF card = ToRectF(layout.displayCard);
        DrawCard(graphics, card, SF(20.0f));

        const RectF icon(card.X + SF(24.0f), card.Y + SF(24.0f), SF(56.0f), SF(56.0f));
        FillRounded(graphics, icon, SF(18.0f), Color(255, 234, 242, 255));
        DrawMonitorGlyph(graphics, RectF(icon.X + SF(12.0f), icon.Y + SF(13.0f), SF(32.0f),
                                         SF(30.0f)),
                         Color(255, 10, 132, 255), SF(2.0f));

        const float textX = icon.GetRight() + SF(16.0f);
        const float dividerX = card.GetRight() - SF(300.0f);
        Font nameFont(UiFamily(), SF(17.0f), FontStyleBold, UnitPixel);
        Font smallFont(UiFamily(), SF(10.0f), FontStyleRegular, UnitPixel);
        std::wstring name = L"显示器 " + std::to_wstring(pathIndex + 1);
        if (DisplayCount() > 1) {
            name += L" / " + std::to_wstring(DisplayCount());
        }
        DrawText(graphics, name, nameFont, Color(255, 17, 24, 39), textX,
                 card.Y + SF(22.0f), dividerX - textX - SF(16.0f), SF(26.0f));

        wchar_t idBuffer[64]{};
        swprintf(idBuffer, 64, L"Display ID 0x%08X", CurrentDisplayId());
        DrawText(graphics, idBuffer, smallFont, Color(255, 107, 114, 128), textX,
                 card.Y + SF(48.0f), dividerX - textX - SF(16.0f), SF(18.0f));

        if (DisplayCount() > 1) {
            DrawSwitchButton(graphics);
        } else if (!gpuName.empty()) {
            DrawText(graphics, gpuName, smallFont, Color(255, 156, 163, 175), textX,
                     card.Y + SF(72.0f), dividerX - textX - SF(16.0f), SF(18.0f));
        }

        Pen divider(Color(28, 15, 23, 42), SF(1.0f));
        graphics.DrawLine(&divider, dividerX, card.Y + SF(22.0f), dividerX,
                          card.GetBottom() - SF(22.0f));

        const bool ok = apiReady && configReady;
        const RectF pill(card.GetRight() - SF(106.0f), card.Y + SF(20.0f), SF(82.0f),
                         SF(24.0f));
        FillRounded(graphics, pill, SF(12.0f),
                    ok ? Color(255, 232, 248, 238) : Color(255, 254, 226, 226));
        SolidBrush pillDot(ok ? Color(255, 34, 197, 94) : Color(255, 239, 68, 68));
        graphics.FillEllipse(&pillDot, pill.X + SF(11.0f), pill.Y + SF(9.0f), SF(6.0f),
                             SF(6.0f));
        Font pillFont(UiFamily(), SF(9.5f), FontStyleRegular, UnitPixel);
        DrawText(graphics, ok ? L"已连接" : L"未连接", pillFont,
                 ok ? Color(255, 22, 101, 52) : Color(255, 153, 27, 27),
                 pill.X + SF(22.0f), pill.Y, pill.Width - SF(24.0f), pill.Height,
                 StringAlignmentNear, StringAlignmentCenter);

        const float rightX = dividerX + SF(24.0f);
        Font labelFont(UiFamily(), SF(9.5f), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"当前模式", labelFont, Color(255, 156, 163, 175), rightX,
                 card.Y + SF(52.0f), SF(120.0f), SF(16.0f));

        Font modeFont(UiFamily(), SF(21.0f), FontStyleBold, UnitPixel);
        DrawText(graphics, FormatResolution(CurrentWidth(), CurrentHeight()), modeFont,
                 Color(255, 17, 24, 39), rightX, card.Y + SF(68.0f), SF(260.0f),
                 SF(30.0f));

        Font metaFont(UiFamily(), SF(10.0f), FontStyleRegular, UnitPixel);
        DrawText(graphics,
                 FormatHz(CurrentRefresh()) + L" Hz  ·  " +
                     std::to_wstring(CurrentColorDepth()) + L" 位色",
                 metaFont, Color(255, 107, 114, 128), rightX, card.Y + SF(100.0f), SF(260.0f),
                 SF(18.0f));
    }

    void DrawSwitchButton(Graphics& graphics) {
        const RectF rect = ToRectF(layout.switchButton);
        const bool isHot = hot == kIdSwitchDisplay;
        FillRounded(graphics, rect, SF(9.0f),
                    isHot ? Color(255, 226, 237, 252) : Color(255, 242, 246, 252));
        Font font(UiFamily(), SF(9.5f), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"切换显示器", font, Color(255, 10, 132, 255), rect.X + SF(10.0f),
                 rect.Y, rect.Width - SF(30.0f), rect.Height, StringAlignmentNear,
                 StringAlignmentCenter);
        DrawChevronGlyph(graphics,
                         RectF(rect.GetRight() - SF(17.0f), rect.Y + SF(5.0f), SF(10.0f),
                               SF(14.0f)),
                         Color(255, 10, 132, 255), SF(1.4f));
    }

    void DrawCustomModesButton(Graphics& graphics) {
        const RectF rect = ToRectF(layout.customButton);
        const bool isHot = hot == kIdCustomModes;
        const bool isPressed = pressed == kIdCustomModes;
        FillRounded(graphics, rect, SF(9.0f),
                    isPressed ? Color(255, 219, 234, 255)
                              : (isHot ? Color(255, 232, 242, 255)
                                       : Color(255, 242, 247, 255)));
        StrokeRounded(graphics, rect, SF(9.0f), Color(255, 210, 228, 252), SF(1.0f));

        Font font(UiFamily(), SF(9.5f), FontStyleBold, UnitPixel);
        DrawText(graphics, L"自定义…", font, Color(255, 0, 102, 255), rect.X, rect.Y,
                 rect.Width, rect.Height, StringAlignmentCenter, StringAlignmentCenter);
    }

    void DrawCustomRefreshButton(Graphics& graphics) {
        const RectF rect = ToRectF(layout.customRefreshButton);
        const bool isHot = hot == kIdCustomRefresh;
        const bool isPressed = pressed == kIdCustomRefresh;
        FillRounded(graphics, rect, SF(9.0f),
                    isPressed ? Color(255, 219, 234, 255)
                              : (isHot ? Color(255, 232, 242, 255)
                                       : Color(255, 242, 247, 255)));
        StrokeRounded(graphics, rect, SF(9.0f), Color(255, 210, 228, 252), SF(1.0f));

        Font font(UiFamily(), SF(9.5f), FontStyleBold, UnitPixel);
        DrawText(graphics, L"自定义…", font, Color(255, 0, 102, 255), rect.X, rect.Y,
                 rect.Width, rect.Height, StringAlignmentCenter, StringAlignmentCenter);
    }

    void DrawResolutionCard(Graphics& graphics) {
        const RectF card = ToRectF(layout.resolutionCard);
        DrawCard(graphics, card, SF(20.0f));

        Font titleFont(UiFamily(), SF(15.5f), FontStyleBold, UnitPixel);
        Font hintFont(UiFamily(), SF(9.5f), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"分辨率", titleFont, Color(255, 17, 24, 39), card.X + SF(24.0f),
                 card.Y + SF(16.0f), SF(180.0f), SF(22.0f));
        DrawText(graphics, L"选择要应用的模式", hintFont, Color(255, 156, 163, 175),
                 card.GetRight() - SF(330.0f), card.Y + SF(20.0f), SF(180.0f), SF(18.0f),
                 StringAlignmentFar);
        DrawCustomModesButton(graphics);

        for (std::size_t i = 0; i < presets.size() && i < layout.presets.size(); ++i) {
            const bool selected = static_cast<int>(i) == selectedPreset;
            const bool isHot = hot == kIdPresetBase + static_cast<int>(i);
            const RectF rect = ToRectF(layout.presets[i]);
            Color fill = Color(255, 247, 248, 250);
            if (selected) {
                fill = Color(255, 234, 242, 255);
            } else if (isHot) {
                fill = Color(255, 240, 244, 250);
            }
            FillRounded(graphics, rect, SF(12.0f), fill);
            StrokeRounded(graphics, rect, SF(12.0f),
                          selected ? Color(255, 10, 132, 255)
                                   : Color(255, 238, 241, 245),
                          selected ? SF(1.4f) : SF(1.0f));

            Font modeFont(UiFamily(), SF(13.5f), selected ? FontStyleBold : FontStyleRegular, UnitPixel);
            Font subFont(UiFamily(), SF(9.0f), FontStyleRegular, UnitPixel);
            DrawText(graphics, presets[i].title, modeFont,
                     selected ? Color(255, 0, 102, 255) : Color(255, 31, 41, 55), rect.X + SF(13.0f),
                     rect.Y + SF(9.0f), rect.Width - SF(42.0f), SF(22.0f));
            DrawText(graphics, presets[i].subtitle, subFont,
                     selected ? Color(255, 59, 130, 246) : Color(255, 156, 163, 175),
                     rect.X + SF(13.0f), rect.Y + SF(33.0f), rect.Width - SF(26.0f),
                     SF(18.0f));

            if (selected) {
                const PointF center(rect.GetRight() - SF(18.0f), rect.Y + SF(18.0f));
                SolidBrush circle(Color(255, 10, 132, 255));
                graphics.FillEllipse(&circle, center.X - SF(8.0f), center.Y - SF(8.0f),
                                     SF(16.0f), SF(16.0f));
                DrawCheckGlyph(graphics, center, SF(10.0f), Color(255, 255, 255, 255),
                               SF(1.7f));
            }
        }

        if (presets.size() <= 4) {
            Font noteFont(UiFamily(), SF(9.0f), FontStyleRegular, UnitPixel);
            DrawText(graphics,
                     L"仅显示 1K 级常用模式；其他尺寸可通过“自定义…”添加。",
                     noteFont, Color(255, 156, 163, 175), card.X + SF(24.0f),
                     card.GetBottom() - SF(42.0f), card.Width - SF(48.0f), SF(18.0f));
        }
    }

    void DrawRefreshCard(Graphics& graphics) {
        const RectF card = ToRectF(layout.refreshCard);
        DrawCard(graphics, card, SF(20.0f));

        Font titleFont(UiFamily(), SF(15.0f), FontStyleBold, UnitPixel);
        Font hintFont(UiFamily(), SF(9.5f), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"刷新率", titleFont, Color(255, 17, 24, 39), card.X + SF(24.0f),
                 card.Y + SF(14.0f), SF(130.0f), SF(22.0f));
        DrawText(graphics, L"Hz", hintFont, Color(255, 156, 163, 175), card.X + SF(82.0f),
                 card.Y + SF(17.0f), SF(30.0f), SF(16.0f));
        DrawText(graphics, L"选择后点击“应用更改”", hintFont, Color(255, 156, 163, 175),
                 card.GetRight() - SF(370.0f), card.Y + SF(17.0f), SF(210.0f), SF(16.0f),
                 StringAlignmentFar);
        DrawCustomRefreshButton(graphics);

        for (std::size_t i = 0; i < refreshOptions.size() && i < layout.refreshes.size(); ++i) {
            const bool selected = static_cast<int>(i) == selectedRefresh;
            const bool isHot = hot == kIdRefreshBase + static_cast<int>(i);
            const RectF rect = ToRectF(layout.refreshes[i]);
            Color fill = Color(255, 247, 248, 250);
            if (selected) {
                fill = Color(255, 234, 242, 255);
            } else if (isHot) {
                fill = Color(255, 240, 244, 250);
            }
            FillRounded(graphics, rect, SF(10.0f), fill);
            StrokeRounded(graphics, rect, SF(10.0f),
                          selected ? Color(255, 10, 132, 255) : Color(255, 238, 241, 245),
                          selected ? SF(1.3f) : SF(1.0f));
            Font font(UiFamily(), SF(11.0f), selected ? FontStyleBold : FontStyleRegular, UnitPixel);
            DrawText(graphics, FormatHz(refreshOptions[i]), font,
                     selected ? Color(255, 0, 102, 255) : Color(255, 75, 85, 99), rect.X, rect.Y,
                     rect.Width, rect.Height, StringAlignmentCenter, StringAlignmentCenter);
        }
    }

    void DrawBottom(Graphics& graphics, int width, int height) {
        (void)width;
        (void)height;
        const Color color = StatusColor();
        const float top = static_cast<float>(layout.applyButton.top);
        SolidBrush dot(color);
        graphics.FillEllipse(&dot, SF(33.0f), top + SF(11.0f), SF(8.0f), SF(8.0f));

        Font statusFont(UiFamily(), SF(10.5f), FontStyleRegular, UnitPixel);
        const float statusWidth = static_cast<float>(layout.restoreButton.left) - SF(56.0f);
        DrawText(graphics, StatusText(), statusFont, color, SF(50.0f), top + SF(4.0f), statusWidth,
                 SF(24.0f), StringAlignmentNear, StringAlignmentCenter);

        DrawRestoreButton(graphics);
        DrawApplyButton(graphics);
    }

    void DrawRestoreButton(Graphics& graphics) {
        const RectF rect = ToRectF(layout.restoreButton);
        const bool isHot = hot == kIdRestore;
        const bool isPressed = pressed == kIdRestore;
        FillRounded(graphics, rect, SF(13.0f),
                    isPressed ? Color(255, 238, 242, 247)
                              : (isHot ? Color(255, 248, 250, 253) : Color(255, 255, 255, 255)));
        StrokeRounded(graphics, rect, SF(13.0f), Color(255, 224, 228, 234), SF(1.0f));

        Font font(UiFamily(), SF(11.5f), FontStyleBold, UnitPixel);
        const std::wstring text = L"恢复";
        RectF measured;
        StringFormat format;
        format.SetFormatFlags(StringFormatFlagsNoWrap);
        graphics.MeasureString(text.c_str(), -1, &font, PointF(0.0f, 0.0f), &format, &measured);
        const float total = SF(16.0f) + SF(7.0f) + measured.Width;
        const float startX = rect.X + (rect.Width - total) / 2.0f;
        DrawResetGlyph(graphics,
                       RectF(startX, rect.Y + SF(14.0f), SF(17.0f), SF(20.0f)),
                       Color(255, 75, 85, 99), SF(1.5f));
        DrawText(graphics, text, font, Color(255, 55, 65, 81), startX + SF(23.0f), rect.Y,
                 measured.Width + SF(4.0f), rect.Height, StringAlignmentNear,
                 StringAlignmentCenter);
    }

    void DrawApplyButton(Graphics& graphics) {
        const RectF rect = ToRectF(layout.applyButton);
        const bool enabled = CanApply() && IsDirty();
        const bool isHot = hot == kIdApply;
        const bool isPressed = pressed == kIdApply;

        if (enabled) {
            for (int i = 6; i >= 1; --i) {
                const float spread = SF(static_cast<float>(i) * 1.2f);
                FillRounded(graphics,
                            RectF(rect.X - spread * 0.3f, rect.Y + SF(3.0f), rect.Width + spread * 0.6f,
                                  rect.Height + spread * 0.4f),
                            SF(13.0f) + spread * 0.2f, Color(8, 10, 132, 255));
            }
            LinearGradientBrush brush(rect,
                                      isPressed ? Color(255, 0, 92, 230)
                                                : (isHot ? Color(255, 52, 154, 255)
                                                         : Color(255, 10, 132, 255)),
                                      isPressed ? Color(255, 0, 72, 200)
                                                : Color(255, 0, 102, 255),
                                      LinearGradientModeVertical);
            GraphicsPath path;
            AddRoundedRect(path, rect, SF(13.0f));
            graphics.FillPath(&brush, &path);
        } else {
            FillRounded(graphics, rect, SF(13.0f), Color(255, 232, 235, 240));
        }

        const wchar_t* label =
            !CanApply() ? L"不可用" : (enabled ? L"应用更改" : L"已是当前模式");
        Font font(UiFamily(), SF(13.0f), FontStyleBold, UnitPixel);
        DrawText(graphics, label, font,
                 enabled ? Color(255, 255, 255, 255) : Color(255, 154, 161, 172), rect.X, rect.Y,
                 rect.Width, rect.Height, StringAlignmentCenter, StringAlignmentCenter);
    }

    void DrawCard(Graphics& graphics, const RectF& card, float radius) {
        for (int i = 7; i >= 1; --i) {
            const float spread = SF(static_cast<float>(i) * 1.1f);
            FillRounded(graphics,
                        RectF(card.X - spread * 0.25f, card.Y + SF(3.0f),
                              card.Width + spread * 0.5f, card.Height + spread * 0.35f),
                        radius + spread * 0.15f, Color(6, 15, 23, 42));
        }
        FillRounded(graphics, card, radius, Color(255, 255, 255, 255));
        StrokeRounded(graphics, card, radius, Color(28, 15, 23, 42), SF(1.0f));
    }
};

App* GetApp(HWND hwnd) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

template <typename T>
T LoadUser32Function(const char* name) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return nullptr;
    }
    FARPROC proc = GetProcAddress(user32, name);
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(proc), "unexpected function pointer size");
    std::memcpy(&function, &proc, sizeof(function));
    return function;
}

void EnableDpiAwareness() {
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    auto setContext =
        LoadUser32Function<SetDpiAwarenessContextFn>("SetProcessDpiAwarenessContext");
    if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }
    using SetProcessDpiAwareFn = BOOL(WINAPI*)();
    auto setAware = LoadUser32Function<SetProcessDpiAwareFn>("SetProcessDPIAware");
    if (setAware) {
        setAware();
    }
}

UINT GetSystemDpi() {
    using GetDpiForSystemFn = UINT(WINAPI*)();
    auto getDpi = LoadUser32Function<GetDpiForSystemFn>("GetDpiForSystem");
    if (getDpi) {
        return getDpi();
    }
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc) {
        ReleaseDC(nullptr, dc);
    }
    return dpi ? dpi : 96;
}

UINT GetWindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    auto getDpi = LoadUser32Function<GetDpiForWindowFn>("GetDpiForWindow");
    if (getDpi) {
        return getDpi(hwnd);
    }
    return 96;
}

void ApplyWindowChrome(HWND hwnd) {
    BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int corner = DWMWCP_ROUND;
    const HRESULT cornerResult =
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    if (FAILED(cornerResult)) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int radius = ScaleInt(24, GetWindowDpi(hwnd));
        HRGN region = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, radius,
                                         radius);
        if (region) {
            SetWindowRgn(hwnd, region, TRUE);
        }
    }
}

// ---------------------------------------------------------------------------
// Custom-resolution editor
// ---------------------------------------------------------------------------

constexpr int kCustomEditBase = 3000;
constexpr int kCustomSave = IDOK;
constexpr int kCustomCancel = IDCANCEL;
constexpr int kCustomClose = 3006;
constexpr int kCustomEditorSlots = 3;

struct CustomEditorLayout {
    RECT client{};
    RECT closeButton{};
    RECT cancelButton{};
    RECT saveButton{};
    RECT errorLabel{};
    RECT edits[kCustomEditorSlots][2]{};
};

struct CustomEditorState {
    UINT dpi = 96;
    std::vector<CustomMode> modes;
    HWND edits[kCustomEditorSlots][2]{};
    HWND closeButton = nullptr;
    HWND cancelButton = nullptr;
    HWND saveButton = nullptr;
    HWND errorLabel = nullptr;
    CustomEditorLayout layout;
    std::wstring error;
    bool closed = false;
    bool saved = false;
    HBRUSH editBrush = nullptr;
    HFONT font = nullptr;
    HFONT errorFont = nullptr;
};

std::wstring TrimText(const std::wstring& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' || text[begin] == L'\r' ||
                           text[begin] == L'\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
                           text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

void LayoutCustomEditor(HWND hwnd, CustomEditorState& state) {
    GetClientRect(hwnd, &state.layout.client);
    const int width = state.layout.client.right;
    const int height = state.layout.client.bottom;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int margin = ScaleInt(24, state.dpi);
    const int closeSize = ScaleInt(28, state.dpi);
    state.layout.closeButton =
        MakeRect(width - margin - closeSize, ScaleInt(24, state.dpi), width - margin,
                 ScaleInt(24, state.dpi) + closeSize);
    state.layout.saveButton =
        MakeRect(width - ScaleInt(32, state.dpi) - ScaleInt(120, state.dpi),
                 ScaleInt(350, state.dpi), width - ScaleInt(32, state.dpi),
                 ScaleInt(388, state.dpi));
    state.layout.cancelButton =
        MakeRect(state.layout.saveButton.left - ScaleInt(12, state.dpi) - ScaleInt(120, state.dpi),
                 ScaleInt(350, state.dpi),
                 state.layout.saveButton.left - ScaleInt(12, state.dpi),
                 ScaleInt(388, state.dpi));
    state.layout.errorLabel =
        MakeRect(ScaleInt(36, state.dpi), ScaleInt(296, state.dpi),
                 width - ScaleInt(36, state.dpi), ScaleInt(324, state.dpi));

    for (int row = 0; row < kCustomEditorSlots; ++row) {
        const int top = ScaleInt(125 + row * 66, state.dpi);
        const int fieldHeight = ScaleInt(26, state.dpi);
        state.layout.edits[row][0] =
            MakeRect(ScaleInt(80, state.dpi), top, ScaleInt(200, state.dpi), top + fieldHeight);
        state.layout.edits[row][1] =
            MakeRect(ScaleInt(236, state.dpi), top, ScaleInt(356, state.dpi), top + fieldHeight);
    }

    if (state.edits[0][0]) {
        for (int row = 0; row < kCustomEditorSlots; ++row) {
            for (int column = 0; column < 2; ++column) {
                const RECT& rect = state.layout.edits[row][column];
                SetWindowPos(state.edits[row][column], nullptr, rect.left, rect.top,
                             rect.right - rect.left, rect.bottom - rect.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                const int fieldWidth = rect.right - rect.left;
                const int fieldHeight = rect.bottom - rect.top;
                HRGN region = CreateRoundRectRgn(0, 0, fieldWidth + 1, fieldHeight + 1,
                                                 ScaleInt(9, state.dpi), ScaleInt(9, state.dpi));
                if (region && !SetWindowRgn(state.edits[row][column], region, TRUE)) {
                    DeleteObject(region);
                }
            }
        }
    }
    if (state.cancelButton) {
        SetWindowPos(state.cancelButton, nullptr, state.layout.cancelButton.left,
                     state.layout.cancelButton.top,
                     state.layout.cancelButton.right - state.layout.cancelButton.left,
                     state.layout.cancelButton.bottom - state.layout.cancelButton.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(state.saveButton, nullptr, state.layout.saveButton.left,
                     state.layout.saveButton.top,
                     state.layout.saveButton.right - state.layout.saveButton.left,
                     state.layout.saveButton.bottom - state.layout.saveButton.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(state.closeButton, nullptr, state.layout.closeButton.left,
                     state.layout.closeButton.top,
                     state.layout.closeButton.right - state.layout.closeButton.left,
                     state.layout.closeButton.bottom - state.layout.closeButton.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (state.errorLabel) {
        SetWindowPos(state.errorLabel, nullptr, state.layout.errorLabel.left,
                     state.layout.errorLabel.top,
                     state.layout.errorLabel.right - state.layout.errorLabel.left,
                     state.layout.errorLabel.bottom - state.layout.errorLabel.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

bool ValidateCustomEditor(CustomEditorState& state) {
    std::vector<CustomMode> result;
    for (int row = 0; row < kCustomEditorSlots; ++row) {
        wchar_t widthText[32]{};
        wchar_t heightText[32]{};
        GetWindowTextW(state.edits[row][0], widthText, 32);
        GetWindowTextW(state.edits[row][1], heightText, 32);
        const std::wstring widthValue = TrimText(widthText);
        const std::wstring heightValue = TrimText(heightText);

        if (widthValue.empty() && heightValue.empty()) {
            continue;
        }
        if (widthValue.empty() || heightValue.empty()) {
            state.error = L"每个槽位需要同时填写宽度和高度，或全部留空。";
            return false;
        }

        wchar_t* widthEnd = nullptr;
        wchar_t* heightEnd = nullptr;
        const unsigned long width = std::wcstoul(widthValue.c_str(), &widthEnd, 10);
        const unsigned long height = std::wcstoul(heightValue.c_str(), &heightEnd, 10);
        if (!widthEnd || *widthEnd != L'\0' || !heightEnd || *heightEnd != L'\0') {
            state.error = L"宽度和高度必须是整数。";
            return false;
        }
        if (width < 320 || width > 16384 || height < 200 || height > 16384) {
            state.error = L"宽度需在 320–16384 之间，高度需在 200–16384 之间。";
            return false;
        }
        for (const CustomMode& existing : result) {
            if (existing.width == width && existing.height == height) {
                state.error = L"自定义分辨率不能重复。";
                return false;
            }
        }
        result.push_back({static_cast<NvU32>(width), static_cast<NvU32>(height)});
    }

    state.error.clear();
    state.modes = std::move(result);
    return true;
}

void PaintCustomEditor(HWND hwnd, CustomEditorState& state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &paint);
        return;
    }

    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    if (buffer.GetLastStatus() != Ok) {
        EndPaint(hwnd, &paint);
        return;
    }
    Graphics graphics(&buffer);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    LinearGradientBrush background(Rect(0, 0, width, height), Color(255, 248, 250, 253),
                                   Color(255, 238, 242, 249),
                                   LinearGradientModeForwardDiagonal);
    graphics.FillRectangle(&background, 0, 0, width, height);

    const RectF card(ScaleFloat(24.0f, state.dpi), ScaleFloat(88.0f, state.dpi),
                     static_cast<REAL>(width) - ScaleFloat(48.0f, state.dpi),
                     ScaleFloat(236.0f, state.dpi));
    for (int i = 7; i >= 1; --i) {
        const float spread = ScaleFloat(static_cast<float>(i) * 1.1f, state.dpi);
        FillRounded(graphics,
                    RectF(card.X - spread * 0.25f, card.Y + ScaleFloat(3.0f, state.dpi),
                          card.Width + spread * 0.5f, card.Height + spread * 0.35f),
                    ScaleFloat(18.0f, state.dpi) + spread * 0.15f, Color(6, 15, 23, 42));
    }
    FillRounded(graphics, card, ScaleFloat(18.0f, state.dpi), Color(255, 255, 255, 255));
    StrokeRounded(graphics, card, ScaleFloat(18.0f, state.dpi), Color(28, 15, 23, 42),
                  ScaleFloat(1.0f, state.dpi));

    for (int row = 0; row < kCustomEditorSlots; ++row) {
        for (int column = 0; column < 2; ++column) {
            RectF field = ToRectF(state.layout.edits[row][column]);
            field.Inflate(1.5f, 3.0f);
            FillRounded(graphics, field, ScaleFloat(9.0f, state.dpi), Color(255, 255, 255, 255));
            StrokeRounded(graphics, field, ScaleFloat(9.0f, state.dpi),
                          Color(255, 211, 218, 227), ScaleFloat(1.2f, state.dpi));
        }
    }

    Font titleFont(UiFamily(), ScaleFloat(20.0f, state.dpi), FontStyleBold, UnitPixel);
    DrawText(graphics, L"自定义分辨率", titleFont, Color(255, 17, 24, 39),
             ScaleFloat(32.0f, state.dpi), ScaleFloat(22.0f, state.dpi),
             ScaleFloat(300.0f, state.dpi), ScaleFloat(30.0f, state.dpi));

    Font subtitleFont(UiFamily(), ScaleFloat(9.5f, state.dpi), FontStyleRegular, UnitPixel);
    DrawText(graphics, L"最多保存 3 个常用模式；留空表示不使用该槽位", subtitleFont,
             Color(255, 107, 114, 128), ScaleFloat(33.0f, state.dpi),
             ScaleFloat(54.0f, state.dpi), ScaleFloat(430.0f, state.dpi),
             ScaleFloat(18.0f, state.dpi));

    if (state.error.empty()) {
        Font hintFont(UiFamily(), ScaleFloat(9.5f, state.dpi), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"刷新率仍在主界面选择", hintFont, Color(255, 156, 163, 175),
                 ScaleFloat(36.0f, state.dpi), ScaleFloat(356.0f, state.dpi),
                 ScaleFloat(220.0f, state.dpi), ScaleFloat(24.0f, state.dpi),
                 StringAlignmentNear, StringAlignmentCenter);
    }

    for (int row = 0; row < kCustomEditorSlots; ++row) {
        const int captionY = 104 + row * 66;
        const int badgeY = 122 + row * 66;
        const int fieldY = 122 + row * 66;
        const RectF badge(ScaleFloat(44.0f, state.dpi), ScaleFloat(static_cast<float>(badgeY + 4),
                                                                  state.dpi),
                          ScaleFloat(24.0f, state.dpi), ScaleFloat(24.0f, state.dpi));
        FillRounded(graphics, badge, ScaleFloat(12.0f, state.dpi), Color(255, 234, 242, 255));
        wchar_t badgeText[8]{};
        swprintf(badgeText, 8, L"%02d", row + 1);
        Font badgeFont(UiFamily(), ScaleFloat(9.0f, state.dpi), FontStyleBold, UnitPixel);
        DrawText(graphics, badgeText, badgeFont, Color(255, 0, 102, 255), badge.X, badge.Y,
                 badge.Width, badge.Height, StringAlignmentCenter, StringAlignmentCenter);

        Font captionFont(UiFamily(), ScaleFloat(8.5f, state.dpi), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"宽度", captionFont, Color(255, 156, 163, 175),
                 ScaleFloat(80.0f, state.dpi), ScaleFloat(static_cast<float>(captionY), state.dpi),
                 ScaleFloat(120.0f, state.dpi), ScaleFloat(14.0f, state.dpi));
        DrawText(graphics, L"高度", captionFont, Color(255, 156, 163, 175),
                 ScaleFloat(236.0f, state.dpi), ScaleFloat(static_cast<float>(captionY), state.dpi),
                 ScaleFloat(120.0f, state.dpi), ScaleFloat(14.0f, state.dpi));

        Font timesFont(UiFamily(), ScaleFloat(14.0f, state.dpi), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"×", timesFont, Color(255, 156, 163, 175),
                 ScaleFloat(206.0f, state.dpi), ScaleFloat(static_cast<float>(fieldY), state.dpi),
                 ScaleFloat(26.0f, state.dpi), ScaleFloat(32.0f, state.dpi),
                 StringAlignmentCenter, StringAlignmentCenter);
        Font unitFont(UiFamily(), ScaleFloat(9.0f, state.dpi), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"px", unitFont, Color(255, 156, 163, 175),
                 ScaleFloat(360.0f, state.dpi), ScaleFloat(static_cast<float>(fieldY), state.dpi),
                 ScaleFloat(32.0f, state.dpi), ScaleFloat(32.0f, state.dpi),
                 StringAlignmentNear, StringAlignmentCenter);
    }

    Graphics screen(dc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &paint);
}

void DrawCustomEditorButton(const DRAWITEMSTRUCT* draw, UINT dpi) {
    if (!draw || !draw->hDC) {
        return;
    }
    Graphics graphics(draw->hDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const RectF rect(static_cast<REAL>(draw->rcItem.left), static_cast<REAL>(draw->rcItem.top),
                     static_cast<REAL>(draw->rcItem.right - draw->rcItem.left),
                     static_cast<REAL>(draw->rcItem.bottom - draw->rcItem.top));
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool close = draw->CtlID == kCustomClose;

    if (close) {
        const float inset = ScaleFloat(3.0f, dpi);
        FillRounded(graphics,
                    RectF(rect.X + inset, rect.Y + inset, rect.Width - inset * 2.0f,
                          rect.Height - inset * 2.0f),
                    ScaleFloat(14.0f, dpi),
                    pressed ? Color(255, 255, 232, 232) : Color(220, 255, 255, 255));
        StrokeRounded(graphics,
                      RectF(rect.X + inset, rect.Y + inset, rect.Width - inset * 2.0f,
                            rect.Height - inset * 2.0f),
                      ScaleFloat(14.0f, dpi), Color(70, 15, 23, 42), ScaleFloat(1.0f, dpi));
        DrawCloseGlyph(graphics, rect, Color(255, 75, 85, 99), ScaleFloat(1.6f, dpi));
        return;
    }

    if (draw->CtlID == kCustomSave) {
        LinearGradientBrush brush(rect,
                                  pressed ? Color(255, 0, 92, 230) : Color(255, 10, 132, 255),
                                  pressed ? Color(255, 0, 72, 200) : Color(255, 0, 102, 255),
                                  LinearGradientModeVertical);
        GraphicsPath path;
        AddRoundedRect(path, rect, ScaleFloat(10.0f, dpi));
        graphics.FillPath(&brush, &path);
    } else {
        FillRounded(graphics, rect, ScaleFloat(10.0f, dpi), Color(255, 255, 255, 255));
        StrokeRounded(graphics, rect, ScaleFloat(10.0f, dpi), Color(255, 224, 228, 234),
                      ScaleFloat(1.0f, dpi));
    }

    Font font(UiFamily(), ScaleFloat(11.5f, dpi), FontStyleBold, UnitPixel);
    DrawText(graphics, draw->CtlID == kCustomSave ? L"保存" : L"取消", font,
             draw->CtlID == kCustomSave ? Color(255, 255, 255, 255) : Color(255, 55, 65, 81),
             rect, StringAlignmentCenter, StringAlignmentCenter);
}

void UpdateCustomEditorError(CustomEditorState& state) {
    if (!state.errorLabel) {
        return;
    }
    if (state.error.empty()) {
        ShowWindow(state.errorLabel, SW_HIDE);
        return;
    }
    SetWindowTextW(state.errorLabel, state.error.c_str());
    ShowWindow(state.errorLabel, SW_SHOW);
}

void CreateCustomEditorControls(HWND hwnd, CustomEditorState& state) {
    state.font = CreateFontW(-ScaleInt(14, state.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFamily());
    state.errorFont = CreateFontW(-ScaleInt(12, state.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFamily());
    state.editBrush = CreateSolidBrush(RGB(255, 255, 255));

    for (int row = 0; row < kCustomEditorSlots; ++row) {
        for (int column = 0; column < 2; ++column) {
            const int index = row * 2 + column;
            state.edits[row][column] = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomEditBase + index)),
                GetModuleHandleW(nullptr), nullptr);
            if (state.edits[row][column]) {
                SendMessageW(state.edits[row][column], WM_SETFONT,
                             reinterpret_cast<WPARAM>(state.font), TRUE);
                SendMessageW(state.edits[row][column], EM_SETLIMITTEXT, 5, 0);
                SendMessageW(state.edits[row][column], EM_SETMARGINS,
                             EC_LEFTMARGIN | EC_RIGHTMARGIN,
                             MAKELPARAM(ScaleInt(8, state.dpi), ScaleInt(8, state.dpi)));
                if (row < static_cast<int>(state.modes.size())) {
                    const CustomMode& mode = state.modes[static_cast<std::size_t>(row)];
                    const NvU32 value = column == 0 ? mode.width : mode.height;
                    if (value > 0) {
                        const std::wstring text = std::to_wstring(value);
                        SetWindowTextW(state.edits[row][column], text.c_str());
                    }
                }
            }
        }
    }

    state.cancelButton = CreateWindowExW(
        0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomCancel)),
        GetModuleHandleW(nullptr), nullptr);
    state.saveButton = CreateWindowExW(
        0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomSave)),
        GetModuleHandleW(nullptr), nullptr);
    state.closeButton = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomClose)), GetModuleHandleW(nullptr),
        nullptr);

    for (HWND button : {state.cancelButton, state.saveButton, state.closeButton}) {
        if (button) {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        }
    }

    state.errorLabel = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, 0, 10, 10, hwnd,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (state.errorLabel) {
        SendMessageW(state.errorLabel, WM_SETFONT, reinterpret_cast<WPARAM>(state.errorFont), TRUE);
        ShowWindow(state.errorLabel, SW_HIDE);
    }
}

LRESULT CALLBACK CustomEditorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    CustomEditorState* state =
        reinterpret_cast<CustomEditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<CustomEditorState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->dpi = GetWindowDpi(hwnd);
        CreateCustomEditorControls(hwnd, *state);
        LayoutCustomEditor(hwnd, *state);
        if (state->edits[0][0]) {
            SetFocus(state->edits[0][0]);
        }
        return 0;
    }

    if (!state) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintCustomEditor(hwnd, *state);
        return 0;

    case WM_SIZE:
        LayoutCustomEditor(hwnd, *state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT client = point;
        ScreenToClient(hwnd, &client);
        if (client.y >= 0 && client.y < ScaleInt(80, state->dpi) &&
            !PtInRect(&state->layout.closeButton, client)) {
            return HTCAPTION;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            SetCursor(LoadCursorW(nullptr, PtInRect(&state->layout.closeButton, point)
                                               ? MAKEINTRESOURCEW(32649)
                                               : MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(31, 41, 55));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(state->editBrush);
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(220, 38, 38));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(state->editBrush);
    }

    case WM_DRAWITEM:
        DrawCustomEditorButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), state->dpi);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == kCustomSave) {
            if (ValidateCustomEditor(*state)) {
                state->saved = true;
                state->closed = true;
                DestroyWindow(hwnd);
            } else {
                UpdateCustomEditorError(*state);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (LOWORD(wParam) == kCustomCancel || LOWORD(wParam) == kCustomClose) {
            state->closed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        state->closed = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (state->font) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        if (state->editBrush) {
            DeleteObject(state->editBrush);
            state->editBrush = nullptr;
        }
        if (state->errorFont) {
            DeleteObject(state->errorFont);
            state->errorFont = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ShowCustomEditor(HWND owner, UINT dpi, std::vector<CustomMode>& modes) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = CustomEditorProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = L"NvChangeCustomEditorWindow";
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        registered = true;
    }

    CustomEditorState state;
    state.dpi = dpi;
    state.modes = modes;
    state.modes.resize(kCustomEditorSlots);

    const int width = ScaleInt(560, dpi);
    const int height = ScaleInt(420, dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
                                  L"NvChangeCustomEditorWindow", L"自定义分辨率",
                                  WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (!dialog) {
        return false;
    }
    ApplyWindowChrome(dialog);

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG message{};
    while (!state.closed && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == VK_ESCAPE) {
                SendMessageW(dialog, WM_CLOSE, 0, 0);
                continue;
            }
            if (message.wParam == VK_RETURN) {
                SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kCustomSave, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.saved) {
        modes = std::move(state.modes);
    }
    return state.saved;
}

// ---------------------------------------------------------------------------
// Custom-refresh editor
// ---------------------------------------------------------------------------

constexpr int kRefreshEditId = 3200;
constexpr int kRefreshSaveId = IDOK;
constexpr int kRefreshCancelId = IDCANCEL;
constexpr int kRefreshCloseId = 3201;

struct RefreshEditorLayout {
    RECT client{};
    RECT closeButton{};
    RECT cancelButton{};
    RECT saveButton{};
    RECT edit{};
    RECT errorLabel{};
};

struct RefreshEditorState {
    UINT dpi = 96;
    UINT hz = 0;
    HWND edit = nullptr;
    HWND closeButton = nullptr;
    HWND cancelButton = nullptr;
    HWND saveButton = nullptr;
    HWND errorLabel = nullptr;
    RefreshEditorLayout layout;
    std::wstring error;
    bool closed = false;
    bool saved = false;
    HBRUSH editBrush = nullptr;
    HFONT font = nullptr;
    HFONT errorFont = nullptr;
};

void LayoutRefreshEditor(HWND hwnd, RefreshEditorState& state) {
    GetClientRect(hwnd, &state.layout.client);
    const int width = state.layout.client.right;
    const int height = state.layout.client.bottom;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int margin = ScaleInt(24, state.dpi);
    const int closeSize = ScaleInt(28, state.dpi);
    state.layout.closeButton =
        MakeRect(width - margin - closeSize, ScaleInt(22, state.dpi), width - margin,
                 ScaleInt(22, state.dpi) + closeSize);
    state.layout.edit =
        MakeRect(ScaleInt(104, state.dpi), ScaleInt(98, state.dpi),
                 width - ScaleInt(76, state.dpi), ScaleInt(130, state.dpi));
    state.layout.saveButton =
        MakeRect(width - ScaleInt(32, state.dpi) - ScaleInt(120, state.dpi),
                 ScaleInt(196, state.dpi), width - ScaleInt(32, state.dpi),
                 ScaleInt(232, state.dpi));
    state.layout.cancelButton =
        MakeRect(state.layout.saveButton.left - ScaleInt(12, state.dpi) - ScaleInt(120, state.dpi),
                 ScaleInt(196, state.dpi),
                 state.layout.saveButton.left - ScaleInt(12, state.dpi),
                 ScaleInt(232, state.dpi));
    state.layout.errorLabel =
        MakeRect(ScaleInt(36, state.dpi), ScaleInt(156, state.dpi),
                 width - ScaleInt(36, state.dpi), ScaleInt(180, state.dpi));

    if (state.edit) {
        SetWindowPos(state.edit, nullptr, state.layout.edit.left, state.layout.edit.top,
                     state.layout.edit.right - state.layout.edit.left,
                     state.layout.edit.bottom - state.layout.edit.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        const int fieldWidth = state.layout.edit.right - state.layout.edit.left;
        const int fieldHeight = state.layout.edit.bottom - state.layout.edit.top;
        HRGN region = CreateRoundRectRgn(0, 0, fieldWidth + 1, fieldHeight + 1,
                                         ScaleInt(8, state.dpi), ScaleInt(8, state.dpi));
        if (region && !SetWindowRgn(state.edit, region, TRUE)) {
            DeleteObject(region);
        }
    }
    if (state.cancelButton) {
        SetWindowPos(state.cancelButton, nullptr, state.layout.cancelButton.left,
                     state.layout.cancelButton.top,
                     state.layout.cancelButton.right - state.layout.cancelButton.left,
                     state.layout.cancelButton.bottom - state.layout.cancelButton.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(state.saveButton, nullptr, state.layout.saveButton.left,
                     state.layout.saveButton.top,
                     state.layout.saveButton.right - state.layout.saveButton.left,
                     state.layout.saveButton.bottom - state.layout.saveButton.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(state.closeButton, nullptr, state.layout.closeButton.left,
                     state.layout.closeButton.top,
                     state.layout.closeButton.right - state.layout.closeButton.left,
                     state.layout.closeButton.bottom - state.layout.closeButton.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (state.errorLabel) {
        SetWindowPos(state.errorLabel, nullptr, state.layout.errorLabel.left,
                     state.layout.errorLabel.top,
                     state.layout.errorLabel.right - state.layout.errorLabel.left,
                     state.layout.errorLabel.bottom - state.layout.errorLabel.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void PaintRefreshEditor(HWND hwnd, RefreshEditorState& state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &paint);
        return;
    }

    Bitmap buffer(width, height, PixelFormat32bppPARGB);
    if (buffer.GetLastStatus() != Ok) {
        EndPaint(hwnd, &paint);
        return;
    }
    Graphics graphics(&buffer);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

    LinearGradientBrush background(Rect(0, 0, width, height), Color(255, 248, 250, 253),
                                   Color(255, 238, 242, 249),
                                   LinearGradientModeForwardDiagonal);
    graphics.FillRectangle(&background, 0, 0, width, height);

    const RectF card(ScaleFloat(24.0f, state.dpi), ScaleFloat(84.0f, state.dpi),
                     static_cast<REAL>(width) - ScaleFloat(48.0f, state.dpi),
                     ScaleFloat(64.0f, state.dpi));
    for (int i = 7; i >= 1; --i) {
        const float spread = ScaleFloat(static_cast<float>(i) * 1.1f, state.dpi);
        FillRounded(graphics,
                    RectF(card.X - spread * 0.25f, card.Y + ScaleFloat(3.0f, state.dpi),
                          card.Width + spread * 0.5f, card.Height + spread * 0.35f),
                    ScaleFloat(16.0f, state.dpi) + spread * 0.15f, Color(6, 15, 23, 42));
    }
    FillRounded(graphics, card, ScaleFloat(16.0f, state.dpi), Color(255, 255, 255, 255));
    StrokeRounded(graphics, card, ScaleFloat(16.0f, state.dpi), Color(28, 15, 23, 42),
                  ScaleFloat(1.0f, state.dpi));

    RectF field = ToRectF(state.layout.edit);
    field.Inflate(1.5f, 3.0f);
    FillRounded(graphics, field, ScaleFloat(9.0f, state.dpi), Color(255, 255, 255, 255));
    StrokeRounded(graphics, field, ScaleFloat(9.0f, state.dpi), Color(255, 211, 218, 227),
                  ScaleFloat(1.2f, state.dpi));

    Font titleFont(UiFamily(), ScaleFloat(20.0f, state.dpi), FontStyleBold, UnitPixel);
    DrawText(graphics, L"自定义刷新率", titleFont, Color(255, 17, 24, 39),
             ScaleFloat(32.0f, state.dpi), ScaleFloat(22.0f, state.dpi),
             ScaleFloat(300.0f, state.dpi), ScaleFloat(30.0f, state.dpi));

    Font subtitleFont(UiFamily(), ScaleFloat(9.5f, state.dpi), FontStyleRegular, UnitPixel);
    DrawText(graphics, L"输入 1–1000 Hz 的整数，保存后添加到刷新率选项", subtitleFont,
             Color(255, 107, 114, 128), ScaleFloat(33.0f, state.dpi),
             ScaleFloat(54.0f, state.dpi), ScaleFloat(350.0f, state.dpi),
             ScaleFloat(18.0f, state.dpi));

    Font labelFont(UiFamily(), ScaleFloat(10.0f, state.dpi), FontStyleRegular, UnitPixel);
    DrawText(graphics, L"刷新率", labelFont, Color(255, 107, 114, 128),
             ScaleFloat(40.0f, state.dpi), ScaleFloat(100.0f, state.dpi),
             ScaleFloat(56.0f, state.dpi), ScaleFloat(24.0f, state.dpi),
             StringAlignmentNear, StringAlignmentCenter);

    Font unitFont(UiFamily(), ScaleFloat(10.0f, state.dpi), FontStyleRegular, UnitPixel);
    DrawText(graphics, L"Hz", unitFont, Color(255, 156, 163, 175),
             static_cast<REAL>(state.layout.edit.right) + ScaleFloat(10.0f, state.dpi),
             ScaleFloat(98.0f, state.dpi), ScaleFloat(34.0f, state.dpi),
             ScaleFloat(32.0f, state.dpi), StringAlignmentNear, StringAlignmentCenter);

    if (state.error.empty()) {
        Font hintFont(UiFamily(), ScaleFloat(9.5f, state.dpi), FontStyleRegular, UnitPixel);
        DrawText(graphics, L"实际可用范围取决于显示器和驱动", hintFont,
                 Color(255, 156, 163, 175), ScaleFloat(36.0f, state.dpi),
                 ScaleFloat(158.0f, state.dpi), ScaleFloat(320.0f, state.dpi),
                 ScaleFloat(20.0f, state.dpi), StringAlignmentNear, StringAlignmentCenter);
    }

    Graphics screen(dc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &paint);
}

void DrawRefreshEditorButton(const DRAWITEMSTRUCT* draw, UINT dpi) {
    if (!draw || !draw->hDC) {
        return;
    }
    Graphics graphics(draw->hDC);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const RectF rect(static_cast<REAL>(draw->rcItem.left), static_cast<REAL>(draw->rcItem.top),
                     static_cast<REAL>(draw->rcItem.right - draw->rcItem.left),
                     static_cast<REAL>(draw->rcItem.bottom - draw->rcItem.top));
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;

    if (draw->CtlID == kRefreshCloseId) {
        const float inset = ScaleFloat(3.0f, dpi);
        FillRounded(graphics,
                    RectF(rect.X + inset, rect.Y + inset, rect.Width - inset * 2.0f,
                          rect.Height - inset * 2.0f),
                    ScaleFloat(14.0f, dpi),
                    pressed ? Color(255, 255, 232, 232) : Color(220, 255, 255, 255));
        StrokeRounded(graphics,
                      RectF(rect.X + inset, rect.Y + inset, rect.Width - inset * 2.0f,
                            rect.Height - inset * 2.0f),
                      ScaleFloat(14.0f, dpi), Color(70, 15, 23, 42), ScaleFloat(1.0f, dpi));
        DrawCloseGlyph(graphics, rect, Color(255, 75, 85, 99), ScaleFloat(1.6f, dpi));
        return;
    }

    if (draw->CtlID == kRefreshSaveId) {
        LinearGradientBrush brush(rect,
                                  pressed ? Color(255, 0, 92, 230) : Color(255, 10, 132, 255),
                                  pressed ? Color(255, 0, 72, 200) : Color(255, 0, 102, 255),
                                  LinearGradientModeVertical);
        GraphicsPath path;
        AddRoundedRect(path, rect, ScaleFloat(10.0f, dpi));
        graphics.FillPath(&brush, &path);
    } else {
        FillRounded(graphics, rect, ScaleFloat(10.0f, dpi), Color(255, 255, 255, 255));
        StrokeRounded(graphics, rect, ScaleFloat(10.0f, dpi), Color(255, 224, 228, 234),
                      ScaleFloat(1.0f, dpi));
    }

    Font font(UiFamily(), ScaleFloat(11.5f, dpi), FontStyleBold, UnitPixel);
    DrawText(graphics, draw->CtlID == kRefreshSaveId ? L"保存" : L"取消", font,
             draw->CtlID == kRefreshSaveId ? Color(255, 255, 255, 255)
                                            : Color(255, 55, 65, 81),
             rect, StringAlignmentCenter, StringAlignmentCenter);
}

void UpdateRefreshEditorError(RefreshEditorState& state) {
    if (!state.errorLabel) {
        return;
    }
    if (state.error.empty()) {
        ShowWindow(state.errorLabel, SW_HIDE);
        return;
    }
    SetWindowTextW(state.errorLabel, state.error.c_str());
    ShowWindow(state.errorLabel, SW_SHOW);
}

void CreateRefreshEditorControls(HWND hwnd, RefreshEditorState& state) {
    state.font = CreateFontW(-ScaleInt(15, state.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFamily());
    state.errorFont = CreateFontW(-ScaleInt(12, state.dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, UiFamily());
    state.editBrush = CreateSolidBrush(RGB(255, 255, 255));

    state.edit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshEditId)), GetModuleHandleW(nullptr),
        nullptr);
    if (state.edit) {
        SendMessageW(state.edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        SendMessageW(state.edit, EM_SETLIMITTEXT, 4, 0);
        SendMessageW(state.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(ScaleInt(8, state.dpi), ScaleInt(8, state.dpi)));
        SendMessageW(state.edit, 0x1501, TRUE, reinterpret_cast<LPARAM>(L"1–1000"));
        if (state.hz > 0) {
            const std::wstring text = std::to_wstring(state.hz);
            SetWindowTextW(state.edit, text.c_str());
            SendMessageW(state.edit, EM_SETSEL, 0, -1);
        }
    }

    state.cancelButton = CreateWindowExW(
        0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshCancelId)),
        GetModuleHandleW(nullptr), nullptr);
    state.saveButton = CreateWindowExW(
        0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshSaveId)),
        GetModuleHandleW(nullptr), nullptr);
    state.closeButton = CreateWindowExW(
        0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshCloseId)),
        GetModuleHandleW(nullptr), nullptr);

    for (HWND button : {state.cancelButton, state.saveButton, state.closeButton}) {
        if (button) {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        }
    }

    state.errorLabel = CreateWindowExW(
        0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, 0, 10, 10, hwnd,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (state.errorLabel) {
        SendMessageW(state.errorLabel, WM_SETFONT, reinterpret_cast<WPARAM>(state.errorFont), TRUE);
        ShowWindow(state.errorLabel, SW_HIDE);
    }
}

bool ValidateRefreshEditor(RefreshEditorState& state) {
    wchar_t text[32]{};
    GetWindowTextW(state.edit, text, 32);
    const std::wstring value = TrimText(text);
    if (value.empty()) {
        state.error = L"请输入刷新率，范围 1–1000 Hz。";
        return false;
    }

    wchar_t* end = nullptr;
    const unsigned long hz = std::wcstoul(value.c_str(), &end, 10);
    if (!end || *end != L'\0' || hz < 1 || hz > 1000) {
        state.error = L"刷新率必须是 1–1000 之间的整数。";
        return false;
    }
    state.error.clear();
    state.hz = static_cast<UINT>(hz);
    return true;
}

LRESULT CALLBACK RefreshEditorProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    RefreshEditorState* state =
        reinterpret_cast<RefreshEditorState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<RefreshEditorState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->dpi = GetWindowDpi(hwnd);
        CreateRefreshEditorControls(hwnd, *state);
        LayoutRefreshEditor(hwnd, *state);
        if (state->edit) {
            SetFocus(state->edit);
        }
        return 0;
    }

    if (!state) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintRefreshEditor(hwnd, *state);
        return 0;

    case WM_SIZE:
        LayoutRefreshEditor(hwnd, *state);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        POINT client = point;
        ScreenToClient(hwnd, &client);
        if (client.y >= 0 && client.y < ScaleInt(80, state->dpi) &&
            !PtInRect(&state->layout.closeButton, client)) {
            return HTCAPTION;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            SetCursor(LoadCursorW(nullptr, PtInRect(&state->layout.closeButton, point)
                                               ? MAKEINTRESOURCEW(32649)
                                               : MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(31, 41, 55));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(state->editBrush);
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(220, 38, 38));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(state->editBrush);
    }

    case WM_DRAWITEM:
        DrawRefreshEditorButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam), state->dpi);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == kRefreshSaveId) {
            if (ValidateRefreshEditor(*state)) {
                state->saved = true;
                state->closed = true;
                DestroyWindow(hwnd);
            } else {
                UpdateRefreshEditorError(*state);
                InvalidateRect(hwnd, nullptr, FALSE);
                if (state->edit) {
                    SetFocus(state->edit);
                    SendMessageW(state->edit, EM_SETSEL, 0, -1);
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == kRefreshCancelId || LOWORD(wParam) == kRefreshCloseId) {
            state->closed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        state->closed = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (state->font) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        if (state->editBrush) {
            DeleteObject(state->editBrush);
            state->editBrush = nullptr;
        }
        if (state->errorFont) {
            DeleteObject(state->errorFont);
            state->errorFont = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ShowRefreshEditor(HWND owner, UINT dpi, UINT& hz) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = RefreshEditorProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = L"NvChangeRefreshEditorWindow";
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        registered = true;
    }

    RefreshEditorState state;
    state.dpi = dpi;
    state.hz = hz;

    const int width = ScaleInt(420, dpi);
    const int height = ScaleInt(250, dpi);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
                                  L"NvChangeRefreshEditorWindow", L"自定义刷新率",
                                  WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr,
                                  GetModuleHandleW(nullptr), &state);
    if (!dialog) {
        return false;
    }
    ApplyWindowChrome(dialog);

    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG message{};
    while (!state.closed && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == VK_ESCAPE) {
                SendMessageW(dialog, WM_CLOSE, 0, 0);
                continue;
            }
            if (message.wParam == VK_RETURN) {
                SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kRefreshSaveId, BN_CLICKED), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.saved) {
        hz = state.hz;
    }
    return state.saved;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = GetApp(hwnd);

    if (message == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->dpi = GetWindowDpi(hwnd);
        app->Relayout();
        return 0;
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        app->Paint(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_SIZE:
        app->Relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DPICHANGED: {
        app->dpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        app->Relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (app->dragging) {
            POINT cursor{};
            GetCursorPos(&cursor);
            SetWindowPos(hwnd, nullptr,
                         app->dragWindow.left + cursor.x - app->dragCursor.x,
                         app->dragWindow.top + cursor.y - app->dragCursor.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        const int id = app->HitTest(point);
        if (id != app->hot) {
            app->hot = id;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, id >= 0 ? MAKEINTRESOURCEW(32649) : MAKEINTRESOURCEW(32512)));
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (app->hot != kIdNone) {
            app->hot = kIdNone;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int id = app->HitTest(point);
        if (id == kIdTitle) {
            app->dragging = true;
            app->dragCursor = point;
            GetCursorPos(&app->dragCursor);
            GetWindowRect(hwnd, &app->dragWindow);
            SetCapture(hwnd);
            return 0;
        }
        if (id >= 0) {
            app->pressed = id;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (app->dragging) {
            app->dragging = false;
            ReleaseCapture();
            return 0;
        }
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int id = app->HitTest(point);
        const int pressed = app->pressed;
        app->pressed = kIdNone;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (pressed >= 0 && id == pressed) {
            app->Activate(id);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        app->dragging = false;
        app->pressed = kIdNone;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const int id = app->HitTest(point);
            SetCursor(LoadCursorW(nullptr, id >= 0 ? MAKEINTRESOURCEW(32649) : MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_RETURN) {
            app->Apply();
            return 0;
        }
        if (wParam == VK_F5) {
            app->ReloadConfig();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    EnableDpiAwareness();

    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    if (GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"无法初始化 GDI+。", L"NvChange", MB_ICONERROR | MB_OK);
        return 1;
    }

    App app;
    app.instance = instance;
    app.Initialize();

    HICON appIcon = static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!appIcon) {
        appIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hIcon = appIcon;
    windowClass.hIconSm = appIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"NvChangeGuiWindow";
    if (!RegisterClassExW(&windowClass)) {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    const UINT systemDpi = GetSystemDpi();
    const int width = ScaleInt(kBaseWidth, systemDpi);
    const int height = ScaleInt(kBaseHeight, systemDpi);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, windowClass.lpszClassName, L"NvChange - 显示设置",
                                WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN, x, y,
                                width, height, nullptr, nullptr, instance, &app);
    if (!hwnd) {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    if (appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
    }
    ApplyWindowChrome(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(message.wParam);
}
