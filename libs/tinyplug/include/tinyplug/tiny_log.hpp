#pragma once

// A general purpose, realtime-safe logging layer shared by all five wrappers.
//
// Two properties drive the design:
//
//   * It is called from the audio thread. Nothing here allocates, locks or formats on
//     the calling thread — a call captures its arguments into a fixed-size POD record,
//     pushes it onto a lock-free ring, and returns. A drain thread does the formatting
//     and the I/O.
//   * Plug-ins are not one process. An AUv3 extension, a distributable VST3 controller
//     and a Pro Tools instance may each be somewhere else, so the default sinks are the
//     ones that merge streams from every process: `os_log` on Apple,
//     `OutputDebugString` on Windows. See "Reading the log" below.
//
// Compiled out entirely unless TINY_LOG_ENABLED — the macros expand to `((void)0)` and
// their arguments are never evaluated. The CMake option `TINY_LOG` defaults to ON for
// Debug builds and OFF otherwise.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#ifndef TINY_LOG_ENABLED
    #define TINY_LOG_ENABLED 0
#endif

namespace tiny::log {

// MARK: - taxonomy

enum class Level : uint8_t { trace, debug, info, warn, error, off };

// A bitmask, so a probing session can ask for one subsystem and get nothing else.
enum class Cat : uint32_t {
    lifecycle = 1u << 0, // configure / clear / snap / activate.
    latency   = 1u << 1, // the propose -> notify -> accept handshake.
    params    = 1u << 2,
    state     = 1u << 3, // chunks, presets, host loads.
    worker    = 1u << 4,
    editor    = 1u << 5,
    process   = 1u << 6, // per-block. Trace level: floods by nature.
    host      = 1u << 7, // what the host asked of us.
    graphics  = 1u << 8,
    general   = 1u << 9,
};

constexpr auto operator|(Cat a, Cat b) noexcept -> Cat
{
    return static_cast<Cat>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

// Which thread a record came from. Tagged explicitly — there is no portable way to ask.
enum class Thread_role : uint8_t { unknown, main, audio, worker, background };

// MARK: - captured arguments

inline constexpr auto max_inline_text = size_t{24};
inline constexpr auto max_args = size_t{6};

// One captured argument. Trivially copyable by construction: a dynamic string is copied
// into `inline_text` (truncated) rather than stored by pointer, because the caller's
// buffer is long gone by the time the drain thread formats the line.
struct Arg {
    enum class Kind : uint8_t { none, sint, uint, real, boolean, text, inline_text, pointer };

    union Store {
        int64_t sint;
        uint64_t uint;
        double real;
        bool boolean;
        const char* text;
        const void* pointer;
        char inline_text[max_inline_text];

        constexpr Store() noexcept : uint{} {}
    };

    Kind kind{Kind::none};
    bool engaged{true}; // A disengaged optional formats as "-".
    Store store{};
};

// One log line, before it is a line. POD so the ring can memcpy it.
struct Record {
    int64_t time_us{};
    const char* fmt{};
    const char* site{};
    const char* tag{}; // Whoever emitted it: "VST3/proc", "AAX/alg", ...
    uint32_t process_id{};
    uint32_t thread_id{};
    uint16_t instance{};
    Thread_role role{};
    Cat cat{};
    Level level{};
    uint8_t num_args{};
    Arg args[max_args]{};
};

// MARK: - lifetime and configuration

// Idempotent. Reads the environment, opens the sinks and starts the drain thread. Called
// for you by the first log call and by every `Probe` constructor — construct those off
// the audio thread and the audio thread never pays for initialization.
auto init() -> void;

// Blocks until everything queued so far has reached the sinks. Never call from audio.
auto flush() -> void;

// Names the emitting module for records that carry no `Probe` tag.
auto set_process_tag(const char* tag) -> void;

// Tags the calling thread for every subsequent record it emits.
auto set_thread_role(Thread_role role) -> void;

auto set_level(Level level) -> void;
auto set_categories(Cat cats) -> void;

namespace detail {

// Defined in the .cpp with permissive defaults so records emitted before `init` are kept
// rather than silently dropped; `init` then narrows them to whatever the environment says.
extern std::atomic<uint32_t> filter_cats;
extern std::atomic<uint8_t> filter_level;

}

// Cheap enough to sit in front of every call site: two relaxed loads and a branch.
inline auto enabled(Cat cat, Level level) noexcept -> bool
{
    if (static_cast<uint8_t>(level) < detail::filter_level.load(std::memory_order_relaxed)) return false;
    return (static_cast<uint32_t>(cat) & detail::filter_cats.load(std::memory_order_relaxed)) != 0;
}

// How many records the ring has dropped. Non-zero means the drain thread fell behind.
auto dropped() noexcept -> uint64_t;

// MARK: - emitting

namespace detail {

auto submit(const Record& record) noexcept -> void;
auto now_us() noexcept -> int64_t;
auto this_process_id() noexcept -> uint32_t;
auto this_thread_id() noexcept -> uint32_t;
auto this_thread_role() noexcept -> Thread_role;
auto process_tag() noexcept -> const char*;
auto copy_text(Arg& arg, std::string_view text) noexcept -> void;

template<typename T>
auto capture(const T& value) noexcept -> Arg
{
    auto arg = Arg{};

    using Bare = std::remove_cvref_t<T>;

    if constexpr (requires { typename Bare::value_type; requires std::is_same_v<Bare, std::optional<typename Bare::value_type>>; }) {
        // Latency lives in optionals throughout the framework, and "absent" is the
        // interesting half of the state — never print it as a zero.
        if (!value) {
            arg.kind = Arg::Kind::none;
            arg.engaged = false;
            return arg;
        }
        return capture(*value);
    }
    else if constexpr (std::is_same_v<Bare, bool>) {
        arg.kind = Arg::Kind::boolean;
        arg.store.boolean = value;
    }
    else if constexpr (std::is_enum_v<Bare>) {
        arg.kind = Arg::Kind::sint;
        arg.store.sint = static_cast<int64_t>(static_cast<std::underlying_type_t<Bare>>(value));
    }
    else if constexpr (std::is_floating_point_v<Bare>) {
        arg.kind = Arg::Kind::real;
        arg.store.real = static_cast<double>(value);
    }
    else if constexpr (std::is_integral_v<Bare> && std::is_signed_v<Bare>) {
        arg.kind = Arg::Kind::sint;
        arg.store.sint = static_cast<int64_t>(value);
    }
    else if constexpr (std::is_integral_v<Bare>) {
        arg.kind = Arg::Kind::uint;
        arg.store.uint = static_cast<uint64_t>(value);
    }
    else if constexpr (std::is_same_v<Bare, const char*> || std::is_same_v<Bare, char*>) {
        // Assumed to outlive the drain. Pass a string_view for anything dynamic.
        arg.kind = Arg::Kind::text;
        arg.store.text = value;
    }
    else if constexpr (std::is_array_v<Bare>) {
        arg.kind = Arg::Kind::text;
        arg.store.text = static_cast<const char*>(value);
    }
    else if constexpr (std::is_convertible_v<const Bare&, std::string_view>) {
        detail::copy_text(arg, std::string_view{value});
    }
    else if constexpr (std::is_pointer_v<Bare>) {
        arg.kind = Arg::Kind::pointer;
        arg.store.pointer = value;
    }
    else {
        static_assert(sizeof(Bare) == 0, "tiny::log cannot capture this type.");
    }

    return arg;
}

} // namespace detail

// The one entry point. `fmt` uses `{}` placeholders and must outlive the call — a string
// literal in practice, since it is stored by pointer and formatted later.
template<typename... Args>
auto write(Cat cat, Level level, const char* site, const char* tag, uint16_t instance, const char* fmt, const Args&... args) noexcept -> void
{
    static_assert(sizeof...(Args) <= max_args, "tiny::log takes at most max_args arguments.");

    if (!enabled(cat, level)) return;

    auto record = Record{};
    record.time_us = detail::now_us();
    record.fmt = fmt;
    record.site = site;
    record.tag = tag ? tag : detail::process_tag();
    record.process_id = detail::this_process_id();
    record.thread_id = detail::this_thread_id();
    record.instance = instance;
    record.role = detail::this_thread_role();
    record.cat = cat;
    record.level = level;
    record.num_args = static_cast<uint8_t>(sizeof...(Args));

    auto index = size_t{};
    ((record.args[index++] = detail::capture(args)), ...);

    detail::submit(record);
}

// MARK: - scopes

// Logs on entry and on exit with the elapsed time, so nesting and ordering across the
// wrapper's call graph are readable at a glance.
class Scope {
public:

    Scope(Cat cat, Level level, const char* site, const char* tag, uint16_t instance, const char* name) noexcept;
    ~Scope();

    Scope(const Scope&) = delete;
    auto operator=(const Scope&) -> Scope& = delete;

private:

    const char* _site{};
    const char* _tag{};
    const char* _name{};
    int64_t _entered_us{};
    uint16_t _instance{};
    Cat _cat{};
    Level _level{};
    bool _live{};
};

} // namespace tiny::log

// MARK: - macros

#define TINY_LOG_STR2(x) #x
#define TINY_LOG_STR(x) TINY_LOG_STR2(x)
#define TINY_LOG_SITE __FILE__ ":" TINY_LOG_STR(__LINE__)

#if TINY_LOG_ENABLED

    #define TINY_LOG(cat, level, fmt, ...) \
        ::tiny::log::write(::tiny::log::Cat::cat, ::tiny::log::Level::level, TINY_LOG_SITE, nullptr, 0, fmt __VA_OPT__(,) __VA_ARGS__)

    #define TINY_LOG_TRACE(cat, fmt, ...) TINY_LOG(cat, trace, fmt __VA_OPT__(,) __VA_ARGS__)
    #define TINY_LOG_DEBUG(cat, fmt, ...) TINY_LOG(cat, debug, fmt __VA_OPT__(,) __VA_ARGS__)
    #define TINY_LOG_INFO(cat, fmt, ...)  TINY_LOG(cat, info,  fmt __VA_OPT__(,) __VA_ARGS__)
    #define TINY_LOG_WARN(cat, fmt, ...)  TINY_LOG(cat, warn,  fmt __VA_OPT__(,) __VA_ARGS__)
    #define TINY_LOG_ERROR(cat, fmt, ...) TINY_LOG(cat, error, fmt __VA_OPT__(,) __VA_ARGS__)

    #define TINY_LOG_CAT2(a, b) a##b
    #define TINY_LOG_CAT(a, b) TINY_LOG_CAT2(a, b)

    #define TINY_LOG_SCOPE(cat, name) \
        const auto TINY_LOG_CAT(tiny_log_scope_, __LINE__) = \
            ::tiny::log::Scope{::tiny::log::Cat::cat, ::tiny::log::Level::debug, TINY_LOG_SITE, nullptr, 0, name}

    // Tags the calling thread once, then costs a thread-local read per call.
    #define TINY_LOG_THREAD(role) \
        do { \
            static thread_local const auto tiny_log_tagged = (::tiny::log::set_thread_role(::tiny::log::Thread_role::role), true); \
            (void)tiny_log_tagged; \
        } while (false)

#else

    #define TINY_LOG(cat, level, fmt, ...) ((void)0)
    #define TINY_LOG_TRACE(cat, fmt, ...) ((void)0)
    #define TINY_LOG_DEBUG(cat, fmt, ...) ((void)0)
    #define TINY_LOG_INFO(cat, fmt, ...) ((void)0)
    #define TINY_LOG_WARN(cat, fmt, ...) ((void)0)
    #define TINY_LOG_ERROR(cat, fmt, ...) ((void)0)
    #define TINY_LOG_SCOPE(cat, name) ((void)0)
    #define TINY_LOG_THREAD(role) ((void)0)

#endif
