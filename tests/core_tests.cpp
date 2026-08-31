#include "../src/core/ActivityStore.h"
#include "../src/core/AIProviders.h"
#include "../src/core/Spring.h"

#include <cassert>
#include <cmath>
#include <iostream>

int main() {
    {
        isle::Spring spring(0.0f);
        spring.configure(520.0f, 38.0f);
        spring.set_target(1.0f);
        for (int i = 0; i < 300; ++i) spring.step(1.0f / 120.0f);
        assert(std::abs(spring.value() - 1.0f) < 0.001f);
    }

    {
        constexpr float overflow = 100.0f;
        constexpr double travel = 2.0;
        constexpr double pause = 1.0;
        if (isle::marquee_offset(0.5, overflow, travel, pause) != 0.0f ||
            isle::marquee_offset(2.0, overflow, travel, pause) != 50.0f ||
            isle::marquee_offset(3.5, overflow, travel, pause) != overflow ||
            isle::marquee_offset(6.0, overflow, travel, pause) != 0.0f) return 1;
    }

    {
        const float low = isle::audio_bar_strength(0.2f, 0.2f, 0.8f);
        const float middle = isle::audio_bar_strength(0.5f, 0.2f, 0.8f);
        const float high = isle::audio_bar_strength(0.8f, 0.2f, 0.8f);
        if (!(low < middle && middle < high && low >= 0.0f && high <= 1.0f)) return 1;
    }

    {
        assert(isle::snap_normalized(0.48f, 1000.0f, 30.0f) == 0.5f);
        assert(isle::snap_normalized(0.04f, 1000.0f, 30.0f) == 0.04f);
        assert(isle::snap_normalized(0.99f, 1000.0f, 30.0f) == 1.0f);
    }

    {
        assert(!isle::prefer_upward_panel(20.0f, 40.0f, 500.0f, 0.0f, 1080.0f));
        assert(isle::prefer_upward_panel(920.0f, 40.0f, 500.0f, 0.0f, 1080.0f));
    }

    {
        assert(isle::compact_island_width(0, 3, true) == 230.0f);
        assert(isle::compact_island_width(1, 3, true) == 252.0f);
        assert(isle::compact_island_width(2, 2, true) == 282.0f);
        assert(isle::compact_island_width(2, 3, false) == 230.0f);
    }

    {
        static_assert(isle::kAIProviders.size() == 56);
        assert((isle::kAIProviders.size() + 6 - 1) / 6 == 10);
    }

    {
        isle::ActivityStore store;
        isle::Activity low;
        low.id = L"low";
        low.source = L"test";
        low.title = L"Low";
        low.priority = 1;
        store.upsert(low);

        isle::Activity high;
        high.id = L"high";
        high.source = L"test";
        high.title = L"High";
        high.priority = 10;
        store.upsert(high);

        auto snapshot = store.snapshot();
        assert(snapshot.size() == 2);
        assert(snapshot.front().id == L"high");

        high.subtitle = L"updated";
        store.upsert(high);
        snapshot = store.snapshot();
        assert(snapshot.size() == 2);
        assert(snapshot.front().subtitle == L"updated");

        store.remove_source(L"test");
        assert(store.snapshot().empty());
    }

    std::cout << "Isle core tests passed\n";
    return 0;
}
