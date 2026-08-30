#include "MonitorPlacement.h"

#include <ShellScalingApi.h>

namespace isle {

MonitorPlacement monitor_for_window(HWND hwnd, bool atCursor) {
    HMONITOR monitor = nullptr;
    if (atCursor) {
        POINT pt{};
        GetCursorPos(&pt);
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    } else {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    }

    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);

    UINT dpi = 96;
    if (const HMODULE shcore = LoadLibraryW(L"Shcore.dll")) {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
        if (const auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            UINT x = 96, y = 96;
            if (SUCCEEDED(fn(monitor, MDT_EFFECTIVE_DPI, &x, &y))) dpi = x;
        }
        FreeLibrary(shcore);
    }

    return {.monitor = info.rcMonitor, .work = info.rcWork, .dpi = dpi};
}

POINT top_center_origin(const MonitorPlacement& monitor, int maxWindowWidthPx, int topOffsetDip) {
    const float scale = static_cast<float>(monitor.dpi) / 96.0f;
    const int offset = static_cast<int>(topOffsetDip * scale);
    const int width = monitor.work.right - monitor.work.left;
    POINT pt{};
    pt.x = monitor.work.left + (width - maxWindowWidthPx) / 2;
    pt.y = monitor.work.top + offset;
    return pt;
}

} // namespace isle
