#pragma once

#include "../core/Provider.h"

#include <chrono>
#include <atomic>
#include <thread>

namespace isle {

class AIUsageProvider final : public IProvider {
public:
    void start(ActivityStore& store) override;
    void stop() override;
    void tick() override;
    void invoke(std::wstring_view activityId, std::wstring_view actionId) override;

private:
    void publish();
    void direct_loop(std::stop_token stopToken);

    ActivityStore* store_{nullptr};
    std::chrono::steady_clock::time_point lastUpdate_{};
    bool hasUsage_{false};
    std::atomic_bool refreshRequested_{true};
    std::jthread refreshThread_;
};

} // namespace isle
