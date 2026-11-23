// rt.hpp -- the real-time plumbing: memory locking, SCHED_DEADLINE, and an
// absolute-time periodic timer.
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <system_error>

namespace rt {

struct DeadlineParams {
    std::chrono::nanoseconds runtime;   // CPU budget per period
    std::chrono::nanoseconds deadline;  // must finish within this, relative to release
    std::chrono::nanoseconds period;    // release interval
};

// mlockall(MCL_CURRENT | MCL_FUTURE): no page faults once the loop runs.
std::expected<void, std::error_code> lock_memory();

// sched_setattr(SCHED_DEADLINE) for the calling thread.
std::expected<void, std::error_code> set_deadline(const DeadlineParams& p);

// CLOCK_MONOTONIC "now" in nanoseconds; used for software-side timing stats.
[[nodiscard]] std::int64_t now_ns() noexcept;

// Wakes on absolute period boundaries. Absolute deadlines do not drift:
// a late wake-up does not push every later cycle back.
class PeriodicTimer {
public:
    explicit PeriodicTimer(std::chrono::nanoseconds period) noexcept;

    // Sleeps until the next boundary; returns that boundary in ns, so the
    // caller can measure how late the wake-up actually was.
    std::int64_t wait_next() noexcept;

private:
    std::chrono::nanoseconds period_;
    timespec next_{};
};

}  // namespace rt
