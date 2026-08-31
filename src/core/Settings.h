#pragma once

#include "AIProviders.h"
#include "Types.h"

#include <array>
#include <filesystem>
#include <string>

namespace isle {

struct ShortcutSetting {
    std::wstring label;
    std::wstring target;
    std::wstring arguments;
    std::wstring glyph;
    bool enabled{true};
};

struct Settings {
    Settings();

    int topOffsetDip{8};
    bool startWithWindows{false};
    bool hideInFullscreen{true};
    bool expandOnHover{false};
    bool showSeconds{false};
    bool monitorAtCursor{false};
    bool showAiUsage{true};
    bool showSystemMetrics{false};
    bool showAppLauncher{true};
    bool showCommandShortcuts{true};
    bool showMusicPlayer{true};
    int islandSizePreset{1};
    int islandShape{0};
    int buttonStyle{0};
    int compactMediaMode{2};
    int compactRingCount{2};
    float positionX{0.5f};
    float positionY{0.0f};
    std::array<int, 5> widgetOrder{4, 0, 1, 2, 3};
    std::array<std::wstring, kAIProviders.size()> aiColors{};
    std::array<bool, kAIProviders.size()> aiRings{};
    std::array<bool, kAIProviders.size()> aiVisible{};
    std::array<ShortcutSetting, kShortcutSlots> appShortcuts{};
    std::array<ShortcutSetting, kShortcutSlots> commandShortcuts{};

    static std::filesystem::path data_directory();
    static std::filesystem::path file_path();
    static Settings load();
    void save() const;
};

} // namespace isle
