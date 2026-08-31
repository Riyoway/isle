#include "Renderer.h"

#include "../core/AIProviders.h"
#include "../core/Spring.h"

#include <d2d1helper.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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

bool activity_visible(const RenderState& state, const Activity& activity) {
    if (activity.source.starts_with(L"ai.")) return state.showAiUsage;
    if (activity.source == L"system") return state.showSystemMetrics;
    if (activity.source == L"shortcut.app") return state.showAppLauncher;
    if (activity.source == L"shortcut.command") return state.showCommandShortcuts;
    return true;
}

bool activity_matches_widget(const Activity& activity, int widget) {
    switch (static_cast<WidgetKind>(widget)) {
        case WidgetKind::AiUsage: return activity.source.starts_with(L"ai.");
        case WidgetKind::AppLauncher: return activity.source == L"shortcut.app";
        case WidgetKind::Commands: return activity.source == L"shortcut.command";
        case WidgetKind::System: return activity.source == L"system";
        case WidgetKind::Music: return activity.kind == ActivityKind::Media;
    }
    return false;
}

const Activity* compact_activity(const RenderState& state, const std::vector<Activity>& activities) {
    if (state.showMusicPlayer) {
        if (const auto* media = media_activity(activities)) return media;
    }
    for (const int widget : state.widgetOrder) {
        if (!widget_enabled(state, widget)) continue;
        const auto match = std::ranges::find_if(activities, [&](const Activity& activity) {
            return activity_matches_widget(activity, widget);
        });
        if (match != activities.end()) return &*match;
    }
    const auto live = std::ranges::find_if(activities, [](const Activity& activity) {
        return activity.kind != ActivityKind::Metric;
    });
    if (live != activities.end() && activity_visible(state, *live)) return &*live;
    const auto visible = std::ranges::find_if(activities, [&](const Activity& activity) {
        return activity_visible(state, activity);
    });
    return visible == activities.end() ? nullptr : &*visible;
}

bool full_width_widget(int widget) noexcept {
    return widget == static_cast<int>(WidgetKind::Music) || widget == static_cast<int>(WidgetKind::AiUsage);
}

int ai_row_count(const RenderState& state) noexcept {
    return std::clamp(state.aiVisibleCount, 1, state.showSystemMetrics ? 3 : 4);
}

float widget_height(const RenderState& state, int widget) noexcept {
    if (widget == static_cast<int>(WidgetKind::Music)) return 220.0f;
    if (widget == static_cast<int>(WidgetKind::AiUsage)) {
        return 36.0f + 56.0f * static_cast<float>(ai_row_count(state));
    }
    return 118.0f;
}

std::wstring_view provider_id_from_source(std::wstring_view source) noexcept {
    return source.starts_with(L"ai.") ? source.substr(3) : std::wstring_view{};
}

std::size_t compact_ai_count(const RenderState& state, const std::vector<Activity>& activities) {
    if (state.compactMediaMode == 0) return 0;
    std::array<std::wstring_view, 3> sources{};
    std::size_t count = 0;
    for (const auto& activity : activities) {
        if (!activity.source.starts_with(L"ai.") || activity.kind != ActivityKind::Metric || !activity.compactRing) continue;
        if (std::ranges::find(sources.begin(), sources.begin() + count, activity.source) != sources.begin() + count) continue;
        sources[count++] = activity.source;
        if (count == static_cast<std::size_t>(std::clamp(state.compactRingCount, 1, 3))) break;
    }
    return count;
}

// The bundled brand assets paint their mark white or with `currentColor`.
// Swapping those two values for the provider accent keeps every logo legible on
// the black shell, while assets carrying a real multi-colour mark are left as authored.
void tint_brand_marks(std::string& markup, std::wstring_view accentHex) {
    std::string accent(1, '#');
    for (const wchar_t ch : accentHex) {
        if (ch == L'#') continue;
        if (ch > 0x7F || !std::isxdigit(static_cast<unsigned char>(ch))) return;
        accent.push_back(static_cast<char>(ch));
    }
    if (accent.size() != 7) return;

    std::string tinted;
    tinted.reserve(markup.size());
    for (std::size_t at = 0; at < markup.size();) {
        if (markup[at] != '"') {
            tinted.push_back(markup[at++]);
            continue;
        }
        const std::size_t close = markup.find('"', at + 1);
        if (close == std::string::npos) {
            tinted.append(markup, at, std::string::npos);
            break;
        }
        const std::string value = markup.substr(at + 1, close - at - 1);
        std::string lowered = value;
        std::ranges::transform(lowered, lowered.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool brand = lowered == "white" || lowered == "#fff" ||
                           lowered == "#ffffff" || lowered == "currentcolor";
        tinted.push_back('"');
        tinted.append(brand ? accent : value);
        tinted.push_back('"');
        at = close + 1;
    }
    markup = std::move(tinted);
}

float card_radius(const RenderState& state) {
    constexpr float radii[]{22.0f, 15.0f, 31.0f};
    return radii[std::clamp(state.islandShape, 0, 2)] * state.dpiScale;
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

    // Direct2D 1.3 draws the bundled provider SVGs natively. Older Windows builds fail the
    // query and every badge falls back to its text mark.
    providerIcons_.clear();
    d2dContext_.As(&svgContext_);

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
    const float top = state.expandUp
        ? static_cast<float>(height_) - 8.0f * state.dpiScale - state.islandHeight
        : 8.0f * state.dpiScale;
    return D2D1::RectF(left, top, left + state.islandWidth, top + state.islandHeight);
}

D2D1_RECT_F Renderer::widget_rect(const RenderState& state, int wanted) const noexcept {
    const auto island = island_rect(state);
    const float s = state.dpiScale;
    const float pad = 18.0f * s;
    const float gap = 8.0f * s;
    const float halfWidth = (island.right - island.left - pad * 2.0f - gap) * 0.5f;
    float y = island.top + 46.0f * s;
    bool halfPending = false;
    for (const int widget : state.widgetOrder) {
        if (!widget_enabled(state, widget)) continue;
        const float height = widget_height(state, widget) * s;
        if (full_width_widget(widget)) {
            if (halfPending) {
                y += 118.0f * s + gap;
                halfPending = false;
            }
            const auto result = D2D1::RectF(island.left + pad, y, island.right - pad, y + height);
            if (widget == wanted) return result;
            y += height + gap;
        } else if (!halfPending) {
            const auto result = D2D1::RectF(island.left + pad, y,
                                            island.left + pad + halfWidth, y + height);
            if (widget == wanted) return result;
            halfPending = true;
        } else {
            const auto result = D2D1::RectF(island.left + pad + halfWidth + gap, y,
                                            island.right - pad, y + height);
            if (widget == wanted) return result;
            y += 118.0f * s + gap;
            halfPending = false;
        }
    }
    return D2D1::RectF();
}

float Renderer::expanded_height(const RenderState& state) const noexcept {
    const auto island = island_rect(state);
    float bottom = island.top + 118.0f * state.dpiScale;
    for (const int widget : state.widgetOrder) {
        if (!widget_enabled(state, widget)) continue;
        bottom = std::max(bottom, widget_rect(state, widget).bottom);
    }
    return bottom - island.top + 14.0f * state.dpiScale;
}

void Renderer::render(const RenderState& state, const std::vector<Activity>& activities) {
    ensure_text_formats(state.dpiScale);
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0, 0.0f));

    if (!state.hidden && state.visibility > 0.001f) {
        const auto rect = island_rect(state);
        constexpr float expandedRadii[]{42.0f, 30.0f, 54.0f};
        const float expandedRadius = expandedRadii[std::clamp(state.islandShape, 0, 2)] * state.dpiScale;
        const float radius = lerp(20.0f * state.dpiScale, expandedRadius, state.expandAmount);
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
    const Activity* primary = compact_activity(state, activities);

    if (!primary) {
        const float center = (rect.left + rect.right) * 0.5f;
        titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(L"ISLE", titleFormat_.Get(), D2D1::RectF(center - 72.0f * s, rect.top,
                  center - 5.0f * s, rect.bottom), D2D1::ColorF(0xF5F5F7), opacity);
        draw_text(state.timeText, titleFormat_.Get(), D2D1::RectF(center + 5.0f * s, rect.top,
                  center + 72.0f * s, rect.bottom), D2D1::ColorF(0xD4D4D8), opacity);
        return;
    }

    const D2D1_COLOR_F accent = color_from_hex(primary->accent);
    if (primary->kind == ActivityKind::Media) {
        const auto art = D2D1::RectF(rect.left + 6.0f * s, rect.top + 6.0f * s,
                                     rect.left + 34.0f * s, rect.bottom - 6.0f * s);
        draw_artwork(*primary, art, 8.0f * s, opacity);
        const std::size_t ringCount = compact_ai_count(state, activities);
        const bool showUsage = ringCount > 0;
        const bool showWaveform = state.compactMediaMode != 1 || !showUsage;
        const float usageWidth = static_cast<float>(ringCount) * 27.0f * s;
        const float waveformWidth = showWaveform ? 46.0f * s : 0.0f;
        const float separation = showUsage && showWaveform ? 6.0f * s : 0.0f;
        const float titleGap = 8.0f * s;
        const float reserved = 14.0f * s + usageWidth + waveformWidth + separation + titleGap;
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_marquee_text(primary->title, bodyFormat_.Get(), D2D1::RectF(art.right + 9.0f * s, rect.top,
                           rect.right - reserved, rect.bottom), D2D1::ColorF(0xF7F7F8), opacity);
        if (showUsage) {
            draw_collapsed_ai_rings(state, activities, rect, opacity);
        }
        if (showWaveform) {
            const float right = rect.right - 14.0f * s - usageWidth - separation;
            draw_waveform(D2D1::RectF(right - waveformWidth, rect.top + 9.0f * s,
                                      right, rect.bottom - 9.0f * s),
                          accent, opacity, primary->active);
        }
        return;
    }

    const auto badge = D2D1::RectF(rect.left + 7.0f * s, rect.top + 7.0f * s,
                                   rect.left + 33.0f * s, rect.bottom - 7.0f * s);
    if (primary->source.starts_with(L"ai.")) draw_provider_badge(*primary, badge, opacity);
    else {
        fill_round_rect(badge, 13.0f * s, accent, 0.20f * opacity);
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(primary->glyph, iconFormat_.Get(), badge, accent, opacity);
    }

    bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    std::wstring compactTitle = primary->title;
    if (primary->source.starts_with(L"ai.")) {
        const int provider = ai_provider_index(provider_id_from_source(primary->source));
        if (provider >= 0) compactTitle = std::wstring(kAIProviders[static_cast<std::size_t>(provider)].name) + L" · " + primary->title;
    }
    draw_marquee_text(compactTitle, bodyFormat_.Get(), D2D1::RectF(badge.right + 9.0f * s, rect.top,
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
    bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(L"ISLE", bodyFormat_.Get(),
              D2D1::RectF(rect.left + pad, rect.top + 5.0f * s,
                          cx, rect.top + 37.0f * s),
              D2D1::ColorF(0x8E8E93), opacity);

    bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(state.timeText, bodyFormat_.Get(), D2D1::RectF(cx, rect.top + 5.0f * s,
              rect.right - 70.0f * s, rect.top + 37.0f * s),
              D2D1::ColorF(0xD4D4D8), opacity);
    const float gearPress = state.pressedControl == 1 ? state.pressAmount : 0.0f;
    const auto gearBase = D2D1::RectF(rect.right - 64.0f * s, rect.top + 5.0f * s,
                                      rect.right - 32.0f * s, rect.top + 37.0f * s);
    const auto gear = inset_rect(gearBase, gearPress * 1.8f * s);
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(L"\uE713", iconFormat_.Get(), gear, D2D1::ColorF(0xF4F4F5), opacity);
    draw_widget_grid(state, activities, rect, opacity);

    fill_round_rect(D2D1::RectF(cx - 18.0f * s, rect.bottom - 6.0f * s,
                                cx + 18.0f * s, rect.bottom - 3.0f * s),
                    1.5f * s, D2D1::ColorF(0x3F3F46), opacity);
}

void Renderer::draw_widget_grid(const RenderState& state, const std::vector<Activity>& activities,
                                const D2D1_RECT_F& rect, float opacity) {
    int count = 0;
    const Activity* media = media_activity(activities);
    for (const int widget : state.widgetOrder) {
        if (!widget_enabled(state, widget)) continue;
        const auto card = widget_rect(state, widget);
        switch (static_cast<WidgetKind>(widget)) {
            case WidgetKind::Music:
                if (media) draw_media_widget(state, *media, card, opacity);
                break;
            case WidgetKind::AiUsage: draw_ai_widget(state, activities, card, opacity); break;
            case WidgetKind::AppLauncher:
            case WidgetKind::Commands: draw_shortcut_widget(state, activities, card, widget, opacity); break;
            case WidgetKind::System: draw_system_widget(state, activities, card, opacity); break;
        }
        ++count;
    }

    if (count == 0) {
        const float s = state.dpiScale;
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(L"\uE713", iconFormat_.Get(),
                  D2D1::RectF(rect.left, rect.top + 104.0f * s, rect.right, rect.top + 146.0f * s),
                  D2D1::ColorF(0x636366), opacity);
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(L"Add widgets in Settings", bodyFormat_.Get(),
                  D2D1::RectF(rect.left + 20.0f * s, rect.top + 146.0f * s,
                              rect.right - 20.0f * s, rect.top + 184.0f * s),
                  D2D1::ColorF(0x8E8E93), opacity);
    }
}

void Renderer::draw_media_widget(const RenderState& state, const Activity& media,
                                 D2D1_RECT_F rect, float opacity) {
    const float s = state.dpiScale;
    const float cx = (rect.left + rect.right) * 0.5f;
    fill_round_rect(rect, card_radius(state), D2D1::ColorF(0x08080A), opacity);
    stroke_round_rect(rect, card_radius(state), D2D1::ColorF(0xFFFFFF), 0.7f * s, 0.055f * opacity);
    const auto art = D2D1::RectF(rect.left + 10.0f * s, rect.top + 10.0f * s,
                                 rect.left + 96.0f * s, rect.top + 96.0f * s);
    draw_artwork(media, art, 20.0f * s, opacity);
    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_marquee_text(media.title, titleFormat_.Get(),
                      D2D1::RectF(art.right + 14.0f * s, art.top + 7.0f * s,
                                  rect.right - 12.0f * s, art.top + 37.0f * s),
                      D2D1::ColorF(0xFAFAFA), opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(media.subtitle, smallFormat_.Get(),
              D2D1::RectF(art.right + 14.0f * s, art.top + 38.0f * s,
                          rect.right - 12.0f * s, art.top + 61.0f * s),
              D2D1::ColorF(0x8E8E93), opacity);
    draw_waveform(D2D1::RectF(art.right + 14.0f * s, art.bottom - 27.0f * s,
                              rect.right - 12.0f * s, art.bottom - 4.0f * s),
                  color_from_hex(media.accent), opacity, media.active, true);

    const double progress = live_progress(media);
    const auto track = D2D1::RectF(rect.left + 10.0f * s, rect.top + 111.0f * s,
                                   rect.right - 10.0f * s, rect.top + 115.0f * s);
    fill_round_rect(track, 2.0f * s, D2D1::ColorF(0x27272A), opacity);
    if (progress > 0.001) {
        const auto elapsed = D2D1::RectF(track.left, track.top,
            track.left + (track.right - track.left) * static_cast<float>(progress), track.bottom);
        fill_round_rect(elapsed, 2.0f * s, D2D1::ColorF(0xF4F4F5), opacity);
        ComPtr<ID2D1SolidColorBrush> knob;
        d2dContext_->CreateSolidColorBrush(with_alpha(D2D1::ColorF(0xFFFFFF), opacity), &knob);
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(elapsed.right, (track.top + track.bottom) * 0.5f),
                                                3.2f * s, 3.2f * s), knob.Get());
    }
    if (media.durationSeconds.has_value()) {
        const double duration = *media.durationSeconds;
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(duration_text(duration * progress), smallFormat_.Get(),
                  D2D1::RectF(track.left, track.bottom + 4.0f * s, cx, track.bottom + 22.0f * s),
                  D2D1::ColorF(0x71717A), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_text(L"-" + duration_text(duration * (1.0 - progress)), smallFormat_.Get(),
                  D2D1::RectF(cx, track.bottom + 4.0f * s, track.right, track.bottom + 22.0f * s),
                  D2D1::ColorF(0x71717A), opacity);
    }

    const float controlY = rect.top + 171.0f * s;
    const std::array<float, 3> controlX{cx - 58.0f * s, cx, cx + 58.0f * s};
    constexpr std::wstring_view actionIds[]{L"previous", L"toggle", L"next"};
    for (std::size_t i = 0; i < controlX.size(); ++i) {
        const float press = state.pressedControl == static_cast<int>(2 + i) ? state.pressAmount : 0.0f;
        const float size = (i == 1 ? 44.0f : 36.0f) * s - press * 4.0f * s;
        const auto button = D2D1::RectF(controlX[i] - size * 0.5f, controlY - size * 0.5f,
                                        controlX[i] + size * 0.5f, controlY + size * 0.5f);
        fill_round_rect(button, size * 0.5f,
                        i == 1 ? (press > 0.01f ? D2D1::ColorF(0xD1D1D6) : D2D1::ColorF(0xFFFFFF))
                               : (press > 0.01f ? D2D1::ColorF(0x303033) : D2D1::ColorF(0x18181B)),
                        opacity);
        draw_media_control_icon(actionIds[i], button,
                                i == 1 ? D2D1::ColorF(0x050505) : D2D1::ColorF(0xF4F4F5),
                                opacity, media.active);
    }
}

void Renderer::draw_ai_widget(const RenderState& state, const std::vector<Activity>& activities,
                              D2D1_RECT_F rect, float opacity) {
    const float s = state.dpiScale;
    fill_round_rect(rect, card_radius(state), D2D1::ColorF(0x111113), opacity);
    stroke_round_rect(rect, card_radius(state), D2D1::ColorF(0xFFFFFF), 0.7f * s, 0.055f * opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(L"AI USAGE", smallFormat_.Get(), D2D1::RectF(rect.left + 13.0f * s, rect.top + 6.0f * s,
              rect.right - 13.0f * s, rect.top + 28.0f * s), D2D1::ColorF(0xA1A1AA), opacity);
    const int visibleRows = ai_row_count(state);
    if (state.aiVisibleCount > visibleRows) {
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        const std::wstring more = L"+" + std::to_wstring(state.aiVisibleCount - visibleRows) + L" selected";
        draw_text(more, smallFormat_.Get(), D2D1::RectF(rect.left + 150.0f * s, rect.top + 6.0f * s,
                  rect.right - 13.0f * s, rect.top + 28.0f * s), D2D1::ColorF(0x71717A), opacity);
    }

    std::vector<std::wstring> sources;
    for (const auto& activity : activities) {
        if (!activity.source.starts_with(L"ai.")) continue;
        if (std::ranges::find(sources, activity.source) == sources.end()) sources.push_back(activity.source);
    }
    if (sources.empty()) {
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(state.aiVisibleCount == 0 ? L"Select providers in Settings" : L"Loading selected providers…",
                   smallFormat_.Get(),
                   D2D1::RectF(rect.left + 10.0f * s, rect.top + 44.0f * s,
                               rect.right - 10.0f * s, rect.bottom - 12.0f * s),
                   D2D1::ColorF(0x71717A), opacity);
        return;
    }

    const std::wstring selectedSource = L"ai." + std::wstring(kAIProviders[static_cast<std::size_t>(
        std::clamp(state.selectedAiProvider, 0, static_cast<int>(kAIProviders.size() - 1)))].id);
    if (const auto selected = std::ranges::find(sources, selectedSource); selected != sources.end()) {
        std::rotate(sources.begin(), selected, selected + 1);
    }
    const std::size_t providerCount = std::min<std::size_t>(static_cast<std::size_t>(visibleRows), sources.size());
    for (std::size_t provider = 0; provider < providerCount; ++provider) {
        std::vector<const Activity*> windows;
        const Activity* status = nullptr;
        for (const auto& activity : activities) {
            if (activity.source != sources[provider]) continue;
            if (activity.kind == ActivityKind::Metric && windows.size() < 2) windows.push_back(&activity);
            else if (!status) status = &activity;
        }
        const Activity* identity = windows.empty() ? status : windows[0];
        if (!identity) continue;
        const float rowTop = rect.top + (30.0f + static_cast<float>(provider) * 56.0f) * s;
        const float rowBottom = rowTop + 52.0f * s;
        const auto badge = D2D1::RectF(rect.left + 14.0f * s, rowTop + 11.0f * s,
                                       rect.left + 40.0f * s, rowTop + 37.0f * s);
        draw_provider_badge(*identity, badge, opacity);
        const int providerIndex = ai_provider_index(provider_id_from_source(sources[provider]));
        const std::wstring name = providerIndex >= 0 ? std::wstring(kAIProviders[static_cast<std::size_t>(providerIndex)].name)
                                                      : std::wstring(provider_id_from_source(sources[provider]));
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(name, bodyFormat_.Get(),
                  D2D1::RectF(badge.right + 8.0f * s, rowTop + 4.0f * s,
                              rect.left + 174.0f * s, rowTop + 27.0f * s), D2D1::ColorF(0xF4F4F5), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(windows.empty() ? identity->subtitle : windows[0]->subtitle, smallFormat_.Get(),
                  D2D1::RectF(badge.right + 8.0f * s, rowTop + 25.0f * s,
                              rect.left + 174.0f * s, rowTop + 46.0f * s), D2D1::ColorF(0x71717A), opacity);

        const float metricsLeft = rect.left + 178.0f * s;
        const float ringStep = (rect.right - 13.0f * s - metricsLeft) /
                               static_cast<float>(std::max<std::size_t>(1, windows.size()));
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const auto& activity = *windows[i];
            const auto center = D2D1::Point2F(metricsLeft + ringStep * (static_cast<float>(i) + 0.5f),
                                              rowTop + 23.0f * s);
            const float radius = 14.0f * s;
            draw_progress_ring(center, radius, 2.8f * s, activity.progress.value_or(0.0),
                               D2D1::ColorF(0x2C2C2E), color_from_hex(activity.accent), opacity);
            smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(metric_value(activity), smallFormat_.Get(),
                      D2D1::RectF(center.x - radius, center.y - 9.0f * s,
                                  center.x + radius, center.y + 9.0f * s), D2D1::ColorF(0xF5F5F7), opacity);
            draw_text(activity.title, smallFormat_.Get(),
                      D2D1::RectF(center.x - ringStep * 0.48f, rowTop + 37.0f * s,
                                  center.x + ringStep * 0.48f, rowBottom), D2D1::ColorF(0x8E8E93), opacity);
        }
        if (provider + 1 < providerCount) {
            ComPtr<ID2D1SolidColorBrush> divider;
            d2dContext_->CreateSolidColorBrush(with_alpha(D2D1::ColorF(0xFFFFFF), 0.055f * opacity), &divider);
            d2dContext_->DrawLine(D2D1::Point2F(rect.left + 13.0f * s, rowBottom),
                                  D2D1::Point2F(rect.right - 13.0f * s, rowBottom),
                                  divider.Get(), 0.7f * s);
        }
    }
}

void Renderer::draw_system_widget(const RenderState& state, const std::vector<Activity>& activities,
                                  D2D1_RECT_F rect, float opacity) {
    const float s = state.dpiScale;
    fill_round_rect(rect, card_radius(state), D2D1::ColorF(0x111113), opacity);
    stroke_round_rect(rect, card_radius(state), D2D1::ColorF(0xFFFFFF), 0.7f * s, 0.055f * opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(L"SYSTEM", smallFormat_.Get(), D2D1::RectF(rect.left + 13.0f * s, rect.top + 6.0f * s,
              rect.right - 13.0f * s, rect.top + 29.0f * s), D2D1::ColorF(0xA1A1AA), opacity);

    std::array<const Activity*, 3> metrics{};
    for (const auto& activity : activities) {
        if (activity.id == L"system.cpu") metrics[0] = &activity;
        else if (activity.id == L"system.gpu") metrics[1] = &activity;
        else if (activity.id == L"system.ram") metrics[2] = &activity;
    }
    const float step = (rect.right - rect.left) / 3.0f;
    constexpr std::wstring_view labels[]{L"CPU", L"GPU", L"MEM"};
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        const auto center = D2D1::Point2F(rect.left + step * (static_cast<float>(i) + 0.5f),
                                          rect.top + 66.0f * s);
        const double progress = metrics[i] ? metrics[i]->progress.value_or(0.0) : 0.0;
        const auto accent = metrics[i] ? color_from_hex(metrics[i]->accent) : D2D1::ColorF(0x636366);
        draw_progress_ring(center, 17.0f * s, 3.0f * s, progress,
                           D2D1::ColorF(0x2C2C2E), accent, opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(metrics[i] ? metric_value(*metrics[i]) : L"--", smallFormat_.Get(),
                  D2D1::RectF(center.x - 21.0f * s, center.y - 11.0f * s,
                              center.x + 21.0f * s, center.y + 11.0f * s),
                  D2D1::ColorF(0xF5F5F7), opacity);
        draw_text(labels[i], smallFormat_.Get(),
                  D2D1::RectF(center.x - 24.0f * s, rect.bottom - 25.0f * s,
                              center.x + 24.0f * s, rect.bottom - 5.0f * s),
                  D2D1::ColorF(0x8E8E93), opacity);
    }
}

void Renderer::draw_shortcut_widget(const RenderState& state, const std::vector<Activity>& activities,
                                    D2D1_RECT_F rect, int widget, float opacity) {
    const float s = state.dpiScale;
    const bool commands = widget == static_cast<int>(WidgetKind::Commands);
    const std::wstring_view source = commands ? L"shortcut.command" : L"shortcut.app";
    const D2D1_COLOR_F accent = commands ? D2D1::ColorF(0xFF9F0A) : D2D1::ColorF(0x0A84FF);
    fill_round_rect(rect, card_radius(state), D2D1::ColorF(0x111113), opacity);
    stroke_round_rect(rect, card_radius(state), D2D1::ColorF(0xFFFFFF), 0.7f * s, 0.055f * opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(commands ? L"COMMANDS" : L"APPS", smallFormat_.Get(),
              D2D1::RectF(rect.left + 13.0f * s, rect.top + 6.0f * s,
                          rect.right - 13.0f * s, rect.top + 29.0f * s),
              D2D1::ColorF(0xA1A1AA), opacity);

    std::array<const Activity*, 2> shortcuts{};
    std::size_t count = 0;
    for (const auto& activity : activities) {
        if (activity.source == source && count < shortcuts.size()) shortcuts[count++] = &activity;
    }
    constexpr int controlBase = 30;
    const int sourceOffset = commands ? 2 : 0;
    const float halfWidth = (rect.right - rect.left) * 0.5f;
    for (std::size_t i = 0; i < count; ++i) {
        const float centerX = rect.left + halfWidth * (static_cast<float>(i) + 0.5f);
        const float press = state.pressedControl == controlBase + sourceOffset + static_cast<int>(i)
                                ? state.pressAmount : 0.0f;
        const float size = (46.0f - press * 3.0f) * s;
        const auto button = D2D1::RectF(centerX - size * 0.5f, rect.top + 34.0f * s + press * 1.5f * s,
                                        centerX + size * 0.5f, rect.top + 34.0f * s + press * 1.5f * s + size);
        const float radius = state.buttonStyle == 1 ? 13.0f * s : size * 0.5f;
        if (state.buttonStyle == 2) {
            fill_round_rect(button, radius, accent, 0.08f * opacity);
            stroke_round_rect(button, radius, accent, 1.3f * s, opacity);
        } else {
            fill_round_rect(button, radius, accent, (press > 0.01f ? 0.78f : 0.22f) * opacity);
        }
        iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(shortcuts[i]->glyph, iconFormat_.Get(), button, accent, opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(shortcuts[i]->title, smallFormat_.Get(),
                  D2D1::RectF(centerX - halfWidth * 0.47f, rect.bottom - 28.0f * s,
                              centerX + halfWidth * 0.47f, rect.bottom - 5.0f * s),
                  D2D1::ColorF(0xD1D1D6), opacity);
    }
}

void Renderer::draw_settings(const RenderState& state, const D2D1_RECT_F& rect) {
    const float s = state.dpiScale;
    const float opacity = state.visibility * smoothstep(0.30f, 0.72f, state.expandAmount);
    const float pad = 18.0f * s;

    const wchar_t* title = state.settingsPage == 1 ? L"Widget Editor" :
                           state.settingsPage == 2 ? L"Appearance" :
                           state.settingsPage == 4 ? L"Select Providers" :
                           state.settingsPage == 3 ? L"AI Providers" : L"Settings";
    const wchar_t* subtitle = state.settingsPage == 1 ? L"Show, hide and arrange cards" :
                               state.settingsPage == 2 ? L"Shape, size and monitor position" :
                               state.settingsPage == 4 ? L"Choose several providers" :
                               state.settingsPage == 3 ? L"Icon, color and compact usage rings" : L"Isle preferences";

    titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(title, titleFormat_.Get(), D2D1::RectF(rect.left + pad, rect.top + 12.0f * s,
              rect.right - 80.0f * s, rect.top + 40.0f * s), D2D1::ColorF(0xFAFAFA), opacity);
    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    draw_text(subtitle, smallFormat_.Get(), D2D1::RectF(rect.left + pad, rect.top + 38.0f * s,
              rect.right - 80.0f * s, rect.top + 60.0f * s), D2D1::ColorF(0x71717A), opacity);

    const float closePress = state.pressedControl == 1 ? state.pressAmount : 0.0f;
    const auto closeBase = D2D1::RectF(rect.right - 64.0f * s, rect.top + 12.0f * s,
                                       rect.right - 32.0f * s, rect.top + 40.0f * s);
    const auto close = inset_rect(closeBase, closePress * 1.8f * s);
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(state.settingsPage == 0 ? L"\uE711" : L"\uE72B", iconFormat_.Get(), close,
              D2D1::ColorF(0xF4F4F5), opacity);

    const auto drawToggle = [&](D2D1_RECT_F row, bool enabled) {
        const auto toggle = D2D1::RectF(row.right - 52.0f * s, row.top + 18.0f * s,
                                        row.right - 12.0f * s, row.top + 42.0f * s);
        fill_round_rect(toggle, 12.0f * s,
                        enabled ? D2D1::ColorF(0x34C759) : D2D1::ColorF(0x3A3A3C), opacity);
        const float knobX = enabled ? toggle.right - 12.0f * s : toggle.left + 12.0f * s;
        ComPtr<ID2D1SolidColorBrush> knob;
        d2dContext_->CreateSolidColorBrush(with_alpha(D2D1::ColorF(0xFFFFFF), opacity), &knob);
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, (toggle.top + toggle.bottom) * 0.5f),
                                                9.0f * s, 9.0f * s), knob.Get());
    };

    const auto drawRow = [&](int rowIndex, const wchar_t* rowTitle, const wchar_t* detail,
                             int control, std::optional<bool> toggle, const wchar_t* value,
                             bool reorder, bool leadingIcon = false, bool checkbox = false) {
        const float y = rect.top + (74.0f + static_cast<float>(rowIndex) * 68.0f) * s;
        const float press = state.pressedControl == control ||
                            (reorder && state.pressedControl >= control && state.pressedControl < control + 3)
                                ? state.pressAmount : 0.0f;
        const auto rowBase = D2D1::RectF(rect.left + pad, y, rect.right - pad, y + 60.0f * s);
        const auto row = inset_rect(rowBase, press * 1.2f * s);
        fill_round_rect(row, 18.0f * s, press > 0.01f ? D2D1::ColorF(0x1C1C1E) : D2D1::ColorF(0x111113), opacity);
        stroke_round_rect(row, 18.0f * s, D2D1::ColorF(0xFFFFFF), 0.7f * s, 0.045f * opacity);
        const float textLeft = row.left + (leadingIcon ? 48.0f : 15.0f) * s;
        bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(rowTitle, bodyFormat_.Get(), D2D1::RectF(textLeft, row.top + 6.0f * s,
                  row.right - (reorder ? 108.0f : 68.0f) * s, row.top + 31.0f * s), D2D1::ColorF(0xF4F4F5), opacity);
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_text(detail, smallFormat_.Get(), D2D1::RectF(textLeft, row.top + 30.0f * s,
                  row.right - (reorder ? 108.0f : 68.0f) * s, row.bottom - 5.0f * s), D2D1::ColorF(0x71717A), opacity);

        if (checkbox) {
            const auto box = D2D1::RectF(row.right - 43.0f * s, row.top + 19.0f * s,
                                         row.right - 19.0f * s, row.top + 43.0f * s);
            fill_round_rect(box, 7.0f * s,
                            toggle.value_or(false) ? D2D1::ColorF(0xA78BFA) : D2D1::ColorF(0x27272A), opacity);
            if (toggle.value_or(false)) {
                iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                draw_text(L"\uE73E", iconFormat_.Get(), box, D2D1::ColorF(0xFFFFFF), opacity);
            } else {
                stroke_round_rect(box, 7.0f * s, D2D1::ColorF(0x636366), 0.8f * s, opacity);
            }
        } else if (reorder) {
            const bool enabled = toggle.value_or(false);
            ComPtr<ID2D1SolidColorBrush> dot;
            d2dContext_->CreateSolidColorBrush(with_alpha(enabled ? D2D1::ColorF(0x34C759) : D2D1::ColorF(0x48484A), opacity), &dot);
            d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(row.right - 88.0f * s, row.top + 30.0f * s),
                                                    5.0f * s, 5.0f * s), dot.Get());
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(L"\uE70E", iconFormat_.Get(),
                      D2D1::RectF(row.right - 70.0f * s, row.top + 12.0f * s,
                                  row.right - 42.0f * s, row.bottom - 12.0f * s),
                      D2D1::ColorF(0x8E8E93), opacity);
            draw_text(L"\uE70D", iconFormat_.Get(),
                      D2D1::RectF(row.right - 40.0f * s, row.top + 12.0f * s,
                                  row.right - 12.0f * s, row.bottom - 12.0f * s),
                      D2D1::ColorF(0x8E8E93), opacity);
        } else if (toggle.has_value()) {
            drawToggle(row, *toggle);
        } else if (value && *value) {
            smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_text(value, smallFormat_.Get(),
                      D2D1::RectF(row.right - 116.0f * s, row.top,
                                  row.right - 31.0f * s, row.bottom),
                      D2D1::ColorF(0xA78BFA), opacity);
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(L"\uE76C", iconFormat_.Get(),
                      D2D1::RectF(row.right - 31.0f * s, row.top + 12.0f * s,
                                  row.right - 7.0f * s, row.bottom - 12.0f * s),
                      D2D1::ColorF(0x8E8E93), opacity);
        } else {
            iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(L"\uE76C", iconFormat_.Get(),
                      D2D1::RectF(row.right - 43.0f * s, row.top + 12.0f * s,
                                  row.right - 11.0f * s, row.bottom - 12.0f * s),
                      D2D1::ColorF(0x8E8E93), opacity);
        }
    };

    if (state.settingsPage == 0) {
        drawRow(0, L"Start with Windows", L"Launch Isle after sign in", 10, state.startWithWindows, nullptr, false);
        drawRow(1, L"Hide in fullscreen", L"Stay out of games and video", 11, state.hideInFullscreen, nullptr, false);
        drawRow(2, L"Expand on hover", L"Open as soon as the pointer arrives", 12, state.expandOnHover, nullptr, false);
        drawRow(3, L"Widget Editor", L"Visibility and card order", 13, std::nullopt, nullptr, false);
        drawRow(4, L"Appearance & Position", L"Size, shape and snapping", 14, std::nullopt, nullptr, false);
        drawRow(5, L"AI Providers", L"Direct provider usage, icons and colors", 15, std::nullopt, nullptr, false);
        drawRow(6, L"External plugins", L"Open the plugins folder", 16, std::nullopt, nullptr, false);
    } else if (state.settingsPage == 1) {
        constexpr const wchar_t* names[]{L"AI Usage", L"App Launcher", L"Commands", L"System Metrics", L"Music Player"};
        constexpr const wchar_t* details[]{L"Provider quotas, reset windows and rings", L"Open favorite applications",
                                            L"Run saved commands", L"CPU, GPU and memory · hidden by default",
                                            L"Artwork, waveform and playback controls"};
        for (int rowIndex = 0; rowIndex < 5; ++rowIndex) {
            const int widget = state.widgetOrder[static_cast<std::size_t>(rowIndex)];
            drawRow(rowIndex, names[widget], details[widget], 20 + rowIndex * 3,
                    widget_enabled(state, widget), nullptr, true);
        }
        drawRow(5, L"Edit shortcuts", L"Open the launcher and command definitions", 35,
                 std::nullopt, nullptr, false);
    } else if (state.settingsPage == 2) {
        constexpr const wchar_t* sizeNames[]{L"Compact", L"Default", L"Large"};
        constexpr const wchar_t* shapeNames[]{L"Continuous", L"Soft", L"Pill"};
        constexpr const wchar_t* buttonNames[]{L"Circle", L"Rounded", L"Outline"};
        drawRow(0, L"Island size", L"Scale every surface together", 60, std::nullopt,
                sizeNames[std::clamp(state.islandSizePreset, 0, 2)], false);
        drawRow(1, L"Island shape", L"Corner language for panels", 61, std::nullopt,
                shapeNames[std::clamp(state.islandShape, 0, 2)], false);
        drawRow(2, L"Button shape", L"Launcher and command controls", 62, std::nullopt,
                buttonNames[std::clamp(state.buttonStyle, 0, 2)], false);
        drawRow(3, L"Reset position", L"Return to the current top center", 63, std::nullopt, L"Reset", false);
        drawRow(4, L"Monitor at cursor", L"Move Isle to the active display", 64, state.monitorAtCursor,
                 nullptr, false);
    } else {
        const int provider = std::clamp(state.selectedAiProvider, 0, static_cast<int>(kAIProviders.size() - 1));
        const std::wstring selected = std::to_wstring(state.aiVisibleCount) + L" selected · choose several";
        constexpr const wchar_t* compactModes[]{L"Waveform", L"Usage", L"Both"};
        if (state.settingsPage == 4) {
            constexpr int pageSize = 6;
            constexpr int pageCount = (static_cast<int>(kAIProviders.size()) + pageSize - 1) / pageSize;
            const int page = std::clamp(state.aiProviderPage, 0, pageCount - 1);
            const int pageStart = page * pageSize;
            for (int row = 0; row < pageSize; ++row) {
                const int index = pageStart + row;
                if (index >= static_cast<int>(kAIProviders.size())) break;
                const auto& info = kAIProviders[static_cast<std::size_t>(index)];
                const std::wstring detail = state.aiPageVisible[static_cast<std::size_t>(row)]
                    ? L"Visible in AI widget" : L"Hidden from AI widget";
                drawRow(row, info.name.data(), detail.c_str(), 80 + row,
                        state.aiPageVisible[static_cast<std::size_t>(row)], nullptr, false, true, true);
                const float y = rect.top + (74.0f + static_cast<float>(row) * 68.0f) * s;
                const auto badge = D2D1::RectF(rect.left + pad + 13.0f * s, y + 17.0f * s,
                                               rect.left + pad + 37.0f * s, y + 41.0f * s);
                if (!draw_provider_icon(info.id, info.color, badge, opacity)) {
                    fill_round_rect(badge, 7.0f * s, color_from_hex(info.color), 0.20f * opacity);
                    bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    draw_text(info.mark, bodyFormat_.Get(), badge, color_from_hex(info.color), opacity);
                }
            }
            const std::wstring pageLabel = L"Page " + std::to_wstring(page + 1) + L" / " + std::to_wstring(pageCount);
            drawRow(pageSize, L"More providers", L"Next provider group", 86, std::nullopt,
                    pageLabel.c_str(), false);
        } else {
        drawRow(0, L"Providers", selected.c_str(), 70, std::nullopt,
                kAIProviders[static_cast<std::size_t>(provider)].name.data(), false);
        drawRow(1, L"Show in AI widget", L"Select several providers independently", 71,
                state.selectedAiVisible, nullptr, false);
        drawRow(2, L"Show compact ring", L"Include this provider in island usage", 72,
                state.selectedAiRing, nullptr, false);
        drawRow(3, L"Provider color", L"Cycle an Apple system accent", 73, std::nullopt,
                state.selectedAiColor.c_str(), false);
        drawRow(4, L"Compact content", L"Waveform, usage, or both together", 74, std::nullopt,
                compactModes[std::clamp(state.compactMediaMode, 0, 2)], false);
        const std::wstring ringCount = std::to_wstring(std::clamp(state.compactRingCount, 1, 3));
        drawRow(5, L"Usage ring count", L"Show one to three selected providers", 75, std::nullopt,
                ringCount.c_str(), false);
        drawRow(6, L"Provider connection", L"Uses provider APIs, OAuth and local credentials", 76,
                std::nullopt, L"Isle", false);
        }
    }

    smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(state.settingsPage == 2 ? L"Drag the island header · panels open away from screen edges" :
              state.settingsPage == 4 ? L"Selected providers appear together in the AI widget" :
              state.settingsPage == 3 ? L"Credentials stay in Isle or the provider's own account store" :
              L"Isle 0.2 · Windows 10/11", smallFormat_.Get(),
              D2D1::RectF(rect.left + pad, rect.bottom - 31.0f * s, rect.right - pad, rect.bottom - 9.0f * s),
              D2D1::ColorF(0x52525B), opacity);
}

ID2D1SvgDocument* Renderer::provider_icon(std::wstring_view providerId, std::wstring_view accentHex) {
    if (!svgContext_ || providerId.empty()) return nullptr;

    // ponytail: the accent belongs in the key because the tint is baked into the markup.
    // Only visible provider/colour pairs land here, so the map stays tiny.
    std::wstring key(providerId);
    key += L'|';
    key += accentHex;
    const auto [entry, inserted] = providerIcons_.try_emplace(key);
    if (!inserted) return entry->second.Get();

    // A null entry caches the miss, so a provider without a bundled asset is never re-read.
    const auto path = executable_directory() / L"icons" /
                      (L"ProviderIcon-" + std::wstring(providerId) + L".svg");
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    std::string markup{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (markup.empty() || markup.size() > 256 * 1024) return nullptr;
    tint_brand_marks(markup, accentHex);

    ComPtr<IStream> stream;
    LARGE_INTEGER start{};
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(stream->Write(markup.data(), static_cast<ULONG>(markup.size()), nullptr)) ||
        FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
        return nullptr;
    }
    // Every asset carries a viewBox, so Direct2D fits its own coordinate system into this
    // viewport and the caller only has to scale 100 units onto the badge.
    if (FAILED(svgContext_->CreateSvgDocument(stream.Get(), D2D1::SizeF(100.0f, 100.0f),
                                              &entry->second))) {
        entry->second.Reset();
    }
    return entry->second.Get();
}

bool Renderer::draw_provider_icon(std::wstring_view providerId, std::wstring_view accentHex,
                                  D2D1_RECT_F rect, float opacity) {
    ID2D1SvgDocument* document = provider_icon(providerId, accentHex);
    if (!document) return false;
    const float side = std::min(rect.right - rect.left, rect.bottom - rect.top);
    if (side <= 0.0f || opacity <= 0.001f) return true;

    D2D1::Matrix3x2F saved;
    d2dContext_->GetTransform(&saved);
    const float scale = side / 100.0f;
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                              D2D1::Matrix3x2F::Translation((rect.left + rect.right - side) * 0.5f,
                                                            (rect.top + rect.bottom - side) * 0.5f) *
                              saved);
    // DrawSvgDocument takes no opacity, so the expand/collapse fade needs a layer.
    const bool fading = opacity < 0.999f;
    if (fading) {
        d2dContext_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr,
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                      D2D1::IdentityMatrix(), opacity),
                               nullptr);
    }
    svgContext_->DrawSvgDocument(document);
    if (fading) d2dContext_->PopLayer();
    d2dContext_->SetTransform(saved);
    return true;
}

void Renderer::draw_provider_badge(const Activity& activity, D2D1_RECT_F rect, float opacity) {
    if (draw_provider_icon(provider_id_from_source(activity.source), activity.accent, rect, opacity)) {
        return;
    }
    // No bundled asset for this provider: fall back to the short mark from the provider table.
    IDWriteTextFormat* format = activity.glyph.size() > 1 ? bodyFormat_.Get() : titleFormat_.Get();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(activity.glyph, format, rect, color_from_hex(activity.accent), opacity);
}

void Renderer::draw_collapsed_ai_rings(const RenderState& state, const std::vector<Activity>& activities,
                                        const D2D1_RECT_F& rect, float opacity) {
    if (state.compactMediaMode == 0) return;
    const float s = state.dpiScale;
    std::array<const Activity*, 3> selected{};
    std::size_t count = 0;
    for (const auto& activity : activities) {
        if (!activity.source.starts_with(L"ai.") || activity.kind != ActivityKind::Metric || !activity.compactRing) continue;
        bool duplicate = false;
        for (std::size_t i = 0; i < count; ++i) duplicate = duplicate || selected[i]->source == activity.source;
        if (!duplicate) selected[count++] = &activity;
        if (count == static_cast<std::size_t>(std::clamp(state.compactRingCount, 1, 3))) break;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const float centerX = rect.right - (16.0f + static_cast<float>(i) * 27.0f) * s;
        const auto center = D2D1::Point2F(centerX, (rect.top + rect.bottom) * 0.5f);
        draw_progress_ring(center, 10.0f * s, 2.4f * s, selected[i]->progress.value_or(0.0),
                           D2D1::ColorF(0x27272A), color_from_hex(selected[i]->accent), opacity);
        const auto mark = D2D1::RectF(center.x - 8.0f * s, center.y - 8.0f * s,
                                      center.x + 8.0f * s, center.y + 8.0f * s);
        if (draw_provider_icon(provider_id_from_source(selected[i]->source),
                               selected[i]->accent, mark, opacity)) continue;
        smallFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(selected[i]->glyph, smallFormat_.Get(), mark, D2D1::ColorF(0xF5F5F7), opacity);
    }
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

void Renderer::draw_waveform(D2D1_RECT_F rect, D2D1_COLOR_F color, float opacity,
                             bool active, bool audioReactive) {
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    const int bars = std::max(9, static_cast<int>(std::round(width / (9.0f * formatScale_))));
    const float gap = width / static_cast<float>(bars);
    const double phase = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * 5.4;
    if (audioReactive) update_audio_history(active);
    const auto [audioMin, audioMax] = std::minmax_element(audioHistory_.begin(), audioHistory_.end());
    for (int i = 0; i < bars; ++i) {
        const std::size_t historyIndex = static_cast<std::size_t>(i) * (audioHistory_.size() - 1) /
                                         static_cast<std::size_t>(std::max(1, bars - 1));
        const float strength = audioReactive
            ? audio_bar_strength(audioHistory_[historyIndex], *audioMin, *audioMax)
            : active
                ? 0.22f + 0.78f * static_cast<float>(std::abs(std::sin(phase + static_cast<double>(i) * 0.86)))
                : 0.24f + 0.13f * static_cast<float>((i * 7) % 5);
        const float barHeight = std::min(height, std::max(3.0f * formatScale_, height * strength));
        const float x = rect.left + gap * (static_cast<float>(i) + 0.5f);
        const auto bar = D2D1::RectF(x - 1.2f * formatScale_, (rect.top + rect.bottom - barHeight) * 0.5f,
                                     x + 1.2f * formatScale_, (rect.top + rect.bottom + barHeight) * 0.5f);
        fill_round_rect(bar, 1.2f * formatScale_, color, opacity * (0.65f + 0.35f * static_cast<float>(i + 1) / bars));
    }
}

void Renderer::update_audio_history(bool active) {
    const auto now = std::chrono::steady_clock::now();
    if (lastAudioSample_.time_since_epoch().count() != 0 &&
        now - lastAudioSample_ < std::chrono::milliseconds(30)) return;

    const bool stale = lastAudioSample_.time_since_epoch().count() == 0 ||
                       now - lastAudioSample_ > std::chrono::milliseconds(250);
    lastAudioSample_ = now;
    const float level = active ? std::min(1.0f, std::pow(audio_peak(), 1.35f) * 1.8f) : 0.0f;
    if (stale) {
        audioHistory_.fill(level);
    } else {
        std::rotate(audioHistory_.begin(), audioHistory_.begin() + 1, audioHistory_.end());
        audioHistory_.back() = level;
    }
}

float Renderer::audio_peak() {
    if (!audioMeter_) {
        if (!audioDeviceEnumerator_ && FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(audioDeviceEnumerator_.ReleaseAndGetAddressOf())))) return 0.0f;

        ComPtr<IMMDevice> device;
        if (FAILED(audioDeviceEnumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) return 0.0f;
        if (FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr,
                                    reinterpret_cast<void**>(audioMeter_.ReleaseAndGetAddressOf())))) return 0.0f;
    }

    float peak = 0.0f;
    if (FAILED(audioMeter_->GetPeakValue(&peak))) {
        audioMeter_.Reset();
        return 0.0f;
    }
    return std::clamp(peak, 0.0f, 1.0f);
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

void Renderer::draw_marquee_text(std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                                 D2D1_COLOR_F color, float opacity) {
    if (text.empty() || !format || opacity <= 0.001f) return;

    const auto now = std::chrono::steady_clock::now();
    if (std::wstring_view(marqueeText_) != text) {
        marqueeText_ = text;
        marqueeStarted_ = now;
    }

    ComPtr<IDWriteTextLayout> layout;
    const float visibleWidth = rect.right - rect.left;
    const float visibleHeight = rect.bottom - rect.top;
    if (FAILED(dwriteFactory_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                                100000.0f * formatScale_, visibleHeight, &layout))) {
        draw_text(text, format, rect, color, opacity);
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)) || metrics.widthIncludingTrailingWhitespace <= visibleWidth) {
        draw_text(text, format, rect, color, opacity);
        return;
    }

    const float overflow = metrics.widthIncludingTrailingWhitespace - visibleWidth;
    constexpr double pause = 0.9;
    const double travel = std::max(0.8, static_cast<double>(overflow / (30.0f * formatScale_)));
    const double elapsed = std::chrono::duration<double>(now - marqueeStarted_).count();
    const float offset = marquee_offset(elapsed, overflow, travel, pause);

    ComPtr<ID2D1SolidColorBrush> brush;
    d2dContext_->CreateSolidColorBrush(with_alpha(color, opacity), &brush);
    d2dContext_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2dContext_->DrawTextLayout(D2D1::Point2F(rect.left - offset, rect.top), layout.Get(), brush.Get(),
                                D2D1_DRAW_TEXT_OPTIONS_CLIP);
    d2dContext_->PopAxisAlignedClip();
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
