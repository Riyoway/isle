#pragma once

#include "../core/Settings.h"

#include <Windows.h>
#include <commctrl.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace isle {

class ShortcutEditor {
public:
    struct InstalledApp {
        std::wstring label;
        std::wstring path;
        int imageIndex{-1};
    };

    struct CachedApp {
        InstalledApp app;
        std::int64_t modified{};
    };

    ~ShortcutEditor();

    void show_embedded(HWND parent, HINSTANCE instance, RECT bounds, int radius,
                       std::function<void()> changed);
    void hide();
    void resize_embedded(RECT bounds, int radius);

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam);

    void create_controls();
    void layout_controls();
    void switch_page(bool commands);
    void start_app_discovery();
    void load_app_batch();
    void finish_app_discovery();
    bool append_app(const std::filesystem::path& path);
    void populate_apps();
    void populate_commands();
    void update_app_check(int item, bool checked);
    void add_command();
    void remove_command();
    void load_selected_command();
    void browse_target();
    void save_changed();
    void destroy_resources();

    static bool same_path(std::wstring_view left, std::wstring_view right) noexcept;
    static std::wstring lower(std::wstring value);

    HWND owner_{};
    HWND hwnd_{};
    HINSTANCE instance_{};
    Settings settings_{};
    std::function<void()> changed_;

    HFONT font_{};
    HFONT titleFont_{};
    HFONT captionFont_{};
    HBRUSH backgroundBrush_{};
    HBRUSH fieldBrush_{};
    HIMAGELIST appImages_{};
    HIMAGELIST rowHeightImages_{};

    HWND appsTab_{};
    HWND commandsTab_{};
    HWND search_{};
    HWND appList_{};
    HWND appHint_{};
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
    std::vector<CachedApp> appCache_;
    std::vector<CachedApp> nextAppCache_;
    std::vector<std::filesystem::path> appRoots_;
    std::vector<std::pair<std::filesystem::path, std::int64_t>> appDirectoryStamps_;
    std::vector<std::pair<std::filesystem::path, std::int64_t>> nextDirectoryStamps_;
    std::unique_ptr<std::filesystem::recursive_directory_iterator> appIterator_;
    std::vector<std::wstring> appSeen_;
    std::size_t appRootIndex_{0};
    bool appLoading_{false};
    bool appCacheReady_{false};
    bool appImageCacheValid_{false};
    bool commandsPage_{false};
    RECT embeddedBounds_{};
    int embeddedRadius_{0};
};

} // namespace isle
