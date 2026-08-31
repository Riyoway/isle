#include "ShortcutEditor.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
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

} // namespace

ShortcutEditor::~ShortcutEditor() {
    if (hwnd_) DestroyWindow(hwnd_);
    destroy_resources();
}

void ShortcutEditor::show(HWND owner, HINSTANCE instance, std::function<void()> changed) {
    owner_ = owner;
    instance_ = instance;
    changed_ = std::move(changed);
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
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

    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    const int width = 760;
    const int height = 640;
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Shortcuts", 
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            x, y, width, height, owner_, nullptr, instance_, this);
    if (!hwnd_) return;

    apps_ = discover_apps();
    switch_page(false);

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
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
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(224, 224, 230));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(18, 18, 20));
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
            DestroyWindow(hwnd_);
            return 0;
        case WM_NCDESTROY:
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
    backgroundBrush_ = CreateSolidBrush(RGB(18, 18, 20));
    fieldBrush_ = CreateSolidBrush(RGB(31, 31, 35));
    font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    titleFont_ = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");

    title_ = CreateWindowExW(0, WC_STATICW, L"Shortcuts", WS_CHILD | WS_VISIBLE,
                             24, 18, 700, 34, hwnd_, nullptr, instance_, nullptr);
    subtitle_ = CreateWindowExW(0, WC_STATICW, L"Add apps and commands to your widgets",
                                WS_CHILD | WS_VISIBLE, 26, 52, 700, 22, hwnd_, nullptr, instance_, nullptr);
    appsTab_ = CreateWindowExW(0, WC_BUTTONW, L"Apps", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               24, 88, 110, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppsTab)), instance_, nullptr);
    commandsTab_ = CreateWindowExW(0, WC_BUTTONW, L"Commands", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   140, 88, 110, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandsTab)), instance_, nullptr);

    search_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              24, 132, 700, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppSearch)), instance_, nullptr);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search installed apps"));
    appList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                               24, 174, 700, 330, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAppList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(appList_, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
    column.pszText = const_cast<LPWSTR>(L"Installed applications");
    column.cx = 320;
    ListView_InsertColumn(appList_, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"Location");
    column.cx = 360;
    ListView_InsertColumn(appList_, 1, &column);
    appHint_ = CreateWindowExW(0, WC_STATICW,
                               L"Check an app to add it to the Apps widget. Up to four apps can be shown.",
                               WS_CHILD | WS_VISIBLE, 26, 516, 560, 24, hwnd_, nullptr, instance_, nullptr);
    addAppButton_ = CreateWindowExW(0, WC_BUTTONW, L"Add selected app",
                                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    574, 510, 150, 34, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddApp)), instance_, nullptr);

    commandLabelCaption_ = CreateWindowExW(0, WC_STATICW, L"Name", WS_CHILD,
                                           24, 132, 80, 22, hwnd_, nullptr, instance_, nullptr);
    commandLabel_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                    106, 128, 250, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandLabel)), instance_, nullptr);
    commandTargetCaption_ = CreateWindowExW(0, WC_STATICW, L"Target", WS_CHILD,
                                            24, 172, 80, 22, hwnd_, nullptr, instance_, nullptr);
    commandTarget_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                     106, 168, 500, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandTarget)), instance_, nullptr);
    browseButton_ = CreateWindowExW(0, WC_BUTTONW, L"Browse…", WS_CHILD | BS_PUSHBUTTON,
                                    614, 168, 110, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowseTarget)), instance_, nullptr);
    commandArgumentsCaption_ = CreateWindowExW(0, WC_STATICW, L"Arguments", WS_CHILD,
                                               24, 212, 80, 22, hwnd_, nullptr, instance_, nullptr);
    commandArguments_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                                        106, 208, 618, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandArguments)), instance_, nullptr);
    addCommandButton_ = CreateWindowExW(0, WC_BUTTONW, L"Add command",
                                        WS_CHILD | BS_PUSHBUTTON, 544, 252, 180, 34,
                                        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddCommand)), instance_, nullptr);
    commandList_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                   WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                   24, 302, 700, 202, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCommandList)), instance_, nullptr);
    ListView_SetExtendedListViewStyle(commandList_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    column.pszText = const_cast<LPWSTR>(L"Name");
    column.cx = 220;
    ListView_InsertColumn(commandList_, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"Target");
    column.cx = 460;
    ListView_InsertColumn(commandList_, 1, &column);
    removeCommandButton_ = CreateWindowExW(0, WC_BUTTONW, L"Remove selected",
                                           WS_CHILD | BS_PUSHBUTTON, 574, 516, 150, 34,
                                           hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveCommand)), instance_, nullptr);

    const std::array<HWND, 18> controls{
        title_, subtitle_, appsTab_, commandsTab_, search_, appList_, appHint_, addAppButton_,
        commandLabelCaption_, commandLabel_, commandTargetCaption_, commandTarget_, browseButton_,
        commandArgumentsCaption_, commandArguments_, addCommandButton_, commandList_, removeCommandButton_};
    for (HWND control : controls) apply_font(control, font_);
    apply_font(title_, titleFont_);

    appImages_ = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 64, 32);
    ListView_SetImageList(appList_, appImages_, LVSIL_SMALL);
}

void ShortcutEditor::switch_page(bool commands) {
    commandsPage_ = commands;
    set_text(subtitle_, commands ? L"Create commands with a target and optional arguments"
                                 : L"Search installed apps and choose the ones to show");
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
    if (titleFont_) {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (backgroundBrush_) {
        DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
    if (fieldBrush_) {
        DeleteObject(fieldBrush_);
        fieldBrush_ = nullptr;
    }
    title_ = subtitle_ = appsTab_ = commandsTab_ = search_ = appList_ = appHint_ = addAppButton_ = nullptr;
    commandLabelCaption_ = commandLabel_ = commandTargetCaption_ = commandTarget_ = nullptr;
    commandArgumentsCaption_ = commandArguments_ = browseButton_ = addCommandButton_ = nullptr;
    commandList_ = removeCommandButton_ = nullptr;
}

std::vector<ShortcutEditor::InstalledApp> ShortcutEditor::discover_apps() {
    std::vector<std::filesystem::path> roots;
    for (const KNOWNFOLDERID folder : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw))) {
            roots.emplace_back(raw);
            CoTaskMemFree(raw);
        }
    }

    std::vector<InstalledApp> result;
    std::vector<std::wstring> seen;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec)) continue;
        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end && result.size() < 500; iterator.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!iterator->is_regular_file(ec)) continue;
            const auto path = iterator->path();
            std::wstring extension = path.extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](wchar_t ch) {
                return static_cast<wchar_t>(std::towlower(ch));
            });
            if (extension != L".lnk" && extension != L".url" && extension != L".exe") continue;
            const std::wstring key = lower(path.wstring());
            if (std::ranges::find(seen, key) != seen.end()) continue;
            seen.push_back(key);

            InstalledApp app;
            app.path = path.wstring();
            app.label = path.stem().wstring();
            SHFILEINFOW info{};
            if (SHGetFileInfoW(app.path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                               SHGFI_ICON | SHGFI_SMALLICON)) {
                if (appImages_) {
                    app.imageIndex = ImageList_AddIcon(appImages_, info.hIcon);
                }
                DestroyIcon(info.hIcon);
            }
            if (!app.label.empty()) result.push_back(std::move(app));
        }
    }
    std::ranges::sort(result, [](const InstalledApp& left, const InstalledApp& right) {
        return _wcsicmp(left.label.c_str(), right.label.c_str()) < 0;
    });
    return result;
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
