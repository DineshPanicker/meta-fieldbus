// modbus_frame.hpp -- Modbus RTU frame encoding/decoding, evaluated at
// compile time where possible. No OS dependencies: testable on any host.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace modbus {

// CRC-16/MODBUS: init 0xFFFF, reflected polynomial 0xA001, no final XOR.
constexpr std::uint16_t crc16(std::span<const std::uint8_t> bytes) noexcept {
    std::uint16_t crc = 0xFFFF;
    for (const std::uint8_t b : bytes) {
        crc ^= b;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) ? static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u)
                             : static_cast<std::uint16_t>(crc >> 1);
    }
    return crc;
}

// Bench vector: request 01 03 00 00 00 04 carries CRC bytes 44 09 on the wire,
// i.e. CRC value 0x0944 sent low byte first.
static_assert(crc16(std::array<std::uint8_t, 6>{0x01, 0x03, 0x00, 0x00, 0x00, 0x04}) == 0x0944);

enum class FunctionCode : std::uint8_t {
    read_holding_registers = 0x03,
};

inline constexpr std::uint8_t kExceptionBit = 0x80;

// A complete 8-byte "read holding registers" request, CRC included.
// Address and count are big-endian; the CRC is little-endian.
constexpr std::array<std::uint8_t, 8> make_read_holding(std::uint8_t slave, std::uint16_t first,
                                                         std::uint16_t count) noexcept {
    std::array<std::uint8_t, 8> f{
        slave,
        std::to_underlying(FunctionCode::read_holding_registers),
        static_cast<std::uint8_t>(first >> 8),
        static_cast<std::uint8_t>(first & 0xFF),
        static_cast<std::uint8_t>(count >> 8),
        static_cast<std::uint8_t>(count & 0xFF),
        0,
        0,
    };
    const std::uint16_t crc = crc16(std::span{f}.first<6>());
    f[6] = static_cast<std::uint8_t>(crc & 0xFF);
    f[7] = static_cast<std::uint8_t>(crc >> 8);
    return f;
}

static_assert(make_read_holding(1, 0, 4) ==
              std::array<std::uint8_t, 8>{0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0x44, 0x09});

enum class ParseError {
    too_short,        // fewer than 5 bytes: cannot even hold an exception reply
    bad_crc,          // trailing CRC does not match the body
    wrong_slave,      // answered by a different address
    exception_reply,  // slave answered 0x83 + exception code
    wrong_function,   // some other function code
    bad_count,        // byte count / length does not match N registers
};

// Decode a 0x03 response carrying exactly N registers into `out`.
// The driver already checked the CRC; we check it again so this function is
// correct on its own (and so a host test can feed it raw bytes).
template <std::size_t N>
constexpr std::expected<void, ParseError> parse_read_holding(std::span<const std::uint8_t> frame,
                                                             std::uint8_t slave,
                                                             std::array<std::uint16_t, N>& out) noexcept {
    if (frame.size() < 5)
        return std::unexpected(ParseError::too_short);

    const auto body = frame.first(frame.size() - 2);
    const auto wire_crc = static_cast<std::uint16_t>(frame[frame.size() - 2] |
                                                     (frame[frame.size() - 1] << 8));
    if (crc16(body) != wire_crc)
        return std::unexpected(ParseError::bad_crc);
    if (frame[0] != slave)
        return std::unexpected(ParseError::wrong_slave);

    constexpr auto fc = std::to_underlying(FunctionCode::read_holding_registers);
    if (frame[1] == (kExceptionBit | fc))
        return std::unexpected(ParseError::exception_reply);
    if (frame[1] != fc)
        return std::unexpected(ParseError::wrong_function);
    if (frame[2] != 2 * N || frame.size() != 3 + 2 * N + 2)
        return std::unexpected(ParseError::bad_count);

    for (std::size_t i = 0; i < N; ++i)
        out[i] = static_cast<std::uint16_t>((frame[3 + 2 * i] << 8) | frame[4 + 2 * i]);
    return {};
}

// The reply captured on the bench must decode to 100, 200, 300, 400.
// Proven by the compiler, not by a test run.
constexpr bool bench_reply_decodes() {
    constexpr std::array<std::uint8_t, 13> reply{0x01, 0x03, 0x08, 0x00, 0x64, 0x00, 0xC8,
                                                 0x01, 0x2C, 0x01, 0x90, 0x90, 0x08};
    std::array<std::uint16_t, 4> regs{};
    return parse_read_holding(std::span{reply}, 1, regs).has_value() &&
           regs == std::array<std::uint16_t, 4>{100, 200, 300, 400};
}
static_assert(bench_reply_decodes());

}  // namespace modbus
