#pragma once

#include "../core/Provider.h"

#include <winrt/Windows.Media.Control.h>

#include <memory>
#include <vector>

namespace isle {

class MediaProvider final : public IProvider {
public:
    MediaProvider() = default;
    ~MediaProvider() override;

    void start(ActivityStore& store) override;
    void stop() override;
    void tick() override;
    void invoke(std::wstring_view activityId, std::wstring_view actionId) override;

private:
    winrt::fire_and_forget initialize_async();
    winrt::fire_and_forget refresh_async();
    winrt::fire_and_forget invoke_async(std::wstring actionId);

    ActivityStore* store_{nullptr};
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session_{nullptr};
    winrt::event_token currentSessionToken_{};
    winrt::event_token mediaPropertiesToken_{};
    winrt::event_token playbackInfoToken_{};
    winrt::event_token timelinePropertiesToken_{};
    std::shared_ptr<const std::vector<std::uint8_t>> artwork_;
    std::wstring artworkKey_;
    bool started_{false};
};

} // namespace isle
