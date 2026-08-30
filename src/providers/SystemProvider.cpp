#include "SystemProvider.h"

#include <algorithm>

namespace isle {

void SystemProvider::start(ActivityStore& store) {
    store_ = &store;
    lastUpdate_ = {};
    publish();
}

void SystemProvider::stop() {
    if (store_) store_->remove_source(L"system");
    store_ = nullptr;
}

void SystemProvider::tick() {
    if (!store_) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastUpdate_.time_since_epoch().count() != 0 && now - lastUpdate_ < std::chrono::milliseconds(750)) return;
    publish();
}

void SystemProvider::invoke(std::wstring_view, std::wstring_view) {
    // Metrics are read-only. Actions are intentionally absent.
}

unsigned long long SystemProvider::filetime_to_u64(const FILETIME& ft) noexcept {
    ULARGE_INTEGER value{};
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

void SystemProvider::publish() {
    if (!store_) return;
    lastUpdate_ = std::chrono::steady_clock::now();

    // CPU, sampled from GetSystemTimes without WMI/PDH overhead.
    FILETIME idle{}, kernel{}, user{};
    double cpuPercent = 0.0;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        if (hasCpuSample_) {
            const auto idleDelta = filetime_to_u64(idle) - filetime_to_u64(prevIdle_);
            const auto kernelDelta = filetime_to_u64(kernel) - filetime_to_u64(prevKernel_);
            const auto userDelta = filetime_to_u64(user) - filetime_to_u64(prevUser_);
            const auto total = kernelDelta + userDelta;
            if (total > 0) {
                cpuPercent = std::clamp((1.0 - static_cast<double>(idleDelta) / static_cast<double>(total)) * 100.0,
                                        0.0, 100.0);
            }
        }
        prevIdle_ = idle;
        prevKernel_ = kernel;
        prevUser_ = user;
        hasCpuSample_ = true;
    }

    Activity cpu;
    cpu.id = L"system.cpu";
    cpu.source = L"system";
    cpu.kind = ActivityKind::Metric;
    cpu.title = L"CPU";
    cpu.subtitle = L"Processor";
    cpu.glyph = L"\uE950";
    cpu.accent = L"#FF5A1F";
    cpu.value = cpuPercent;
    cpu.valueSuffix = L"%";
    cpu.progress = cpuPercent / 100.0;
    cpu.priority = 100;
    cpu.pinned = true;
    store_->upsert(std::move(cpu));

    // Memory load.
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) {
        Activity ram;
        ram.id = L"system.ram";
        ram.source = L"system";
        ram.kind = ActivityKind::Metric;
        ram.title = L"RAM";
        ram.subtitle = L"Memory";
        ram.glyph = L"\uE9D9";
        ram.accent = L"#00E89A";
        ram.value = static_cast<double>(memory.dwMemoryLoad);
        ram.valueSuffix = L"%";
        ram.progress = static_cast<double>(memory.dwMemoryLoad) / 100.0;
        ram.priority = 90;
        ram.pinned = true;
        store_->upsert(std::move(ram));
    }

    // Battery/charge state. On desktops without a battery this becomes a power status card.
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        Activity battery;
        battery.id = L"system.battery";
        battery.source = L"system";
        battery.kind = ActivityKind::Metric;
        battery.title = L"Battery";
        battery.glyph = L"\uE850";
        battery.accent = L"#E8FF19";
        battery.priority = 80;
        battery.pinned = true;
        if (power.BatteryLifePercent != 255) {
            battery.value = static_cast<double>(power.BatteryLifePercent);
            battery.valueSuffix = L"%";
            battery.progress = static_cast<double>(power.BatteryLifePercent) / 100.0;
            battery.subtitle = (power.ACLineStatus == 1) ? L"Charging / AC" : L"On battery";
        } else {
            battery.value = 100.0;
            battery.valueSuffix = L"";
            battery.progress = 1.0;
            battery.subtitle = power.ACLineStatus == 1 ? L"AC power" : L"Power";
        }
        store_->upsert(std::move(battery));
    }
}

} // namespace isle
