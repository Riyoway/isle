#pragma once

#include "../core/Types.h"

#include <Windows.h>
#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dcomp.h>
#include <dwrite_3.h>
#include <endpointvolume.h>
#include <dxgi1_6.h>
#include <mmdeviceapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace isle {

class Renderer {
public:
    Renderer() = default;
    ~Renderer() = default;

    void initialize(HWND hwnd, UINT widthPx, UINT heightPx);
    void resize(UINT widthPx, UINT heightPx);
    void render(const RenderState& state, const std::vector<Activity>& activities);

    [[nodiscard]] D2D1_RECT_F island_rect(const RenderState& state) const noexcept;
    [[nodiscard]] D2D1_RECT_F widget_rect(const RenderState& state, int widget) const noexcept;
    [[nodiscard]] float expanded_height(const RenderState& state) const noexcept;
    [[nodiscard]] UINT width() const noexcept { return width_; }
    [[nodiscard]] UINT height() const noexcept { return height_; }

private:
    void create_device_resources();
    void create_size_resources();
    void ensure_text_formats(float scale);

    void draw_shadow(const D2D1_ROUNDED_RECT& rect, float opacity);
    void draw_collapsed(const RenderState& state, const std::vector<Activity>& activities, const D2D1_RECT_F& rect);
    void draw_expanded(const RenderState& state, const std::vector<Activity>& activities, const D2D1_RECT_F& rect);
    void draw_settings(const RenderState& state, const D2D1_RECT_F& rect);
    void draw_widget_grid(const RenderState& state, const std::vector<Activity>& activities,
                          const D2D1_RECT_F& rect, float opacity);
    void draw_media_widget(const RenderState& state, const Activity& media,
                           D2D1_RECT_F rect, float opacity);
    void draw_ai_widget(const RenderState& state, const std::vector<Activity>& activities,
                        D2D1_RECT_F rect, float opacity);
    void draw_system_widget(const RenderState& state, const std::vector<Activity>& activities,
                            D2D1_RECT_F rect, float opacity);
    void draw_shortcut_widget(const RenderState& state, const std::vector<Activity>& activities,
                              D2D1_RECT_F rect, int widget, float opacity);
    void draw_metric(const Activity& activity, D2D1_POINT_2F center, float radius, float scale, float opacity);
    void draw_provider_badge(const Activity& activity, D2D1_RECT_F rect, float opacity);
    bool draw_provider_icon(std::wstring_view providerId, std::wstring_view accentHex,
                            D2D1_RECT_F rect, float opacity);
    ID2D1SvgDocument* provider_icon(std::wstring_view providerId, std::wstring_view accentHex);
    void draw_collapsed_ai_rings(const RenderState& state, const std::vector<Activity>& activities,
                                 const D2D1_RECT_F& rect, float opacity);
    void draw_artwork(const Activity& activity, D2D1_RECT_F rect, float radius, float opacity);
    void draw_waveform(D2D1_RECT_F rect, D2D1_COLOR_F color, float opacity,
                       bool active, bool audioReactive = false);
    void update_audio_history(bool active);
    [[nodiscard]] float audio_peak();
    void draw_media_control_icon(std::wstring_view action, D2D1_RECT_F rect,
                                 D2D1_COLOR_F color, float opacity, bool playing);
    void draw_progress_ring(D2D1_POINT_2F center, float radius, float thickness, double progress,
                            const D2D1_COLOR_F& track, const D2D1_COLOR_F& accent, float opacity);
    void draw_marquee_text(std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                           D2D1_COLOR_F color, float opacity = 1.0f);
    void draw_text(std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                   D2D1_COLOR_F color, float opacity = 1.0f);
    void fill_round_rect(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color, float opacity = 1.0f);
    void stroke_round_rect(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color, float width, float opacity = 1.0f);

    static D2D1_COLOR_F color_from_hex(std::wstring_view hex, float alpha = 1.0f);
    ID2D1Bitmap1* artwork_bitmap(const Activity& activity);

    HWND hwnd_{};
    UINT width_{1};
    UINT height_{1};
    float formatScale_{0.0f};

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;

    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dContext_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext5> svgContext_;
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1SvgDocument>> providerIcons_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> artworkBitmap_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStrokeStyle_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory_;
    const void* artworkKey_{};

    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteFontCollection1> fontCollection_;
    std::wstring uiFontFamily_{L"Segoe UI Variable Text"};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> metricFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> iconFormat_;
    std::wstring marqueeText_;
    std::chrono::steady_clock::time_point marqueeStarted_{};
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> audioDeviceEnumerator_;
    Microsoft::WRL::ComPtr<IAudioMeterInformation> audioMeter_;
    std::array<float, 32> audioHistory_{};
    std::chrono::steady_clock::time_point lastAudioSample_{};

    Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
};

} // namespace isle
