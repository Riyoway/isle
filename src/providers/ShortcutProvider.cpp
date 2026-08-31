#include "ShortcutProvider.h"

#include "../core/Settings.h"

#include <Windows.h>
#include <shellapi.h>

#include <array>
#include <string>

namespace isle {

namespace {
void publish_shortcuts(ActivityStore& store, const std::array<ShortcutSetting, 2>& shortcuts,
                       std::wstring_view source, std::wstring_view idPrefix,
                       std::wstring_view subtitle, std::wstring_view accent, int priority) {
    for (std::size_t i = 0; i < shortcuts.size(); ++i) {
        const auto& shortcut = shortcuts[i];
        if (!shortcut.enabled || shortcut.label.empty() || shortcut.target.empty()) continue;
        Activity activity;
        activity.id = std::wstring(idPrefix) + std::to_wstring(i);
        activity.source = std::wstring(source);
        activity.kind = ActivityKind::Shortcut;
        activity.title = shortcut.label;
        activity.subtitle = std::wstring(subtitle);
        activity.glyph = shortcut.glyph.empty() ? L"\uE8A7" : shortcut.glyph;
        activity.accent = std::wstring(accent);
        activity.priority = priority - static_cast<int>(i);
        activity.actions = {{L"launch", L"Open", L"\uE768"}};
        store.upsert(std::move(activity));
    }
}

bool launch(const ShortcutSetting& shortcut) {
    const auto result = ShellExecuteW(nullptr, L"open", shortcut.target.c_str(),
                                      shortcut.arguments.empty() ? nullptr : shortcut.arguments.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}
} // namespace

void ShortcutProvider::start(ActivityStore& store) {
    store_ = &store;
    publish();
}

void ShortcutProvider::stop() {
    if (store_) {
        store_->remove_source(L"shortcut.app");
        store_->remove_source(L"shortcut.command");
    }
    store_ = nullptr;
}

void ShortcutProvider::tick() {
    if (!store_) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastCheck_.time_since_epoch().count() != 0 && now - lastCheck_ < std::chrono::seconds(1)) return;
    lastCheck_ = now;

    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(Settings::file_path(), ec);
    if (!ec && modified != settingsModified_) publish();
}

void ShortcutProvider::invoke(std::wstring_view activityId, std::wstring_view actionId) {
    if (actionId != L"launch") return;
    const Settings settings = Settings::load();
    if (activityId.starts_with(L"shortcut.app.")) {
        const auto index = activityId.back() == L'1' ? 1u : 0u;
        if (!launch(settings.appShortcuts[index]) && index == 1 && settings.appShortcuts[index].target == L"wt.exe") {
            ShellExecuteW(nullptr, L"open", L"powershell.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (activityId.starts_with(L"shortcut.command.")) {
        const auto index = activityId.back() == L'1' ? 1u : 0u;
        launch(settings.commandShortcuts[index]);
    }
}

void ShortcutProvider::publish() {
    if (!store_) return;
    const Settings settings = Settings::load();
    store_->remove_source(L"shortcut.app");
    store_->remove_source(L"shortcut.command");
    publish_shortcuts(*store_, settings.appShortcuts, L"shortcut.app", L"shortcut.app.",
                      L"Application", L"#0A84FF", 190);
    publish_shortcuts(*store_, settings.commandShortcuts, L"shortcut.command", L"shortcut.command.",
                      L"Command", L"#FF9F0A", 180);
    std::error_code ec;
    settingsModified_ = std::filesystem::last_write_time(Settings::file_path(), ec);
    lastCheck_ = std::chrono::steady_clock::now();
}

} // namespace isle
