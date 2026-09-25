#include "support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <execinfo.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace tiny::hosts {

namespace {

thread_local bool t_armed = false;
std::atomic<long> g_trapped{0};

} // namespace

auto trap_armed() -> bool& { return t_armed; }
auto trapped_allocations() -> long { return g_trapped.load(); }

auto pump_main(std::chrono::microseconds duration) -> void
{
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
#else
    (void)duration;
#endif
}

auto parse_options(int argc, char** argv) -> Options
{
    auto options = Options{};
    for (auto i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) options.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) options.chaos_seconds = std::strtod(argv[++i], nullptr);
        else options.bundle = argv[i];
    }
    return options;
}

} // namespace tiny::hosts

// The process's operator new: counts, when armed, then allocates normally.
auto operator new(std::size_t n) -> void*
{
    if (tiny::hosts::t_armed) {
        // The first few get a stack, written straight to stderr: backtrace_symbols_fd doesn't allocate.
        if (tiny::hosts::g_trapped.fetch_add(1, std::memory_order_relaxed) < 3) {
            void* frames[32];
            const auto n = backtrace(frames, 32);
            static constexpr char header[] = "host: allocation on the audio thread:\n";
            (void)!write(STDERR_FILENO, header, sizeof(header) - 1);
            backtrace_symbols_fd(frames, n, STDERR_FILENO);
        }
    }
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc{};
}
auto operator new[](std::size_t n) -> void* { return ::operator new(n); }
auto operator delete(void* p) noexcept -> void { std::free(p); }
auto operator delete[](void* p) noexcept -> void { std::free(p); }
auto operator delete(void* p, std::size_t) noexcept -> void { std::free(p); }
auto operator delete[](void* p, std::size_t) noexcept -> void { std::free(p); }
