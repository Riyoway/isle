#include "OverlayWindow.h"

#include "Autostart.h"
#include "MonitorPlacement.h"
#include "../core/AIProviders.h"
#include "../providers/AIUsageProvider.h"
#include "../providers/SystemProvider.h"
#include "../providers/MediaProvider.h"
#include "../providers/ShortcutProvider.h"
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
constexpr float kCollapsedHeight = 40.0f;
constexpr float kExpandedWidth = 408.0f;
constexpr float kSettingsHeight = 610.0f;
constexpr int kControlIsland = 0;
constexpr int kControlGear = 1;
constexpr int kControlMediaBase = 2;
constexpr int kControlSettingBase = 10;
constexpr int kControlWidgetBase = 20;
constexpr int kControlShortcutBase = 40;
constexpr int kControlAppearanceBase = 60;
constexpr int kControlAiBase = 70;
constexpr int kControlAiPickerBase = 80;
constexpr int kAiProviderPageSize = 6;
constexpr int kAiProviderPageCount = (static_cast<int>(kAIProviders.size()) + kAiProviderPageSize - 1) /
                                     kAiProviderPageSize;

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
    positionXSpring_.configure(520.0f, 38.0f);
    positionYSpring_.configure(520.0f, 38.0f);
}

OverlayWindow::~OverlayWindow() {
    shortcutEditor_.reset();
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
            windowDragging_ = false;
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
    const float s = layout_scale();
    maxWidthPx_ = static_cast<UINT>(std::lround(520.0f * s));
    maxHeightPx_ = static_cast<UINT>(std::lround(760.0f * s));

    renderer_.initialize(hwnd_, maxWidthPx_, maxHeightPx_);
    renderState_.dpiScale = s;
    apply_settings_to_render_state();
    widthSpring_.snap(collapsed_width_px());
    heightSpring_.snap(kCollapsedHeight * s);
    renderState_.islandWidth = widthSpring_.value();
    renderState_.islandHeight = heightSpring_.value();

    providers_.push_back(std::make_unique<SystemProvider>());
    providers_.push_back(std::make_unique<AIUsageProvider>());
    providers_.push_back(std::make_unique<MediaProvider>());
    providers_.push_back(std::make_unique<ShortcutProvider>());
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

    const auto activities = store_.snapshot();
    const bool hadMedia = renderState_.hasMedia;
    renderState_.hasMedia = std::ranges::any_of(activities, [](const Activity& activity) {
        return activity.kind == ActivityKind::Media;
    });
    if (hadMedia != renderState_.hasMedia) {
        if (expanded_ && !settingsMode_) heightSpring_.set_target(expanded_height_px());
        else if (!expanded_) widthSpring_.set_target(collapsed_width_px());
        update_monitor_position(false);
    }
    update_animation(dt);
    update_shortcut_editor_bounds();
    update_region();
    renderer_.render(renderState_, activities);
}

void OverlayWindow::on_mouse_move(int x, int y) {
    if (pressHeld_ && pressedControl_ == kControlIsland) {
        POINT cursor{};
        GetCursorPos(&cursor);
        const int dx = cursor.x - dragStartCursor_.x;
        const int dy = cursor.y - dragStartCursor_.y;
        const int threshold = std::max(4, static_cast<int>(std::lround(6.0f * scale_for_dpi(dpi_))));
        if (!windowDragging_ && dx * dx + dy * dy >= threshold * threshold) windowDragging_ = true;
        if (windowDragging_) {
            const int left = dragStartOrigin_.x + dx;
            const int top = dragStartOrigin_.y + dy;
            positionXSpring_.snap(static_cast<float>(left));
            positionYSpring_.snap(static_cast<float>(top));
            positionInitialized_ = true;
            SetWindowPos(hwnd_, HWND_TOPMOST, left, top, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return;
        }
    }

    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        trackingMouse_ = true;
    }
    hoverSpring_.set_target(1.0f);
    if (settings_.expandOnHover && !expanded_) set_expanded(true);
    else if (!expanded_) widthSpring_.set_target(collapsed_width_px() + 14.0f * renderState_.dpiScale);
    if (pressHeld_) {
        pressSpring_.set_target(control_at(static_cast<float>(x), static_cast<float>(y)) == pressedControl_ ? 1.0f : 0.0f);
    }
}

void OverlayWindow::on_mouse_leave() {
    if (windowDragging_) return;
    trackingMouse_ = false;
    hoverSpring_.set_target(0.0f);
    if (settingsMode_) {
        if (pressHeld_) pressSpring_.set_target(0.0f);
        return;
    }
    if (settings_.expandOnHover && expanded_) set_expanded(false);
    else if (!expanded_) widthSpring_.set_target(collapsed_width_px());
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
    if (control == kControlIsland) {
        GetCursorPos(&dragStartCursor_);
        RECT window{};
        GetWindowRect(hwnd_, &window);
        dragStartOrigin_ = {window.left, window.top};
    }
    SetCapture(hwnd_);
}

void OverlayWindow::on_left_button_up(int x, int y) {
    if (manualHidden_ || fullscreenHidden_) return;

    const bool wasDragging = windowDragging_;
    if (wasDragging) finish_window_drag();
    const int releasedControl = control_at(static_cast<float>(x), static_cast<float>(y));
    const int pressedControl = pressedControl_;
    const bool hadPress = pressHeld_;
    pressHeld_ = false;
    pressSpring_.set_target(0.0f);
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (wasDragging) return;
    if (hadPress && releasedControl != pressedControl) return;

    if (expanded_) {
        if (hit_test_gear(static_cast<float>(x), static_cast<float>(y))) {
            if (settingsMode_ && settingsPage_ == 5) {
                if (shortcutEditor_) shortcutEditor_->hide();
                settingsPage_ = 0;
                renderState_.settingsPage = 0;
            } else if (settingsMode_ && settingsPage_ == 4) {
                settingsPage_ = 3;
                renderState_.settingsPage = settingsPage_;
            } else if (settingsMode_ && settingsPage_ != 0) {
                settingsPage_ = 0;
                renderState_.settingsPage = 0;
            } else {
                set_settings_mode(!settingsMode_);
            }
            return;
        }
        if (settingsMode_) {
            const int control = hit_test_setting_control(static_cast<float>(x), static_cast<float>(y));
            if (settingsPage_ == 0) {
                if (control == 10) {
                    settings_.startWithWindows = !settings_.startWithWindows;
                    set_autostart(settings_.startWithWindows);
                } else if (control == 11) {
                    settings_.hideInFullscreen = !settings_.hideInFullscreen;
                } else if (control == 12) {
                    settings_.expandOnHover = !settings_.expandOnHover;
                } else if (control >= 13 && control <= 15) {
                    settingsPage_ = control - 12;
                    renderState_.settingsPage = settingsPage_;
                    return;
                } else if (control == 16) {
                    const auto path = Settings::data_directory() / L"plugins";
                    std::error_code ec;
                    std::filesystem::create_directories(path, ec);
                    ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return;
                } else {
                    return;
                }
            } else if (settingsPage_ == 1) {
                if (control == 35) {
                    open_shortcut_editor();
                    return;
                }
                if (control < kControlWidgetBase || control >= 35) return;
                const int row = (control - kControlWidgetBase) / 3;
                const int action = (control - kControlWidgetBase) % 3;
                if (row < 0 || row >= 5) return;
                if (action == 1) move_widget(row, -1);
                else if (action == 2) move_widget(row, 1);
                else {
                    switch (static_cast<WidgetKind>(settings_.widgetOrder[static_cast<std::size_t>(row)])) {
                        case WidgetKind::AiUsage: settings_.showAiUsage = !settings_.showAiUsage; break;
                        case WidgetKind::AppLauncher: settings_.showAppLauncher = !settings_.showAppLauncher; break;
                        case WidgetKind::Commands: settings_.showCommandShortcuts = !settings_.showCommandShortcuts; break;
                        case WidgetKind::System: settings_.showSystemMetrics = !settings_.showSystemMetrics; break;
                        case WidgetKind::Music: settings_.showMusicPlayer = !settings_.showMusicPlayer; break;
                    }
                }
            } else if (settingsPage_ == 2) {
                if (control == 60) {
                    settings_.islandSizePreset = (settings_.islandSizePreset + 1) % 3;
                } else if (control == 61) {
                    settings_.islandShape = (settings_.islandShape + 1) % 3;
                } else if (control == 62) {
                    settings_.buttonStyle = (settings_.buttonStyle + 1) % 3;
                } else if (control == 63) {
                    settings_.positionX = 0.5f;
                    settings_.positionY = 0.0f;
                } else if (control == 64) {
                    settings_.monitorAtCursor = !settings_.monitorAtCursor;
                } else {
                    return;
                }
            } else if (settingsPage_ == 4) {
                if (control >= kControlAiPickerBase && control < kControlAiPickerBase + kAiProviderPageSize) {
                    const int provider = aiProviderPage_ * kAiProviderPageSize + control - kControlAiPickerBase;
                    if (provider < 0 || provider >= static_cast<int>(kAIProviders.size())) return;
                    selectedAiProvider_ = provider;
                    settings_.aiVisible[static_cast<std::size_t>(provider)] =
                        !settings_.aiVisible[static_cast<std::size_t>(provider)];
                } else if (control == kControlAiPickerBase + kAiProviderPageSize) {
                    aiProviderPage_ = (aiProviderPage_ + 1) % kAiProviderPageCount;
                    apply_settings_to_render_state();
                    update_monitor_position(false);
                    return;
                } else {
                    return;
                }
            } else {
                constexpr std::wstring_view palette[]{L"#64D2FF", L"#0A84FF", L"#30D158", L"#FFD60A",
                                                       L"#FF9F0A", L"#FF453A", L"#BF5AF2", L"#FF375F", L"#FFFFFF"};
                const std::size_t provider = static_cast<std::size_t>(std::clamp(
                    selectedAiProvider_, 0, static_cast<int>(kAIProviders.size() - 1)));
                if (control == kControlAiBase) {
                    aiProviderPage_ = std::clamp(selectedAiProvider_ / kAiProviderPageSize, 0, kAiProviderPageCount - 1);
                    settingsPage_ = 4;
                    apply_settings_to_render_state();
                    update_monitor_position(false);
                    return;
                } else if (control == kControlAiBase + 1) {
                    settings_.aiVisible[provider] = !settings_.aiVisible[provider];
                } else if (control == kControlAiBase + 2) {
                    settings_.aiRings[provider] = !settings_.aiRings[provider];
                } else if (control == kControlAiBase + 3) {
                    const auto current = std::ranges::find(palette, settings_.aiColors[provider]);
                    const std::size_t next = current == std::end(palette)
                        ? 0 : (static_cast<std::size_t>(current - std::begin(palette)) + 1) % std::size(palette);
                    settings_.aiColors[provider] = palette[next];
                } else if (control == kControlAiBase + 4) {
                    settings_.compactMediaMode = (settings_.compactMediaMode + 1) % 3;
                } else if (control == kControlAiBase + 5) {
                    settings_.compactRingCount = settings_.compactRingCount % 3 + 1;
                } else {
                    return;
                }
            }
            settings_.save();
            apply_settings_to_render_state();
            if (settingsPage_ == 3 || settingsPage_ == 4) {
                for (auto& provider : providers_) provider->invoke(L"ai.settings", L"refresh");
            }
            update_monitor_position(false);
            set_expanded(true);
            return;
        }
        const int mediaAction = hit_test_media_action(static_cast<float>(x), static_cast<float>(y));
        if (mediaAction >= 0) {
            constexpr std::wstring_view actions[]{L"previous", L"toggle", L"next"};
            for (auto& provider : providers_) provider->invoke(L"media.now-playing", actions[mediaAction]);
            return;
        }
        const int shortcutControl = hit_test_shortcut(static_cast<float>(x), static_cast<float>(y));
        if (shortcutControl >= kControlShortcutBase) {
            const auto activityId = shortcut_activity_id(shortcutControl);
            if (!activityId.empty()) {
                for (auto& provider : providers_) provider->invoke(activityId, L"launch");
            }
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
    update_monitor_position(true);
    const float s = layout_scale();
    widthSpring_.snap(expanded_ ? kExpandedWidth * s : collapsed_width_px());
    heightSpring_.snap(expanded_ ? expanded_height_px() : kCollapsedHeight * s);
}

void OverlayWindow::update_animation(float dtSeconds) {
    widthSpring_.step(dtSeconds);
    heightSpring_.step(dtSeconds);
    hoverSpring_.step(dtSeconds);
    expandSpring_.step(dtSeconds);
    visibilitySpring_.step(dtSeconds);
    pressSpring_.step(dtSeconds);
    positionXSpring_.step(dtSeconds);
    positionYSpring_.step(dtSeconds);

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

    if (positionInitialized_ && !windowDragging_) {
        RECT current{};
        GetWindowRect(hwnd_, &current);
        const int left = static_cast<int>(std::lround(positionXSpring_.value()));
        const int top = static_cast<int>(std::lround(positionYSpring_.value()));
        if (current.left != left || current.top != top) {
            SetWindowPos(hwnd_, HWND_TOPMOST, left, top, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
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
    const float physicalScale = scale_for_dpi(dpi_);
    const float s = layout_scale();
    renderState_.dpiScale = s;

    const UINT desiredW = static_cast<UINT>(std::lround(520.0f * s));
    const UINT desiredH = static_cast<UINT>(std::lround(760.0f * s));
    if (forceResize || desiredW != maxWidthPx_ || desiredH != maxHeightPx_) {
        maxWidthPx_ = desiredW;
        maxHeightPx_ = desiredH;
        renderer_.resize(maxWidthPx_, maxHeightPx_);
    }

    const float collapsedWidth = collapsed_width_px();
    const float collapsedHeight = kCollapsedHeight * s;
    const float marginX = 12.0f * physicalScale;
    const float topMargin = static_cast<float>(settings_.topOffsetDip) * physicalScale + 8.0f * s;
    const float bottomMargin = 12.0f * physicalScale;
    const float availableX = std::max(0.0f, static_cast<float>(monitor.work.right - monitor.work.left) -
                                      marginX * 2.0f - collapsedWidth);
    const float availableY = std::max(0.0f, static_cast<float>(monitor.work.bottom - monitor.work.top) -
                                      topMargin - bottomMargin - collapsedHeight);
    const float anchorLeft = static_cast<float>(monitor.work.left) + marginX + availableX * settings_.positionX;
    const float anchorTop = static_cast<float>(monitor.work.top) + topMargin + availableY * settings_.positionY;
    const float panelHeight = settingsMode_ ? kSettingsHeight * s : renderer_.expanded_height(renderState_);
    renderState_.expandUp = prefer_upward_panel(anchorTop, collapsedHeight, panelHeight,
                                                 static_cast<float>(monitor.work.top),
                                                 static_cast<float>(monitor.work.bottom));
    const float localLeft = (static_cast<float>(maxWidthPx_) - collapsedWidth) * 0.5f;
    const float localTop = renderState_.expandUp
        ? static_cast<float>(maxHeightPx_) - 8.0f * s - collapsedHeight
        : 8.0f * s;
    const int originX = static_cast<int>(std::lround(anchorLeft - localLeft));
    const int originY = static_cast<int>(std::lround(anchorTop - localTop));
    if (!positionInitialized_ || forceResize) {
        positionXSpring_.snap(static_cast<float>(originX));
        positionYSpring_.snap(static_cast<float>(originY));
        positionInitialized_ = true;
    } else {
        positionXSpring_.set_target(static_cast<float>(originX));
        positionYSpring_.set_target(static_cast<float>(originY));
    }
    SetWindowPos(hwnd_, HWND_TOPMOST,
                 static_cast<int>(std::lround(positionXSpring_.value())),
                 static_cast<int>(std::lround(positionYSpring_.value())),
                 static_cast<int>(maxWidthPx_), static_cast<int>(maxHeightPx_),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

float OverlayWindow::layout_scale() const noexcept {
    constexpr float presets[]{0.88f, 1.0f, 1.15f};
    return scale_for_dpi(dpi_) * presets[std::clamp(settings_.islandSizePreset, 0, 2)];
}

float OverlayWindow::collapsed_width_px() const noexcept {
    return compact_island_width(settings_.compactMediaMode, settings_.compactRingCount,
                                renderState_.hasMedia) * layout_scale();
}

float OverlayWindow::expanded_height_px() const noexcept {
    return settingsMode_ ? kSettingsHeight * renderState_.dpiScale : renderer_.expanded_height(renderState_);
}

void OverlayWindow::apply_settings_to_render_state() {
    renderState_.startWithWindows = settings_.startWithWindows;
    renderState_.hideInFullscreen = settings_.hideInFullscreen;
    renderState_.expandOnHover = settings_.expandOnHover;
    renderState_.showAiUsage = settings_.showAiUsage;
    renderState_.compactMediaMode = settings_.compactMediaMode;
    renderState_.compactRingCount = settings_.compactRingCount;
    renderState_.showSystemMetrics = settings_.showSystemMetrics;
    renderState_.showAppLauncher = settings_.showAppLauncher;
    renderState_.showCommandShortcuts = settings_.showCommandShortcuts;
    renderState_.showMusicPlayer = settings_.showMusicPlayer;
    renderState_.monitorAtCursor = settings_.monitorAtCursor;
    renderState_.islandSizePreset = settings_.islandSizePreset;
    renderState_.islandShape = settings_.islandShape;
    renderState_.buttonStyle = settings_.buttonStyle;
    renderState_.widgetOrder = settings_.widgetOrder;
    renderState_.selectedAiProvider = selectedAiProvider_;
    renderState_.aiProviderPage = std::clamp(aiProviderPage_, 0, kAiProviderPageCount - 1);
    const std::size_t provider = static_cast<std::size_t>(std::clamp(
        selectedAiProvider_, 0, static_cast<int>(kAIProviders.size() - 1)));
    renderState_.selectedAiVisible = settings_.aiVisible[provider];
    renderState_.selectedAiRing = settings_.aiRings[provider];
    renderState_.selectedAiColor = settings_.aiColors[provider];
    renderState_.aiVisibleCount = static_cast<int>(std::ranges::count(settings_.aiVisible, true));
    renderState_.aiPageVisible.fill(false);
    const int pageStart = renderState_.aiProviderPage * kAiProviderPageSize;
    for (int row = 0; row < kAiProviderPageSize; ++row) {
        const int index = pageStart + row;
        if (index < static_cast<int>(kAIProviders.size())) {
            renderState_.aiPageVisible[static_cast<std::size_t>(row)] =
                settings_.aiVisible[static_cast<std::size_t>(index)];
        }
    }
    renderState_.settingsPage = settingsPage_;
}

void OverlayWindow::move_widget(int row, int direction) {
    const int destination = row + direction;
    if (row < 0 || row >= 5 || destination < 0 || destination >= 5) return;
    std::swap(settings_.widgetOrder[static_cast<std::size_t>(row)],
              settings_.widgetOrder[static_cast<std::size_t>(destination)]);
}

void OverlayWindow::finish_window_drag() {
    windowDragging_ = false;
    RECT window{};
    GetWindowRect(hwnd_, &window);
    const auto monitor = monitor_for_window(hwnd_, false);
    dpi_ = monitor.dpi;
    const float physicalScale = scale_for_dpi(monitor.dpi);
    const float s = layout_scale();
    const float collapsedWidth = collapsed_width_px();
    const float collapsedHeight = kCollapsedHeight * s;
    const float localTop = renderState_.expandUp
        ? static_cast<float>(maxHeightPx_) - 8.0f * s - collapsedHeight
        : 8.0f * s;
    const float marginX = 12.0f * physicalScale;
    const float topMargin = static_cast<float>(settings_.topOffsetDip) * physicalScale + 8.0f * s;
    const float bottomMargin = 12.0f * physicalScale;
    const float availableX = std::max(0.0f, static_cast<float>(monitor.work.right - monitor.work.left) -
                                      marginX * 2.0f - collapsedWidth);
    const float availableY = std::max(0.0f, static_cast<float>(monitor.work.bottom - monitor.work.top) -
                                      topMargin - bottomMargin - collapsedHeight);
    const float islandLeft = static_cast<float>(window.left) +
                             (static_cast<float>(maxWidthPx_) - collapsedWidth) * 0.5f;
    const float islandTop = static_cast<float>(window.top) + localTop;
    const float rawX = availableX > 0.0f
        ? (islandLeft - (static_cast<float>(monitor.work.left) + marginX)) / availableX : 0.5f;
    const float rawY = availableY > 0.0f
        ? (islandTop - (static_cast<float>(monitor.work.top) + topMargin)) / availableY : 0.0f;
    const float threshold = 56.0f * physicalScale;
    settings_.positionX = snap_normalized(rawX, availableX, threshold);
    settings_.positionY = snap_normalized(rawY, availableY, threshold);
    settings_.save();
    apply_settings_to_render_state();
    update_monitor_position(false);
}

void OverlayWindow::update_visibility_policy() {
    fullscreenHidden_ = settings_.hideInFullscreen && foreground_is_fullscreen();
    const bool shouldHide = manualHidden_ || fullscreenHidden_;
    visibilitySpring_.set_target(shouldHide ? 0.0f : 1.0f);
}

void OverlayWindow::set_expanded(bool expanded) {
    expanded_ = expanded;
    if (!expanded_) {
        if (shortcutEditor_) shortcutEditor_->hide();
        settingsMode_ = false;
        settingsPage_ = 0;
        renderState_.settingsPage = 0;
    }
    const float s = renderState_.dpiScale;
    widthSpring_.set_target(expanded ? kExpandedWidth * s :
                            collapsed_width_px() + (trackingMouse_ ? 14.0f * s : 0.0f));
    heightSpring_.set_target(expanded ? expanded_height_px() : kCollapsedHeight * s);
    expandSpring_.set_target(expanded ? 1.0f : 0.0f);
    if (expanded) update_monitor_position(false);
}

void OverlayWindow::set_settings_mode(bool enabled) {
    settingsMode_ = enabled;
    if (enabled) settingsPage_ = 0;
    if (!enabled && shortcutEditor_) shortcutEditor_->hide();
    if (enabled && !expanded_) set_expanded(true);
    if (expanded_) heightSpring_.set_target(expanded_height_px());
    renderState_.settingsMode = enabled;
    renderState_.settingsPage = settingsPage_;
    update_monitor_position(false);
}

void OverlayWindow::open_shortcut_editor() {
    if (!shortcutEditor_) shortcutEditor_ = std::make_unique<ShortcutEditor>();
    settingsPage_ = 5;
    renderState_.settingsPage = settingsPage_;
    apply_settings_to_render_state();
    set_expanded(true);
    update_shortcut_editor_bounds();
    const float s = renderState_.dpiScale;
    const float width = kExpandedWidth * s;
    const float height = kSettingsHeight * s;
    const float left = (static_cast<float>(renderer_.width()) - width) * 0.5f;
    const float top = renderState_.expandUp
        ? static_cast<float>(renderer_.height()) - 8.0f * s - height
        : 8.0f * s;
    const RECT bounds{
        static_cast<LONG>(std::lround(left)),
        static_cast<LONG>(std::lround(top + 64.0f * s)),
        static_cast<LONG>(std::lround(left + width)),
        static_cast<LONG>(std::lround(top + height))};
    shortcutEditor_->show_embedded(hwnd_, instance_, bounds, static_cast<int>(24.0f * s), [this] {
        settings_ = Settings::load();
        apply_settings_to_render_state();
        update_monitor_position(false);
        update_shortcut_editor_bounds();
    });
}

void OverlayWindow::update_shortcut_editor_bounds() {
    if (!shortcutEditor_ || !settingsMode_ || settingsPage_ != 5 || !expanded_) return;
    const auto island = renderer_.island_rect(renderState_);
    const float s = renderState_.dpiScale;
    const RECT bounds{
        static_cast<LONG>(std::lround(island.left)),
        static_cast<LONG>(std::lround(island.top + 64.0f * s)),
        static_cast<LONG>(std::lround(island.right)),
        static_cast<LONG>(std::lround(island.bottom))};
    shortcutEditor_->resize_embedded(bounds, static_cast<int>(24.0f * s));
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
    const float top = rect.top + (settingsMode_ ? 4.0f : 0.0f) * s;
    const float bottom = rect.top + (settingsMode_ ? 48.0f : 42.0f) * s;
    return point_in_rect(x, y, D2D1::RectF(rect.right - 70.0f * s, top,
                                           rect.right - 26.0f * s, bottom));
}

int OverlayWindow::hit_test_media_action(float x, float y) const {
    const float s = renderState_.dpiScale;
    const auto activities = store_.snapshot();
    const bool hasMedia = std::ranges::any_of(activities, [](const Activity& a) { return a.kind == ActivityKind::Media; });
    if (!hasMedia || settingsMode_ || !settings_.showMusicPlayer) return -1;
    const auto rect = renderer_.widget_rect(renderState_, static_cast<int>(WidgetKind::Music));
    const float centerX = (rect.left + rect.right) * 0.5f;
    const float centerY = rect.top + 171.0f * s;
    constexpr float offsets[]{-58.0f, 0.0f, 58.0f};
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
    const int rowCount = settingsPage_ == 0 ? 7 : settingsPage_ == 1 ? 6 :
                         settingsPage_ == 3 ? 6 : settingsPage_ == 4 ? 7 :
                         settingsPage_ == 5 ? 0 : 5;
    for (int row = 0; row < rowCount; ++row) {
        if (point_in_rect(x, y, D2D1::RectF(left, top, right, top + 60.0f * s))) return row;
        top += 68.0f * s;
    }
    return -1;
}

int OverlayWindow::hit_test_setting_control(float x, float y) const {
    const int row = hit_test_setting_row(x, y);
    if (row < 0) return -1;
    if (settingsPage_ == 0) return kControlSettingBase + row;
    if (settingsPage_ == 2) return kControlAppearanceBase + row;
    if (settingsPage_ == 3) return kControlAiBase + row;
    if (settingsPage_ == 4) return kControlAiPickerBase + row;
    if (row == 5) return 35;

    const float s = renderState_.dpiScale;
    const auto rect = renderer_.island_rect(renderState_);
    const float right = rect.right - 22.0f * s;
    int action = 0;
    if (x >= right - 42.0f * s) action = 2;
    else if (x >= right - 72.0f * s) action = 1;
    return kControlWidgetBase + row * 3 + action;
}

int OverlayWindow::hit_test_shortcut(float x, float y) const {
    if (!expanded_ || settingsMode_) return -1;
    const auto activities = store_.snapshot();
    const float s = renderState_.dpiScale;
    for (const int widget : {static_cast<int>(WidgetKind::AppLauncher), static_cast<int>(WidgetKind::Commands)}) {
        if (!widget_enabled(renderState_, widget)) continue;
        const auto card = renderer_.widget_rect(renderState_, widget);
        const float cardWidth = card.right - card.left;
        const std::wstring_view source = widget == static_cast<int>(WidgetKind::Commands)
            ? L"shortcut.command" : L"shortcut.app";
        const int itemCount = static_cast<int>(std::ranges::count_if(activities, [&](const Activity& activity) {
            return activity.source == source;
        }));
        if (itemCount <= 0) continue;
        int item = 0;
        for (const auto& activity : activities) {
            if (activity.source != source || item >= static_cast<int>(kShortcutSlots)) continue;
            const float cellWidth = cardWidth / static_cast<float>(itemCount);
            const float centerX = card.left + cellWidth * (static_cast<float>(item) + 0.5f);
            if (point_in_rect(x, y, D2D1::RectF(centerX - cellWidth * 0.46f, card.top + 29.0f * s,
                                                centerX + cellWidth * 0.46f, card.bottom - 3.0f * s))) {
                return kControlShortcutBase +
                       (widget == static_cast<int>(WidgetKind::Commands) ? static_cast<int>(kShortcutSlots) : 0) + item;
            }
            ++item;
        }
    }
    return -1;
}

std::wstring OverlayWindow::shortcut_activity_id(int control) const {
    if (control < kControlShortcutBase || control >= kControlShortcutBase + static_cast<int>(kShortcutSlots * 2)) return {};
    const bool commands = control >= kControlShortcutBase + static_cast<int>(kShortcutSlots);
    const int wanted = commands ? control - kControlShortcutBase - static_cast<int>(kShortcutSlots)
                                : control - kControlShortcutBase;
    const std::wstring_view source = commands ? L"shortcut.command" : L"shortcut.app";
    int index = 0;
    for (const auto& activity : store_.snapshot()) {
        if (activity.source != source) continue;
        if (index++ == wanted) return activity.id;
    }
    return {};
}

int OverlayWindow::control_at(float x, float y) const {
    if (hit_test_gear(x, y) && expanded_) return kControlGear;
    if (!expanded_) return kControlIsland;
    if (settingsMode_) {
        return hit_test_setting_control(x, y);
    }
    const int mediaAction = hit_test_media_action(x, y);
    if (mediaAction >= 0) return kControlMediaBase + mediaAction;
    const int shortcut = hit_test_shortcut(x, y);
    if (shortcut >= 0) return shortcut;
    const auto rect = renderer_.island_rect(renderState_);
    return y <= rect.top + 44.0f * renderState_.dpiScale ? kControlIsland : -1;
}

} // namespace isle
