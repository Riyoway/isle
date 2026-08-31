#include "SystemProvider.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace isle {

void SystemProvider::start(ActivityStore& store) {
    store_ = &store;
    lastUpdate_ = {};
    initialize_gpu();
    publish();
}

void SystemProvider::stop() {
    if (store_) store_->remove_source(L"system");
    if (gpuQuery_) PdhCloseQuery(gpuQuery_);
    gpuQuery_ = {};
    gpuCounter_ = {};
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

void SystemProvider::initialize_gpu() {
    if (PdhOpenQueryW(nullptr, 0, &gpuQuery_) != ERROR_SUCCESS) return;
    if (PdhAddEnglishCounterW(gpuQuery_, L"\\GPU Engine(*)\\Utilization Percentage", 0,
                              &gpuCounter_) != ERROR_SUCCESS) {
        PdhCloseQuery(gpuQuery_);
        gpuQuery_ = {};
        return;
    }
    PdhCollectQueryData(gpuQuery_);
}

std::optional<double> SystemProvider::gpu_usage() {
    if (!gpuQuery_ || !gpuCounter_ || PdhCollectQueryData(gpuQuery_) != ERROR_SUCCESS) return std::nullopt;
    DWORD bytes = 0;
    DWORD count = 0;
    if (PdhGetFormattedCounterArrayW(gpuCounter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA ||
        bytes == 0) return std::nullopt;

    std::vector<std::byte> buffer(bytes);
    auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(gpuCounter_, PDH_FMT_DOUBLE, &bytes, &count, values) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    double total = 0.0;
    for (DWORD i = 0; i < count; ++i) {
        const auto status = values[i].FmtValue.CStatus;
        if (status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA) {
            total += std::max(0.0, values[i].FmtValue.doubleValue);
        }
    }
    return std::clamp(total, 0.0, 100.0);
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

    Activity gpu;
    gpu.id = L"system.gpu";
    gpu.source = L"system";
    gpu.kind = ActivityKind::Metric;
    gpu.title = L"GPU";
    gpu.subtitle = L"Graphics";
    gpu.glyph = L"\uE7F4";
    gpu.accent = L"#0A84FF";
    gpu.value = gpu_usage().value_or(0.0);
    gpu.valueSuffix = L"%";
    gpu.progress = *gpu.value / 100.0;
    gpu.priority = 95;
    gpu.pinned = true;
    store_->upsert(std::move(gpu));

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
