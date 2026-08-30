#pragma once

#include "Types.h"

#include <mutex>
#include <string_view>
#include <vector>

namespace isle {

class ActivityStore {
public:
    void upsert(Activity activity);
    void remove(std::wstring_view id);
    void remove_source(std::wstring_view source);
    [[nodiscard]] std::vector<Activity> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<Activity> activities_;
};

} // namespace isle
