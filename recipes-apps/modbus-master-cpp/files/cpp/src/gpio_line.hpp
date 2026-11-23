// gpio_line.hpp -- one output line via the GPIO character device (uAPI v2).
//
// Replaces the sysfs interface. Offsets are relative to the chip, so BCM27 is
// offset 27 on /dev/gpiochip0 whatever global base (512 on 6.6+) the kernel
// assigned. No more "echo 539 > export".
#pragma once

#include "unique_fd.hpp"

#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

class GpioLine {
public:
    // Static factory instead of a constructor: without exceptions a
    // constructor has no way to report failure. Either you get a working
    // line or an error code; a half-built GpioLine cannot exist.
    static std::expected<GpioLine, std::error_code> request_output(const char* chip_path,
                                                                   std::uint32_t offset,
                                                                   std::string_view consumer);

    // One ioctl. No allocation, no string formatting: safe in the RT loop.
    void set(bool high) const noexcept;

private:
    explicit GpioLine(UniqueFd line) noexcept : line_{std::move(line)} {}
    UniqueFd line_;  // closing this fd hands the line back to the kernel
};
