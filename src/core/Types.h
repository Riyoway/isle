#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace isle {

enum class ActivityKind {
    Metric,
    Media,
    Shortcut,
    Status,
    Timer,
    Text,
};

enum class WidgetKind : int {
    AiUsage = 0,
    AppLauncher = 1,
    Commands = 2,
    System = 3,
    Music = 4,
};

struct Action {
    std::wstring id;
    std::wstring label;
    std::wstring glyph;
};

struct Activity {
    std::wstring id;
    std::wstring source;
    ActivityKind kind{ActivityKind::Status};
    std::wstring title;
    std::wstring subtitle;
    std::wstring glyph;
    std::wstring accent{L"#FFFFFF"};
    std::optional<double> progress;
    std::optional<double> value;
    std::optional<double> elapsedSeconds;
    std::optional<double> durationSeconds;
    std::wstring valueSuffix;
    std::shared_ptr<const std::vector<std::uint8_t>> artwork;
    int priority{0};
    bool pinned{false};
    bool stale{false};
    bool active{false};
    bool compactRing{false};
    std::vector<Action> actions;
    std::chrono::steady_clock::time_point updatedAt{std::chrono::steady_clock::now()};
};

struct RenderState {
    float islandWidth{230.0f};
    float islandHeight{40.0f};
    float hoverAmount{0.0f};
    float expandAmount{0.0f};
    float visibility{1.0f};
    float pressAmount{0.0f};
    float dpiScale{1.0f};
    int pressedControl{-1};
    bool expanded{false};
    bool settingsMode{false};
    bool hidden{false};
    bool expandUp{false};
    bool hasMedia{false};
    bool startWithWindows{false};
    bool hideInFullscreen{true};
    bool expandOnHover{false};
    bool showAiUsage{true};
    bool showSystemMetrics{false};
    bool showAppLauncher{true};
    bool showCommandShortcuts{true};
    bool showMusicPlayer{true};
    bool monitorAtCursor{false};
    int settingsPage{0};
    int islandSizePreset{1};
    int islandShape{0};
    int buttonStyle{0};
    int compactMediaMode{2};
    int compactRingCount{2};
    int selectedAiProvider{0};
    int aiProviderPage{0};
    int aiVisibleCount{0};
    bool selectedAiVisible{true};
    bool selectedAiRing{true};
    std::wstring selectedAiColor{L"#64D2FF"};
    std::array<bool, 6> aiPageVisible{};
    std::array<int, 5> widgetOrder{4, 0, 1, 2, 3};
    std::wstring timeText;
    std::wstring dateText;
};

inline double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

inline float compact_island_width(int mode, int rings, bool hasMedia) noexcept {
    if (!hasMedia) return 230.0f;
    mode = std::clamp(mode, 0, 2);
    rings = std::clamp(rings, 1, 3);
    if (mode == 2) return 260.0f + static_cast<float>(rings - 1) * 22.0f;
    return mode == 1 && rings == 3 ? 252.0f : 230.0f;
}

inline bool widget_enabled(const RenderState& state, int widget) noexcept {
    switch (static_cast<WidgetKind>(widget)) {
        case WidgetKind::AiUsage: return state.showAiUsage;
        case WidgetKind::AppLauncher: return state.showAppLauncher;
        case WidgetKind::Commands: return state.showCommandShortcuts;
        case WidgetKind::System: return state.showSystemMetrics;
        case WidgetKind::Music: return state.showMusicPlayer && state.hasMedia;
    }
    return false;
}

inline int widget_slot(const RenderState& state, int widget) noexcept {
    int slot = 0;
    for (const int candidate : state.widgetOrder) {
        if (!widget_enabled(state, candidate)) continue;
        if (candidate == widget) return slot;
        ++slot;
    }
    return -1;
}

} // namespace isle
