#include "AIUsageProvider.h"

#include "../core/AIProviders.h"
#include "../core/Settings.h"

#include <ShlObj.h>
#include <Windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace isle {

using namespace winrt;
using namespace Windows::Data::Json;

namespace {
struct UsageWindow {
    double usedPercent{};
    std::int64_t resetsAt{};
};

struct CodexUsage {
    UsageWindow primary;
    std::optional<UsageWindow> secondary;
};

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::filesystem::path codex_home() {
    std::array<wchar_t, 32768> configured{};
    const DWORD length = GetEnvironmentVariableW(L"CODEX_HOME", configured.data(),
                                                  static_cast<DWORD>(configured.size()));
    if (length > 0 && length < configured.size()) return configured.data();
    return known_folder(FOLDERID_Profile) / L".codex";
}

std::filesystem::path newest_session_file() {
    const auto root = codex_home() / L"sessions";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return {};

    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != L".jsonl") continue;
        const auto modified = it->last_write_time(ec);
        if (!ec && (newest.empty() || modified > newestTime)) {
            newest = it->path();
            newestTime = modified;
        }
    }
    return newest;
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::optional<std::string> read_file(const std::filesystem::path& path, std::size_t maxBytes = 4 * 1024 * 1024) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto length = input.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > maxBytes) return std::nullopt;
    input.seekg(0);
    std::string data(static_cast<std::size_t>(length), '\0');
    input.read(data.data(), length);
    data.resize(static_cast<std::size_t>(input.gcount()));
    return data;
}

std::optional<CodexUsage> read_latest_codex_usage(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto length = input.tellg();
    if (length <= 0) return std::nullopt;

    constexpr std::streamoff maxTail = 1024 * 1024;
    const std::streamoff bytes = std::min<std::streamoff>(length, maxTail);
    input.seekg(length - bytes);
    std::string tail(static_cast<std::size_t>(bytes), '\0');
    input.read(tail.data(), bytes);
    tail.resize(static_cast<std::size_t>(input.gcount()));

    std::size_t match = tail.rfind("\"rate_limits\"");
    while (match != std::string::npos) {
        const auto lineStartMarker = tail.rfind('\n', match);
        const std::size_t lineStart = lineStartMarker == std::string::npos ? 0 : lineStartMarker + 1;
        const auto lineEndMarker = tail.find('\n', match);
        const std::size_t lineEnd = lineEndMarker == std::string::npos ? tail.size() : lineEndMarker;
        try {
            const auto root = JsonObject::Parse(hstring(utf8_to_wide(
                std::string_view(tail).substr(lineStart, lineEnd - lineStart))));
            const auto limits = root.GetNamedObject(L"payload").GetNamedObject(L"rate_limits");
            const auto primary = limits.GetNamedObject(L"primary");
            CodexUsage usage;
            usage.primary.usedPercent = std::clamp(primary.GetNamedNumber(L"used_percent"), 0.0, 100.0);
            usage.primary.resetsAt = static_cast<std::int64_t>(primary.GetNamedNumber(L"resets_at", 0));
            if (limits.HasKey(L"secondary") && limits.GetNamedValue(L"secondary").ValueType() == JsonValueType::Object) {
                const auto secondary = limits.GetNamedObject(L"secondary");
                usage.secondary = UsageWindow{
                    std::clamp(secondary.GetNamedNumber(L"used_percent"), 0.0, 100.0),
                    static_cast<std::int64_t>(secondary.GetNamedNumber(L"resets_at", 0)),
                };
            }
            return usage;
        } catch (...) {
            if (lineStart == 0) break;
            match = tail.rfind("\"rate_limits\"", lineStart - 1);
        }
    }
    return std::nullopt;
}

std::wstring reset_text(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) return L"Live quota";
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto seconds = std::max<std::int64_t>(0, epochSeconds - static_cast<std::int64_t>(now));
    const auto minutes = (seconds + 59) / 60;
    if (minutes < 60) return L"Resets in " + std::to_wstring(minutes) + L"m";
    const auto hours = (minutes + 59) / 60;
    if (hours < 48) return L"Resets in " + std::to_wstring(hours) + L"h";
    return L"Resets in " + std::to_wstring((hours + 23) / 24) + L"d";
}

std::wstring window_title(std::wstring_view fallback, const JsonObject& window) {
    const auto minutes = static_cast<int>(window.GetNamedNumber(L"window_minutes", 0));
    if (minutes == 300) return L"5 hours";
    if (minutes == 1440) return L"Daily";
    if (minutes == 10080) return L"Weekly";
    if (minutes >= 40320 && minutes <= 44640) return L"Monthly";
    return std::wstring(fallback);
}

Activity usage_activity(std::wstring_view provider, std::wstring_view key, std::wstring title,
                        double usedPercent, std::wstring subtitle, const Settings& settings) {
    const int providerIndex = ai_provider_index(provider);
    const auto& info = providerIndex >= 0 ? kAIProviders[static_cast<std::size_t>(providerIndex)] : kAIProviders[0];
    Activity activity;
    activity.id = L"ai." + std::wstring(provider) + L"." + std::wstring(key);
    activity.source = L"ai." + std::wstring(provider);
    activity.kind = ActivityKind::Metric;
    activity.title = std::move(title);
    activity.subtitle = subtitle.empty() ? L"Live quota" : std::move(subtitle);
    activity.glyph = info.mark;
    activity.accent = providerIndex >= 0 ? settings.aiColors[static_cast<std::size_t>(providerIndex)]
                                         : std::wstring(info.color);
    activity.value = std::clamp(usedPercent, 0.0, 100.0);
    activity.valueSuffix = L"%";
    activity.progress = *activity.value / 100.0;
    const int windowPriority = key == L"primary" ? 4 : key == L"secondary" ? 3 : key == L"model_specific" ? 2 : 1;
    activity.priority = 320 - std::max(providerIndex, 0) + windowPriority;
    activity.pinned = true;
    activity.compactRing = providerIndex >= 0 && settings.aiRings[static_cast<std::size_t>(providerIndex)];
    activity.actions = {{L"refresh", L"Refresh", L"\uE72C"}};
    return activity;
}

Activity status_activity(std::wstring_view provider, std::wstring message, const Settings& settings) {
    const int providerIndex = ai_provider_index(provider);
    const auto& info = providerIndex >= 0 ? kAIProviders[static_cast<std::size_t>(providerIndex)] : kAIProviders[0];
    if (message.size() > 96) message.resize(96);
    Activity activity;
    activity.id = L"ai." + std::wstring(provider) + L".status";
    activity.source = L"ai." + std::wstring(provider);
    activity.kind = ActivityKind::Status;
    activity.title = std::wstring(info.name);
    activity.subtitle = message.empty() ? L"Not connected" : std::move(message);
    activity.glyph = info.mark;
    activity.accent = providerIndex >= 0 ? settings.aiColors[static_cast<std::size_t>(providerIndex)]
                                         : std::wstring(info.color);
    activity.priority = 250 - std::max(providerIndex, 0);
    activity.actions = {{L"refresh", L"Refresh", L"\uE72C"}};
    return activity;
}

bool publish_window(ActivityStore& store, const Settings& settings, std::wstring_view provider,
                    std::wstring_view key, std::wstring_view fallbackTitle, const JsonObject& usage) {
    if (!usage.HasKey(key) || usage.GetNamedValue(key).ValueType() != JsonValueType::Object) return false;
    const auto window = usage.GetNamedObject(key);
    const double used = window.GetNamedNumber(L"used_percent", -1.0);
    if (used < 0.0) return false;
    std::wstring subtitle = window.GetNamedString(L"reset_description", L"").c_str();
    if (subtitle.empty()) subtitle = window.GetNamedString(L"resets_at", L"").c_str();
    store.upsert(usage_activity(provider, key, window_title(fallbackTitle, window), used,
                                std::move(subtitle), settings));
    return true;
}

bool publish_provider(ActivityStore& store, const Settings& settings, const JsonObject& item) {
    const std::wstring provider = item.GetNamedString(L"provider", L"").c_str();
    const int providerIndex = ai_provider_index(provider);
    if (provider.empty() || providerIndex < 0) return false;
    const std::wstring source = L"ai." + provider;
    store.remove_source(source);
    if (!settings.aiVisible[static_cast<std::size_t>(providerIndex)]) return false;
    if (item.HasKey(L"error")) {
        store.upsert(status_activity(provider, item.GetNamedString(L"error", L"Not connected").c_str(), settings));
        return true;
    }
    if (!item.HasKey(L"usage") || item.GetNamedValue(L"usage").ValueType() != JsonValueType::Object) return false;
    const auto usage = item.GetNamedObject(L"usage");
    bool added = publish_window(store, settings, provider, L"primary", L"Primary", usage);
    added = publish_window(store, settings, provider, L"secondary", L"Secondary", usage) || added;
    added = publish_window(store, settings, provider, L"model_specific", L"Model", usage) || added;
    added = publish_window(store, settings, provider, L"tertiary", L"Tertiary", usage) || added;
    return added;
}

bool publish_cli_json(ActivityStore& store, const Settings& settings, std::string_view json) {
    try {
        const auto results = JsonArray::Parse(hstring(utf8_to_wide(json)));
        bool added = false;
        for (const auto& value : results) {
            if (value.ValueType() == JsonValueType::Object) added = publish_provider(store, settings, value.GetObject()) || added;
        }
        return added;
    } catch (...) {
        return false;
    }
}

bool publish_widget_snapshot(ActivityStore& store, const Settings& settings) {
    const auto data = read_file(known_folder(FOLDERID_LocalAppData) / L"CodexBar" / L"widget-snapshot.json");
    if (!data) return false;
    try {
        const auto root = JsonObject::Parse(hstring(utf8_to_wide(*data)));
        const auto entries = root.GetNamedArray(L"entries");
        bool added = false;
        for (const auto& value : entries) {
            if (value.ValueType() != JsonValueType::Object) continue;
            const auto entry = value.GetObject();
            JsonObject wrapper;
            wrapper.Insert(L"provider", JsonValue::CreateStringValue(entry.GetNamedString(L"provider", L"")));
            wrapper.Insert(L"usage", entry);
            added = publish_provider(store, settings, wrapper) || added;
        }
        return added;
    } catch (...) {
        return false;
    }
}

std::filesystem::path resolve_bridge(const Settings& settings) {
    std::error_code ec;
    if (!settings.aiBridgePath.empty() && std::filesystem::is_regular_file(settings.aiBridgePath, ec)) {
        return settings.aiBridgePath;
    }
    std::array<wchar_t, 32768> found{};
    DWORD foundLength = SearchPathW(nullptr, L"codexbar-cli.exe", nullptr,
                                          static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (foundLength > 0 && foundLength < found.size()) return found.data();
    foundLength = SearchPathW(nullptr, L"codexbar.exe", nullptr,
                             static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (foundLength > 0 && foundLength < found.size()) return found.data();
    const auto profile = known_folder(FOLDERID_Profile);
    const std::array candidates{
        known_folder(FOLDERID_LocalAppData) / L"Programs" / L"CodexBar" / L"codexbar-cli.exe",
        known_folder(FOLDERID_ProgramFiles) / L"CodexBar" / L"codexbar.exe",
        profile / L"Developments" / L"public-oss-projects" / L"Win-CodexBar" /
            L"rust" / L"target" / L"release" / L"codexbar.exe",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::vector<std::wstring> enabled_bridge_providers(const Settings& settings) {
    std::vector<std::wstring> result;
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
        if (settings.aiVisible[i]) result.emplace_back(kAIProviders[i].id);
    }
    return result;
}

std::optional<std::string> run_bridge(const std::filesystem::path& executable,
                                      std::wstring_view provider, std::stop_token stopToken) {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) return std::nullopt;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe;
    startup.hStdError = nullError;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + executable.wstring() + L"\" usage --provider " +
                           std::wstring(provider) + L" --json --no-color --no-credits --web-timeout 15";
    const BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (nullError != INVALID_HANDLE_VALUE) CloseHandle(nullError);
    if (!started) {
        CloseHandle(readPipe);
        return std::nullopt;
    }

    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    std::array<char, 4096> buffer{};
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            if (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available)),
                         &read, nullptr) && read > 0 && output.size() + read <= 2 * 1024 * 1024) {
                output.append(buffer.data(), read);
            }
        }
        if (WaitForSingleObject(process.hProcess, 40) == WAIT_OBJECT_0) break;
    }
    if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 1);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
        if (output.size() + read <= 2 * 1024 * 1024) output.append(buffer.data(), read);
    }
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return output.empty() ? std::nullopt : std::optional<std::string>(std::move(output));
}
} // namespace

void AIUsageProvider::start(ActivityStore& store) {
    store_ = &store;
    lastUpdate_ = {};
    publish();
    bridgeThread_ = std::jthread([this](std::stop_token stopToken) { bridge_loop(stopToken); });
}

void AIUsageProvider::stop() {
    if (bridgeThread_.joinable()) {
        bridgeThread_.request_stop();
        bridgeThread_.join();
    }
    if (store_) {
        for (const auto& provider : kAIProviders) store_->remove_source(L"ai." + std::wstring(provider.id));
    }
    store_ = nullptr;
    hasUsage_ = false;
}

void AIUsageProvider::tick() {
    if (!store_) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastUpdate_.time_since_epoch().count() != 0 && now - lastUpdate_ < std::chrono::seconds(12)) return;
    publish();
}

void AIUsageProvider::invoke(std::wstring_view activityId, std::wstring_view actionId) {
    if (activityId.starts_with(L"ai.") && actionId == L"refresh") {
        publish();
        refreshBridge_ = true;
    }
}

void AIUsageProvider::publish() {
    if (!store_) return;
    lastUpdate_ = std::chrono::steady_clock::now();
    const Settings settings = Settings::load();
    for (std::size_t i = 0; i < kAIProviders.size(); ++i) {
        if (!settings.aiVisible[i]) store_->remove_source(L"ai." + std::wstring(kAIProviders[i].id));
    }
    const bool imported = publish_widget_snapshot(*store_, settings);
    const auto usage = read_latest_codex_usage(newest_session_file());
    if (usage && settings.aiVisible[0]) {
        hasUsage_ = true;
        store_->remove_source(L"ai.codex");
        store_->upsert(usage_activity(L"codex", L"primary", L"5 hours", usage->primary.usedPercent,
                                      reset_text(usage->primary.resetsAt), settings));
        if (usage->secondary) {
            store_->upsert(usage_activity(L"codex", L"secondary", L"Weekly", usage->secondary->usedPercent,
                                          reset_text(usage->secondary->resetsAt), settings));
        }
    } else if (settings.aiVisible[0] && !hasUsage_ && !imported) {
        Activity waiting;
        waiting.id = L"ai.codex.waiting";
        waiting.source = L"ai.codex";
        waiting.kind = ActivityKind::Status;
        waiting.title = L"Codex";
        waiting.subtitle = L"Waiting for usage data";
        waiting.glyph = kAIProviders[0].mark;
        waiting.accent = settings.aiColors[0];
        waiting.priority = 220;
        waiting.actions = {{L"refresh", L"Refresh", L"\uE72C"}};
        store_->upsert(std::move(waiting));
    }
}

void AIUsageProvider::bridge_loop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        refreshBridge_ = false;
        const Settings settings = Settings::load();
        const auto executable = resolve_bridge(settings);
        if (!executable.empty() && store_) {
            // ponytail: sequential refresh avoids credential/API bursts; parallelize only if many enabled providers make latency material.
            for (const auto& provider : enabled_bridge_providers(settings)) {
                if (stopToken.stop_requested() || !store_) break;
                if (const auto output = run_bridge(executable, provider, stopToken)) {
                    publish_cli_json(*store_, settings, *output);
                }
            }
        }
        for (int second = 0; second < 300 && !stopToken.stop_requested(); ++second) {
            if (refreshBridge_.exchange(false)) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

} // namespace isle
