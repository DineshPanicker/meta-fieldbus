// modbus_master -- 100 Hz SCHED_DEADLINE Modbus RTU scan-cycle master (C++23).
//
// Three phases:
//   setup  -- may allocate, may fail, runs once
//   loop   -- bounded work only: no heap, no exceptions, no formatting
//   report -- back outside the RT path
//
// GPIO27 is the cycle marker for the logic analyzer: high from wake-up to the
// end of the transaction, so its rising edges give the true period and its
// high time gives the request->response round trip.
#include "gpio_line.hpp"
#include "modbus_frame.hpp"
#include "modbus_port.hpp"
#include "rt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <span>
#include <system_error>

using namespace std::chrono_literals;

namespace {

constexpr auto kPeriod = 10ms;          // 100 Hz scan cycle
constexpr auto kRuntime = 3ms;          // SCHED_DEADLINE CPU budget per period
constexpr auto kDeadline = 8ms;         // must be done 8 ms after release
constexpr auto kReplyTimeout = 5ms;     // < deadline: a lost reply cannot overrun the cycle
constexpr int kCycles = 6000;           // 60 s
constexpr std::uint8_t kSlave = 1;
constexpr std::uint32_t kMarkerOffset = 27;  // BCM27 on gpiochip0

// Built by the compiler; lives in .rodata; costs nothing per cycle.
constexpr auto kRequest = modbus::make_read_holding(kSlave, 0, 4);

struct Stats {
    std::uint32_t ok = 0;
    std::uint32_t timeouts = 0;
    std::uint32_t io_errors = 0;
    std::uint32_t bad_replies = 0;
    std::int64_t max_wake_late_ns = 0;  // how late the thread woke after the boundary
    std::int64_t max_txn_ns = 0;        // write -> validated reply
    std::int64_t sum_txn_ns = 0;        // for the mean, successful cycles only
};

int fail(const char* what, const std::error_code& ec) {
    std::fputs(std::format("{}: {}\n", what, ec.message()).c_str(), stderr);
    return 1;
}

}  // namespace

int main() {
    // ---------------- setup ----------------
    if (auto r = rt::lock_memory(); !r)
        return fail("mlockall", r.error());

    auto port = ModbusPort::open("/dev/modbus0");
    if (!port)
        return fail("open /dev/modbus0", port.error());

    auto marker = GpioLine::request_output("/dev/gpiochip0", kMarkerOffset, "modbus_master");
    if (!marker)
        return fail("gpio line request", marker.error());

    if (auto r = rt::set_deadline({.runtime = kRuntime, .deadline = kDeadline, .period = kPeriod}); !r)
        return fail("sched_setattr", r.error());

    // Everything the loop touches is constructed here, before the loop.
    Stats st;
    modbus_record rec{};
    std::array<std::uint16_t, 4> regs{};
    rt::PeriodicTimer timer{kPeriod};

    // ---------------- RT loop ----------------
    for (int i = 0; i < kCycles; ++i) {
        const std::int64_t boundary = timer.wait_next();
        const std::int64_t woke = rt::now_ns();
        marker->set(true);

        st.max_wake_late_ns = std::max(st.max_wake_late_ns, woke - boundary);

        auto res = port->transact(kRequest, kReplyTimeout, rec);
        if (!res) {
            res.error() == PortError::timeout ? ++st.timeouts : ++st.io_errors;
        } else if (modbus::parse_read_holding(std::span{rec.data, rec.len}, kSlave, regs)) {
            const std::int64_t txn = rt::now_ns() - woke;
            st.max_txn_ns = std::max(st.max_txn_ns, txn);
            st.sum_txn_ns += txn;
            ++st.ok;
        } else {
            ++st.bad_replies;
        }

        marker->set(false);
    }

    // ---------------- report ----------------
    const auto us = [](std::int64_t ns) { return static_cast<double>(ns) / 1000.0; };
    const double mean_us = st.ok ? us(st.sum_txn_ns) / st.ok : 0.0;
    std::fputs(std::format("done: {} cycles | ok {} | timeouts {} | io errors {} | bad replies {} | "
                           "stale drained {}\n"
                           "wake-up lateness max {:.1f} us | transaction mean {:.1f} us max {:.1f} us\n"
                           "last registers {} {} {} {}\n",
                           kCycles, st.ok, st.timeouts, st.io_errors, st.bad_replies,
                           port->stale_drained(), us(st.max_wake_late_ns), mean_us,
                           us(st.max_txn_ns), regs[0], regs[1], regs[2], regs[3])
                   .c_str(),
               stdout);
    return 0;
}
