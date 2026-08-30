#pragma once

#include "../core/Provider.h"

#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace isle {

class PluginHost final : public IProvider {
public:
    PluginHost() = default;
    ~PluginHost() override;

    void start(ActivityStore& store) override;
    void stop() override;
    void tick() override;
    void invoke(std::wstring_view activityId, std::wstring_view actionId) override;

    [[nodiscard]] std::filesystem::path plugins_directory() const;

private:
    struct Process {
        std::wstring id;
        std::wstring name;
        std::filesystem::path directory;
        HANDLE process{nullptr};
        HANDLE threadHandle{nullptr};
        HANDLE stdoutRead{nullptr};
        HANDLE stdinWrite{nullptr};
        std::jthread reader;
        std::mutex writeMutex;
        std::atomic_bool healthy{false};
    };

    void discover_and_launch();
    void launch_manifest(const std::filesystem::path& manifest);
    void reader_loop(Process* plugin, std::stop_token stopToken);
    void process_line(Process& plugin, const std::string& line);
    void send(Process& plugin, const std::wstring& json);
    void publish_plugin_status(const Process& plugin, bool online, std::wstring_view detail);

    ActivityStore* store_{nullptr};
    std::vector<std::unique_ptr<Process>> processes_;
};

} // namespace isle
