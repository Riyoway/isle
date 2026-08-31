#pragma once

#include <algorithm>
#include <cmath>

namespace isle {

// Small critically/near-critically damped spring used for island geometry.
// It deliberately runs on the UI thread so resize, hit testing and rendering
// observe the exact same state on each frame.
class Spring {
public:
    Spring() = default;
    explicit Spring(float initial) : value_(initial), target_(initial) {}

    void set_target(float target) noexcept { target_ = target; }
    [[nodiscard]] float target() const noexcept { return target_; }
    [[nodiscard]] float value() const noexcept { return value_; }
    [[nodiscard]] float velocity() const noexcept { return velocity_; }

    void snap(float value) noexcept {
        value_ = value;
        target_ = value;
        velocity_ = 0.0f;
    }

    void configure(float stiffness, float damping) noexcept {
        stiffness_ = stiffness;
        damping_ = damping;
    }

    bool step(float dtSeconds) noexcept {
        // Very large deltas occur after sleep/resume or when a debugger stops.
        // Clamp them to prevent an unstable integration step.
        const float dt = std::clamp(dtSeconds, 0.0f, 1.0f / 30.0f);
        const float displacement = target_ - value_;
        const float acceleration = displacement * stiffness_ - velocity_ * damping_;
        velocity_ += acceleration * dt;
        value_ += velocity_ * dt;

        if (std::abs(target_ - value_) < 0.025f && std::abs(velocity_) < 0.025f) {
            value_ = target_;
            velocity_ = 0.0f;
            return false;
        }
        return true;
    }

private:
    float value_{0.0f};
    float target_{0.0f};
    float velocity_{0.0f};
    float stiffness_{520.0f};
    float damping_{38.0f};
};

[[nodiscard]] inline float marquee_offset(double elapsed, float overflow,
                                           double travel, double pause = 0.9) noexcept {
    if (overflow <= 0.0f || travel <= 0.0) return 0.0f;
    const double cycle = pause * 2.0 + travel * 2.0;
    const double phase = std::fmod(std::max(0.0, elapsed), cycle);
    const auto eased = [](double value) {
        const float t = std::clamp(static_cast<float>(value), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    if (phase < pause) return 0.0f;
    if (phase < pause + travel) return overflow * eased((phase - pause) / travel);
    if (phase < pause * 2.0 + travel) return overflow;
    return overflow * (1.0f - eased((phase - pause * 2.0 - travel) / travel));
}

[[nodiscard]] inline float audio_bar_strength(float level, float minimum, float maximum) noexcept {
    level = std::clamp(level, 0.0f, 1.0f);
    const float contrast = std::clamp((level - minimum) / std::max(0.02f, maximum - minimum), 0.0f, 1.0f);
    return 0.08f + 0.92f * (level * 0.15f + contrast * 0.85f);
}

[[nodiscard]] inline float snap_normalized(float value, float extent, float threshold) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    for (const float target : {0.0f, 0.5f, 1.0f}) {
        if (std::abs(value - target) * std::max(0.0f, extent) <= threshold) return target;
    }
    return value;
}

[[nodiscard]] inline bool prefer_upward_panel(float anchorTop, float collapsedHeight,
                                               float panelHeight, float workTop,
                                               float workBottom) noexcept {
    const float below = workBottom - anchorTop;
    const float above = anchorTop + collapsedHeight - workTop;
    return panelHeight > below && above > below;
}

} // namespace isle
