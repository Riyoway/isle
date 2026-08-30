#include "ActivityStore.h"

#include <algorithm>

namespace isle {

void ActivityStore::upsert(Activity activity) {
    std::scoped_lock lock(mutex_);
    activity.updatedAt = std::chrono::steady_clock::now();
    const auto it = std::find_if(activities_.begin(), activities_.end(), [&](const Activity& current) {
        return current.id == activity.id;
    });
    if (it == activities_.end()) {
        activities_.push_back(std::move(activity));
    } else {
        *it = std::move(activity);
    }
}

void ActivityStore::remove(std::wstring_view id) {
    std::scoped_lock lock(mutex_);
    std::erase_if(activities_, [&](const Activity& current) { return current.id == id; });
}

void ActivityStore::remove_source(std::wstring_view source) {
    std::scoped_lock lock(mutex_);
    std::erase_if(activities_, [&](const Activity& current) { return current.source == source; });
}

std::vector<Activity> ActivityStore::snapshot() const {
    std::scoped_lock lock(mutex_);
    auto result = activities_;
    std::ranges::sort(result, [](const Activity& a, const Activity& b) {
        if (a.pinned != b.pinned) return a.pinned > b.pinned;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.updatedAt > b.updatedAt;
    });
    return result;
}

} // namespace isle
