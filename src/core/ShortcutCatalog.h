#pragma once

#include <algorithm>
#include <array>
#include <cwctype>
#include <string>
#include <string_view>

namespace isle {

inline std::wstring shortcut_catalog_lower(std::wstring_view value) {
    std::wstring result(value);
    std::ranges::transform(result, result.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return result;
}

inline bool shortcut_is_auxiliary(std::wstring_view label, std::wstring_view path) {
    const std::wstring name = shortcut_catalog_lower(label);
    const std::wstring location = shortcut_catalog_lower(path);
    constexpr std::array systemGroups{
        L"\\administrative tools\\", L"\\system tools\\",
        L"\\accessibility\\", L"\\startup\\"};
    if (std::ranges::any_of(systemGroups, [&](std::wstring_view group) {
            return location.find(group) != std::wstring::npos;
        })) return true;

    constexpr std::array auxiliaryTerms{
        L" changelog", L" eula", L" help", L" manuals", L" manual",
        L" documentation", L" module docs", L" readme", L" release notes",
        L" version history", L" website", L" on the web", L" user guide",
        L" user manual", L" tutorial", L" faqs", L" samples"};
    if (std::ranges::any_of(auxiliaryTerms, [&](std::wstring_view term) {
            return name.find(term) != std::wstring::npos;
        })) return true;

    return name == L"eula" || name == L"help" || name == L"manual" ||
           name == L"documentation" || name == L"readme" || name.starts_with(L"changelog") ||
           name == L"administrative tools" || name == L"system tools" ||
           name.starts_with(L"uninstall ") || name.ends_with(L" uninstall") ||
           name.find(L"アンインストール") != std::wstring::npos ||
           name.starts_with(L"license") || name.starts_with(L"properties ") ||
           name.starts_with(L"configure display language") || name.starts_with(L"visit website") ||
           name.starts_with(L"donate") || name.starts_with(L"reset settings") ||
           (name.find(L"check for ") != std::wstring::npos && name.find(L"update") != std::wstring::npos);
}

inline std::wstring shortcut_group_key(std::wstring_view label) {
    std::wstring key = shortcut_catalog_lower(label);
    while (!key.empty() && std::iswspace(key.back())) key.pop_back();
    if (key.ends_with(L")")) {
        const std::size_t open = key.rfind(L'(');
        if (open != std::wstring::npos) {
            const std::wstring_view suffix(key.data() + open, key.size() - open);
            constexpr std::array architectureTerms{
                L"32-bit", L"32bit", L"64-bit", L"64bit", L"x86", L"x64",
                L"arm64", L"sse", L"avx"};
            if (std::ranges::any_of(architectureTerms, [&](std::wstring_view term) {
                    return suffix.find(term) != std::wstring_view::npos;
                })) {
                key.erase(open);
                while (!key.empty() && std::iswspace(key.back())) key.pop_back();
            }
        }
    }
    return key;
}

} // namespace isle
