#pragma once

#include "../core/Settings.h"

#include <Windows.h>
#include <commctrl.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace isle {

class ShortcutEditor {
public:
    struct InstalledApp {
        std::wstring label;
        std::wstring path;
        int imageIndex{-1};
    };

    ~ShortcutEditor();

    void show(HWND owner, HINSTANCE instance, std::function<void()> changed);

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam);

    void create_controls();
    void switch_page(bool commands);
    void populate_apps();
    void populate_commands();
    void add_selected_app();
    void update_app_check(int item, bool checked);
    void add_command();
    void remove_command();
    void load_selected_command();
    void browse_target();
    void save_changed();
    void destroy_resources();

    std::vector<InstalledApp> discover_apps();
    static bool same_path(std::wstring_view left, std::wstring_view right) noexcept;
    static std::wstring lower(std::wstring value);

    HWND owner_{};
    HWND hwnd_{};
    HINSTANCE instance_{};
    Settings settings_{};
    std::function<void()> changed_;

    HFONT font_{};
    HFONT titleFont_{};
    HBRUSH backgroundBrush_{};
    HBRUSH fieldBrush_{};
    HIMAGELIST appImages_{};

    HWND title_{};
    HWND subtitle_{};
    HWND appsTab_{};
    HWND commandsTab_{};
    HWND search_{};
    HWND appList_{};
    HWND appHint_{};
    HWND addAppButton_{};
    HWND commandLabelCaption_{};
    HWND commandLabel_{};
    HWND commandTargetCaption_{};
    HWND commandTarget_{};
    HWND commandArgumentsCaption_{};
    HWND commandArguments_{};
    HWND browseButton_{};
    HWND addCommandButton_{};
    HWND commandList_{};
    HWND removeCommandButton_{};

    std::vector<InstalledApp> apps_;
    bool commandsPage_{false};
    bool refreshing_{false};
};

} // namespace isle
