#pragma once

#include <Windows.h>
#include <shellapi.h>

namespace isle {

class TrayIcon {
public:
    static constexpr UINT kMessage = WM_APP + 42;
    static constexpr UINT kCommandToggle = 1001;
    static constexpr UINT kCommandSettings = 1002;
    static constexpr UINT kCommandExit = 1003;

    explicit TrayIcon(HWND hwnd);
    ~TrayIcon();

    bool create();
    void remove();
    void show_menu(POINT screenPoint) const;

private:
    HWND hwnd_{};
    NOTIFYICONDATAW data_{};
    bool created_{false};
};

} // namespace isle
