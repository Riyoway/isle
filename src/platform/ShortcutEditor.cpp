#include "ShortcutEditor.h"
#include "../core/ShortcutCatalog.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <string_view>

#include <wrl/client.h>

namespace isle {

namespace {
constexpr wchar_t kWindowClass[] = L"IsleShortcutEditorWindow";
constexpr int kAppsTab = 1001;
constexpr int kCommandsTab = 1002;
constexpr int kAppSearch = 1101;
constexpr int kAppList = 1102;
constexpr int kRefreshApps = 1103;
constexpr int kCommandLabel = 1201;
constexpr int kCommandTarget = 1202;
constexpr int kCommandArguments = 1203;
constexpr int kBrowseTarget = 1204;
constexpr int kAddCommand = 1205;
constexpr int kCommandList = 1206;
constexpr int kRemoveCommand = 1207;
constexpr UINT_PTR kAppLoadTimer = 2;
constexpr int kAppLoadBatchSize = 12;
constexpr std::size_t kMaxApps = 500;
constexpr wchar_t kAppCacheFile[] = L"shortcut-apps.ini";

bool visible_match(std::wstring_view query, const ShortcutEditor::InstalledApp& app) {
    if (query.empty()) return true;
    std::wstring haystack = app.label + L"\n" + app.path;
    std::ranges::transform(haystack, haystack.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    std::wstring needle(query);
    std::ranges::transform(needle, needle.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return haystack.find(needle) != std::wstring::npos;
}

int item_data(HWND list, int item) {
    LVITEMW data{LVIF_PARAM};
    data.iItem = item;
    data.iSubItem = 0;
    return ListView_GetItem(list, &data) ? static_cast<int>(data.lParam) : -1;
}

void set_text(HWND control, std::wstring_view text) {
    if (control) SetWindowTextW(control, std::wstring(text).c_str());
}

std::wstring text_of(HWND control) {
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(std::wcslen(result.c_str()));
    return result;
}

void show_control(HWND control, bool visible) {
    if (control) ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void apply_font(HWND control, HFONT font) {
    if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void apply_dark_theme(HWND control) {
    if (control) SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
}

RECT screen_bounds(HWND owner, RECT bounds) {
    if (owner) MapWindowPoints(owner, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}

void fill_rounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void stroke_rounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void draw_toggle(HDC dc, RECT rect, bool enabled) {
    fill_rounded(dc, rect, rect.bottom - rect.top,
                 enabled ? RGB(52, 199, 89) : RGB(58, 58, 60));
    const int inset = 3;
    const int diameter = rect.bottom - rect.top - inset * 2;
    const int left = enabled ? rect.right - inset - diameter : rect.left + inset;
    HBRUSH knob = CreateSolidBrush(RGB(255, 255, 255));
    const HGDIOBJ oldBrush = SelectObject(dc, knob);
    const HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, left, rect.top + inset, left + diameter, rect.top + inset + diameter);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(knob);
}

std::int64_t file_stamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto value = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(value.time_since_epoch().count());
}

std::vector<std::filesystem::path> installed_app_roots() {
    std::vector<std::filesystem::path> roots;
    for (const KNOWNFOLDERID folder : {FOLDERID_Programs, FOLDERID_CommonPrograms,
                                       FOLDERID_Desktop, FOLDERID_PublicDesktop}) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw))) {
            roots.emplace_back(raw);
            CoTaskMemFree(raw);
        }
    }
    return roots;
}

std::filesystem::path app_cache_path() {
    return Settings::data_directory() / kAppCacheFile;
}

std::wstring read_cache_value(const std::filesystem::path& path, const wchar_t* section,
                              const wchar_t* key, const wchar_t* fallback = L"") {
    std::array<wchar_t, 4096> buffer{};
    GetPrivateProfileStringW(section, key, fallback, buffer.data(),
                             static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

bool parse_cache_integer(const std::wstring& value, std::int64_t& result) {
    try {
        std::size_t consumed = 0;
        result = std::stoll(value, &consumed);
        return consumed == value.size();
    } catch (...) {
        return false;
    }
}

} // namespace

ShortcutEditor::~ShortcutEditor() {
    if (hwnd_) {
        KillTimer(hwnd_, kAppLoadTimer);
        DestroyWindow(hwnd_);
    }
    destroy_resources();
}

void ShortcutEditor::show_embedded(HWND parent, HINSTANCE instance, RECT bounds, int radius,
                                   std::function<void()> changed) {
    owner_ = parent;
    instance_ = instance;
    changed_ = std::move(changed);
    if (hwnd_) {
        settings_ = Settings::load();
        resize_embedded(bounds, radius);
        if (!appCacheReady_ && !load_app_cache()) start_app_discovery();
        if (!appLoading_) {
            populate_apps();
            set_text(appHint_, L"Select apps to show in the Apps widget.");
        }
        switch_page(commandsPage_);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return;
    }

    settings_ = Settings::load();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &ShortcutEditor::window_proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        registered = true;
    }

    const RECT screen = screen_bounds(owner_, bounds);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, L"Shortcuts",
                            WS_POPUP | WS_CLIPCHILDREN,
                            screen.left, screen.top, screen.right - screen.left,
                            screen.bottom - screen.top, owner_, nullptr, instance_, this);
    if (!hwnd_) return;

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    resize_embedded(bounds, radius);
    if (!appCacheReady_ && !load_app_cache()) start_app_discovery();
    switch_page(false);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void ShortcutEditor::hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kAppLoadTimer);
    appLoading_ = false;
    appIterator_.reset();
    appRoots_.clear();
    nextAppCache_.clear();
    ShowWindow(hwnd_, SW_HIDE);
}

void ShortcutEditor::resize_embedded(RECT bounds, int radius) {
    if (!hwnd_) return;
    const RECT screen = screen_bounds(owner_, bounds);
    const int width = std::max(1L, screen.right - screen.left);
    const int height = std::max(1L, screen.bottom - screen.top);
    if (EqualRect(&screen, &embeddedBounds_) && radius == embeddedRadius_) {
        return;
    }
    embeddedBounds_ = screen;
    embeddedRadius_ = radius;
    SetWindowPos(hwnd_, HWND_TOPMOST, screen.left, screen.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                     std::max(0, radius * 2), std::max(0, radius * 2));
    if (region && SetWindowRgn(hwnd_, region, TRUE) == 0) DeleteObject(region);
    layout_controls();
    RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

LRESULT CALLBACK ShortcutEditor::window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<ShortcutEditor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<ShortcutEditor*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->handle_message(message, wParam, lParam)
                : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT ShortcutEditor::handle_message(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            create_controls();
            layout_controls();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(hwnd_, &paint);
            FillRect(dc, &paint.rcPaint, backgroundBrush_);
            const UINT dpi = GetDpiForWindow(hwnd_);
            const int radius = MulDiv(14, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
            const auto drawField = [&](HWND control) {
                if (!control || !IsWindowVisible(control)) return;
                RECT field{};
                GetWindowRect(control, &field);
                MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&field), 2);
                InflateRect(&field, MulDiv(10, dpi == 0 ? 96 : static_cast<int>(dpi), 96),
                            MulDiv(5, dpi == 0 ? 96 : static_cast<int>(dpi), 96));
                fill_rounded(dc, field, radius, RGB(17, 17, 19));
                stroke_rounded(dc, field, radius, RGB(29, 29, 32));
            };
            drawField(search_);
            drawField(commandLabel_);
            drawField(commandTarget_);
            drawField(commandArguments_);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_SIZE:
            layout_controls();
            return 0;
        case WM_TIMER:
            if (wParam == kAppLoadTimer) load_app_batch();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam) == appHint_
                ? RGB(113, 113, 122) : RGB(224, 224, 230));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(0, 0, 0));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(244, 244, 245));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(17, 17, 19));
            return reinterpret_cast<LRESULT>(fieldBrush_);
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!draw || (draw->CtlType != ODT_BUTTON && draw->CtlType != ODT_STATIC)) return FALSE;
            const bool tab = draw->CtlID == kAppsTab || draw->CtlID == kCommandsTab;
            const bool selectedTab = (draw->CtlID == kAppsTab && !commandsPage_) ||
                                     (draw->CtlID == kCommandsTab && commandsPage_);
            const bool primary = draw->CtlID == kAddCommand;
            const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
            const COLORREF fill = primary ? (pressed ? RGB(210, 210, 214) : RGB(245, 245, 247)) :
                                  selectedTab ? (pressed ? RGB(72, 72, 74) : RGB(58, 58, 60)) :
                                  pressed ? RGB(50, 50, 54) : RGB(28, 28, 30);
            FillRect(draw->hDC, &draw->rcItem, backgroundBrush_);
            const UINT dpi = GetDpiForWindow(hwnd_);
            fill_rounded(draw->hDC, draw->rcItem,
                         MulDiv(tab ? 18 : 16, dpi == 0 ? 96 : static_cast<int>(dpi), 96), fill);

            std::array<wchar_t, 96> label{};
            GetWindowTextW(draw->hwndItem, label.data(), static_cast<int>(label.size()));
            const HGDIOBJ oldFont = font_ ? SelectObject(draw->hDC, font_) : nullptr;
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, primary ? RGB(12, 12, 14) : RGB(245, 245, 247));
            RECT textRect = draw->rcItem;
            DrawTextW(draw->hDC, label.data(), -1, &textRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(draw->hDC, oldFont);
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kAppsTab && notification == BN_CLICKED) switch_page(false);
            else if (id == kCommandsTab && notification == BN_CLICKED) switch_page(true);
            else if (id == kRefreshApps && notification == BN_CLICKED) refresh_apps();
            else if (id == kAppSearch && notification == EN_CHANGE) populate_apps();
            else if (id == kBrowseTarget && notification == BN_CLICKED) browse_target();
            else if (id == kAddCommand && notification == BN_CLICKED) add_command();
            else if (id == kRemoveCommand && notification == BN_CLICKED) remove_command();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (!header) return 0;
            if (header->code == NM_CLICK && header->hwndFrom == appList_) {
                const auto* click = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                if (click->iItem >= 0) {
                    const int index = item_data(appList_, click->iItem);
                    if (index >= 0 && index < static_cast<int>(apps_.size())) {
                        const auto& app = apps_[static_cast<std::size_t>(index)];
                        const bool enabled = std::ranges::any_of(settings_.appShortcuts, [&](const ShortcutSetting& shortcut) {
                            return shortcut.enabled && same_path(shortcut.target, app.path);
                        });
                        update_app_check(click->iItem, !enabled);
                        InvalidateRect(appList_, nullptr, FALSE);
                    }
                }
            } else if (header->code == LVN_KEYDOWN && header->hwndFrom == appList_) {
                const auto* key = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                if (key->wVKey == VK_SPACE || key->wVKey == VK_RETURN) {
                    const int item = ListView_GetNextItem(appList_, -1, LVNI_SELECTED);
                    if (item >= 0) {
                        const int index = item_data(appList_, item);
                        if (index >= 0 && index < static_cast<int>(apps_.size())) {
                            const auto& app = apps_[static_cast<std::size_t>(index)];
                            const bool enabled = std::ranges::any_of(settings_.appShortcuts, [&](const ShortcutSetting& shortcut) {
                                return shortcut.enabled && same_path(shortcut.target, app.path);
                            });
                            update_app_check(item, !enabled);
                            InvalidateRect(appList_, nullptr, FALSE);
                        }
                    }
                }
            } else if (header->code == LVN_ITEMCHANGED && header->hwndFrom == commandList_) {
                const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((change->uNewState & LVIS_SELECTED) != 0 &&
                    (change->uOldState & LVIS_SELECTED) == 0) {
                    load_selected_command();
                }
            } else if (header->code == NM_CUSTOMDRAW &&
                       (header->hwndFrom == appList_ || header->hwndFrom == commandList_)) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT)) {
                    const UINT dpi = GetDpiForWindow(hwnd_);
                    const float scale = dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
                    const auto px = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
                    const int item = static_cast<int>(draw->nmcd.dwItemSpec);
                    RECT row{};
                    if (!ListView_GetItemRect(header->hwndFrom, item, &row, LVIR_BOUNDS)) {
                        row = draw->nmcd.rc;
                    }
                    RECT client{};
                    GetClientRect(header->hwndFrom, &client);
                    row.left = client.left;
                    row.right = client.right;
                    FillRect(draw->nmcd.hdc, &row, backgroundBrush_);
                    RECT card{row.left + px(4.0f), row.top + px(4.0f),
                              row.right - px(4.0f), row.bottom - px(4.0f)};
                    const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
                    fill_rounded(draw->nmcd.hdc, card, px(18.0f),
                                 selected ? RGB(28, 28, 30) : RGB(17, 17, 19));
                    stroke_rounded(draw->nmcd.hdc, card, px(18.0f), RGB(29, 29, 32));
                    SetBkMode(draw->nmcd.hdc, TRANSPARENT);

                    const int index = static_cast<int>(draw->nmcd.lItemlParam);
                    if (header->hwndFrom == appList_ && index >= 0 && index < static_cast<int>(apps_.size())) {
                        auto& app = apps_[static_cast<std::size_t>(index)];
                        if (app.imageIndex < 0) load_app_image(app);
                        const int iconSize = px(32.0f);
                        const int iconX = card.left + px(14.0f);
                        const int iconY = card.top + (card.bottom - card.top - iconSize) / 2;
                        if (appImages_ && app.imageIndex >= 0) {
                            ImageList_Draw(appImages_, app.imageIndex, draw->nmcd.hdc, iconX, iconY, ILD_TRANSPARENT);
                        }
                        const int textLeft = card.left + px(58.0f);
                        RECT title{textLeft, card.top + px(7.0f), card.right - px(66.0f), card.top + px(31.0f)};
                        RECT caption{textLeft, card.top + px(30.0f), card.right - px(66.0f), card.bottom - px(5.0f)};
                        const HGDIOBJ oldFont = SelectObject(draw->nmcd.hdc, titleFont_);
                        SetTextColor(draw->nmcd.hdc, RGB(244, 244, 245));
                        DrawTextW(draw->nmcd.hdc, app.label.c_str(), -1, &title,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        SelectObject(draw->nmcd.hdc, captionFont_);
                        SetTextColor(draw->nmcd.hdc, RGB(113, 113, 122));
                        DrawTextW(draw->nmcd.hdc, app.path.c_str(), -1, &caption,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        SelectObject(draw->nmcd.hdc, oldFont);
                        const bool enabled = std::ranges::any_of(settings_.appShortcuts, [&](const ShortcutSetting& shortcut) {
                            return shortcut.enabled && same_path(shortcut.target, app.path);
                        });
                        RECT toggle{card.right - px(52.0f), card.top + px(18.0f),
                                    card.right - px(12.0f), card.top + px(42.0f)};
                        draw_toggle(draw->nmcd.hdc, toggle, enabled);
                    } else if (header->hwndFrom == commandList_ && index >= 0 &&
                               index < static_cast<int>(settings_.commandShortcuts.size())) {
                        const auto& command = settings_.commandShortcuts[static_cast<std::size_t>(index)];
                        RECT title{card.left + px(15.0f), card.top + px(7.0f), card.right - px(15.0f), card.top + px(31.0f)};
                        RECT caption{card.left + px(15.0f), card.top + px(30.0f), card.right - px(15.0f), card.bottom - px(5.0f)};
                        const HGDIOBJ oldFont = SelectObject(draw->nmcd.hdc, titleFont_);
                        SetTextColor(draw->nmcd.hdc, RGB(244, 244, 245));
                        DrawTextW(draw->nmcd.hdc, command.label.c_str(), -1, &title,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        SelectObject(draw->nmcd.hdc, captionFont_);
                        SetTextColor(draw->nmcd.hdc, RGB(113, 113, 122));
                        DrawTextW(draw->nmcd.hdc, command.target.c_str(), -1, &caption,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        SelectObject(draw->nmcd.hdc, oldFont);
                    }
                    return CDRF_SKIPDEFAULT;
                }
            }
            return 0;
        }
        case WM_CLOSE:
            hide();
            return 0;
        case WM_NCDESTROY:
            KillTimer(hwnd_, kAppLoadTimer);
            destroy_resources();
            hwnd_ = nullptr;
            owner_ = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

void ShortcutEditor::create_controls() {
    backgroundBrush_ = CreateSolidBrush(RGB(0, 0, 0));
    fieldBrush_ = CreateSolidBrush(RGB(17, 17, 19));
    const UINT dpi = GetDpiForWindow(hwnd_);
    const int fontHeight = -MulDiv(11, dpi == 0 ? 96 : static_cast<int>(dpi), 72);
    const int captionHeight = -MulDiv(9, dpi == 0 ? 96 : static_cast<int>(dpi), 72);
    font_ = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    titleFont_ = CreateFontW(fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    captionFont_ = CreateFontW(captionHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    appsTab_ = CreateWindowExW(0, WC_BUTTONW, L"Apps", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                               0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppsTab)), instance_, nullptr);
    commandsTab_ = CreateWindowExW(0, WC_BUTTONW, L"Commands", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                                   0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandsTab)), instance_, nullptr);

    search_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                              0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppSearch)), instance_, nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search installed apps"));
    refreshAppsButton_ = CreateWindowExW(0, WC_BUTTONW, L"Refresh",
                                         WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
                                         0, 0, 1, 1, hwnd_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshApps)),
                                         instance_, nullptr);
    appList_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER |
                               LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                               0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(appList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
    column.pszText = const_cast<LPWSTR>(L"Installed applications");
    column.cx = 320;
    ListView_InsertColumn(appList_, 0, &column);
    appHint_ = CreateWindowExW(0, WC_STATICW,
                               L"Select apps to show in the Apps widget.",
                               WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandLabelCaption_ = CreateWindowExW(0, WC_STATICW, L"Name", WS_CHILD,
                                           0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandLabel_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                                    0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandLabel)), instance_, nullptr);
    commandTargetCaption_ = CreateWindowExW(0, WC_STATICW, L"Target", WS_CHILD,
                                            0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandTarget_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                                     0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandTarget)), instance_, nullptr);
    browseButton_ = CreateWindowExW(0, WC_BUTTONW, L"Browse…", WS_CHILD | BS_OWNERDRAW | WS_TABSTOP,
                                    0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowseTarget)), instance_, nullptr);
    commandArgumentsCaption_ = CreateWindowExW(0, WC_STATICW, L"Arguments", WS_CHILD,
                                               0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandArguments_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                                        0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandArguments)), instance_, nullptr);
    addCommandButton_ = CreateWindowExW(0, WC_BUTTONW, L"Add command",
                                        WS_CHILD | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 1, 1,
                                        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddCommand)), instance_, nullptr);
    commandList_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                   WS_CHILD | LVS_REPORT | LVS_NOCOLUMNHEADER |
                                   LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                                   0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(commandList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    column.pszText = const_cast<LPWSTR>(L"Name");
    column.cx = 220;
    ListView_InsertColumn(commandList_, 0, &column);
    removeCommandButton_ = CreateWindowExW(0, WC_BUTTONW, L"Remove selected",
                                           WS_CHILD | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 1, 1,
                                           hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveCommand)), instance_, nullptr);

    const std::array<HWND, 16> controls{
        appsTab_, commandsTab_, search_, refreshAppsButton_, appList_, appHint_,
        commandLabelCaption_, commandLabel_, commandTargetCaption_, commandTarget_, browseButton_,
        commandArgumentsCaption_, commandArguments_, addCommandButton_, commandList_, removeCommandButton_};
    for (HWND control : controls) apply_font(control, font_);
    apply_font(appHint_, captionFont_);
    for (HWND control : {search_, appList_, commandLabel_, commandTarget_, commandArguments_, commandList_}) {
        apply_dark_theme(control);
    }

    for (HWND list : {appList_, commandList_}) {
        ListView_SetBkColor(list, RGB(0, 0, 0));
        ListView_SetTextBkColor(list, RGB(0, 0, 0));
        ListView_SetTextColor(list, RGB(238, 238, 242));
    }

    const float scale = dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
    appImages_ = ImageList_Create(static_cast<int>(std::lround(32.0f * scale)),
                                  static_cast<int>(std::lround(32.0f * scale)),
                                  ILC_COLOR32 | ILC_MASK, 64, 32);
    rowHeightImages_ = ImageList_Create(1, static_cast<int>(std::lround(68.0f * scale)),
                                        ILC_COLOR32, 1, 1);
    ListView_SetImageList(appList_, rowHeightImages_, LVSIL_SMALL);
    ListView_SetImageList(commandList_, rowHeightImages_, LVSIL_SMALL);
    appImageCacheValid_ = appImages_ != nullptr;
}

void ShortcutEditor::layout_controls() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    const UINT dpi = GetDpiForWindow(hwnd_);
    const float scale = dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
    const auto px = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
    const int pad = px(10.0f);
    const int gap = px(6.0f);
    const int buttonHeight = px(34.0f);
    const int tabsWidth = std::max(1, (width - pad * 2 - gap) / 2);

    MoveWindow(appsTab_, pad, px(6.0f), tabsWidth, buttonHeight, TRUE);
    MoveWindow(commandsTab_, pad + tabsWidth + gap, px(6.0f), tabsWidth, buttonHeight, TRUE);

    const int refreshWidth = px(72.0f);
    MoveWindow(search_, pad + px(10.0f), px(54.0f),
               std::max(1, width - pad * 2 - px(20.0f) - refreshWidth - gap), px(30.0f), TRUE);
    MoveWindow(refreshAppsButton_, width - pad - refreshWidth, px(54.0f), refreshWidth, px(30.0f), TRUE);
    MoveWindow(appList_, pad, px(98.0f), width - pad * 2,
               std::max(px(170.0f), height - px(136.0f)), TRUE);
    RECT appClient{};
    GetClientRect(appList_, &appClient);
    ListView_SetColumnWidth(appList_, 0, std::max(1L, appClient.right - px(2.0f) -
        GetSystemMetricsForDpi(SM_CXVSCROLL, dpi == 0 ? 96 : dpi)));
    MoveWindow(appHint_, pad + px(4.0f), height - px(28.0f), width - pad * 2, px(20.0f), TRUE);

    const int labelWidth = px(72.0f);
    const int fieldLeft = pad + labelWidth + px(10.0f);
    MoveWindow(commandLabelCaption_, pad, px(59.0f), labelWidth, px(22.0f), TRUE);
    MoveWindow(commandLabel_, fieldLeft, px(55.0f), width - fieldLeft - pad - px(10.0f), px(30.0f), TRUE);
    MoveWindow(commandTargetCaption_, pad, px(104.0f), labelWidth, px(22.0f), TRUE);
    MoveWindow(commandTarget_, fieldLeft, px(100.0f), width - fieldLeft - px(88.0f), px(30.0f), TRUE);
    MoveWindow(browseButton_, width - px(72.0f), px(100.0f), px(62.0f), px(30.0f), TRUE);
    MoveWindow(commandArgumentsCaption_, pad, px(149.0f), labelWidth, px(22.0f), TRUE);
    MoveWindow(commandArguments_, fieldLeft, px(145.0f), width - fieldLeft - pad - px(10.0f), px(30.0f), TRUE);
    MoveWindow(addCommandButton_, width - px(122.0f), px(189.0f), px(112.0f), buttonHeight, TRUE);
    MoveWindow(commandList_, pad, px(235.0f), width - pad * 2,
               std::max(px(120.0f), height - px(282.0f)), TRUE);
    RECT commandClient{};
    GetClientRect(commandList_, &commandClient);
    ListView_SetColumnWidth(commandList_, 0, std::max(1L, commandClient.right - px(2.0f) -
        GetSystemMetricsForDpi(SM_CXVSCROLL, dpi == 0 ? 96 : dpi)));
    MoveWindow(removeCommandButton_, width - px(136.0f), height - px(43.0f), px(126.0f), px(32.0f), TRUE);
}

void ShortcutEditor::switch_page(bool commands) {
    commandsPage_ = commands;
    InvalidateRect(appsTab_, nullptr, TRUE);
    InvalidateRect(commandsTab_, nullptr, TRUE);
    InvalidateRect(hwnd_, nullptr, TRUE);
    layout_controls();
    show_control(search_, !commands);
    show_control(refreshAppsButton_, !commands);
    show_control(appList_, !commands);
    show_control(appHint_, !commands);
    show_control(commandLabelCaption_, commands);
    show_control(commandLabel_, commands);
    show_control(commandTargetCaption_, commands);
    show_control(commandTarget_, commands);
    show_control(commandArgumentsCaption_, commands);
    show_control(commandArguments_, commands);
    show_control(browseButton_, commands);
    show_control(addCommandButton_, commands);
    show_control(commandList_, commands);
    show_control(removeCommandButton_, commands);
    if (commands) populate_commands();
    else populate_apps();
    SetFocus(commands ? commandLabel_ : search_);
}

void ShortcutEditor::refresh_apps() {
    if (!commandsPage_) start_app_discovery();
}

void ShortcutEditor::start_app_discovery() {
    if (!hwnd_ || !appList_) return;
    KillTimer(hwnd_, kAppLoadTimer);
    appLoading_ = true;
    appRootIndex_ = 0;
    appIterator_.reset();
    appRoots_.clear();
    appSeen_.clear();
    nextAppCache_.clear();

    if (!appCache_.empty()) {
        apps_.clear();
        apps_.reserve(appCache_.size());
        for (const auto& entry : appCache_) {
            auto app = entry.app;
            if (!appImageCacheValid_) app.imageIndex = -1;
            apps_.push_back(std::move(app));
        }
        std::ranges::sort(apps_, [](const InstalledApp& left, const InstalledApp& right) {
            return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
        });
    }
    populate_apps();
    set_text(appHint_, apps_.empty() ? L"Loading installed apps…" : L"Refreshing installed apps…");

    appRoots_ = installed_app_roots();
    SetTimer(hwnd_, kAppLoadTimer, 16, nullptr);
}

void ShortcutEditor::load_app_batch() {
    if (!appLoading_) return;
    const std::filesystem::recursive_directory_iterator end;
    int processed = 0;
    while (processed < kAppLoadBatchSize && nextAppCache_.size() < kMaxApps) {
        if (!appIterator_) {
            while (appRootIndex_ < appRoots_.size()) {
                std::error_code ec;
                auto iterator = std::make_unique<std::filesystem::recursive_directory_iterator>(
                    appRoots_[appRootIndex_], std::filesystem::directory_options::skip_permission_denied, ec);
                if (!ec && *iterator != end) {
                    appIterator_ = std::move(iterator);
                    break;
                }
                ++appRootIndex_;
            }
            if (!appIterator_) {
                finish_app_discovery();
                return;
            }
        }
        if (*appIterator_ == end) {
            appIterator_.reset();
            ++appRootIndex_;
            continue;
        }

        const auto path = appIterator_->operator*().path();
        std::error_code fileError;
        const bool regular = appIterator_->operator*().is_regular_file(fileError);
        std::error_code directoryError;
        const bool directory = appIterator_->operator*().is_directory(directoryError);
        if (appRootIndex_ >= 2 && directory) appIterator_->disable_recursion_pending();
        std::error_code iteratorError;
        appIterator_->increment(iteratorError);
        if (iteratorError || *appIterator_ == end) {
            appIterator_.reset();
            ++appRootIndex_;
        }
        ++processed;
        if (regular) append_app(path);
    }

    if (nextAppCache_.size() >= kMaxApps) {
        finish_app_discovery();
        return;
    }
    if (!commandsPage_) populate_apps();
    std::wstring status = appLoading_ ? L"Loading installed apps… " : L"";
    status += std::to_wstring(apps_.size());
    set_text(appHint_, status);
}

void ShortcutEditor::finish_app_discovery() {
    if (!appLoading_) return;
    KillTimer(hwnd_, kAppLoadTimer);
    appLoading_ = false;
    appIterator_.reset();
    appRoots_.clear();
    apps_.erase(std::remove_if(apps_.begin(), apps_.end(), [&](const InstalledApp& app) {
        return std::ranges::none_of(nextAppCache_, [&](const CachedApp& entry) {
            return same_path(entry.app.path, app.path);
        });
    }), apps_.end());
    appCache_ = std::move(nextAppCache_);
    std::ranges::sort(apps_, [](const InstalledApp& left, const InstalledApp& right) {
        return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
    });
    appImageCacheValid_ = appImages_ != nullptr;
    appCacheReady_ = true;
    save_app_cache();
    set_text(appHint_, L"Select apps to show in the Apps widget.");
    if (!commandsPage_) populate_apps();
}

bool ShortcutEditor::append_app(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (extension != L".lnk" && extension != L".url" && extension != L".exe") return false;

    const std::wstring appPath = path.wstring();
    const std::wstring label = path.stem().wstring();
    if (shortcut_is_auxiliary(label, appPath)) return false;
    const std::wstring key = lower(appPath);
    if (std::ranges::find(appSeen_, key) != appSeen_.end()) return false;
    appSeen_.push_back(key);

    const std::int64_t modified = file_stamp(path);
    // ponytail: linear lookup is bounded by the 500-item picker; use a map if that cap grows.
    const auto cached = std::ranges::find_if(appCache_, [&](const CachedApp& entry) {
        return entry.modified == modified && same_path(entry.app.path, appPath);
    });

    InstalledApp app;
    if (cached != appCache_.end() && appImageCacheValid_ && cached->app.imageIndex >= 0) {
        app = cached->app;
        app.path = appPath;
    } else {
        app.path = appPath;
        app.label = label;
        app.identity = shortcut_group_key(label);
        load_app_image(app);
    }
    if (app.label.empty()) return false;
    if (app.identity.empty()) app.identity = shortcut_group_key(app.label);
    const auto duplicate = std::ranges::find_if(nextAppCache_, [&](const CachedApp& entry) {
        return entry.app.identity == app.identity;
    });
    if (duplicate != nextAppCache_.end() && duplicate->app.label.size() <= app.label.size()) return true;
    if (duplicate != nextAppCache_.end()) *duplicate = {app, modified};
    else nextAppCache_.push_back({app, modified});

    const auto visible = std::ranges::find_if(apps_, [&](const InstalledApp& item) {
        return item.identity == app.identity;
    });
    if (visible != apps_.end()) {
        *visible = std::move(app);
    } else {
        apps_.push_back(std::move(app));
    }
    return true;
}

void ShortcutEditor::load_app_image(InstalledApp& app) {
    app.imageIndex = -1;
    if (!appImages_) return;
    SHFILEINFOW info{};
    if (SHGetFileInfoW(app.path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_SMALLICON)) {
        app.imageIndex = ImageList_AddIcon(appImages_, info.hIcon);
        DestroyIcon(info.hIcon);
    }
}

bool ShortcutEditor::load_app_cache() {
    const auto path = app_cache_path();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;

    std::int64_t countValue = 0;
    if (!parse_cache_integer(read_cache_value(path, L"cache", L"count", L"-1"), countValue) ||
        countValue < 0 || countValue > static_cast<std::int64_t>(kMaxApps)) {
        return false;
    }

    std::vector<CachedApp> loaded;
    loaded.reserve(static_cast<std::size_t>(countValue));
    for (std::int64_t i = 0; i < countValue; ++i) {
        const std::wstring section = L"app." + std::to_wstring(i);
        InstalledApp app;
        app.label = read_cache_value(path, section.c_str(), L"label");
        app.path = read_cache_value(path, section.c_str(), L"path");
        app.identity = read_cache_value(path, section.c_str(), L"identity");
        std::int64_t modified = 0;
        if (app.label.empty() || app.path.empty() ||
            !parse_cache_integer(read_cache_value(path, section.c_str(), L"modified", L""), modified)) {
            return false;
        }
        if (app.identity.empty()) app.identity = shortcut_group_key(app.label);
        loaded.push_back({std::move(app), modified});
    }

    appCache_ = std::move(loaded);
    apps_.clear();
    apps_.reserve(appCache_.size());
    for (const auto& entry : appCache_) apps_.push_back(entry.app);
    std::ranges::sort(apps_, [](const InstalledApp& left, const InstalledApp& right) {
        return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
    });
    appImageCacheValid_ = false;
    appCacheReady_ = true;
    return true;
}

void ShortcutEditor::save_app_cache() const {
    const auto path = app_cache_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::int64_t previousCount = 0;
    parse_cache_integer(read_cache_value(path, L"cache", L"count", L"0"), previousCount);
    previousCount = std::clamp<std::int64_t>(previousCount, 0, static_cast<std::int64_t>(kMaxApps));
    for (std::int64_t i = 0; i < previousCount; ++i) {
        const std::wstring section = L"app." + std::to_wstring(i);
        WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, path.c_str());
    }

    const auto write = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
    };
    write(L"cache", L"version", L"1");
    write(L"cache", L"count", std::to_wstring(appCache_.size()));
    for (std::size_t i = 0; i < appCache_.size() && i < kMaxApps; ++i) {
        const std::wstring section = L"app." + std::to_wstring(i);
        const auto& entry = appCache_[i];
        write(section.c_str(), L"label", entry.app.label);
        write(section.c_str(), L"path", entry.app.path);
        write(section.c_str(), L"identity", entry.app.identity);
        write(section.c_str(), L"modified", std::to_wstring(entry.modified));
    }
}

void ShortcutEditor::populate_apps() {
    if (!appList_) return;
    std::wstring query = text_of(search_);
    ListView_DeleteAllItems(appList_);
    for (std::size_t index = 0; index < apps_.size(); ++index) {
        const auto& app = apps_[index];
        if (!visible_match(query, app)) continue;
        LVITEMW item{LVIF_TEXT | LVIF_PARAM};
        item.iItem = ListView_GetItemCount(appList_);
        item.pszText = const_cast<LPWSTR>(app.label.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(appList_, &item);
    }
}

void ShortcutEditor::populate_commands() {
    if (!commandList_) return;
    ListView_DeleteAllItems(commandList_);
    for (std::size_t index = 0; index < settings_.commandShortcuts.size(); ++index) {
        const auto& command = settings_.commandShortcuts[index];
        if (!command.enabled || command.label.empty() || command.target.empty()) continue;
        LVITEMW item{LVIF_TEXT | LVIF_PARAM};
        item.iItem = ListView_GetItemCount(commandList_);
        item.pszText = const_cast<LPWSTR>(command.label.c_str());
        item.lParam = static_cast<LPARAM>(index);
        ListView_InsertItem(commandList_, &item);
    }
}

void ShortcutEditor::update_app_check(int item, bool checked) {
    const int index = item_data(appList_, item);
    if (index < 0 || index >= static_cast<int>(apps_.size())) return;
    const auto& app = apps_[static_cast<std::size_t>(index)];
    const auto existing = std::ranges::find_if(settings_.appShortcuts, [&](const ShortcutSetting& shortcut) {
        return shortcut.enabled && same_path(shortcut.target, app.path);
    });
    if (checked) {
        if (existing != settings_.appShortcuts.end()) return;
        const auto free = std::ranges::find_if(settings_.appShortcuts, [](const ShortcutSetting& shortcut) {
            return !shortcut.enabled || shortcut.target.empty();
        });
        if (free == settings_.appShortcuts.end()) {
            MessageBoxW(hwnd_, L"The Apps widget can contain up to four apps.", L"Apps", MB_OK | MB_ICONINFORMATION);
            return;
        }
        *free = {app.label, app.path, L"", L"\uE8A7", true};
    } else {
        for (auto& shortcut : settings_.appShortcuts) {
            if (shortcut.enabled && same_path(shortcut.target, app.path)) shortcut = {};
        }
    }
    save_changed();
}

void ShortcutEditor::add_command() {
    const std::wstring target = text_of(commandTarget_);
    if (target.empty()) {
        MessageBoxW(hwnd_, L"Enter a command or application path first.", L"Command", MB_OK | MB_ICONINFORMATION);
        SetFocus(commandTarget_);
        return;
    }
    const auto free = std::ranges::find_if(settings_.commandShortcuts, [](const ShortcutSetting& shortcut) {
        return !shortcut.enabled || shortcut.target.empty();
    });
    if (free == settings_.commandShortcuts.end()) {
        MessageBoxW(hwnd_, L"The Commands widget can contain up to four commands.", L"Command", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring label = text_of(commandLabel_);
    if (label.empty()) label = std::filesystem::path(target).stem().wstring();
    *free = {std::move(label), target, text_of(commandArguments_), L"\uE768", true};
    save_changed();
    populate_commands();
    set_text(commandLabel_, L"");
    set_text(commandTarget_, L"");
    set_text(commandArguments_, L"");
    SetFocus(commandLabel_);
}

void ShortcutEditor::remove_command() {
    const int row = ListView_GetNextItem(commandList_, -1, LVNI_SELECTED);
    if (row < 0) return;
    const int index = item_data(commandList_, row);
    if (index < 0 || index >= static_cast<int>(settings_.commandShortcuts.size())) return;
    settings_.commandShortcuts[static_cast<std::size_t>(index)] = {};
    save_changed();
    populate_commands();
    set_text(commandLabel_, L"");
    set_text(commandTarget_, L"");
    set_text(commandArguments_, L"");
}

void ShortcutEditor::load_selected_command() {
    const int row = ListView_GetNextItem(commandList_, -1, LVNI_SELECTED);
    if (row < 0) return;
    const int index = item_data(commandList_, row);
    if (index < 0 || index >= static_cast<int>(settings_.commandShortcuts.size())) return;
    const auto& command = settings_.commandShortcuts[static_cast<std::size_t>(index)];
    set_text(commandLabel_, command.label);
    set_text(commandTarget_, command.target);
    set_text(commandArguments_, command.arguments);
}

void ShortcutEditor::browse_target() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(hwnd_))) return;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return;
    PWSTR raw = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) {
        set_text(commandTarget_, raw);
        CoTaskMemFree(raw);
    }
}

void ShortcutEditor::save_changed() {
    settings_.save();
    if (changed_) changed_();
}

void ShortcutEditor::destroy_resources() {
    if (appImages_) {
        ImageList_Destroy(appImages_);
        appImages_ = nullptr;
    }
    if (rowHeightImages_) {
        ImageList_Destroy(rowHeightImages_);
        rowHeightImages_ = nullptr;
    }
    for (auto& cached : appCache_) cached.app.imageIndex = -1;
    for (auto& app : apps_) app.imageIndex = -1;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (titleFont_) {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (captionFont_) {
        DeleteObject(captionFont_);
        captionFont_ = nullptr;
    }
    if (backgroundBrush_) {
        DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
    if (fieldBrush_) {
        DeleteObject(fieldBrush_);
        fieldBrush_ = nullptr;
    }
    appImageCacheValid_ = false;
    appsTab_ = commandsTab_ = search_ = refreshAppsButton_ = appList_ = appHint_ = nullptr;
    commandLabelCaption_ = commandLabel_ = commandTargetCaption_ = commandTarget_ = nullptr;
    commandArgumentsCaption_ = commandArguments_ = browseButton_ = addCommandButton_ = nullptr;
    commandList_ = removeCommandButton_ = nullptr;
    SetRectEmpty(&embeddedBounds_);
    embeddedRadius_ = 0;
}

bool ShortcutEditor::same_path(std::wstring_view left, std::wstring_view right) noexcept {
    return !left.empty() && !right.empty() &&
           CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring ShortcutEditor::lower(std::wstring value) {
    std::ranges::transform(value, value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

} // namespace isle
