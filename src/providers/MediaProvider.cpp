#include "MediaProvider.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstdint>

namespace isle {

using namespace winrt;
using namespace Windows::Media;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

namespace {
constexpr std::uint64_t kMaxArtworkBytes = 8ull * 1024ull * 1024ull;
}

MediaProvider::~MediaProvider() {
    stop();
}

void MediaProvider::start(ActivityStore& store) {
    store_ = &store;
    started_ = true;
    initialize_async();
}

void MediaProvider::stop() {
    started_ = false;
    try {
        if (session_) {
            if (mediaPropertiesToken_.value) session_.MediaPropertiesChanged(mediaPropertiesToken_);
            if (playbackInfoToken_.value) session_.PlaybackInfoChanged(playbackInfoToken_);
            if (timelinePropertiesToken_.value) session_.TimelinePropertiesChanged(timelinePropertiesToken_);
        }
        if (manager_ && currentSessionToken_.value) manager_.CurrentSessionChanged(currentSessionToken_);
    } catch (...) {}
    session_ = nullptr;
    manager_ = nullptr;
    mediaPropertiesToken_ = {};
    playbackInfoToken_ = {};
    timelinePropertiesToken_ = {};
    currentSessionToken_ = {};
    artwork_.reset();
    artworkKey_.clear();
    if (store_) store_->remove(L"media.now-playing");
    store_ = nullptr;
}

void MediaProvider::tick() {
    // GSMTC is event-driven; no polling is necessary.
}

void MediaProvider::invoke(std::wstring_view activityId, std::wstring_view actionId) {
    if (activityId != L"media.now-playing") return;
    invoke_async(std::wstring(actionId));
}

fire_and_forget MediaProvider::initialize_async() {
    try {
        manager_ = co_await GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        if (!started_) co_return;
        currentSessionToken_ = manager_.CurrentSessionChanged([this](auto const&, auto const&) {
            refresh_async();
        });
        refresh_async();
    } catch (...) {
        if (store_) store_->remove(L"media.now-playing");
    }
}

fire_and_forget MediaProvider::refresh_async() {
    try {
        if (!started_ || !manager_ || !store_) co_return;

        const auto newSession = manager_.GetCurrentSession();
        if (session_ != newSession) {
            if (session_) {
                if (mediaPropertiesToken_.value) session_.MediaPropertiesChanged(mediaPropertiesToken_);
                if (playbackInfoToken_.value) session_.PlaybackInfoChanged(playbackInfoToken_);
                if (timelinePropertiesToken_.value) session_.TimelinePropertiesChanged(timelinePropertiesToken_);
            }
            session_ = newSession;
            mediaPropertiesToken_ = {};
            playbackInfoToken_ = {};
            timelinePropertiesToken_ = {};
            artwork_.reset();
            artworkKey_.clear();
            if (session_) {
                mediaPropertiesToken_ = session_.MediaPropertiesChanged([this](auto const&, auto const&) { refresh_async(); });
                playbackInfoToken_ = session_.PlaybackInfoChanged([this](auto const&, auto const&) { refresh_async(); });
                timelinePropertiesToken_ = session_.TimelinePropertiesChanged([this](auto const&, auto const&) { refresh_async(); });
            }
        }

        if (!session_) {
            store_->remove(L"media.now-playing");
            co_return;
        }

        const auto props = co_await session_.TryGetMediaPropertiesAsync();
        if (!props || !started_ || !store_) co_return;

        const auto playback = session_.GetPlaybackInfo();
        const bool playing = playback && playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        const std::wstring title = props.Title().empty() ? L"Now playing" : std::wstring(props.Title().c_str());
        const std::wstring artist = props.Artist().empty() ? std::wstring(session_.SourceAppUserModelId().c_str()) : std::wstring(props.Artist().c_str());

        const std::wstring artworkKey = title + L"\n" + artist;
        if (artworkKey != artworkKey_ || !artwork_) {
            artwork_.reset();
            if (const auto thumbnail = props.Thumbnail()) {
                const auto stream = co_await thumbnail.OpenReadAsync();
                const auto size = stream.Size();
                if (size > 0 && size <= kMaxArtworkBytes) {
                    auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(size));
                    DataReader reader(stream.GetInputStreamAt(0));
                    const auto loaded = co_await reader.LoadAsync(static_cast<std::uint32_t>(size));
                    if (loaded == size) {
                        reader.ReadBytes(*bytes);
                        artwork_ = std::move(bytes);
                    }
                    reader.Close();
                    stream.Close();
                }
            }
            artworkKey_ = artworkKey;
        }

        Activity activity;
        activity.id = L"media.now-playing";
        activity.source = L"media";
        activity.kind = ActivityKind::Media;
        activity.title = title;
        activity.subtitle = artist;
        activity.glyph = L"\uE8D6";
        activity.accent = L"#A78BFA";
        activity.priority = 200;
        activity.active = playing;
        activity.artwork = artwork_;

        const auto timeline = session_.GetTimelineProperties();
        if (timeline) {
            const auto start = timeline.StartTime().count();
            const auto end = timeline.EndTime().count();
            const auto position = timeline.Position().count();
            if (end > start) {
                constexpr double ticksPerSecond = 10'000'000.0;
                const double duration = static_cast<double>(end - start) / ticksPerSecond;
                const double elapsed = std::clamp(static_cast<double>(position - start) / ticksPerSecond, 0.0, duration);
                activity.elapsedSeconds = elapsed;
                activity.durationSeconds = duration;
                activity.progress = elapsed / duration;
            }
        }
        activity.actions = {
            {L"previous", L"Previous", L"\uE892"},
            {L"toggle", playing ? L"Pause" : L"Play", playing ? L"\uE769" : L"\uE768"},
            {L"next", L"Next", L"\uE893"},
        };
        store_->upsert(std::move(activity));
    } catch (...) {}
}

fire_and_forget MediaProvider::invoke_async(std::wstring actionId) {
    try {
        if (!session_) co_return;
        if (actionId == L"toggle") {
            co_await session_.TryTogglePlayPauseAsync();
        } else if (actionId == L"next") {
            co_await session_.TrySkipNextAsync();
        } else if (actionId == L"previous") {
            co_await session_.TrySkipPreviousAsync();
        }
    } catch (...) {}
}

} // namespace isle
