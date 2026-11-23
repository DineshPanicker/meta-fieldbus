#include "rt.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>

namespace rt
{

    namespace
    {
        constexpr long kNsPerSec = 1'000'000'000;

        // The kernel's struct sched_attr (include/uapi/linux/sched/types.h), declared
        // here on purpose. Including that header next to glibc's <sched.h> (pulled in
        // by <chrono>) redefines struct sched_param with linux-libc-headers 6.6, as in
        // Yocto scarthgap; and glibc 2.41+ declares its own struct sched_attr. A
        // private name avoids both. glibc 2.39 has no sched_setattr() wrapper.
        struct KernelSchedAttr
        {
            std::uint32_t size;
            std::uint32_t sched_policy;
            std::uint64_t sched_flags;
            std::int32_t sched_nice;
            std::uint32_t sched_priority;
            std::uint64_t sched_runtime;  // ns
            std::uint64_t sched_deadline; // ns
            std::uint64_t sched_period;   // ns
            std::uint32_t sched_util_min;
            std::uint32_t sched_util_max;
        };
        static_assert(sizeof(KernelSchedAttr) == 56); // SCHED_ATTR_SIZE_VER1

        constexpr std::uint32_t kSchedDeadline = 6; // SCHED_DEADLINE in <linux/sched.h>

        std::int64_t to_ns(const timespec &t) noexcept
        {
            return static_cast<std::int64_t>(t.tv_sec) * kNsPerSec + t.tv_nsec;
        }
    } // namespace

    std::expected<void, std::error_code> lock_memory()
    {
        if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
            return std::unexpected(std::error_code{errno, std::system_category()});
        return {};
    }

    std::expected<void, std::error_code> set_deadline(const DeadlineParams &p)
    {
        KernelSchedAttr attr{};
        attr.size = sizeof attr;
        attr.sched_policy = kSchedDeadline;
        attr.sched_runtime = static_cast<std::uint64_t>(p.runtime.count());
        attr.sched_deadline = static_cast<std::uint64_t>(p.deadline.count());
        attr.sched_period = static_cast<std::uint64_t>(p.period.count());
        if (::syscall(SYS_sched_setattr, 0, &attr, 0) != 0)
            return std::unexpected(std::error_code{errno, std::system_category()});
        return {};
    }

    std::int64_t now_ns() noexcept
    {
        timespec t{};
        ::clock_gettime(CLOCK_MONOTONIC, &t);
        return to_ns(t);
    }

    PeriodicTimer::PeriodicTimer(std::chrono::nanoseconds period) noexcept : period_{period}
    {
        ::clock_gettime(CLOCK_MONOTONIC, &next_);
    }

    std::int64_t PeriodicTimer::wait_next() noexcept
    {
        next_.tv_nsec += static_cast<long>(period_.count());
        while (next_.tv_nsec >= kNsPerSec)
        {
            next_.tv_nsec -= kNsPerSec;
            ++next_.tv_sec;
        }
        // clock_nanosleep returns the error number directly (it does not set errno).
        while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_, nullptr) == EINTR)
        {
        }
        return to_ns(next_);
    }

} // namespace rt