// unique_fd.hpp -- RAII owner for one POSIX file descriptor.
//
// The C master had to remember close() on every exit path. Here the
// destructor does it, so an early return can never leak a descriptor.
// Move-only: copying would give two owners and therefore a double close.
#pragma once

#include <unistd.h>

#include <type_traits>
#include <utility>

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_{fd} {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    // Move: steal the descriptor, leave the source empty (-1) so its
    // destructor does nothing.
    UniqueFd(UniqueFd&& other) noexcept : fd_{std::exchange(other.fd_, -1)} {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other)  // a = std::move(a) must not close our own fd
            reset(std::exchange(other.fd_, -1));
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

    // Give up ownership without closing (the caller now owns it).
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    // Close what we hold (if anything) and take ownership of `fd`.
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

static_assert(!std::is_copy_constructible_v<UniqueFd>);
static_assert(!std::is_copy_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);
