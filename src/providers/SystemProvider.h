#pragma once

#include "../core/Provider.h"

#include <Windows.h>
#include <chrono>

namespace isle {

class SystemProvider final : public IProvider {
public:
    void start(ActivityStore& store) override;
    void stop() override;
    void tick() override;
    void invoke(std::wstring_view activityId, std::wstring_view actionId) override;

private:
    static unsigned long long filetime_to_u64(const FILETIME& ft) noexcept;
    void publish();

    ActivityStore* store_{nullptr};
    FILETIME prevIdle_{};
    FILETIME prevKernel_{};
    FILETIME prevUser_{};
    bool hasCpuSample_{false};
    std::chrono::steady_clock::time_point lastUpdate_{};
};

} // namespace isle
