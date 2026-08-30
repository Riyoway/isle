#include "../src/core/ActivityStore.h"
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
