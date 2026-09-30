// Shared by the fake hosts: the audio-thread allocation trap, seeded randomness and the main run
// loop. Each host executable compiles support.cpp once, which replaces the global operator new.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <string>

namespace tiny::hosts {

// Counts allocations made on this thread while armed, inside the plug-in too: the host
// executable's operator new is the process's operator new (on Windows, ASan's malloc hook).
// One thread armed at a time.
auto arm_trap(bool on) -> void;
auto trapped_allocations() -> long;

struct Trap_scope {
    Trap_scope() { arm_trap(true); }
    ~Trap_scope() { arm_trap(false); }
    Trap_scope(const Trap_scope&) = delete;
    auto operator=(const Trap_scope&) -> Trap_scope& = delete;
};

// Run the main run loop (dispatch's main queue with it) for `duration`, as a host's main thread
// would between calls. Relay delivers there. On Windows, the thread's message queue.
auto pump_main(std::chrono::microseconds duration) -> void;

// A plug-in's code, loaded and never unloaded (macOS keeps Objective-C images anyway). `path` is a
// bundle directory, resolved to the binary inside it, or the binary itself.
class Library {
public:
    explicit Library(const std::string& path);
    auto symbol(const char* name) const -> void*;
private:
    void* _handle{};
};

// The bundle's name without its extension: "GainDemo" for ".../GainDemo.clap".
auto bundle_name(const std::string& path) -> std::string;

// One seed per run, printed, taken from --seed when given.
struct Options {
    uint32_t seed{1};
    double chaos_seconds{2};
    std::string bundle{};
};
auto parse_options(int argc, char** argv) -> Options;

// Uniform helpers over one engine.
class Random {
public:
    explicit Random(uint32_t seed) : _engine{seed} {}
    auto below(uint32_t n) -> uint32_t { return n == 0 ? 0 : std::uniform_int_distribution<uint32_t>{0, n - 1}(_engine); }
    auto real(double lo, double hi) -> double { return std::uniform_real_distribution<double>{lo, hi}(_engine); }
    auto chance(double p) -> bool { return real(0, 1) < p; }
private:
    std::mt19937 _engine;
};

} // namespace tiny::hosts
