#include "gpio_line.hpp"

#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>

static_assert(sizeof(gpio_v2_line_request) == 592);
static_assert(sizeof(gpio_v2_line_values) == 16);

std::expected<GpioLine, std::error_code> GpioLine::request_output(const char* chip_path,
                                                                  std::uint32_t offset,
                                                                  std::string_view consumer) {
    UniqueFd chip{::open(chip_path, O_RDWR | O_CLOEXEC)};
    if (!chip)
        return std::unexpected(std::error_code{errno, std::system_category()});

    gpio_v2_line_request req{};  // value-initialised: every field and padding byte zero
    req.offsets[0] = offset;
    req.num_lines = 1;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    const auto n = std::min(consumer.size(), sizeof(req.consumer) - 1);  // keep the NUL
    std::copy_n(consumer.data(), n, req.consumer);

    if (::ioctl(chip.get(), GPIO_V2_GET_LINE_IOCTL, &req) < 0)
        return std::unexpected(std::error_code{errno, std::system_category()});

    // The kernel returned a new fd for the line in req.fd. It is independent
    // of the chip fd, which `chip` closes when this function returns.
    GpioLine line{UniqueFd{req.fd}};
    line.set(false);
    return line;
}

void GpioLine::set(bool high) const noexcept {
    gpio_v2_line_values v{};
    v.mask = 1;              // bit 0 = the first (and only) line of this request
    v.bits = high ? 1u : 0u;
    (void)::ioctl(line_.get(), GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}
