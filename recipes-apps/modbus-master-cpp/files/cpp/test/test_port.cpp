// Tests ModbusPort::transact() without hardware.
//
// A SOCK_SEQPACKET socketpair stands in for /dev/modbus0: like the driver,
// it keeps message boundaries (one read() = one record), supports poll(), and
// returns EAGAIN when empty in non-blocking mode. A thread plays the driver.
#include "modbus_frame.hpp"
#include "modbus_port.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace std::chrono_literals;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

namespace {

constexpr auto kRequest = modbus::make_read_holding(1, 0, 4);
constexpr std::array<std::uint8_t, 13> kReply{0x01, 0x03, 0x08, 0x00, 0x64, 0x00, 0xC8,
                                              0x01, 0x2C, 0x01, 0x90, 0x90, 0x08};

struct Pair {
    UniqueFd master;  // what ModbusPort gets
    UniqueFd driver;  // what the fake driver thread uses
};

Pair make_pair() {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
    Pair p{UniqueFd{sv[0]}, UniqueFd{sv[1]}};
    // master side non-blocking, exactly like ModbusPort::open() does
    CHECK(::fcntl(p.master.get(), F_SETFL, O_NONBLOCK) == 0);
    return p;
}

// Fake driver: wait for one request, then reply after `delay` with registers
// tagged by `tag` in the timestamp field.
void fake_driver(int fd, std::chrono::milliseconds delay, std::uint64_t tag) {
    std::array<std::uint8_t, 16> req{};
    const ssize_t n = ::read(fd, req.data(), req.size());  // blocking side
    CHECK(n == 8 && std::memcmp(req.data(), kRequest.data(), 8) == 0);
    std::this_thread::sleep_for(delay);
    modbus_record rec{};
    rec.ts_ns = tag;
    rec.len = kReply.size();
    std::memcpy(rec.data, kReply.data(), kReply.size());
    CHECK(::write(fd, &rec, sizeof rec) == static_cast<ssize_t>(sizeof rec));
}

void test_normal_reply() {
    auto [m, d] = make_pair();
    auto port = ModbusPort::adopt(std::move(m));
    std::thread drv{fake_driver, d.get(), 1ms, 1u};
    modbus_record rec{};
    auto res = port.transact(kRequest, 50ms, rec);
    drv.join();
    CHECK(res.has_value());
    CHECK(rec.ts_ns == 1 && rec.len == 13);
    std::array<std::uint16_t, 4> regs{};
    CHECK(modbus::parse_read_holding(std::span{rec.data, rec.len}, 1, regs));
    CHECK(regs[2] == 300);
    std::puts("port: normal reply ok");
}

void test_timeout_then_stale_drain() {
    auto [m, d] = make_pair();
    auto port = ModbusPort::adopt(std::move(m));
    modbus_record rec{};

    // Cycle 1: the reply comes 30 ms late; we give up after 5 ms.
    std::thread late{fake_driver, d.get(), 30ms, 111u};
    const auto t0 = std::chrono::steady_clock::now();
    auto r1 = port.transact(kRequest, 5ms, rec);
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(!r1 && r1.error() == PortError::timeout);
    CHECK(waited < 20ms);  // bounded: did not block until the late reply
    late.join();           // the late reply (tag 111) is now sitting in the queue

    // Cycle 2: must drain tag 111 and return the fresh reply (tag 222).
    std::thread fresh{fake_driver, d.get(), 1ms, 222u};
    auto r2 = port.transact(kRequest, 50ms, rec);
    fresh.join();
    CHECK(r2.has_value());
    CHECK(rec.ts_ns == 222);
    CHECK(port.stale_drained() == 1);
    std::puts("port: timeout + stale drain ok");
}

void test_short_record() {
    auto [m, d] = make_pair();
    auto port = ModbusPort::adopt(std::move(m));
    std::thread drv{[fd = d.get()] {
        std::array<std::uint8_t, 16> req{};
        CHECK(::read(fd, req.data(), req.size()) == 8);
        const std::array<std::uint8_t, 6> junk{};  // shorter than the record header
        CHECK(::write(fd, junk.data(), junk.size()) == 6);
    }};
    modbus_record rec{};
    auto res = port.transact(kRequest, 50ms, rec);
    drv.join();
    CHECK(!res && res.error() == PortError::short_record);
    std::puts("port: short record rejected ok");
}

}  // namespace

int main() {
    test_normal_reply();
    test_timeout_then_stale_drain();
    test_short_record();
    std::puts("port: all tests passed");
    return 0;
}
