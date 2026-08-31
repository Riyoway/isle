#pragma once

#include "../core/Provider.h"

#include <chrono>
#include <filesystem>

namespace isle {

class ShortcutProvider final : public IProvider {
public:
    void start(ActivityStore& store) override;
    void stop() override;
    void tick() override;
    void invoke(std::wstring_view activityId, std::wstring_view actionId) override;

private:
    void publish();

    ActivityStore* store_{nullptr};
    std::filesystem::file_time_type settingsModified_{};
    std::chrono::steady_clock::time_point lastCheck_{};
};

} // namespace isle
