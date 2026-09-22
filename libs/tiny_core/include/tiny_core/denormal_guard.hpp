#pragma once

#include <cstdint>

#include "platform_defs.hpp"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define TINY_DENORMALS_SSE 1
    #include <xmmintrin.h>
    #include <pmmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define TINY_DENORMALS_ARM64 1
#endif

namespace tiny {

// Flushes denormals to zero for the guard's lifetime, restoring the host's control
// register on scope exit. Construct at the top of each wrapper's render callback.
class Denormal_guard {
public:

    Denormal_guard()
    {
#if TINY_DENORMALS_SSE
        _saved = _mm_getcsr();
        // 0x8040 = _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON.
        _mm_setcsr(_saved | 0x8040u);
#elif TINY_DENORMALS_ARM64
        _saved = read_fpcr();
        write_fpcr(_saved | fz_bit); // FZ (bit 24): flush-to-zero.
#endif
    }

    ~Denormal_guard()
    {
#if TINY_DENORMALS_SSE
        _mm_setcsr(_saved);
#elif TINY_DENORMALS_ARM64
        write_fpcr(_saved);
#endif
    }

    Denormal_guard(const Denormal_guard&) = delete;
    Denormal_guard(Denormal_guard&&) = delete;
    auto operator=(const Denormal_guard&) -> Denormal_guard& = delete;
    auto operator=(Denormal_guard&&) -> Denormal_guard& = delete;

private:

#if TINY_DENORMALS_SSE
    unsigned int _saved{};
#elif TINY_DENORMALS_ARM64
    static constexpr auto fz_bit = uint64_t{1} << 24;

    uint64_t _saved{};

    static auto read_fpcr() -> uint64_t
    {
        auto value = uint64_t{};
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
        return value;
    }

    static auto write_fpcr(uint64_t value) -> void
    {
        __asm__ __volatile__("msr fpcr, %0" : : "r"(value));
    }
#endif

};

} // namespace tiny
