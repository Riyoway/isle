#include "OverlayWindow.h"

#include "Autostart.h"
#include "MonitorPlacement.h"
#include "../providers/SystemProvider.h"
#include "../providers/MediaProvider.h"
#include "../plugins/PluginHost.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace isle {

namespace {
constexpr wchar_t kWindowClass[] = L"IsleOverlayWindow";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT kAnimationMs = 8; // 120 Hz ceiling; Present(1) still syncs to DWM.
constexpr int kHotkeyId = 1;
constexpr float kCollapsedWidth = 230.0f;
constexpr float kCollapsedHeight = 40.0f;
constexpr float kHoverWidth = 244.0f;
constexpr float kExpandedWidth = 408.0f;
constexpr float kExpandedHeight = 328.0f;
constexpr float kSettingsHeight = 390.0f;
constexpr int kControlIsland = 0;
constexpr int kControlGear = 1;
constexpr int kControlMediaBase = 2;
constexpr int kControlSettingBase = 10;

float scale_for_dpi(UINT dpi) noexcept {
    return static_cast<float>(dpi) / 96.0f;
}

bool point_in_rect(float x, float y, D2D1_RECT_F rect) noexcept {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}
} // namespace

OverlayWindow::OverlayWindow() {
    widthSpring_.configure(560.0f, 40.0f);
    heightSpring_.configure(540.0f, 40.0f);
    hoverSpring_.configure(460.0f, 34.0f);
    expandSpring_.configure(520.0f, 38.0f);
    visibilitySpring_.configure(420.0f, 36.0f);
    pressSpring_.configure(900.0f, 55.0f);
}

OverlayWindow::~OverlayWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool OverlayWindow::create(HINSTANCE instance, int showCommand) {
    (void)showCommand;
    instance_ = instance;
    settings_ = Settings::load();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &OverlayWindow::window_proc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
    const DWORD style = WS_POPUP;

    hwnd_ = CreateWindowExW(exStyle, kWindowClass, L"Isle", style,
                            0, 0, static_cast<int>(maxWidthPx_), static_cast<int>(maxHeightPx_),
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    // Keep DWM from adding its own corner treatment to our custom, per-pixel shape.
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    return true;
}

int OverlayWindow::message_loop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK OverlayWindow::window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) return self->handle_message(message, wParam, lParam);
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT OverlayWindow::handle_message(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: on_create(); return 0;
        case WM_DESTROY: on_destroy(); return 0;
        case WM_TIMER:
            if (wParam == kAnimationTimer) on_timer();
            return 0;
        case WM_MOUSEMOVE:
            on_mouse_move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSELEAVE: on_mouse_leave(); return 0;
        case WM_LBUTTONDOWN:
            on_left_button_down(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            on_left_button_up(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_CAPTURECHANGED:
            if (pressHeld_) {
                pressHeld_ = false;
                pressSpring_.set_target(0.0f);
            }
            return 0;
        case WM_RBUTTONUP:
            on_right_button_up(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DPICHANGED:
            on_dpi_changed(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            update_monitor_position(true);
            return 0;
        case WM_HOTKEY:
            if (wParam == kHotkeyId) toggle_manual_hidden();
            return 0;
        case WM_COMMAND:
            on_command(LOWORD(wParam));
            return 0;
        default:
            if (message == TrayIcon::kMessage) {
                on_tray_message(wParam, lParam);
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void OverlayWindow::on_create() {
    settings_.startWithWindows = is_autostart_enabled();
    dpi_ = GetDpiForWindow(hwnd_);
    const float s = scale_for_dpi(dpi_);
    maxWidthPx_ = static_cast<UINT>(std::lround(520.0f * s));
    maxHeightPx_ = static_cast<UINT>(std::lround(640.0f * s));

    renderer_.initialize(hwnd_, maxWidthPx_, maxHeightPx_);
    renderState_.dpiScale = s;
    widthSpring_.snap(kCollapsedWidth * s);
    heightSpring_.snap(kCollapsedHeight * s);
    renderState_.islandWidth = widthSpring_.value();
    renderState_.islandHeight = heightSpring_.value();
    renderState_.startWithWindows = settings_.startWithWindows;
    renderState_.hideInFullscreen = settings_.hideInFullscreen;
    renderState_.expandOnHover = settings_.expandOnHover;

    providers_.push_back(std::make_unique<SystemProvider>());
    providers_.push_back(std::make_unique<MediaProvider>());
    providers_.push_back(std::make_unique<PluginHost>());
    for (auto& provider : providers_) provider->start(store_);
    providersStarted_ = true;

    tray_ = std::make_unique<TrayIcon>(hwnd_);
    tray_->create();

    RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    SetTimer(hwnd_, kAnimationTimer, kAnimationMs, nullptr);
    lastFrame_ = std::chrono::steady_clock::now();
    update_clock();
    update_monitor_position(true);
    update_region();
    renderer_.render(renderState_, store_.snapshot());
}

void OverlayWindow::on_destroy() {
    KillTimer(hwnd_, kAnimationTimer);
    UnregisterHotKey(hwnd_, kHotkeyId);
    if (providersStarted_) {
        for (auto& provider : providers_) provider->stop();
        providersStarted_ = false;
    }
    providers_.clear();
    tray_.reset();
    hwnd_ = nullptr;
    PostQuitMessage(0);
}

void OverlayWindow::on_timer() {
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastFrame_).count();
    lastFrame_ = now;

    for (auto& provider : providers_) provider->tick();
    if (lastClockUpdate_.time_since_epoch().count() == 0 || now - lastClockUpdate_ >= std::chrono::milliseconds(settings_.showSeconds ? 250 : 1000)) {
        update_clock();
    }

    update_visibility_policy();

    if (settings_.expandOnHover && !expanded_ && hoverSpring_.target() > 0.5f) {
        if (hoverBegan_.time_since_epoch().count() != 0 && now - hoverBegan_ > std::chrono::milliseconds(420)) {
            set_expanded(true);
        }
    }

    update_animation(dt);
    update_region();
    renderer_.render(renderState_, store_.snapshot());
}

void OverlayWindow::on_mouse_move(int x, int y) {
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        trackingMouse_ = true;
        hoverBegan_ = std::chrono::steady_clock::now();
    }
    hoverSpring_.set_target(1.0f);
    if (!expanded_) widthSpring_.set_target(kHoverWidth * renderState_.dpiScale);
    if (pressHeld_) {
        pressSpring_.set_target(control_at(static_cast<float>(x), static_cast<float>(y)) == pressedControl_ ? 1.0f : 0.0f);
    }
}

void OverlayWindow::on_mouse_leave() {
    trackingMouse_ = false;
    hoverSpring_.set_target(0.0f);
    hoverBegan_ = {};
    if (!expanded_) widthSpring_.set_target(kCollapsedWidth * renderState_.dpiScale);
    if (pressHeld_) pressSpring_.set_target(0.0f);
}

void OverlayWindow::on_left_button_down(int x, int y) {
    if (manualHidden_ || fullscreenHidden_) return;
    const int control = control_at(static_cast<float>(x), static_cast<float>(y));
    if (control < 0) return;
    pressedControl_ = control;
    pressHeld_ = true;
    pressSpring_.snap(1.0f);
    renderState_.pressedControl = control;
    renderState_.pressAmount = 1.0f;
    SetCapture(hwnd_);
}

void OverlayWindow::on_left_button_up(int x, int y) {
    if (manualHidden_ || fullscreenHidden_) return;

    const int releasedControl = control_at(static_cast<float>(x), static_cast<float>(y));
    const int pressedControl = pressedControl_;
    const bool hadPress = pressHeld_;
    pressHeld_ = false;
    pressSpring_.set_target(0.0f);
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (hadPress && releasedControl != pressedControl) return;

    if (expanded_) {
        if (hit_test_gear(static_cast<float>(x), static_cast<float>(y))) {
            set_settings_mode(!settingsMode_);
            return;
        }
        if (settingsMode_) {
            const int row = hit_test_setting_row(static_cast<float>(x), static_cast<float>(y));
            if (row == 0) {
                settings_.startWithWindows = !settings_.startWithWindows;
                set_autostart(settings_.startWithWindows);
            } else if (row == 1) {
                settings_.hideInFullscreen = !settings_.hideInFullscreen;
            } else if (row == 2) {
                settings_.expandOnHover = !settings_.expandOnHover;
            } else if (row == 3) {
                const auto path = Settings::data_directory() / L"plugins";
                ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else {
                set_settings_mode(false);
                return;
            }
            settings_.save();
            renderState_.startWithWindows = settings_.startWithWindows;
            renderState_.hideInFullscreen = settings_.hideInFullscreen;
            renderState_.expandOnHover = settings_.expandOnHover;
            return;
        }
        const int mediaAction = hit_test_media_action(static_cast<float>(x), static_cast<float>(y));
        if (mediaAction >= 0) {
            constexpr std::wstring_view actions[]{L"previous", L"toggle", L"next"};
            for (auto& provider : providers_) provider->invoke(L"media.now-playing", actions[mediaAction]);
            return;
        }

        // Top header collapses. The rest of the panel stays interactive rather than
        // accidentally disappearing on any click.
        const auto rect = renderer_.island_rect(renderState_);
        if (y <= rect.top + 44.0f * renderState_.dpiScale) {
            set_expanded(false);
        }
    } else {
        set_expanded(true);
    }
}

void OverlayWindow::on_right_button_up(int, int) {
    POINT pt{};
    GetCursorPos(&pt);
    if (tray_) tray_->show_menu(pt);
}

void OverlayWindow::on_tray_message(WPARAM, LPARAM lParam) {
    const UINT event = LOWORD(lParam);
    if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) {
        toggle_manual_hidden();
    } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
        POINT pt{};
        GetCursorPos(&pt);
        if (tray_) tray_->show_menu(pt);
    }
}

void OverlayWindow::on_command(UINT id) {
    switch (id) {
        case TrayIcon::kCommandToggle: toggle_manual_hidden(); break;
        case TrayIcon::kCommandSettings:
            manualHidden_ = false;
            set_expanded(true);
            set_settings_mode(true);
            break;
        case TrayIcon::kCommandExit:
            DestroyWindow(hwnd_);
            break;
        default: break;
    }
}

void OverlayWindow::on_dpi_changed(UINT dpi, const RECT*) {
    dpi_ = dpi;
    const float s = scale_for_dpi(dpi_);
    renderState_.dpiScale = s;
    maxWidthPx_ = static_cast<UINT>(std::lround(520.0f * s));
    maxHeightPx_ = static_cast<UINT>(std::lround(640.0f * s));
    renderer_.resize(maxWidthPx_, maxHeightPx_);

    widthSpring_.snap((expanded_ ? kExpandedWidth : kCollapsedWidth) * s);
    heightSpring_.snap((expanded_ ? (settingsMode_ ? kSettingsHeight : kExpandedHeight) : kCollapsedHeight) * s);
    update_monitor_position(true);
}

void OverlayWindow::update_animation(float dtSeconds) {
    widthSpring_.step(dtSeconds);
    heightSpring_.step(dtSeconds);
    hoverSpring_.step(dtSeconds);
    expandSpring_.step(dtSeconds);
    visibilitySpring_.step(dtSeconds);
    pressSpring_.step(dtSeconds);

    renderState_.islandWidth = widthSpring_.value();
    renderState_.islandHeight = heightSpring_.value();
    renderState_.hoverAmount = std::clamp(hoverSpring_.value(), 0.0f, 1.0f);
    renderState_.expandAmount = std::clamp(expandSpring_.value(), 0.0f, 1.0f);
    renderState_.visibility = std::clamp(visibilitySpring_.value(), 0.0f, 1.0f);
    renderState_.pressAmount = std::clamp(pressSpring_.value(), 0.0f, 1.0f);
    if (!pressHeld_ && renderState_.pressAmount < 0.001f) pressedControl_ = -1;
    renderState_.pressedControl = pressedControl_;
    renderState_.expanded = expanded_;
    renderState_.settingsMode = settingsMode_;
    renderState_.hidden = renderState_.visibility < 0.005f;
}

void OverlayWindow::update_clock() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::array<wchar_t, 64> time{};
    std::array<wchar_t, 96> date{};
    const wchar_t* timePattern = settings_.showSeconds ? L"HH':'mm':'ss" : L"HH':'mm";
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, timePattern, time.data(), static_cast<int>(time.size()));
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"ddd, MMM d", date.data(), static_cast<int>(date.size()), nullptr);
    renderState_.timeText = time.data();
    renderState_.dateText = date.data();
    lastClockUpdate_ = std::chrono::steady_clock::now();
}

void OverlayWindow::update_region() {
    if (!hwnd_) return;
    if (renderState_.visibility < 0.01f) {
        if (regionEmpty_) return;
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (region && SetWindowRgn(hwnd_, region, FALSE) == 0) DeleteObject(region);
        regionEmpty_ = true;
        SetRectEmpty(&lastRegion_);
        return;
    }

    const auto rect = renderer_.island_rect(renderState_);
    const float pad = 13.0f * renderState_.dpiScale;
    const int left = std::max(0, static_cast<int>(std::floor(rect.left - pad)));
    const int top = std::max(0, static_cast<int>(std::floor(rect.top - pad)));
    const int right = std::min(static_cast<int>(renderer_.width()), static_cast<int>(std::ceil(rect.right + pad)));
    const int bottom = std::min(static_cast<int>(renderer_.height()), static_cast<int>(std::ceil(rect.bottom + pad)));
    const RECT next{left, top, right, bottom};
    if (!regionEmpty_ && EqualRect(&next, &lastRegion_)) return;
    HRGN region = CreateRectRgn(left, top, right, bottom);
    if (region && SetWindowRgn(hwnd_, region, FALSE) == 0) {
        DeleteObject(region);
    } else {
        lastRegion_ = next;
        regionEmpty_ = false;
    }
}

void OverlayWindow::update_monitor_position(bool forceResize) {
    if (!hwnd_) return;
    const auto monitor = monitor_for_window(hwnd_, settings_.monitorAtCursor);
    dpi_ = monitor.dpi;
    const float s = scale_for_dpi(dpi_);
    renderState_.dpiScale = s;

    const UINT desiredW = static_cast<UINT>(std::lround(520.0f * s));
    const UINT desiredH = static_cast<UINT>(std::lround(640.0f * s));
    if (forceResize || desiredW != maxWidthPx_ || desiredH != maxHeightPx_) {
        maxWidthPx_ = desiredW;
        maxHeightPx_ = desiredH;
        renderer_.resize(maxWidthPx_, maxHeightPx_);
    }

    const POINT origin = top_center_origin(monitor, static_cast<int>(maxWidthPx_), settings_.topOffsetDip);
    SetWindowPos(hwnd_, HWND_TOPMOST, origin.x, origin.y, static_cast<int>(maxWidthPx_), static_cast<int>(maxHeightPx_),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void OverlayWindow::update_visibility_policy() {
    fullscreenHidden_ = settings_.hideInFullscreen && foreground_is_fullscreen();
    const bool shouldHide = manualHidden_ || fullscreenHidden_;
    visibilitySpring_.set_target(shouldHide ? 0.0f : 1.0f);
}

void OverlayWindow::set_expanded(bool expanded) {
    expanded_ = expanded;
    if (!expanded_) settingsMode_ = false;
    const float s = renderState_.dpiScale;
    widthSpring_.set_target((expanded ? kExpandedWidth : (trackingMouse_ ? kHoverWidth : kCollapsedWidth)) * s);
    heightSpring_.set_target((expanded ? (settingsMode_ ? kSettingsHeight : kExpandedHeight) : kCollapsedHeight) * s);
    expandSpring_.set_target(expanded ? 1.0f : 0.0f);
}

void OverlayWindow::set_settings_mode(bool enabled) {
    settingsMode_ = enabled;
    if (enabled && !expanded_) set_expanded(true);
    if (expanded_) heightSpring_.set_target((enabled ? kSettingsHeight : kExpandedHeight) * renderState_.dpiScale);
    renderState_.settingsMode = enabled;
}

void OverlayWindow::toggle_manual_hidden() {
    manualHidden_ = !manualHidden_;
    if (!manualHidden_) update_monitor_position(false);
}

bool OverlayWindow::foreground_is_fullscreen() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == hwnd_) return false;

    wchar_t className[128]{};
    GetClassNameW(foreground, className, ARRAYSIZE(className));
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Shell_TrayWnd") == 0) {
        return false;
    }

    RECT windowRect{};
    if (!GetWindowRect(foreground, &windowRect)) return false;
    const HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;

    constexpr int tolerance = 2;
    const bool covers = windowRect.left <= info.rcMonitor.left + tolerance &&
                        windowRect.top <= info.rcMonitor.top + tolerance &&
                        windowRect.right >= info.rcMonitor.right - tolerance &&
                        windowRect.bottom >= info.rcMonitor.bottom - tolerance;
    return covers && IsZoomed(foreground) == FALSE;
}

bool OverlayWindow::hit_test_gear(float x, float y) const {
    const float s = renderState_.dpiScale;
    const auto rect = renderer_.island_rect(renderState_);
    return point_in_rect(x, y, D2D1::RectF(rect.right - 48.0f * s, rect.top + 3.0f * s,
                                           rect.right - 6.0f * s, rect.top + 47.0f * s));
}

int OverlayWindow::hit_test_media_action(float x, float y) const {
    const float s = renderState_.dpiScale;
    const auto activities = store_.snapshot();
    const bool hasMedia = std::ranges::any_of(activities, [](const Activity& a) { return a.kind == ActivityKind::Media; });
    if (!hasMedia || settingsMode_) return -1;
    const auto rect = renderer_.island_rect(renderState_);
    const float centerX = (rect.left + rect.right) * 0.5f;
    const float centerY = rect.top + 231.0f * s;
    constexpr float offsets[]{-62.0f, 0.0f, 62.0f};
    for (int i = 0; i < 3; ++i) {
        const float cx = centerX + offsets[i] * s;
        if (point_in_rect(x, y, D2D1::RectF(cx - 26.0f * s, centerY - 26.0f * s,
                                            cx + 26.0f * s, centerY + 26.0f * s))) return i;
    }
    return -1;
}

int OverlayWindow::hit_test_setting_row(float x, float y) const {
    const float s = renderState_.dpiScale;
    const auto rect = renderer_.island_rect(renderState_);
    const float left = rect.left + 22.0f * s;
    const float right = rect.right - 22.0f * s;
    float top = rect.top + 74.0f * s;
    for (int row = 0; row < 4; ++row) {
        if (point_in_rect(x, y, D2D1::RectF(left, top, right, top + 60.0f * s))) return row;
        top += 68.0f * s;
    }
    return -1;
}

int OverlayWindow::control_at(float x, float y) const {
    if (hit_test_gear(x, y) && expanded_) return kControlGear;
    if (!expanded_) return kControlIsland;
    if (settingsMode_) {
        const int row = hit_test_setting_row(x, y);
        return row < 0 ? -1 : kControlSettingBase + row;
    }
    const int mediaAction = hit_test_media_action(x, y);
    if (mediaAction >= 0) return kControlMediaBase + mediaAction;
    const auto rect = renderer_.island_rect(renderState_);
    return y <= rect.top + 44.0f * renderState_.dpiScale ? kControlIsland : -1;
}

} // namespace isle
