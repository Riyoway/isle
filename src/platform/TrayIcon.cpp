#include "TrayIcon.h"

#include <strsafe.h>

namespace isle {

TrayIcon::TrayIcon(HWND hwnd) : hwnd_(hwnd) {
    data_.cbSize = sizeof(data_);
    data_.hWnd = hwnd_;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data_.uCallbackMessage = kMessage;
    data_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(data_.szTip, ARRAYSIZE(data_.szTip), L"Isle");
}

TrayIcon::~TrayIcon() {
    remove();
}

bool TrayIcon::create() {
    if (created_) return true;
    created_ = Shell_NotifyIconW(NIM_ADD, &data_) == TRUE;
    if (created_) {
        data_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data_);
    }
    return created_;
}

void TrayIcon::remove() {
    if (!created_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    created_ = false;
}

void TrayIcon::show_menu(POINT screenPoint) const {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCommandToggle, L"Show / hide island");
    AppendMenuW(menu, MF_STRING, kCommandSettings, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, L"Exit");

    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

} // namespace isle
