#include "ShortcutEditor.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

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
constexpr int kAddApp = 1103;
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

std::int64_t file_stamp(const std::filesystem::path& path) {
    std::error_code ec;
    const auto value = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(value.time_since_epoch().count());
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
        start_app_discovery();
        switch_page(commandsPage_);
        ShowWindow(hwnd_, SW_SHOW);
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

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Shortcuts", WS_CHILD | WS_CLIPCHILDREN,
                            bounds.left, bounds.top, bounds.right - bounds.left,
                            bounds.bottom - bounds.top, owner_, nullptr, instance_, this);
    if (!hwnd_) return;

    resize_embedded(bounds, radius);
    switch_page(false);
    start_app_discovery();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void ShortcutEditor::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void ShortcutEditor::resize_embedded(RECT bounds, int radius) {
    if (!hwnd_) return;
    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    if (EqualRect(&bounds, &embeddedBounds_) && radius == embeddedRadius_) {
        layout_controls();
        return;
    }
    embeddedBounds_ = bounds;
    embeddedRadius_ = radius;
    SetWindowPos(hwnd_, nullptr, bounds.left, bounds.top, width, height,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                     std::max(0, radius * 2), std::max(0, radius * 2));
    if (region && SetWindowRgn(hwnd_, region, TRUE) == 0) DeleteObject(region);
    layout_controls();
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
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(224, 224, 230));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(0, 0, 0));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(244, 244, 245));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(31, 31, 35));
            return reinterpret_cast<LRESULT>(fieldBrush_);
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == kAppsTab && notification == BN_CLICKED) switch_page(false);
            else if (id == kCommandsTab && notification == BN_CLICKED) switch_page(true);
            else if (id == kAppSearch && notification == EN_CHANGE) populate_apps();
            else if (id == kAddApp && notification == BN_CLICKED) add_selected_app();
            else if (id == kBrowseTarget && notification == BN_CLICKED) browse_target();
            else if (id == kAddCommand && notification == BN_CLICKED) add_command();
            else if (id == kRemoveCommand && notification == BN_CLICKED) remove_command();
            return 0;
        }
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (!header) return 0;
            if (header->code == LVN_ITEMCHANGED && header->hwndFrom == appList_) {
                const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
                if ((change->uChanged & LVIF_STATE) != 0 &&
                    ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) != 0) {
                    update_app_check(change->iItem, ListView_GetCheckState(appList_, change->iItem) != FALSE);
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
                    draw->clrText = RGB(238, 238, 242);
                    draw->clrTextBk = RGB(24, 24, 27);
                    return CDRF_NEWFONT;
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
    fieldBrush_ = CreateSolidBrush(RGB(31, 31, 35));
    font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    appsTab_ = CreateWindowExW(0, WC_BUTTONW, L"Apps", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                               0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppsTab)), instance_, nullptr);
    commandsTab_ = CreateWindowExW(0, WC_BUTTONW, L"Commands", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                   0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandsTab)), instance_, nullptr);

    search_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                              0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppSearch)), instance_, nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search installed apps"));
    appList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                               0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(appList_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
    column.pszText = const_cast<LPWSTR>(L"Installed applications");
    column.cx = 320;
    ListView_InsertColumn(appList_, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"Location");
    column.cx = 360;
    ListView_InsertColumn(appList_, 1, &column);
    appHint_ = CreateWindowExW(0, WC_STATICW,
                               L"Check an app to show it.",
                               WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    addAppButton_ = CreateWindowExW(0, WC_BUTTONW, L"Add selected app",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                    0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddApp)), instance_, nullptr);

    commandLabelCaption_ = CreateWindowExW(0, WC_STATICW, L"Name", WS_CHILD,
                                           0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandLabel_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                    0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandLabel)), instance_, nullptr);
    commandTargetCaption_ = CreateWindowExW(0, WC_STATICW, L"Target", WS_CHILD,
                                            0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandTarget_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                     0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandTarget)), instance_, nullptr);
    browseButton_ = CreateWindowExW(0, WC_BUTTONW, L"Browse…", WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
                                    0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowseTarget)), instance_, nullptr);
    commandArgumentsCaption_ = CreateWindowExW(0, WC_STATICW, L"Arguments", WS_CHILD,
                                               0, 0, 1, 1, hwnd_, nullptr, instance_, nullptr);
    commandArguments_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                        0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandArguments)), instance_, nullptr);
    addCommandButton_ = CreateWindowExW(0, WC_BUTTONW, L"Add command",
                                        WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 1, 1,
                                        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddCommand)), instance_, nullptr);
    commandList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                                   0, 0, 1, 1, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(commandList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    column.pszText = const_cast<LPWSTR>(L"Name");
    column.cx = 220;
    ListView_InsertColumn(commandList_, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"Target");
    column.cx = 460;
    ListView_InsertColumn(commandList_, 1, &column);
    removeCommandButton_ = CreateWindowExW(0, WC_BUTTONW, L"Remove selected",
                                           WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 1, 1,
                                           hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveCommand)), instance_, nullptr);

    const std::array<HWND, 16> controls{
        appsTab_, commandsTab_, search_, appList_, appHint_, addAppButton_,
        commandLabelCaption_, commandLabel_, commandTargetCaption_, commandTarget_, browseButton_,
        commandArgumentsCaption_, commandArguments_, addCommandButton_, commandList_, removeCommandButton_};
    for (HWND control : controls) apply_font(control, font_);

    appImages_ = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 64, 32);
    ListView_SetImageList(appList_, appImages_, LVSIL_SMALL);
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
    const int buttonHeight = px(28.0f);
    const int tabsWidth = px(92.0f);

    MoveWindow(appsTab_, pad, px(4.0f), tabsWidth, buttonHeight, TRUE);
    MoveWindow(commandsTab_, pad + tabsWidth + gap, px(4.0f), px(112.0f), buttonHeight, TRUE);

    MoveWindow(search_, pad, px(40.0f), width - pad * 2, px(30.0f), TRUE);
    MoveWindow(appList_, pad, px(78.0f), width - pad * 2,
               std::max(px(170.0f), height - px(136.0f)), TRUE);
    ListView_SetColumnWidth(appList_, 0, std::max(1, (width - pad * 2) * 3 / 5));
    ListView_SetColumnWidth(appList_, 1, std::max(1, (width - pad * 2) * 2 / 5));
    MoveWindow(appHint_, pad, height - px(45.0f), width - px(156.0f), px(24.0f), TRUE);
    MoveWindow(addAppButton_, width - px(146.0f), height - px(48.0f), px(136.0f), px(32.0f), TRUE);

    const int labelWidth = px(72.0f);
    const int fieldLeft = pad + labelWidth;
    MoveWindow(commandLabelCaption_, pad, px(43.0f), labelWidth, px(22.0f), TRUE);
    MoveWindow(commandLabel_, fieldLeft, px(39.0f), width - fieldLeft - pad, px(30.0f), TRUE);
    MoveWindow(commandTargetCaption_, pad, px(80.0f), labelWidth, px(22.0f), TRUE);
    MoveWindow(commandTarget_, fieldLeft, px(76.0f), width - fieldLeft - px(78.0f), px(30.0f), TRUE);
    MoveWindow(browseButton_, width - px(72.0f), px(76.0f), px(62.0f), px(30.0f), TRUE);
    MoveWindow(commandArgumentsCaption_, pad, px(117.0f), labelWidth, px(22.0f), TRUE);
    MoveWindow(commandArguments_, fieldLeft, px(113.0f), width - fieldLeft - pad, px(30.0f), TRUE);
    MoveWindow(addCommandButton_, width - px(122.0f), px(151.0f), px(112.0f), buttonHeight, TRUE);
    MoveWindow(commandList_, pad, px(187.0f), width - pad * 2,
               std::max(px(120.0f), height - px(234.0f)), TRUE);
    ListView_SetColumnWidth(commandList_, 0, std::max(1, (width - pad * 2) * 2 / 5));
    ListView_SetColumnWidth(commandList_, 1, std::max(1, (width - pad * 2) * 3 / 5));
    MoveWindow(removeCommandButton_, width - px(136.0f), height - px(43.0f), px(126.0f), px(32.0f), TRUE);
}

void ShortcutEditor::switch_page(bool commands) {
    commandsPage_ = commands;
    layout_controls();
    show_control(search_, !commands);
    show_control(appList_, !commands);
    show_control(appHint_, !commands);
    show_control(addAppButton_, !commands);
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
        for (const auto& entry : appCache_) apps_.push_back(entry.app);
        std::ranges::sort(apps_, [](const InstalledApp& left, const InstalledApp& right) {
            return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
        });
    }
    populate_apps();
    set_text(appHint_, apps_.empty() ? L"Loading installed apps…" : L"Refreshing installed apps…");

    for (const KNOWNFOLDERID folder : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw))) {
            appRoots_.emplace_back(raw);
            CoTaskMemFree(raw);
        }
    }
    SetTimer(hwnd_, kAppLoadTimer, 16, nullptr);
}

void ShortcutEditor::load_app_batch() {
    if (!appLoading_) return;
    const std::filesystem::recursive_directory_iterator end;
    int processed = 0;
    while (processed < kAppLoadBatchSize && appSeen_.size() < kMaxApps) {
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
        std::error_code iteratorError;
        appIterator_->increment(iteratorError);
        if (iteratorError || *appIterator_ == end) {
            appIterator_.reset();
            ++appRootIndex_;
        }
        ++processed;
        if (regular) append_app(path);
    }

    if (appSeen_.size() >= kMaxApps) {
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
    set_text(appHint_, L"Check an app to show it.");
    if (!commandsPage_) populate_apps();
}

bool ShortcutEditor::append_app(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    if (extension != L".lnk" && extension != L".url" && extension != L".exe") return false;

    const std::wstring appPath = path.wstring();
    const std::wstring key = lower(appPath);
    if (std::ranges::find(appSeen_, key) != appSeen_.end()) return false;
    appSeen_.push_back(key);

    const std::int64_t modified = file_stamp(path);
    // ponytail: linear lookup is bounded by the 500-item picker; use a map if that cap grows.
    const auto cached = std::ranges::find_if(appCache_, [&](const CachedApp& entry) {
        return entry.modified == modified && same_path(entry.app.path, appPath);
    });

    InstalledApp app;
    if (cached != appCache_.end() && appImageCacheValid_) {
        app = cached->app;
        app.path = appPath;
    } else {
        app.path = appPath;
        app.label = path.stem().wstring();
        SHFILEINFOW info{};
        if (SHGetFileInfoW(app.path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                           SHGFI_ICON | SHGFI_SMALLICON)) {
            if (appImages_) app.imageIndex = ImageList_AddIcon(appImages_, info.hIcon);
            DestroyIcon(info.hIcon);
        }
    }
    if (app.label.empty()) return false;
    nextAppCache_.push_back({app, modified});
    const auto existing = std::ranges::find_if(apps_, [&](const InstalledApp& item) {
        return same_path(item.path, app.path);
    });
    if (existing != apps_.end()) {
        *existing = std::move(app);
    } else {
        apps_.push_back(std::move(app));
    }
    return true;
}

void ShortcutEditor::populate_apps() {
    if (!appList_) return;
    std::wstring query = text_of(search_);
    refreshing_ = true;
    ListView_DeleteAllItems(appList_);
    for (std::size_t index = 0; index < apps_.size(); ++index) {
        const auto& app = apps_[index];
        if (!visible_match(query, app)) continue;
        LVITEMW item{LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM};
        item.iItem = ListView_GetItemCount(appList_);
        item.pszText = const_cast<LPWSTR>(app.label.c_str());
        item.iImage = app.imageIndex;
        item.lParam = static_cast<LPARAM>(index);
        const int row = ListView_InsertItem(appList_, &item);
        ListView_SetItemText(appList_, row, 1, const_cast<LPWSTR>(app.path.c_str()));
        const bool checked = std::ranges::any_of(settings_.appShortcuts, [&](const ShortcutSetting& shortcut) {
            return shortcut.enabled && same_path(shortcut.target, app.path);
        });
        ListView_SetCheckState(appList_, row, checked ? TRUE : FALSE);
    }
    refreshing_ = false;
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
        const int row = ListView_InsertItem(commandList_, &item);
        ListView_SetItemText(commandList_, row, 1, const_cast<LPWSTR>(command.target.c_str()));
    }
}

void ShortcutEditor::add_selected_app() {
    const int row = ListView_GetNextItem(appList_, -1, LVNI_SELECTED);
    if (row < 0) return;
    if (!ListView_GetCheckState(appList_, row)) {
        ListView_SetCheckState(appList_, row, TRUE);
    } else {
        update_app_check(row, true);
    }
}

void ShortcutEditor::update_app_check(int item, bool checked) {
    if (refreshing_) return;
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
            refreshing_ = true;
            ListView_SetCheckState(appList_, item, FALSE);
            refreshing_ = false;
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
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
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
    appsTab_ = commandsTab_ = search_ = appList_ = appHint_ = addAppButton_ = nullptr;
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
