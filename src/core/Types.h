#pragma once

#include <algorithm>
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
    Status,
    Timer,
    Text,
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
    bool startWithWindows{false};
    bool hideInFullscreen{true};
    bool expandOnHover{false};
    std::wstring timeText;
    std::wstring dateText;
};

inline double clamp01(double value) noexcept {
    return std::clamp(value, 0.0, 1.0);
}

} // namespace isle
