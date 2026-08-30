#include "Renderer.h"

#include <d2d1helper.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace isle {

using Microsoft::WRL::ComPtr;

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(what);
}

D2D1_COLOR_F with_alpha(D2D1_COLOR_F color, float opacity) {
    color.a *= opacity;
    return color;
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float smoothstep(float start, float end, float value) {
    const float t = std::clamp((value - start) / (end - start), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

D2D1_RECT_F inset_rect(D2D1_RECT_F rect, float inset) {
    return D2D1::RectF(rect.left + inset, rect.top + inset,
                       rect.right - inset, rect.bottom - inset);
}

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length == 0 || length >= path.size()
        ? std::filesystem::path{}
        : std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

std::wstring metric_value(const Activity& activity) {
    if (activity.value.has_value()) {
        return std::to_wstring(static_cast<int>(std::round(*activity.value))) + activity.valueSuffix;
    }
    if (activity.progress.has_value()) {
        return std::to_wstring(static_cast<int>(std::round(*activity.progress * 100.0))) + L"%";
    }
    return activity.subtitle;
}

const Activity* media_activity(const std::vector<Activity>& activities) {
    const auto it = std::ranges::find_if(activities, [](const Activity& activity) {
        return activity.kind == ActivityKind::Media;
    });
    return it == activities.end() ? nullptr : &*it;
}

const Activity* compact_activity(const std::vector<Activity>& activities) {
    if (const auto* media = media_activity(activities)) return media;
    const auto live = std::ranges::find_if(activities, [](const Activity& activity) {
        return activity.kind != ActivityKind::Metric;
    });
    return live == activities.end() ? (activities.empty() ? nullptr : &activities.front()) : &*live;
}

double live_progress(const Activity& activity) {
    if (!activity.durationSeconds.has_value() || *activity.durationSeconds <= 0.0 ||
        !activity.elapsedSeconds.has_value()) {
        return clamp01(activity.progress.value_or(0.0));
    }
    double elapsed = *activity.elapsedSeconds;
    if (activity.active) {
        elapsed += std::chrono::duration<double>(std::chrono::steady_clock::now() - activity.updatedAt).count();
    }
    return clamp01(elapsed / *activity.durationSeconds);
}

std::wstring duration_text(double seconds) {
    const int total = std::max(0, static_cast<int>(std::round(seconds)));
    const int hours = total / 3600;
    const int minutes = (total / 60) % 60;
    const int secs = total % 60;
    std::array<wchar_t, 24> text{};
    if (hours > 0) {
        swprintf_s(text.data(), text.size(), L"%d:%02d:%02d", hours, minutes, secs);
    } else {
        swprintf_s(text.data(), text.size(), L"%d:%02d", minutes, secs);
    }
    return text.data();
}

} // namespace

void Renderer::initialize(HWND hwnd, UINT widthPx, UINT heightPx) {
    hwnd_ = hwnd;
    width_ = std::max<UINT>(1, widthPx);
    height_ = std::max<UINT>(1, heightPx);
    create_device_resources();
    create_size_resources();
}

void Renderer::create_device_resources() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL actual{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &d3dDevice_, &actual, &d3dContext_);
    if (FAILED(hr)) {
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                &d3dDevice_, &actual, &d3dContext_),
              "D3D11CreateDevice failed");
    }

    check(d3dDevice_.As(&dxgiDevice_), "Query IDXGIDevice failed");
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory_)), "CreateDXGIFactory2 failed");

    D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                            &options, reinterpret_cast<void**>(d2dFactory_.GetAddressOf())),
          "D2D1CreateFactory failed");
    check(d2dFactory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_), "Create D2D device failed");
    check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_),
          "Create D2D context failed");

    D2D1_STROKE_STYLE_PROPERTIES stroke{};
    stroke.startCap = D2D1_CAP_STYLE_ROUND;
    stroke.endCap = D2D1_CAP_STYLE_ROUND;
    stroke.dashCap = D2D1_CAP_STYLE_ROUND;
    stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
    stroke.miterLimit = 10.0f;
    stroke.dashStyle = D2D1_DASH_STYLE_SOLID;
    check(d2dFactory_->CreateStrokeStyle(stroke, nullptr, 0, &roundStrokeStyle_),
          "Create stroke style failed");

    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())),
          "DWriteCreateFactory failed");

    const auto fontPath = executable_directory() / L"fonts" / L"InterVariable.ttf";
    ComPtr<IDWriteFactory5> factory5;
    ComPtr<IDWriteFontFile> fontFile;
    ComPtr<IDWriteFontSetBuilder1> fontSetBuilder;
    ComPtr<IDWriteFontSet> fontSet;
    if (std::filesystem::exists(fontPath) &&
        SUCCEEDED(dwriteFactory_.As(&factory5)) &&
        SUCCEEDED(dwriteFactory_->CreateFontFileReference(fontPath.c_str(), nullptr, &fontFile)) &&
        SUCCEEDED(factory5->CreateFontSetBuilder(&fontSetBuilder)) &&
        SUCCEEDED(fontSetBuilder->AddFontFile(fontFile.Get())) &&
        SUCCEEDED(fontSetBuilder->CreateFontSet(&fontSet)) &&
        SUCCEEDED(factory5->CreateFontCollectionFromFontSet(fontSet.Get(), &fontCollection_)) &&
        fontCollection_->GetFontFamilyCount() > 0) {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        UINT32 length = 0;
        if (SUCCEEDED(fontCollection_->GetFontFamily(0, &family)) &&
            SUCCEEDED(family->GetFamilyNames(&names)) && names->GetCount() > 0 &&
            SUCCEEDED(names->GetStringLength(0, &length))) {
            uiFontFamily_.assign(static_cast<std::size_t>(length) + 1, L'\0');
            if (SUCCEEDED(names->GetString(0, uiFontFamily_.data(), length + 1))) {
                uiFontFamily_.resize(length);
            }
        }
    }
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&wicFactory_)),
          "Create WIC factory failed");

    check(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcompDevice_)),
          "DCompositionCreateDevice failed");
    check(dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_),
          "CreateTargetForHwnd failed");
    check(dcompDevice_->CreateVisual(&dcompVisual_), "CreateVisual failed");
    check(dcompTarget_->SetRoot(dcompVisual_.Get()), "SetRoot failed");
}

void Renderer::create_size_resources() {
    targetBitmap_.Reset();
    swapChain_.Reset();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    check(dxgiFactory_->CreateSwapChainForComposition(d3dDevice_.Get(), &desc, nullptr, &swapChain_),
          "CreateSwapChainForComposition failed");

    ComPtr<IDXGISurface> surface;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)), "Get swap-chain buffer failed");
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    check(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &targetBitmap_),
          "CreateBitmapFromDxgiSurface failed");
    d2dContext_->SetTarget(targetBitmap_.Get());

    check(dcompVisual_->SetContent(swapChain_.Get()), "Set DirectComposition content failed");
    check(dcompDevice_->Commit(), "DirectComposition commit failed");
}

void Renderer::resize(UINT widthPx, UINT heightPx) {
    widthPx = std::max<UINT>(1, widthPx);
    heightPx = std::max<UINT>(1, heightPx);
    if (widthPx == width_ && heightPx == height_) return;
    width_ = widthPx;
    height_ = heightPx;
    create_size_resources();
}

void Renderer::ensure_text_formats(float scale) {
    if (std::abs(formatScale_ - scale) < 0.01f && titleFormat_) return;
    formatScale_ = scale;

    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, bool useUiFont,
                    ComPtr<IDWriteTextFormat>& out) {
        out.Reset();
        const wchar_t* family = useUiFont ? uiFontFamily_.c_str() : L"Segoe Fluent Icons";
        IDWriteFontCollection* collection = useUiFont ? fontCollection_.Get() : nullptr;
        HRESULT result = dwriteFactory_->CreateTextFormat(family, collection, weight,
                                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                           size * scale, L"en-us", &out);
        if (FAILED(result) && useUiFont) {
            result = dwriteFactory_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, weight,
                                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                      size * scale, L"en-us", &out);
        }
        check(result, "CreateTextFormat failed");
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (useUiFont) {
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(out.Get(), &ellipsis))) {
                out->SetTrimming(&trimming, ellipsis.Get());
            }
        }
    };

    make(18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, titleFormat_);
    make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, bodyFormat_);
    make(10.5f, DWRITE_FONT_WEIGHT_NORMAL, true, smallFormat_);
    make(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, true, metricFormat_);
    make(18.0f, DWRITE_FONT_WEIGHT_NORMAL, false, iconFormat_);
}

D2D1_RECT_F Renderer::island_rect(const RenderState& state) const noexcept {
    const float left = (static_cast<float>(width_) - state.islandWidth) * 0.5f;
    const float top = 8.0f * state.dpiScale;
    return D2D1::RectF(left, top, left + state.islandWidth, top + state.islandHeight);
}

void Renderer::render(const RenderState& state, const std::vector<Activity>& activities) {
    ensure_text_formats(state.dpiScale);
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0, 0.0f));

    if (!state.hidden && state.visibility > 0.001f) {
        const auto rect = island_rect(state);
        const float radius = lerp(20.0f * state.dpiScale, 42.0f * state.dpiScale, state.expandAmount);
        const D2D1_ROUNDED_RECT rounded{rect, radius, radius};
        draw_shadow(rounded, state.visibility);

        fill_round_rect(rect, radius, D2D1::ColorF(0x020202), 0.995f * state.visibility);
        stroke_round_rect(rect, radius, D2D1::ColorF(0xFFFFFF), state.dpiScale,
                          0.12f * state.visibility);
        const auto inner = D2D1::RectF(rect.left + state.dpiScale, rect.top + state.dpiScale,
                                       rect.right - state.dpiScale, rect.bottom - state.dpiScale);
        stroke_round_rect(inner, std::max(0.0f, radius - state.dpiScale), D2D1::ColorF(0x000000),
                          state.dpiScale, 0.45f * state.visibility);

        if (state.expandAmount < 0.62f) draw_collapsed(state, activities, rect);
        if (state.expandAmount > 0.20f) {
            if (state.settingsMode) draw_settings(state, rect);
            else draw_expanded(state, activities, rect);
        }
    }

    const HRESULT end = d2dContext_->EndDraw();
    if (end == D2DERR_RECREATE_TARGET) {
        create_size_resources();
        return;
    }
    check(end, "D2D EndDraw failed");
    check(swapChain_->Present(1, 0), "Swap-chain present failed");
}

void Renderer::draw_shadow(const D2D1_ROUNDED_RECT& rect, float opacity) {
    const float s = formatScale_ <= 0.0f ? 1.0f : formatScale_;
    for (int i = 5; i >= 1; --i) {
        const float spread = static_cast<float>(i) * 2.2f * s;
        const auto shadow = D2D1::RectF(rect.rect.left - spread, rect.rect.top - spread * 0.15f,
                                        rect.rect.right + spread, rect.rect.bottom + spread);
        stroke_round_rect(shadow, rect.radiusX + spread, D2D1::ColorF(0x000000),
                          3.4f * s, opacity * (0.018f + static_cast<float>(5 - i) * 0.012f));
    }
}

void Renderer::draw_collapsed(const RenderState& state, const std::vector<Activity>& activities,
                              const D2D1_RECT_F& rect) {
    const float s = state.dpiScale;
    const float opacity = state.visibility * (1.0f - smoothstep(0.08f, 0.48f, state.expandAmount));
    if (state.pressedControl == 0 && state.pressAmount > 0.001f) {
        fill_round_rect(inset_rect(rect, 1.0f * s), 19.0f * s, D2D1::ColorF(0xFFFFFF),
                        0.045f * opacity * state.pressAmount);
    }
    const Activity* primary = compact_activity(activities);
    const float cy = (rect.top + rect.bottom) * 0.5f;

    if (!primary) {
        ComPtr<ID2D1SolidColorBrush> dot;
        d2dContext_->CreateSolidColorBrush(with_alpha(D2D1::ColorF(0xA1A1AA), opacity), &dot);
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(rect.left + 18.0f * s, cy), 3.5f * s, 3.5f * s), dot.Get());
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(L"Isle", bodyFormat_.Get(), D2D1::RectF(rect.left + 30.0f * s, rect.top,
                  rect.right - 70.0f * s, rect.bottom), D2D1::ColorF(0xF5F5F7), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_text(state.timeText, smallFormat_.Get(), D2D1::RectF(rect.right - 66.0f * s, rect.top,
                  rect.right - 14.0f * s, rect.bottom), D2D1::ColorF(0xA1A1AA), opacity);
        return;
    }

    const D2D1_COLOR_F accent = color_from_hex(primary->accent);
    if (primary->kind == ActivityKind::Media) {
        const auto art = D2D1::RectF(rect.left + 6.0f * s, rect.top + 6.0f * s,
                                     rect.left + 34.0f * s, rect.bottom - 6.0f * s);
        draw_artwork(*primary, art, 8.0f * s, opacity);
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(primary->title, bodyFormat_.Get(), D2D1::RectF(art.right + 9.0f * s, rect.top,
                  rect.right - 72.0f * s, rect.bottom), D2D1::ColorF(0xF7F7F8), opacity);
        draw_waveform(D2D1::RectF(rect.right - 60.0f * s, rect.top + 9.0f * s,
                                  rect.right - 14.0f * s, rect.bottom - 9.0f * s),
                      accent, opacity, primary->active);
        return;
    }

    const auto badge = D2D1::RectF(rect.left + 7.0f * s, rect.top + 7.0f * s,
                                   rect.left + 33.0f * s, rect.bottom - 7.0f * s);
    fill_round_rect(badge, 13.0f * s, accent, 0.20f * opacity);
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(primary->glyph, iconFormat_.Get(), badge, accent, opacity);

    bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(primary->title, bodyFormat_.Get(), D2D1::RectF(badge.right + 9.0f * s, rect.top,
              rect.right - 76.0f * s, rect.bottom), D2D1::ColorF(0xF7F7F8), opacity);
    metricFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(metric_value(*primary), metricFormat_.Get(), D2D1::RectF(rect.right - 72.0f * s, rect.top,
              rect.right - 14.0f * s, rect.bottom), accent, opacity);
}

void Renderer::draw_expanded(const RenderState& state, const std::vector<Activity>& activities,
                             const D2D1_RECT_F& rect) {
    const float s = state.dpiScale;
    const float opacity = state.visibility * smoothstep(0.30f, 0.72f, state.expandAmount);
    const float pad = 18.0f * s;
    const float cx = (rect.left + rect.right) * 0.5f;
    const Activity* media = media_activity(activities);

    const D2D1_COLOR_F accent = media ? color_from_hex(media->accent) : D2D1::ColorF(0x64D2FF);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(media ? L"NOW PLAYING" : L"ISLE", smallFormat_.Get(),
              D2D1::RectF(rect.left + pad, rect.top + 7.0f * s,
                          cx, rect.top + 30.0f * s),
              D2D1::ColorF(0x8E8E93), opacity);

    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(state.timeText, smallFormat_.Get(), D2D1::RectF(cx, rect.top + 7.0f * s,
              rect.right - 52.0f * s, rect.top + 30.0f * s), D2D1::ColorF(0xD4D4D8), opacity);
    const float gearPress = state.pressedControl == 1 ? state.pressAmount : 0.0f;
    const auto gearBase = D2D1::RectF(rect.right - 42.0f * s, rect.top + 6.0f * s,
                                      rect.right - 10.0f * s, rect.top + 38.0f * s);
    const auto gear = inset_rect(gearBase, gearPress * 1.8f * s);
    fill_round_rect(gear, 16.0f * s, gearPress > 0.01f ? D2D1::ColorF(0x2C2C2E) : D2D1::ColorF(0x18181B), opacity);
    stroke_round_rect(gear, 16.0f * s, D2D1::ColorF(0xFFFFFF), 0.8f * s, (0.07f + 0.09f * gearPress) * opacity);
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(L"\uE713", iconFormat_.Get(), gear, D2D1::ColorF(0xF4F4F5), opacity);

    if (media) {
        const auto art = D2D1::RectF(rect.left + pad, rect.top + 50.0f * s,
                                     rect.left + pad + 106.0f * s, rect.top + 156.0f * s);
        draw_artwork(*media, art, 24.0f * s, opacity);

        titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(media->title, titleFormat_.Get(), D2D1::RectF(art.right + 16.0f * s, art.top + 9.0f * s,
                  rect.right - pad, art.top + 40.0f * s), D2D1::ColorF(0xFAFAFA), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(media->subtitle, smallFormat_.Get(), D2D1::RectF(art.right + 16.0f * s, art.top + 40.0f * s,
                  rect.right - pad, art.top + 63.0f * s), D2D1::ColorF(0x8E8E93), opacity);
        draw_waveform(D2D1::RectF(art.right + 16.0f * s, art.bottom - 29.0f * s,
                                  rect.right - pad, art.bottom - 5.0f * s), accent, opacity, media->active);

        const double progress = live_progress(*media);
        const float trackTop = rect.top + 177.0f * s;
        const auto track = D2D1::RectF(rect.left + pad, trackTop, rect.right - pad, trackTop + 4.0f * s);
        fill_round_rect(track, 2.0f * s, D2D1::ColorF(0x27272A), opacity);
        if (progress > 0.001) {
            const auto elapsed = D2D1::RectF(track.left, track.top,
                track.left + (track.right - track.left) * static_cast<float>(progress), track.bottom);
            fill_round_rect(elapsed, 2.0f * s, D2D1::ColorF(0xF4F4F5), opacity);
            ComPtr<ID2D1SolidColorBrush> knob;
            d2dContext_->CreateSolidColorBrush(with_alpha(D2D1::ColorF(0xFFFFFF), opacity), &knob);
            d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(elapsed.right, (track.top + track.bottom) * 0.5f),
                                                    3.4f * s, 3.4f * s), knob.Get());
        }

        if (media->durationSeconds.has_value()) {
            const double duration = *media->durationSeconds;
            smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            draw_text(duration_text(duration * progress), smallFormat_.Get(),
                      D2D1::RectF(track.left, track.bottom + 5.0f * s, cx, track.bottom + 24.0f * s),
                      D2D1::ColorF(0x71717A), opacity);
            smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_text(L"-" + duration_text(duration * (1.0 - progress)), smallFormat_.Get(),
                      D2D1::RectF(cx, track.bottom + 5.0f * s, track.right, track.bottom + 24.0f * s),
                      D2D1::ColorF(0x71717A), opacity);
        }

        const float controlY = rect.top + 231.0f * s;
        const std::array<float, 3> controlX{cx - 62.0f * s, cx, cx + 62.0f * s};
        constexpr std::wstring_view actionIds[]{L"previous", L"toggle", L"next"};
        for (std::size_t i = 0; i < controlX.size(); ++i) {
            const float press = state.pressedControl == static_cast<int>(2 + i) ? state.pressAmount : 0.0f;
            const float size = (i == 1 ? 46.0f : 38.0f) * s - press * 4.0f * s;
            const auto button = D2D1::RectF(controlX[i] - size * 0.5f, controlY - size * 0.5f,
                                            controlX[i] + size * 0.5f, controlY + size * 0.5f);
            if (i == 1) {
                fill_round_rect(button, size * 0.5f, press > 0.01f ? D2D1::ColorF(0xD1D1D6) : D2D1::ColorF(0xFFFFFF), opacity);
            } else {
                fill_round_rect(button, size * 0.5f, press > 0.01f ? D2D1::ColorF(0x303033) : D2D1::ColorF(0x18181B), opacity);
            }
            draw_media_control_icon(actionIds[i], button,
                                    i == 1 ? D2D1::ColorF(0x050505) : D2D1::ColorF(0xF4F4F5),
                                    opacity, media->active);
        }

        std::vector<const Activity*> metrics;
        for (const auto& activity : activities) {
            if (activity.kind == ActivityKind::Metric && metrics.size() < 3) metrics.push_back(&activity);
        }
        if (!metrics.empty()) {
            const float chipTop = rect.bottom - 50.0f * s;
            const float gap = 7.0f * s;
            const float chipWidth = (rect.right - rect.left - 2.0f * pad - gap * 2.0f) / 3.0f;
            for (std::size_t i = 0; i < metrics.size(); ++i) {
                const auto chip = D2D1::RectF(rect.left + pad + static_cast<float>(i) * (chipWidth + gap), chipTop,
                                              rect.left + pad + static_cast<float>(i) * (chipWidth + gap) + chipWidth,
                                              rect.bottom - 12.0f * s);
                fill_round_rect(chip, 13.0f * s, D2D1::ColorF(0x111113), opacity);
                draw_progress_ring(D2D1::Point2F(chip.left + 18.0f * s, (chip.top + chip.bottom) * 0.5f),
                                   9.0f * s, 2.5f * s, metrics[i]->progress.value_or(0.0),
                                   D2D1::ColorF(0x27272A), color_from_hex(metrics[i]->accent), opacity);
                metricFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                draw_text(metric_value(*metrics[i]), metricFormat_.Get(),
                          D2D1::RectF(chip.left + 33.0f * s, chip.top, chip.right - 7.0f * s, chip.bottom),
                          D2D1::ColorF(0xE4E4E7), opacity);
            }
        }
    } else {
        titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(state.timeText, titleFormat_.Get(), D2D1::RectF(rect.left + pad, rect.top + 48.0f * s,
                  rect.left + 140.0f * s, rect.top + 78.0f * s), D2D1::ColorF(0xFAFAFA), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_text(state.dateText, smallFormat_.Get(), D2D1::RectF(rect.left + 140.0f * s, rect.top + 48.0f * s,
                  rect.right - pad, rect.top + 78.0f * s), D2D1::ColorF(0x8E8E93), opacity);

        std::vector<const Activity*> metrics;
        for (const auto& activity : activities) {
            if (activity.kind == ActivityKind::Metric && metrics.size() < 3) metrics.push_back(&activity);
        }
        if (!metrics.empty()) {
            const float usable = rect.right - rect.left - 2.0f * pad;
            const float step = usable / static_cast<float>(metrics.size());
            for (std::size_t i = 0; i < metrics.size(); ++i) {
                draw_metric(*metrics[i],
                            D2D1::Point2F(rect.left + pad + step * (static_cast<float>(i) + 0.5f), rect.top + 123.0f * s),
                            27.0f * s, s, opacity);
            }
        }

        float y = rect.top + 210.0f * s;
        int rows = 0;
        for (const auto& activity : activities) {
            if (activity.kind == ActivityKind::Metric || rows >= 2) continue;
            const auto row = D2D1::RectF(rect.left + pad, y, rect.right - pad, y + 44.0f * s);
            fill_round_rect(row, 15.0f * s, D2D1::ColorF(0x111113), opacity);
            ComPtr<ID2D1SolidColorBrush> dot;
            d2dContext_->CreateSolidColorBrush(with_alpha(color_from_hex(activity.accent), opacity), &dot);
            d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(row.left + 16.0f * s, (row.top + row.bottom) * 0.5f),
                                                    4.0f * s, 4.0f * s), dot.Get());
            bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            draw_text(activity.title, bodyFormat_.Get(), D2D1::RectF(row.left + 29.0f * s, row.top,
                      row.right - 74.0f * s, row.bottom), D2D1::ColorF(0xEDEDEF), opacity);
            metricFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_text(metric_value(activity), metricFormat_.Get(), D2D1::RectF(row.right - 72.0f * s, row.top,
                      row.right - 12.0f * s, row.bottom), D2D1::ColorF(0xA1A1AA), opacity);
            y += 51.0f * s;
            ++rows;
        }
    }

    fill_round_rect(D2D1::RectF(cx - 18.0f * s, rect.bottom - 6.0f * s,
                                cx + 18.0f * s, rect.bottom - 3.0f * s),
                    1.5f * s, D2D1::ColorF(0x3F3F46), opacity);
}

void Renderer::draw_settings(const RenderState& state, const D2D1_RECT_F& rect) {
    const float s = state.dpiScale;
    const float opacity = state.visibility * smoothstep(0.30f, 0.72f, state.expandAmount);
    const float pad = 18.0f * s;

    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(L"Settings", titleFormat_.Get(), D2D1::RectF(rect.left + pad, rect.top + 12.0f * s,
              rect.right - 58.0f * s, rect.top + 40.0f * s), D2D1::ColorF(0xFAFAFA), opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(L"Isle preferences", smallFormat_.Get(), D2D1::RectF(rect.left + pad, rect.top + 38.0f * s,
              rect.right - 58.0f * s, rect.top + 60.0f * s), D2D1::ColorF(0x71717A), opacity);

    const float closePress = state.pressedControl == 1 ? state.pressAmount : 0.0f;
    const auto closeBase = D2D1::RectF(rect.right - 43.0f * s, rect.top + 10.0f * s,
                                       rect.right - 11.0f * s, rect.top + 42.0f * s);
    const auto close = inset_rect(closeBase, closePress * 1.8f * s);
    fill_round_rect(close, 16.0f * s, closePress > 0.01f ? D2D1::ColorF(0x2C2C2E) : D2D1::ColorF(0x18181B), opacity);
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(L"\uE711", iconFormat_.Get(), close, D2D1::ColorF(0xF4F4F5), opacity);

    struct SettingRow { const wchar_t* title; const wchar_t* detail; bool enabled; };
    const SettingRow rows[] = {
        {L"Start with Windows", L"Launch Isle after sign in", state.startWithWindows},
        {L"Hide in fullscreen", L"Stay out of games and video", state.hideInFullscreen},
        {L"Expand on hover", L"Open after a short hover", state.expandOnHover},
        {L"External plugins", L"Open the plugins folder", true},
    };

    float y = rect.top + 74.0f * s;
    for (std::size_t rowIndex = 0; rowIndex < std::size(rows); ++rowIndex) {
        const auto& rowData = rows[rowIndex];
        const float press = state.pressedControl == static_cast<int>(10 + rowIndex) ? state.pressAmount : 0.0f;
        const auto rowBase = D2D1::RectF(rect.left + pad, y, rect.right - pad, y + 60.0f * s);
        const auto row = inset_rect(rowBase, press * 1.2f * s);
        fill_round_rect(row, 18.0f * s, press > 0.01f ? D2D1::ColorF(0x1C1C1E) : D2D1::ColorF(0x111113), opacity);
        stroke_round_rect(row, 18.0f * s, D2D1::ColorF(0xFFFFFF), 0.7f * s, 0.045f * opacity);
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(rowData.title, bodyFormat_.Get(), D2D1::RectF(row.left + 15.0f * s, row.top + 6.0f * s,
                  row.right - 68.0f * s, row.top + 31.0f * s), D2D1::ColorF(0xF4F4F5), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(rowData.detail, smallFormat_.Get(), D2D1::RectF(row.left + 15.0f * s, row.top + 30.0f * s,
                  row.right - 68.0f * s, row.bottom - 5.0f * s), D2D1::ColorF(0x71717A), opacity);

        if (rowIndex == 3) {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(L"\uE76C", iconFormat_.Get(),
                      D2D1::RectF(row.right - 43.0f * s, row.top + 12.0f * s,
                                  row.right - 11.0f * s, row.bottom - 12.0f * s),
                      D2D1::ColorF(0x8E8E93), opacity);
        } else {
            const auto toggle = D2D1::RectF(row.right - 52.0f * s, row.top + 18.0f * s,
                                            row.right - 12.0f * s, row.top + 42.0f * s);
            fill_round_rect(toggle, 12.0f * s,
                            rowData.enabled ? D2D1::ColorF(0x34C759) : D2D1::ColorF(0x3A3A3C), opacity);
            const float knobX = rowData.enabled ? toggle.right - 12.0f * s : toggle.left + 12.0f * s;
            ComPtr<ID2D1SolidColorBrush> knob;
            d2dContext_->CreateSolidColorBrush(with_alpha(D2D1::ColorF(0xFFFFFF), opacity), &knob);
            d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, (toggle.top + toggle.bottom) * 0.5f),
                                                    9.0f * s, 9.0f * s), knob.Get());
        }
        y += 68.0f * s;
    }

    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(L"Isle 0.1 · Windows 10/11", smallFormat_.Get(),
              D2D1::RectF(rect.left + pad, rect.bottom - 31.0f * s, rect.right - pad, rect.bottom - 9.0f * s),
              D2D1::ColorF(0x52525B), opacity);
}

void Renderer::draw_metric(const Activity& activity, D2D1_POINT_2F center, float radius, float scale, float opacity) {
    const double progress = activity.progress.value_or(activity.value.has_value() ? clamp01(*activity.value / 100.0) : 0.0);
    const D2D1_COLOR_F accent = color_from_hex(activity.accent);
    draw_progress_ring(center, radius, 4.0f * scale, progress,
                       D2D1::ColorF(0x27272A), accent, opacity);

    const bool fluentGlyph = !activity.glyph.empty() && activity.glyph.front() >= 0xE000 && activity.glyph.front() <= 0xF8FF;
    IDWriteTextFormat* glyphFormat = fluentGlyph ? iconFormat_.Get() : bodyFormat_.Get();
    glyphFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(activity.glyph, glyphFormat,
              D2D1::RectF(center.x - radius * 0.72f, center.y - radius * 0.70f,
                          center.x + radius * 0.72f, center.y + radius * 0.45f),
              D2D1::ColorF(0xFAFAFA), opacity);

    metricFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(metric_value(activity), metricFormat_.Get(),
              D2D1::RectF(center.x - radius * 1.15f, center.y + radius + 7.0f * scale,
                          center.x + radius * 1.15f, center.y + radius + 27.0f * scale),
              D2D1::ColorF(0xF4F4F5), opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(activity.title, smallFormat_.Get(),
              D2D1::RectF(center.x - radius * 1.45f, center.y + radius + 26.0f * scale,
                          center.x + radius * 1.45f, center.y + radius + 46.0f * scale),
              D2D1::ColorF(0x71717A), opacity);
}

ID2D1Bitmap1* Renderer::artwork_bitmap(const Activity& activity) {
    const void* key = activity.artwork.get();
    if (key == artworkKey_) return artworkBitmap_.Get();
    artworkKey_ = key;
    artworkBitmap_.Reset();
    if (!activity.artwork || activity.artwork->empty() ||
        activity.artwork->size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        return nullptr;
    }

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(activity.artwork->data()),
                                            static_cast<DWORD>(activity.artwork->size()))) ||
        FAILED(wicFactory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(wicFactory_->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) ||
        FAILED(d2dContext_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &artworkBitmap_))) {
        artworkBitmap_.Reset();
    }
    return artworkBitmap_.Get();
}

void Renderer::draw_artwork(const Activity& activity, D2D1_RECT_F rect, float radius, float opacity) {
    if (auto* bitmap = artwork_bitmap(activity)) {
        ComPtr<ID2D1RoundedRectangleGeometry> geometry;
        ComPtr<ID2D1Layer> layer;
        if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius), &geometry)) &&
            SUCCEEDED(d2dContext_->CreateLayer(&layer))) {
            D2D1_LAYER_PARAMETERS parameters{};
            parameters.contentBounds = rect;
            parameters.geometricMask = geometry.Get();
            parameters.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
            parameters.maskTransform = D2D1::Matrix3x2F::Identity();
            parameters.opacity = opacity;
            parameters.layerOptions = D2D1_LAYER_OPTIONS_NONE;
            d2dContext_->PushLayer(parameters, layer.Get());
            d2dContext_->DrawBitmap(bitmap, rect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            d2dContext_->PopLayer();
            stroke_round_rect(rect, radius, D2D1::ColorF(0xFFFFFF), 0.8f * formatScale_, 0.13f * opacity);
            return;
        }
    }

    const D2D1_COLOR_F accent = color_from_hex(activity.accent);
    fill_round_rect(rect, radius, D2D1::ColorF(0x17171A), opacity);
    stroke_round_rect(inset_rect(rect, 0.7f * formatScale_), std::max(0.0f, radius - 0.7f * formatScale_),
                      accent, 1.4f * formatScale_, 0.72f * opacity);
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(activity.glyph.empty() ? L"\uE8D6" : activity.glyph, iconFormat_.Get(), rect,
              accent, opacity);
}

void Renderer::draw_waveform(D2D1_RECT_F rect, D2D1_COLOR_F color, float opacity, bool active) {
    constexpr int bars = 9;
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const float gap = width / static_cast<float>(bars);
    const double phase = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 5.4;
    for (int i = 0; i < bars; ++i) {
        const float strength = active
            ? 0.22f + 0.78f * static_cast<float>(std::abs(std::sin(phase + static_cast<double>(i) * 0.86)))
            : 0.24f + 0.13f * static_cast<float>((i * 7) % 5);
        const float barHeight = std::min(height, std::max(3.0f * formatScale_, height * strength));
        const float x = rect.left + gap * (static_cast<float>(i) + 0.5f);
        const auto bar = D2D1::RectF(x - 1.2f * formatScale_, (rect.top + rect.bottom - barHeight) * 0.5f,
                                     x + 1.2f * formatScale_, (rect.top + rect.bottom + barHeight) * 0.5f);
        fill_round_rect(bar, 1.2f * formatScale_, color, opacity * (0.65f + 0.35f * static_cast<float>(i + 1) / bars));
    }
}

void Renderer::draw_media_control_icon(std::wstring_view action, D2D1_RECT_F rect,
                                       D2D1_COLOR_F color, float opacity, bool playing) {
    ComPtr<ID2D1SolidColorBrush> brush;
    d2dContext_->CreateSolidColorBrush(with_alpha(color, opacity), &brush);
    const float s = formatScale_ <= 0.0f ? 1.0f : formatScale_;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;

    if (action == L"toggle" && playing) {
        fill_round_rect(D2D1::RectF(cx - 5.5f * s, cy - 7.0f * s, cx - 1.5f * s, cy + 7.0f * s),
                        1.5f * s, color, opacity);
        fill_round_rect(D2D1::RectF(cx + 1.5f * s, cy - 7.0f * s, cx + 5.5f * s, cy + 7.0f * s),
                        1.5f * s, color, opacity);
        return;
    }

    const bool previous = action == L"previous";
    const bool next = action == L"next";
    const float direction = previous ? -1.0f : 1.0f;
    const float offset = (previous || next) ? -direction * 1.0f * s : 1.0f * s;
    const auto tip = D2D1::Point2F(cx + direction * 6.0f * s + offset, cy);
    const auto top = D2D1::Point2F(cx - direction * 5.0f * s + offset, cy - 7.0f * s);
    const auto bottom = D2D1::Point2F(cx - direction * 5.0f * s + offset, cy + 7.0f * s);

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(d2dFactory_->CreatePathGeometry(&geometry)) && SUCCEEDED(geometry->Open(&sink))) {
        sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(top);
        sink->AddLine(bottom);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (SUCCEEDED(sink->Close())) d2dContext_->FillGeometry(geometry.Get(), brush.Get());
    }

    if (previous) {
        fill_round_rect(D2D1::RectF(cx - 8.0f * s, cy - 7.0f * s, cx - 5.8f * s, cy + 7.0f * s),
                        1.0f * s, color, opacity);
    } else if (next) {
        fill_round_rect(D2D1::RectF(cx + 5.8f * s, cy - 7.0f * s, cx + 8.0f * s, cy + 7.0f * s),
                        1.0f * s, color, opacity);
    }
}

void Renderer::draw_progress_ring(D2D1_POINT_2F center, float radius, float thickness, double progress,
                                  const D2D1_COLOR_F& track, const D2D1_COLOR_F& accent, float opacity) {
    ComPtr<ID2D1SolidColorBrush> trackBrush;
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    d2dContext_->CreateSolidColorBrush(with_alpha(track, opacity), &trackBrush);
    d2dContext_->CreateSolidColorBrush(with_alpha(accent, opacity), &accentBrush);
    d2dContext_->DrawEllipse(D2D1::Ellipse(center, radius, radius), trackBrush.Get(), thickness, roundStrokeStyle_.Get());

    const double p = clamp01(progress);
    if (p <= 0.001) return;
    if (p >= 0.999) {
        d2dContext_->DrawEllipse(D2D1::Ellipse(center, radius, radius), accentBrush.Get(), thickness, roundStrokeStyle_.Get());
        return;
    }

    const float startAngle = -90.0f;
    const float sweep = static_cast<float>(p * 360.0);
    const float endAngle = startAngle + sweep;
    const auto to_point = [&](float degrees) {
        const float radians = degrees * 3.14159265358979323846f / 180.0f;
        return D2D1::Point2F(center.x + std::cos(radians) * radius, center.y + std::sin(radians) * radius);
    };

    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2dFactory_->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure(to_point(startAngle), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(to_point(endAngle), D2D1::SizeF(radius, radius), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                  sweep >= 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close())) {
        d2dContext_->DrawGeometry(geometry.Get(), accentBrush.Get(), thickness, roundStrokeStyle_.Get());
    }
}

void Renderer::draw_text(std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                         D2D1_COLOR_F color, float opacity) {
    if (text.empty() || !format || opacity <= 0.001f) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    d2dContext_->CreateSolidColorBrush(with_alpha(color, opacity), &brush);
    d2dContext_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect, brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void Renderer::fill_round_rect(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color, float opacity) {
    if (opacity <= 0.001f || rect.right <= rect.left || rect.bottom <= rect.top) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    d2dContext_->CreateSolidColorBrush(with_alpha(color, opacity), &brush);
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get());
}

void Renderer::stroke_round_rect(D2D1_RECT_F rect, float radius, D2D1_COLOR_F color, float width, float opacity) {
    if (opacity <= 0.001f || rect.right <= rect.left || rect.bottom <= rect.top) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    d2dContext_->CreateSolidColorBrush(with_alpha(color, opacity), &brush);
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush.Get(), width);
}

D2D1_COLOR_F Renderer::color_from_hex(std::wstring_view hex, float alpha) {
    if (!hex.empty() && hex.front() == L'#') hex.remove_prefix(1);
    if (hex.size() != 6) return D2D1::ColorF(0xFFFFFF, alpha);
    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return 10 + c - L'a';
        if (c >= L'A' && c <= L'F') return 10 + c - L'A';
        return 0;
    };
    auto byte = [&](std::size_t i) { return nibble(hex[i]) * 16 + nibble(hex[i + 1]); };
    return D2D1::ColorF(static_cast<float>(byte(0)) / 255.0f,
                        static_cast<float>(byte(2)) / 255.0f,
                        static_cast<float>(byte(4)) / 255.0f,
                        alpha);
}

} // namespace isle
