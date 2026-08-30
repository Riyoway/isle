#include "Settings.h"

#include <ShlObj.h>
#include <Windows.h>

#include <array>
#include <fstream>
#include <string>

namespace isle {

namespace {
std::wstring read_ini(const wchar_t* section, const wchar_t* key, const wchar_t* fallback, const std::filesystem::path& path) {
    std::array<wchar_t, 128> buffer{};
    GetPrivateProfileStringW(section, key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

bool parse_bool(const std::wstring& value, bool fallback) {
    if (value == L"1" || value == L"true" || value == L"yes") return true;
    if (value == L"0" || value == L"false" || value == L"no") return false;
    return fallback;
}
} // namespace

std::filesystem::path Settings::data_directory() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
        return std::filesystem::current_path() / L"IsleData";
    }
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    result /= L"Isle";
    return result;
}

std::filesystem::path Settings::file_path() {
    return data_directory() / L"settings.ini";
}

Settings Settings::load() {
    Settings s;
    const auto path = file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    try {
        s.topOffsetDip = std::stoi(read_ini(L"island", L"topOffsetDip", L"8", path));
    } catch (...) {}
    s.startWithWindows = parse_bool(read_ini(L"general", L"startWithWindows", L"0", path), false);
    s.hideInFullscreen = parse_bool(read_ini(L"island", L"hideInFullscreen", L"1", path), true);
    s.expandOnHover = parse_bool(read_ini(L"island", L"expandOnHover", L"0", path), false);
    s.showSeconds = parse_bool(read_ini(L"clock", L"showSeconds", L"0", path), false);
    s.monitorAtCursor = parse_bool(read_ini(L"island", L"monitorAtCursor", L"0", path), false);
    return s;
}

void Settings::save() const {
    const auto path = file_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const auto write = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
    };
    write(L"island", L"topOffsetDip", std::to_wstring(topOffsetDip));
    write(L"general", L"startWithWindows", startWithWindows ? L"1" : L"0");
    write(L"island", L"hideInFullscreen", hideInFullscreen ? L"1" : L"0");
    write(L"island", L"expandOnHover", expandOnHover ? L"1" : L"0");
    write(L"clock", L"showSeconds", showSeconds ? L"1" : L"0");
    write(L"island", L"monitorAtCursor", monitorAtCursor ? L"1" : L"0");
}

} // namespace isle
