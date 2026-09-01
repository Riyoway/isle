#include "Settings.h"

#include <ShlObj.h>
#include <Windows.h>

#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace isle {

namespace {
std::wstring read_ini(const wchar_t* section, const wchar_t* key, const wchar_t* fallback, const std::filesystem::path& path) {
    std::array<wchar_t, 2048> buffer{};
    GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

bool parse_bool(const std::wstring& value, bool fallback) {
    if (value == L"1" || value == L"true" || value == L"yes") return true;
    if (value == L"0" || value == L"false" || value == L"no") return false;
    return fallback;
}

int parse_int(const std::wstring& value, int fallback, int minimum, int maximum) {
    try {
        return std::clamp(std::stoi(value), minimum, maximum);
    } catch (...) {
        return fallback;
    }
}

float parse_float(const std::wstring& value, float fallback) {
    try {
        return std::clamp(std::stof(value), 0.0f, 1.0f);
    } catch (...) {
        return fallback;
    }
}

std::array<int, 5> parse_widget_order(const std::wstring& value) {
    constexpr std::array<int, 5> fallback{4, 0, 1, 2, 3};
    std::wistringstream input(value);
    std::vector<int> parsed;
    while (input) {
        int item = -1;
        if (!(input >> item)) break;
        parsed.push_back(item);
        wchar_t comma{};
        if (!(input >> comma)) break;
        if (comma != L',') return fallback;
    }
    if (parsed.size() == 4) parsed.insert(parsed.begin(), 4);
    if (parsed.size() != fallback.size()) return fallback;
    std::array<bool, 5> seen{};
    std::array<int, 5> result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const int item = parsed[i];
        if (item < 0 || item >= 5 || seen[static_cast<std::size_t>(item)]) return fallback;
        result[i] = item;
        seen[static_cast<std::size_t>(item)] = true;
    }
    return result;
}

void load_shortcut(ShortcutSetting& shortcut, const wchar_t* section, const std::filesystem::path& path) {
    shortcut.label = read_ini(section, L"label", shortcut.label.c_str(), path);
    shortcut.target = read_ini(section, L"target", shortcut.target.c_str(), path);
    shortcut.arguments = read_ini(section, L"arguments", shortcut.arguments.c_str(), path);
    const auto glyphCode = read_ini(section, L"glyphCode", L"", path);
    if (!glyphCode.empty()) {
        try {
            shortcut.glyph.assign(1, static_cast<wchar_t>(std::stoul(glyphCode, nullptr, 16)));
        } catch (...) {
        }
    } else {
        const auto glyph = read_ini(section, L"glyph", shortcut.glyph.c_str(), path);
        if (glyph != L"?") shortcut.glyph = glyph;
    }
    shortcut.enabled = parse_bool(read_ini(section, L"enabled", shortcut.enabled ? L"1" : L"0", path), shortcut.enabled);
}

bool is_default_app_shortcut(const ShortcutSetting& shortcut, const wchar_t* label,
                             const wchar_t* target) noexcept {
    return shortcut.enabled && shortcut.label == label && shortcut.target == target;
}
} // namespace

Settings::Settings() {
    commandShortcuts[0] = {L"Task Manager", L"taskmgr.exe", L"", L"\uE9D9", true};
    commandShortcuts[1] = {L"Settings", L"ms-settings:", L"", L"\uE713", true};
    aiRings.fill(true);
    aiVisible.fill(false);
    for (const std::wstring_view id : {L"codex", L"claude", L"cursor", L"gemini"}) {
        const int index = ai_provider_index(id);
        if (index >= 0) aiVisible[static_cast<std::size_t>(index)] = true;
    }
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) aiColors[i] = kAIProviders[i].color;
}

std::filesystem::path Settings::data_directory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return std::filesystem::current_path() / L"IsleData";
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    result /= L"Isle";
    return result;
}

std::filesystem::path Settings::file_path() {
    return data_directory() / L"settings.ini";
}

Settings Settings::load() {
    Settings s;
    const auto path = file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    s.topOffsetDip = parse_int(read_ini(L"island", L"topOffsetDip", L"8", path), 8, 0, 96);
    s.startWithWindows = parse_bool(read_ini(L"general", L"startWithWindows", L"0", path), false);
    s.hideInFullscreen = parse_bool(read_ini(L"island", L"hideInFullscreen", L"1", path), true);
    s.expandOnHover = parse_bool(read_ini(L"island", L"expandOnHover", L"0", path), false);
    s.showSeconds = parse_bool(read_ini(L"clock", L"showSeconds", L"0", path), false);
    s.monitorAtCursor = parse_bool(read_ini(L"island", L"monitorAtCursor", L"0", path), false);
    s.showAiUsage = parse_bool(read_ini(L"widgets", L"aiUsage", L"1", path), true);
    const bool legacyAiRings = parse_bool(read_ini(L"widgets", L"aiRings", L"1", path), true);
    s.compactMediaMode = parse_int(read_ini(L"widgets", L"compactContent", L"", path),
                                  legacyAiRings ? 2 : 0, 0, 2);
    s.compactRingCount = parse_int(read_ini(L"widgets", L"compactRingCount", L"2", path), 2, 1, 3);
    s.showSystemMetrics = parse_bool(read_ini(L"widgets", L"systemMetrics", L"0", path), false);
    s.showAppLauncher = parse_bool(read_ini(L"widgets", L"appLauncher", L"1", path), true);
    s.showCommandShortcuts = parse_bool(read_ini(L"widgets", L"commands", L"1", path), true);
    s.showMusicPlayer = parse_bool(read_ini(L"widgets", L"music", L"1", path), true);
    s.islandSizePreset = parse_int(read_ini(L"appearance", L"sizePreset", L"1", path), 1, 0, 2);
    s.islandShape = parse_int(read_ini(L"appearance", L"islandShape", L"0", path), 0, 0, 2);
    s.buttonStyle = parse_int(read_ini(L"appearance", L"buttonStyle", L"0", path), 0, 0, 2);
    s.positionX = parse_float(read_ini(L"position", L"x", L"0.5", path), 0.5f);
    s.positionY = parse_float(read_ini(L"position", L"y", L"0", path), 0.0f);
    s.widgetOrder = parse_widget_order(read_ini(L"widgets", L"order", L"4,0,1,2,3", path));
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
        const std::wstring section = L"ai." + std::wstring(kAIProviders[i].id);
        s.aiColors[i] = read_ini(section.c_str(), L"color", s.aiColors[i].c_str(), path);
        s.aiRings[i] = parse_bool(read_ini(section.c_str(), L"ring", L"1", path), true);
        s.aiVisible[i] = parse_bool(read_ini(section.c_str(), L"visible", s.aiVisible[i] ? L"1" : L"0", path),
                                   s.aiVisible[i]);
    }
    for (std::size_t i = 0; i < kShortcutSlots; ++i) {
        const std::wstring appSection = L"app." + std::to_wstring(i);
        const std::wstring commandSection = L"command." + std::to_wstring(i);
        load_shortcut(s.appShortcuts[i], appSection.c_str(), path);
        load_shortcut(s.commandShortcuts[i], commandSection.c_str(), path);
    }
    if (!parse_bool(read_ini(L"shortcuts", L"appDefaultsRemoved", L"0", path), false)) {
        const auto clear_default = [&](std::size_t index, const wchar_t* label, const wchar_t* target) {
            if (!is_default_app_shortcut(s.appShortcuts[index], label, target)) return;
            s.appShortcuts[index] = {};
            const std::wstring section = L"app." + std::to_wstring(index);
            for (const wchar_t* key : {L"label", L"target", L"arguments"}) {
                WritePrivateProfileStringW(section.c_str(), key, L"", path.c_str());
            }
            WritePrivateProfileStringW(section.c_str(), L"glyph", nullptr, path.c_str());
            WritePrivateProfileStringW(section.c_str(), L"glyphCode", nullptr, path.c_str());
            WritePrivateProfileStringW(section.c_str(), L"enabled", L"0", path.c_str());
        };
        clear_default(0, L"Files", L"explorer.exe");
        clear_default(1, L"Terminal", L"wt.exe");
        WritePrivateProfileStringW(L"shortcuts", L"appDefaultsRemoved", L"1", path.c_str());
    }
    return s;
}

void Settings::save() const {
    const auto path = file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const auto write = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
    };
    write(L"island", L"topOffsetDip", std::to_wstring(topOffsetDip));
    write(L"general", L"startWithWindows", startWithWindows ? L"1" : L"0");
    write(L"island", L"hideInFullscreen", hideInFullscreen ? L"1" : L"0");
    write(L"island", L"expandOnHover", expandOnHover ? L"1" : L"0");
    write(L"clock", L"showSeconds", showSeconds ? L"1" : L"0");
    write(L"island", L"monitorAtCursor", monitorAtCursor ? L"1" : L"0");
    write(L"widgets", L"aiUsage", showAiUsage ? L"1" : L"0");
    write(L"widgets", L"aiRings", compactMediaMode != 0 ? L"1" : L"0");
    write(L"widgets", L"compactContent", std::to_wstring(compactMediaMode));
    write(L"widgets", L"compactRingCount", std::to_wstring(compactRingCount));
    write(L"widgets", L"systemMetrics", showSystemMetrics ? L"1" : L"0");
    write(L"widgets", L"appLauncher", showAppLauncher ? L"1" : L"0");
    write(L"widgets", L"commands", showCommandShortcuts ? L"1" : L"0");
    write(L"widgets", L"music", showMusicPlayer ? L"1" : L"0");
    write(L"widgets", L"order", std::to_wstring(widgetOrder[0]) + L"," + std::to_wstring(widgetOrder[1]) +
          L"," + std::to_wstring(widgetOrder[2]) + L"," + std::to_wstring(widgetOrder[3]) +
          L"," + std::to_wstring(widgetOrder[4]));
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
        const std::wstring section = L"ai." + std::wstring(kAIProviders[i].id);
        write(section.c_str(), L"color", aiColors[i]);
        write(section.c_str(), L"ring", aiRings[i] ? L"1" : L"0");
        write(section.c_str(), L"visible", aiVisible[i] ? L"1" : L"0");
    }
    write(L"appearance", L"sizePreset", std::to_wstring(islandSizePreset));
    write(L"appearance", L"islandShape", std::to_wstring(islandShape));
    write(L"appearance", L"buttonStyle", std::to_wstring(buttonStyle));
    write(L"position", L"x", std::to_wstring(positionX));
    write(L"position", L"y", std::to_wstring(positionY));

    const auto writeShortcut = [&](const wchar_t* section, const ShortcutSetting& shortcut) {
        write(section, L"label", shortcut.label);
        write(section, L"target", shortcut.target);
        write(section, L"arguments", shortcut.arguments);
        if (shortcut.glyph.size() == 1 && shortcut.glyph.front() >= 0xE000 && shortcut.glyph.front() <= 0xF8FF) {
            std::array<wchar_t, 8> code{};
            swprintf_s(code.data(), code.size(), L"%04X", static_cast<unsigned>(shortcut.glyph.front()));
            write(section, L"glyphCode", code.data());
            WritePrivateProfileStringW(section, L"glyph", nullptr, path.c_str());
        } else {
            write(section, L"glyph", shortcut.glyph);
            WritePrivateProfileStringW(section, L"glyphCode", nullptr, path.c_str());
        }
        write(section, L"enabled", shortcut.enabled ? L"1" : L"0");
    };
    for (std::size_t i = 0; i < kShortcutSlots; ++i) {
        const std::wstring appSection = L"app." + std::to_wstring(i);
        const std::wstring commandSection = L"command." + std::to_wstring(i);
        writeShortcut(appSection.c_str(), appShortcuts[i]);
        writeShortcut(commandSection.c_str(), commandShortcuts[i]);
    }
}

} // namespace isle
