#include "ShortcutProvider.h"

#include "../core/Settings.h"

#include <Windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace isle {

using Microsoft::WRL::ComPtr;

namespace {
std::shared_ptr<const std::vector<std::uint8_t>> shortcut_icon(std::wstring_view path) {
    HICON icon = nullptr;
    SHFILEINFOW info{};
    if (SHGetFileInfoW(std::wstring(path).c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
        ComPtr<IImageList> imageList;
        if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                                     reinterpret_cast<void**>(imageList.GetAddressOf())))) {
            imageList->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon);
        }
    }
    if (!icon) {
        SHFILEINFOW large{};
        if (SHGetFileInfoW(std::wstring(path).c_str(), FILE_ATTRIBUTE_NORMAL, &large, sizeof(large),
                           SHGFI_ICON | SHGFI_LARGEICON)) {
            icon = large.hIcon;
        }
    }
    if (!icon) return {};

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IStream> output;
    std::shared_ptr<const std::vector<std::uint8_t>> result;
    UINT width = 0;
    UINT height = 0;
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateBitmapFromHICON(icon, &bitmap)) &&
        SUCCEEDED(bitmap->GetSize(&width, &height)) &&
        SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &output)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(output.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width, height)) &&
        SUCCEEDED(frame->SetPixelFormat(&pixelFormat)) &&
        SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
        SUCCEEDED(frame->Commit()) &&
        SUCCEEDED(encoder->Commit())) {
        STATSTG stat{};
        LARGE_INTEGER start{};
        if (SUCCEEDED(output->Stat(&stat, STATFLAG_NONAME)) &&
            stat.cbSize.QuadPart > 0 &&
            stat.cbSize.QuadPart <= static_cast<ULONGLONG>(std::numeric_limits<std::uint32_t>::max()) &&
            SUCCEEDED(output->Seek(start, STREAM_SEEK_SET, nullptr))) {
            auto bytes = std::make_shared<std::vector<std::uint8_t>>(
                static_cast<std::size_t>(stat.cbSize.QuadPart));
            ULONG read = 0;
            if (SUCCEEDED(output->Read(bytes->data(), static_cast<ULONG>(bytes->size()), &read)) &&
                read == static_cast<ULONG>(bytes->size())) {
                result = std::move(bytes);
            }
        }
    }
    DestroyIcon(icon);
    return result;
}

void publish_shortcuts(ActivityStore& store, const std::array<ShortcutSetting, kShortcutSlots>& shortcuts,
                       std::wstring_view source, std::wstring_view idPrefix,
                       std::wstring_view subtitle, std::wstring_view accent, int priority) {
    for (std::size_t i = 0; i < shortcuts.size(); ++i) {
        const auto& shortcut = shortcuts[i];
        if (!shortcut.enabled || shortcut.label.empty() || shortcut.target.empty()) continue;
        Activity activity;
        activity.id = std::wstring(idPrefix) + std::to_wstring(i);
        activity.source = std::wstring(source);
        activity.kind = ActivityKind::Shortcut;
        activity.title = shortcut.label;
        activity.subtitle = std::wstring(subtitle);
        activity.glyph = shortcut.glyph.empty() ? L"\uE8A7" : shortcut.glyph;
        if (source == L"shortcut.app") activity.artwork = shortcut_icon(shortcut.target);
        activity.accent = std::wstring(accent);
        activity.priority = priority - static_cast<int>(i);
        activity.actions = {{L"launch", L"Open", L"\uE768"}};
        store.upsert(std::move(activity));
    }
}

bool launch(const ShortcutSetting& shortcut) {
    const auto result = ShellExecuteW(nullptr, L"open", shortcut.target.c_str(),
                                      shortcut.arguments.empty() ? nullptr : shortcut.arguments.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}
} // namespace

void ShortcutProvider::start(ActivityStore& store) {
    store_ = &store;
    publish();
}

void ShortcutProvider::stop() {
    if (store_) {
        store_->remove_source(L"shortcut.app");
        store_->remove_source(L"shortcut.command");
    }
    store_ = nullptr;
}

void ShortcutProvider::tick() {
    if (!store_) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastCheck_.time_since_epoch().count() != 0 && now - lastCheck_ < std::chrono::seconds(1)) return;
    lastCheck_ = now;

    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(Settings::file_path(), ec);
    if (!ec && modified != settingsModified_) publish();
}

void ShortcutProvider::invoke(std::wstring_view activityId, std::wstring_view actionId) {
    if (actionId != L"launch") return;
    const Settings settings = Settings::load();
    if (activityId.starts_with(L"shortcut.app.")) {
        std::size_t index = 0;
        try {
            index = std::stoul(std::wstring(activityId.substr(std::wstring_view(L"shortcut.app.").size())));
        } catch (...) {
            return;
        }
        if (index >= settings.appShortcuts.size()) return;
        if (!launch(settings.appShortcuts[index]) && index == 1 && settings.appShortcuts[index].target == L"wt.exe") {
            ShellExecuteW(nullptr, L"open", L"powershell.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (activityId.starts_with(L"shortcut.command.")) {
        std::size_t index = 0;
        try {
            index = std::stoul(std::wstring(activityId.substr(std::wstring_view(L"shortcut.command.").size())));
        } catch (...) {
            return;
        }
        if (index >= settings.commandShortcuts.size()) return;
        launch(settings.commandShortcuts[index]);
    }
}

void ShortcutProvider::publish() {
    if (!store_) return;
    const Settings settings = Settings::load();
    store_->remove_source(L"shortcut.app");
    store_->remove_source(L"shortcut.command");
    publish_shortcuts(*store_, settings.appShortcuts, L"shortcut.app", L"shortcut.app.",
                      L"Application", L"#0A84FF", 190);
    publish_shortcuts(*store_, settings.commandShortcuts, L"shortcut.command", L"shortcut.command.",
                      L"Command", L"#FF9F0A", 180);
    std::error_code ec;
    settingsModified_ = std::filesystem::last_write_time(Settings::file_path(), ec);
    lastCheck_ = std::chrono::steady_clock::now();
}

} // namespace isle
