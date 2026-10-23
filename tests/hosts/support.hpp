// Shared by the fake hosts: the audio-thread allocation trap, seeded randomness and the main run
// loop. Each host executable compiles support.cpp once, which replaces the global operator new.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

// One seed per run, printed, taken from --seed when given. `--fixtures DIR` checks the sessions saved
// there; `--write-fixtures` saves them instead, from whatever build is running. `--preset FILE` loads
// one exported native preset and checks it against DIR/<stem>.<format>.expect, and nothing else.
struct Options {
    uint32_t seed{1};
    double chaos_seconds{2};
    std::string bundle{};
    std::string fixtures{};
    std::string preset{};
    bool write_fixtures{};
};
auto parse_options(int argc, char** argv) -> Options;

// MARK: - session fixtures

// A session saved by an older build, and what loading it must give. Checked in under
// tests/fixtures/sessions as <Demo>.<format>.state (the format's bytes) and .expect (text).
struct Session_fixture {
    std::vector<uint8_t> state{};
    std::vector<uint8_t> extra{};                      // A second chunk, where the format has one (VST3's controller).
    std::vector<std::pair<uint32_t, double>> params{}; // Host-visible value per parameter id after the load.
    int32_t width{}, height{};                         // The editor size it opens at; 0 when not checked.
    std::vector<std::string> texts{};                  // Strings a save after the load must contain (the view's).
};

// Whether `bytes` holds `text` anywhere: a name the view model keeps, in a re-saved state.
auto contains_text(std::span<const uint8_t> bytes, const std::string& text) -> bool;

// <dir>/<demo>.<format>, or empty when no fixture directory was given.
auto fixture_base(const Options& options, const std::string& demo, const std::string& format) -> std::filesystem::path;
auto write_fixture(const std::filesystem::path& base, const Session_fixture& fixture) -> void;
auto read_fixture(const std::filesystem::path& base) -> std::optional<Session_fixture>;

// An exported preset's expectations: <dir>/<preset stem>.<format>.expect, no state (the preset is it).
auto preset_base(const Options& options, const std::string& format) -> std::filesystem::path;
auto write_expect(const std::filesystem::path& base, const Session_fixture& fixture) -> void;
auto read_expect(const std::filesystem::path& base, Session_fixture& fixture) -> bool;
auto read_file(const std::filesystem::path& path) -> std::optional<std::vector<uint8_t>>;

// The first state record in `bytes`, header and payload, found by its magic; empty when there is none.
auto find_record(std::span<const uint8_t> bytes) -> std::vector<uint8_t>;

// A fixed, non-default value in [lo, hi] for the parameter at `index`; rounded when stepped.
auto fixture_value(size_t index, double lo, double hi, bool stepped) -> double;

// The editor size a fixture is saved at: not any demo's preferred size.
inline constexpr auto fixture_width = int32_t{613};
inline constexpr auto fixture_height = int32_t{411};

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
