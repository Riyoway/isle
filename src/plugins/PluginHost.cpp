#include "PluginHost.h"

#include "../core/Settings.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

#include <array>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace isle {

using namespace winrt;
using namespace Windows::Data::Json;

namespace {
std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring read_all_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return utf8_to_wide(ss.str());
}

ActivityKind parse_kind(const std::wstring& value) {
    if (value == L"metric") return ActivityKind::Metric;
    if (value == L"media") return ActivityKind::Media;
    if (value == L"timer") return ActivityKind::Timer;
    if (value == L"text") return ActivityKind::Text;
    return ActivityKind::Status;
}

std::wstring quote_arg(const std::filesystem::path& path) {
    return L"\"" + path.wstring() + L"\"";
}
} // namespace

PluginHost::~PluginHost() {
    stop();
}

std::filesystem::path PluginHost::plugins_directory() const {
    return Settings::data_directory() / L"plugins";
}

void PluginHost::start(ActivityStore& store) {
    store_ = &store;
    std::error_code ec;
    std::filesystem::create_directories(plugins_directory(), ec);
    discover_and_launch();
}

void PluginHost::stop() {
    for (auto& plugin : processes_) {
        if (plugin->reader.joinable()) plugin->reader.request_stop();
        if (plugin->stdinWrite) {
            CloseHandle(plugin->stdinWrite);
            plugin->stdinWrite = nullptr;
        }
        if (plugin->stdoutRead) {
            CloseHandle(plugin->stdoutRead);
            plugin->stdoutRead = nullptr;
        }
        if (plugin->process) {
            if (WaitForSingleObject(plugin->process, 150) == WAIT_TIMEOUT) {
                TerminateProcess(plugin->process, 0);
            }
            CloseHandle(plugin->process);
            plugin->process = nullptr;
        }
        if (plugin->threadHandle) {
            CloseHandle(plugin->threadHandle);
            plugin->threadHandle = nullptr;
        }
        if (store_) store_->remove_source(plugin->id);
    }
    processes_.clear();
    store_ = nullptr;
}

void PluginHost::tick() {
    for (const auto& plugin : processes_) {
        if (!plugin->process) continue;
        const DWORD state = WaitForSingleObject(plugin->process, 0);
        if (state == WAIT_OBJECT_0 && plugin->healthy) {
            plugin->healthy = false;
            publish_plugin_status(*plugin, false, L"Plugin process exited");
        }
    }
}

void PluginHost::invoke(std::wstring_view activityId, std::wstring_view actionId) {
    // External activity ids are namespaced as "plugin-id:activity-id".
    const auto separator = activityId.find(L':');
    if (separator == std::wstring_view::npos) return;
    const std::wstring pluginId(activityId.substr(0, separator));
    const std::wstring localId(activityId.substr(separator + 1));

    for (auto& plugin : processes_) {
        if (plugin->id != pluginId || !plugin->healthy) continue;
        JsonObject message;
        message.Insert(L"type", JsonValue::CreateStringValue(L"action.invoke"));
        message.Insert(L"activityId", JsonValue::CreateStringValue(localId));
        message.Insert(L"actionId", JsonValue::CreateStringValue(std::wstring(actionId)));
        send(*plugin, std::wstring(message.Stringify().c_str()));
        return;
    }
}

void PluginHost::discover_and_launch() {
    const auto root = plugins_directory();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const auto manifest = entry.path() / L"plugin.json";
        if (std::filesystem::exists(manifest)) launch_manifest(manifest);
    }
}

void PluginHost::launch_manifest(const std::filesystem::path& manifest) {
    try {
        const JsonObject json = JsonObject::Parse(winrt::hstring(read_all_text(manifest)));
        const std::wstring id = json.GetNamedString(L"id", L"").c_str();
        const std::wstring name = json.GetNamedString(L"name", winrt::hstring(id)).c_str();
        const std::wstring executable = json.GetNamedString(L"executable", L"").c_str();
        if (id.empty() || executable.empty()) return;

        const auto directory = manifest.parent_path();
        auto executablePath = std::filesystem::path(executable);
        if (executablePath.is_relative()) {
            const auto localCandidate = directory / executablePath;
            if (std::filesystem::exists(localCandidate)) executablePath = localCandidate;
            // Otherwise keep the bare name so CreateProcess can search PATH.
        }

        std::wstring command = quote_arg(executablePath);
        if (json.HasKey(L"args")) {
            const auto args = json.GetNamedArray(L"args");
            for (uint32_t i = 0; i < args.Size(); ++i) {
                command += L" \"" + std::wstring(args.GetStringAt(i).c_str()) + L"\"";
            }
        }
        command += L" --isle-plugin";

        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE childStdoutRead{}, childStdoutWrite{};
        HANDLE childStdinRead{}, childStdinWrite{};
        if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) return;
        if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)) {
            CloseHandle(childStdoutRead); CloseHandle(childStdoutWrite); return;
        }
        SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0);

        // stdout is protocol-only NDJSON. Keep stderr out of the protocol stream so a
        // plugin can log/debug freely without corrupting messages.
        const auto logsDirectory = Settings::data_directory() / L"logs";
        std::error_code logEc;
        std::filesystem::create_directories(logsDirectory, logEc);
        const auto logPath = logsDirectory / (id + L".log");
        HANDLE childStderr = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childStderr == INVALID_HANDLE_VALUE) {
            childStderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = childStdoutWrite;
        si.hStdError = (childStderr == INVALID_HANDLE_VALUE) ? childStdoutWrite : childStderr;
        si.hStdInput = childStdinRead;
        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                       nullptr, directory.c_str(), &si, &pi);
        CloseHandle(childStdoutWrite);
        CloseHandle(childStdinRead);
        if (childStderr != INVALID_HANDLE_VALUE) CloseHandle(childStderr);
        if (!ok) {
            CloseHandle(childStdoutRead); CloseHandle(childStdinWrite); return;
        }

        auto plugin = std::make_unique<Process>();
        plugin->id = id;
        plugin->name = name;
        plugin->directory = directory;
        plugin->process = pi.hProcess;
        plugin->threadHandle = pi.hThread;
        plugin->stdoutRead = childStdoutRead;
        plugin->stdinWrite = childStdinWrite;
        plugin->healthy = true;

        Process* raw = plugin.get();
        plugin->reader = std::jthread([this, raw](std::stop_token token) { reader_loop(raw, token); });
        processes_.push_back(std::move(plugin));

        JsonObject hello;
        hello.Insert(L"type", JsonValue::CreateStringValue(L"host.hello"));
        hello.Insert(L"api", JsonValue::CreateNumberValue(1));
        hello.Insert(L"host", JsonValue::CreateStringValue(L"Isle"));
        send(*raw, std::wstring(hello.Stringify().c_str()));
        publish_plugin_status(*raw, true, L"Plugin connected");
    } catch (...) {
        // A bad manifest must never take down the shell.
    }
}

void PluginHost::reader_loop(Process* plugin, std::stop_token stopToken) {
    std::array<char, 4096> buffer{};
    std::string pending;
    while (!stopToken.stop_requested() && plugin->stdoutRead) {
        DWORD read = 0;
        if (!ReadFile(plugin->stdoutRead, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) break;
        pending.append(buffer.data(), read);
        size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) process_line(*plugin, line);
        }
    }
    plugin->healthy = false;
    publish_plugin_status(*plugin, false, L"Plugin disconnected");
}

void PluginHost::process_line(Process& plugin, const std::string& line) {
    try {
        const JsonObject message = JsonObject::Parse(winrt::hstring(utf8_to_wide(line)));
        const std::wstring type = message.GetNamedString(L"type", L"").c_str();
        if (type == L"activity.remove") {
            const std::wstring localId = message.GetNamedString(L"id", L"").c_str();
            if (store_ && !localId.empty()) store_->remove(plugin.id + L":" + localId);
            return;
        }
        if (type != L"activity.upsert" || !store_) return;

        const JsonObject data = message.GetNamedObject(L"activity");
        Activity activity;
        const std::wstring localId = data.GetNamedString(L"id", L"").c_str();
        if (localId.empty()) return;
        activity.id = plugin.id + L":" + localId;
        activity.source = plugin.id;
        activity.kind = parse_kind(data.GetNamedString(L"kind", L"status").c_str());
        activity.title = data.GetNamedString(L"title", winrt::hstring(plugin.name)).c_str();
        activity.subtitle = data.GetNamedString(L"subtitle", L"").c_str();
        activity.glyph = data.GetNamedString(L"glyph", L"\uE946").c_str();
        activity.accent = data.GetNamedString(L"accent", L"#8B8B8B").c_str();
        activity.priority = static_cast<int>(data.GetNamedNumber(L"priority", 0));
        activity.pinned = data.GetNamedBoolean(L"pinned", false);
        if (data.HasKey(L"progress")) activity.progress = clamp01(data.GetNamedNumber(L"progress"));
        if (data.HasKey(L"value")) activity.value = data.GetNamedNumber(L"value");
        activity.valueSuffix = data.GetNamedString(L"valueSuffix", L"").c_str();

        if (data.HasKey(L"actions")) {
            const auto actions = data.GetNamedArray(L"actions");
            for (uint32_t i = 0; i < actions.Size(); ++i) {
                const auto actionObj = actions.GetObjectAt(i);
                activity.actions.push_back({
                    actionObj.GetNamedString(L"id", L"").c_str(),
                    actionObj.GetNamedString(L"label", L"").c_str(),
                    actionObj.GetNamedString(L"glyph", L"").c_str(),
                });
            }
        }
        store_->upsert(std::move(activity));
    } catch (...) {
        publish_plugin_status(plugin, false, L"Invalid plugin message");
    }
}

void PluginHost::send(Process& plugin, const std::wstring& json) {
    if (!plugin.stdinWrite) return;
    std::string bytes = wide_to_utf8(json);
    bytes.push_back('\n');
    std::scoped_lock lock(plugin.writeMutex);
    DWORD written = 0;
    WriteFile(plugin.stdinWrite, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void PluginHost::publish_plugin_status(const Process& plugin, bool online, std::wstring_view detail) {
    if (!store_) return;
    const std::wstring id = plugin.id + L":host-status";
    if (online) {
        // Do not clutter the UI with healthy-plugin rows. The host status is only shown on errors.
        store_->remove(id);
        return;
    }
    Activity activity;
    activity.id = id;
    activity.source = plugin.id;
    activity.kind = ActivityKind::Status;
    activity.title = plugin.name;
    activity.subtitle = std::wstring(detail);
    activity.glyph = L"\uEA39";
    activity.accent = L"#FF5A5F";
    activity.priority = 5;
    store_->upsert(std::move(activity));
}

} // namespace isle
