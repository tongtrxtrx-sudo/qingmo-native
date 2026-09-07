#include "scroll_motion.hpp"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
}

int main() {
    md::ScalarScrollMotion motion;
    check(!motion.active() && motion.sample(0) == 0, "default is idle at zero");
    motion.scrollBy(100, 0, 10000, 0);
    check(motion.active() && motion.target() == 100, "wheel starts motion");
    check(near(motion.sample(60), 87.5), "half duration has cubic ease-out position");
    check(motion.sample(120) == 100 && !motion.active(), "finishes exactly at 120ms");
    check(motion.sample(10000) == 100, "idle does not drift");

    motion.reset(0);
    double last = 0;
    for (int i = 0; i != 100; ++i) {
        motion.scrollBy(0.25, 0, 10000, i * 2.0);
        const double current = motion.sample(i * 2.0);
        check(current >= last, "high frequency input stays monotonic");
        last = current;
    }
    check(near(motion.target(), 25), "fractional wheel deltas accumulate without quantization");
    check(motion.sample(318) == 25 && !motion.active(), "burst has bounded tail after last input");

    motion.reset(500);
    motion.scrollBy(400, 0, 10000, 0);
    const double turn = motion.sample(30);
    motion.scrollBy(-20, 0, 10000, 30);
    check(near(motion.target(), turn - 20), "reverse input discards forward backlog");
    check(motion.sample(31) < turn, "reverse input changes direction on next sample");
    check(near(motion.sample(150), turn - 20), "reverse motion finishes on new target");

    motion.reset(90);
    motion.scrollBy(50, 0, 100, 0);
    check(motion.target() == 100, "target clamps at bottom");
    motion.scrollBy(50, 0, 100, 100);
    check(motion.sample(120) == 100 && !motion.active(), "edge input does not restart saturated motion");
    motion.scrollBy(50, 0, 100, 130);
    check(!motion.active(), "outward input at edge stays idle");
    motion.scrollBy(-500, 0, 100, 140);
    check(motion.target() == 0 && motion.sample(260) == 0, "top clamps without overshoot");

    motion.reset(500);
    motion.scrollBy(400, 0, 1000, 0);
    motion.sample(30);
    motion.reset(42.5);
    check(!motion.active() && motion.sample(1000) == 42.5, "scrollbar reset cancels old trajectory");
    motion.scrollBy(1.5, 0, 1000, 1000);
    check(motion.target() == 44, "input after reset uses new scrollbar position");

    motion.reset(500);
    motion.scrollBy(400, 0, 1000, 0);
    motion.scrollBy(10, 0, 100, 30);
    check(motion.sample(30) == 100 && !motion.active(), "shrinking content clamps current and target");
    motion.scrollBy(100, 5, 5, 40);
    check(motion.sample(40) == 5 && !motion.active(), "zero scroll range is idle");

    if (failures) return 1;
    std::cout << "scroll motion tests passed\n";
}
