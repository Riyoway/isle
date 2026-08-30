#include "Autostart.h"

#include <Windows.h>

#include <array>
#include <string>

namespace isle {

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"Isle";
}

bool set_autostart(bool enabled) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;

    bool ok = false;
    if (!enabled) {
        const LSTATUS status = RegDeleteValueW(key, kValueName);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    } else {
        std::array<wchar_t, 32768> exe{};
        const DWORD len = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (len > 0 && len < exe.size()) {
            const std::wstring command = L"\"" + std::wstring(exe.data(), len) + L"\" --background";
            ok = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return ok;
}

bool is_autostart_enabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

} // namespace isle
