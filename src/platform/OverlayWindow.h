#pragma once

#include "../core/ActivityStore.h"
#include "../core/Settings.h"
#include "../core/Spring.h"
#include "../core/Provider.h"
#include "Renderer.h"
#include "ShortcutEditor.h"
#include "TrayIcon.h"

#include <Windows.h>

#include <chrono>
#include <memory>
#include <vector>

namespace isle {

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    bool create(HINSTANCE instance, int showCommand);
    int message_loop();

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam);

    void on_create();
    void on_destroy();
    void on_timer();
    void on_mouse_move(int x, int y);
    void on_mouse_leave();
    void on_left_button_down(int x, int y);
    void on_left_button_up(int x, int y);
    void on_right_button_up(int x, int y);
    void on_tray_message(WPARAM wParam, LPARAM lParam);
    void on_command(UINT id);
    void on_dpi_changed(UINT dpi, const RECT* suggested);

    void update_animation(float dtSeconds);
    void update_clock();
    void update_region();
    void update_monitor_position(bool forceResize = false);
    void update_visibility_policy();
    void apply_settings_to_render_state();
    void finish_window_drag();
    void move_widget(int row, int direction);
    void set_expanded(bool expanded);
    void set_settings_mode(bool enabled);
    void open_shortcut_editor();
    void update_shortcut_editor_bounds();
    void toggle_manual_hidden();
    bool foreground_is_fullscreen() const;
    bool hit_test_gear(float x, float y) const;
    int hit_test_media_action(float x, float y) const;
    int hit_test_setting_row(float x, float y) const;
    int hit_test_setting_control(float x, float y) const;
    int hit_test_shortcut(float x, float y) const;
    std::wstring shortcut_activity_id(int control) const;
    float layout_scale() const noexcept;
    float collapsed_width_px() const noexcept;
    float expanded_height_px() const noexcept;
    int control_at(float x, float y) const;

    HINSTANCE instance_{};
    HWND hwnd_{};
    UINT dpi_{96};
    UINT maxWidthPx_{520};
    UINT maxHeightPx_{760};
    bool trackingMouse_{false};
    bool expanded_{false};
    bool settingsMode_{false};
    bool manualHidden_{false};
    bool fullscreenHidden_{false};
    bool providersStarted_{false};
    bool regionEmpty_{false};
    bool pressHeld_{false};
    bool windowDragging_{false};
    bool positionInitialized_{false};
    int pressedControl_{-1};
    int settingsPage_{0};
    int aiProviderPage_{0};
    int selectedAiProvider_{0};
    RECT lastRegion_{};
    POINT dragStartCursor_{};
    POINT dragStartOrigin_{};

    Settings settings_{};
    RenderState renderState_{};
    Renderer renderer_{};
    ActivityStore store_{};
    std::vector<std::unique_ptr<IProvider>> providers_;
    std::unique_ptr<TrayIcon> tray_;
    std::unique_ptr<ShortcutEditor> shortcutEditor_;

    Spring widthSpring_{230.0f};
    Spring heightSpring_{40.0f};
    Spring hoverSpring_{0.0f};
    Spring expandSpring_{0.0f};
    Spring visibilitySpring_{1.0f};
    Spring pressSpring_{0.0f};
    Spring positionXSpring_{0.0f};
    Spring positionYSpring_{0.0f};

    std::chrono::steady_clock::time_point lastFrame_{};
    std::chrono::steady_clock::time_point lastClockUpdate_{};
};

} // namespace isle
