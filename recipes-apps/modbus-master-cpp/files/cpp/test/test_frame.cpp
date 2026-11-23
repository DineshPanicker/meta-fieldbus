// Runtime tests for the error paths of modbus_frame.hpp.
// The happy paths are already proven by static_assert at compile time.
#include "modbus_frame.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

int main() {
    using modbus::ParseError;
    std::array<std::uint16_t, 4> regs{};

    const std::array<std::uint8_t, 13> good{0x01, 0x03, 0x08, 0x00, 0x64, 0x00, 0xC8,
                                            0x01, 0x2C, 0x01, 0x90, 0x90, 0x08};
    CHECK(modbus::parse_read_holding(std::span{good}, 1, regs));
    CHECK(regs[0] == 100 && regs[3] == 400);

    auto flipped = good;
    flipped[4] ^= 0x01;  // one bit error on the wire
    CHECK(modbus::parse_read_holding(std::span{flipped}, 1, regs).error() == ParseError::bad_crc);

    CHECK(modbus::parse_read_holding(std::span{good}, 2, regs).error() == ParseError::wrong_slave);
    CHECK(modbus::parse_read_holding(std::span{good}.first(4), 1, regs).error() == ParseError::too_short);

    // Exception reply: 01 83 02 <crc> (illegal data address).
    std::array<std::uint8_t, 5> exc{0x01, 0x83, 0x02, 0, 0};
    const auto c = modbus::crc16(std::span{exc}.first<3>());
    exc[3] = static_cast<std::uint8_t>(c & 0xFF);
    exc[4] = static_cast<std::uint8_t>(c >> 8);
    CHECK(modbus::parse_read_holding(std::span{exc}, 1, regs).error() == ParseError::exception_reply);

    // Asking for 2 registers when the reply carries 4.
    std::array<std::uint16_t, 2> two{};
    CHECK(modbus::parse_read_holding(std::span{good}, 1, two).error() == ParseError::bad_count);

    std::puts("frame: all tests passed");
    return 0;
}
