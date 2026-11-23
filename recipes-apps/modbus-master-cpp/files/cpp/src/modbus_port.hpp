// modbus_port.hpp -- owns /dev/modbus0 and runs one bounded transaction.
#pragma once

#include "modbus_uapi.h"
#include "unique_fd.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>

// The layout the bench dump showed (len at byte 8, frame at byte 12).
// If the driver ever disagrees, the build fails here instead of the bus
// silently returning garbage.
static_assert(offsetof(modbus_record, ts_ns) == 0);
static_assert(offsetof(modbus_record, len) == 8);
static_assert(offsetof(modbus_record, data) == 12);
static_assert(sizeof(modbus_record) == 272);

enum class PortError {
    write_failed,  // request not fully handed to the driver
    timeout,       // no reply within the deadline
    poll_failed,   // ppoll() itself failed
    read_failed,   // read() failed after ppoll() said readable
    short_record,  // read() returned less than the record claims to hold
};

class ModbusPort {
public:
    static std::expected<ModbusPort, std::error_code> open(const char* path);

    // Adopt an already-open descriptor. Used by the host tests, which put a
    // socketpair in place of the driver. The fd must be non-blocking.
    static ModbusPort adopt(UniqueFd fd) noexcept { return ModbusPort{std::move(fd)}; }

    // 1. drain records left over from earlier, timed-out cycles
    // 2. write the request
    // 3. wait at most `timeout` for one record
    // 4. read it into `reply` (caller-owned: nothing is allocated per cycle)
    std::expected<void, PortError> transact(std::span<const std::uint8_t> request,
                                            std::chrono::nanoseconds timeout,
                                            modbus_record& reply) noexcept;

    // How many late replies were thrown away by drain() so far.
    [[nodiscard]] std::uint32_t stale_drained() const noexcept { return stale_; }

private:
    explicit ModbusPort(UniqueFd fd) noexcept : fd_{std::move(fd)} {}
    void drain() noexcept;

    UniqueFd fd_;
    std::uint32_t stale_ = 0;
};
