#pragma once

#include "ActivityStore.h"

#include <string_view>

namespace isle {

class IProvider {
public:
    virtual ~IProvider() = default;
    virtual void start(ActivityStore& store) = 0;
    virtual void stop() = 0;
    virtual void tick() = 0;
    virtual void invoke(std::wstring_view activityId, std::wstring_view actionId) = 0;
};

} // namespace isle
