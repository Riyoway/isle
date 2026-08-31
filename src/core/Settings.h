#pragma once

#include "AIProviders.h"

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
    std::array<ShortcutSetting, 2> appShortcuts{{
        {L"Files", L"explorer.exe", L"", L"\uE8B7", true},
        {L"Terminal", L"wt.exe", L"", L"\uE756", true},
    }};
    std::array<ShortcutSetting, 2> commandShortcuts{{
        {L"Task Manager", L"taskmgr.exe", L"", L"\uE9D9", true},
        {L"Settings", L"ms-settings:", L"", L"\uE713", true},
    }};

    static std::filesystem::path data_directory();
    static std::filesystem::path file_path();
    static Settings load();
    void save() const;
};

} // namespace isle
