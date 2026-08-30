#pragma once

#include <Windows.h>

namespace isle {

struct MonitorPlacement {
    RECT monitor{};
    RECT work{};
    UINT dpi{96};
};

MonitorPlacement monitor_for_window(HWND hwnd, bool atCursor);
POINT top_center_origin(const MonitorPlacement& monitor, int maxWindowWidthPx, int topOffsetDip);

} // namespace isle
