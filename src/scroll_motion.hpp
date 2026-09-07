#pragma once

#include <algorithm>

namespace md {

// UI-thread-owned scalar motion. All timestamps are monotonic milliseconds.
// The caller owns the timer and can stop it whenever active() becomes false.
class ScalarScrollMotion {
public:
    static constexpr double durationMs = 120.0;

    void reset(double position) noexcept {
        position_ = start_ = target_ = position;
        active_ = false;
    }

    void scrollBy(double delta, double minimum, double maximum, double nowMs) noexcept {
        if (maximum < minimum) maximum = minimum;
        const double current = std::clamp(sample(nowMs), minimum, maximum);
        const double previousTarget = std::clamp(target_, minimum, maximum);
        const double pending = previousTarget - current;
        const bool reversing = (delta < 0.0 && pending > 0.0) ||
                               (delta > 0.0 && pending < 0.0);
        const double base = reversing ? current : previousTarget;
        const double next = std::clamp(base + delta, minimum, maximum);

        // Repeated wheel input at an already saturated edge must not postpone
        // arrival. A changed viewport range, however, starts a clamped motion.
        if (active_ && next == target_ && current == position_) return;
        position_ = start_ = current;
        target_ = next;
        startTime_ = nowMs;
        active_ = target_ != start_;
    }

    double sample(double nowMs) noexcept {
        if (!active_) return position_;
        const double t = std::clamp((nowMs - startTime_) / durationMs, 0.0, 1.0);
        if (t >= 1.0) {
            position_ = target_;
            active_ = false;
        } else {
            const double remaining = 1.0 - t;
            position_ = start_ + (target_ - start_) *
                (1.0 - remaining * remaining * remaining);
        }
        return position_;
    }

    bool active() const noexcept { return active_; }
    double target() const noexcept { return target_; }

private:
    double position_ = 0.0;
    double start_ = 0.0;
    double target_ = 0.0;
    double startTime_ = 0.0;
    bool active_ = false;
};

} // namespace md
