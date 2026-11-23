#include "modbus_port.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

std::expected<ModbusPort, std::error_code> ModbusPort::open(const char* path) {
    // O_NONBLOCK: drain() must never sleep. Requires the driver's read() to
    // return -EAGAIN on an empty kfifo when the file was opened non-blocking.
    UniqueFd fd{::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC)};
    if (!fd)
        return std::unexpected(std::error_code{errno, std::system_category()});
    return ModbusPort{std::move(fd)};
}

void ModbusPort::drain() noexcept {
    modbus_record junk;  // uninitialised on purpose: read() fills it
    while (::read(fd_.get(), &junk, sizeof junk) > 0)  // stops at EAGAIN
        ++stale_;
}

std::expected<void, PortError> ModbusPort::transact(std::span<const std::uint8_t> request,
                                                    std::chrono::nanoseconds timeout,
                                                    modbus_record& reply) noexcept {
    // A reply that arrived after the previous cycle gave up must not be
    // taken as the answer to this cycle's request.
    drain();

    const ssize_t w = ::write(fd_.get(), request.data(), request.size());
    if (w != static_cast<ssize_t>(request.size()))
        return std::unexpected(PortError::write_failed);

    // ppoll() rather than poll(): it takes a timespec, so sub-millisecond
    // timeouts are possible. Requires the driver to implement .poll.
    pollfd pfd{.fd = fd_.get(), .events = POLLIN, .revents = 0};
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const timespec to{.tv_sec = static_cast<time_t>(secs.count()),
                      .tv_nsec = static_cast<long>((timeout - secs).count())};

    const int r = ::ppoll(&pfd, 1, &to, nullptr);
    if (r == 0)
        return std::unexpected(PortError::timeout);
    if (r < 0)
        return std::unexpected(PortError::poll_failed);

    const ssize_t n = ::read(fd_.get(), &reply, sizeof reply);
    if (n < 0)
        return std::unexpected(PortError::read_failed);

    constexpr auto header = offsetof(modbus_record, data);
    if (static_cast<std::size_t>(n) < header || reply.len > MODBUS_MAX_ADU ||
        static_cast<std::size_t>(n) < header + reply.len)
        return std::unexpected(PortError::short_record);
    return {};
}
