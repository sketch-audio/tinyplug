#include "tinyplug/tiny_log.hpp"

#include "tinyplug/platform_defs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#if TINY_PLATFORM_APPLE
    #include <fcntl.h>
    #include <os/log.h>
    #include <unistd.h>
#elif TINY_PLATFORM_WINDOWS
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <io.h>
#endif

namespace tiny::log {

namespace {

constexpr auto ring_slots = size_t{1024}; // Power of two. ~250 KB of records.
constexpr auto max_line = size_t{1024};
[[maybe_unused]] constexpr auto subsystem = "com.tinyplug.log";

constexpr auto level_names = std::array<const char*, 6>{"trace", "debug", "info", "warn", "error", "off"};
constexpr auto level_marks = std::array<char, 6>{'T', 'D', 'I', 'W', 'E', '-'};

constexpr auto role_names = std::array<const char*, 5>{"?", "main", "audio", "worker", "bg"};

struct Cat_name {
    Cat cat;
    const char* name;
};

constexpr auto cat_names = std::array<Cat_name, 10>{{
    {Cat::lifecycle, "lifecycle"},
    {Cat::latency, "latency"},
    {Cat::params, "params"},
    {Cat::state, "state"},
    {Cat::worker, "worker"},
    {Cat::editor, "editor"},
    {Cat::process, "process"},
    {Cat::host, "host"},
    {Cat::graphics, "graphics"},
    {Cat::general, "general"},
}};

auto name_of(Cat cat) noexcept -> const char*
{
    for (const auto& entry : cat_names) {
        if (entry.cat == cat) return entry.name;
    }
    return "?";
}

// MARK: - ring

// A bounded MPMC ring (Vyukov). Deliberately not `Lock_free_queue<..., mpsc>`: that one
// keeps a fixed registry of writer threads that is never reclaimed, and a logger is
// called from whatever threads the host happens to own over a long session.
class Ring {
public:

    Ring()
    {
        for (auto i = size_t{}; i < ring_slots; ++i) {
            _cells[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    auto push(const Record& record) noexcept -> bool
    {
        auto pos = _enqueue.load(std::memory_order_relaxed);

        for (;;) {
            auto& cell = _cells[pos & mask];
            const auto seq = cell.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);

            if (diff == 0) {
                if (_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    cell.record = record;
                    cell.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0) {
                return false; // Full.
            }
            else {
                pos = _enqueue.load(std::memory_order_relaxed);
            }
        }
    }

    auto pop(Record& out) noexcept -> bool
    {
        auto pos = _dequeue.load(std::memory_order_relaxed);

        for (;;) {
            auto& cell = _cells[pos & mask];
            const auto seq = cell.seq.load(std::memory_order_acquire);
            const auto diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);

            if (diff == 0) {
                if (_dequeue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = cell.record;
                    cell.seq.store(pos + ring_slots, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0) {
                return false; // Empty.
            }
            else {
                pos = _dequeue.load(std::memory_order_relaxed);
            }
        }
    }

    auto enqueued() const noexcept -> uint64_t { return _enqueue.load(std::memory_order_acquire); }
    auto dequeued() const noexcept -> uint64_t { return _dequeue.load(std::memory_order_acquire); }

private:

    static constexpr auto mask = ring_slots - 1;

    struct Cell {
        std::atomic<uint64_t> seq{};
        Record record{};
    };

    alignas(64) std::atomic<uint64_t> _enqueue{};
    alignas(64) std::atomic<uint64_t> _dequeue{};
    std::array<Cell, ring_slots> _cells{};
};

// MARK: - formatting

auto append(std::string& out, std::string_view text) -> void
{
    if (out.size() + text.size() > max_line) {
        text = text.substr(0, max_line > out.size() ? max_line - out.size() : 0);
    }
    out.append(text);
}

auto append_arg(std::string& out, const Arg& arg) -> void
{
    auto scratch = std::array<char, 64>{};

    if (!arg.engaged) {
        append(out, "-");
        return;
    }

    switch (arg.kind) {
        case Arg::Kind::none:
            append(out, "-");
            break;
        case Arg::Kind::sint:
            std::snprintf(scratch.data(), scratch.size(), "%lld", static_cast<long long>(arg.store.sint));
            append(out, scratch.data());
            break;
        case Arg::Kind::uint:
            std::snprintf(scratch.data(), scratch.size(), "%llu", static_cast<unsigned long long>(arg.store.uint));
            append(out, scratch.data());
            break;
        case Arg::Kind::real:
            std::snprintf(scratch.data(), scratch.size(), "%.6g", arg.store.real);
            append(out, scratch.data());
            break;
        case Arg::Kind::boolean:
            append(out, arg.store.boolean ? "true" : "false");
            break;
        case Arg::Kind::text:
            append(out, arg.store.text ? arg.store.text : "(null)");
            break;
        case Arg::Kind::inline_text:
            append(out, arg.store.inline_text);
            break;
        case Arg::Kind::pointer:
            std::snprintf(scratch.data(), scratch.size(), "%p", arg.store.pointer);
            append(out, scratch.data());
            break;
    }
}

// `{}` substitution, `{{` and `}}` escapes. Surplus placeholders print as `{?}` and
// surplus arguments are appended, so a mismatched call is visible rather than silent.
auto format_message(std::string& out, const Record& record) -> void
{
    const auto* fmt = record.fmt ? record.fmt : "";
    auto next = size_t{};

    for (auto i = size_t{}; fmt[i] != '\0'; ++i) {
        if (fmt[i] == '{' && fmt[i + 1] == '{') {
            out.push_back('{');
            ++i;
        }
        else if (fmt[i] == '}' && fmt[i + 1] == '}') {
            out.push_back('}');
            ++i;
        }
        else if (fmt[i] == '{' && fmt[i + 1] == '}') {
            if (next < record.num_args) {
                append_arg(out, record.args[next++]);
            }
            else {
                append(out, "{?}");
            }
            ++i;
        }
        else {
            out.push_back(fmt[i]);
        }
    }

    for (; next < record.num_args; ++next) {
        append(out, " ");
        append_arg(out, record.args[next]);
    }
}

auto basename_of(const char* path) noexcept -> const char*
{
    if (!path) return "";

    const auto* last = path;
    for (const auto* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

auto append_timestamp(std::string& out, int64_t time_us) -> void
{
    const auto seconds = static_cast<std::time_t>(time_us / 1000000);
    const auto micros = static_cast<int>(time_us % 1000000);

    auto parts = std::tm{};
#if TINY_PLATFORM_WINDOWS
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif

    auto scratch = std::array<char, 32>{};
    std::snprintf(scratch.data(), scratch.size(), "%02d:%02d:%02d.%06d",
                  parts.tm_hour, parts.tm_min, parts.tm_sec, micros);
    append(out, scratch.data());
}

auto format_line(std::string& out, const Record& record) -> void
{
    out.clear();

    append_timestamp(out, record.time_us);

    auto scratch = std::array<char, 128>{};

    // "CLAP#3" — instance numbers restart per process, so they only mean anything read
    // together with the pid in the next column.
    auto source = std::array<char, 32>{};
    if (record.instance != 0) {
        std::snprintf(source.data(), source.size(), "%s#%u",
                      record.tag ? record.tag : "tinyplug", static_cast<unsigned>(record.instance));
    }
    else {
        std::snprintf(source.data(), source.size(), "%s", record.tag ? record.tag : "tinyplug");
    }

    std::snprintf(scratch.data(), scratch.size(), "  %c  %-14s %6u:%04x/%-6s %-9s  ",
                  level_marks[static_cast<size_t>(record.level)],
                  source.data(),
                  static_cast<unsigned>(record.process_id),
                  static_cast<unsigned>(record.thread_id & 0xffffu),
                  role_names[static_cast<size_t>(record.role)],
                  name_of(record.cat));
    append(out, scratch.data());

    format_message(out, record);

    // Trailing so the message column stays readable; still clickable in most terminals.
    // Probe lines carry no site — their tag and verb already say where they came from.
    if (record.site) {
        append(out, "   @");
        append(out, basename_of(record.site));
    }
}

// MARK: - environment

auto env_value(const char* name) -> std::string
{
#if TINY_PLATFORM_WINDOWS
    auto buffer = std::array<char, 512>{};
    const auto length = GetEnvironmentVariableA(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::string{buffer.data(), length};
#else
    const auto* value = std::getenv(name);
    return value ? std::string{value} : std::string{};
#endif
}

#if TINY_PLATFORM_APPLE
// Apple routes a GUI process's stderr into the unified log, which is where the os_log sink
// already writes — so with both enabled every line appears twice in a `log stream`. A
// process with a controlling terminal is not treated that way and can safely have both.
auto has_controlling_terminal() -> bool
{
    const auto fd = ::open("/dev/tty", O_RDONLY | O_NOCTTY);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}
#endif

auto level_from(std::string_view text, Level fallback) -> Level
{
    for (auto i = size_t{}; i < level_names.size(); ++i) {
        if (text == level_names[i]) return static_cast<Level>(i);
    }
    if (text == "1" || text == "on" || text == "yes") return Level::debug;
    if (text == "0" || text == "no") return Level::off;
    return fallback;
}

// Comma-separated category names, or "all".
auto cats_from(std::string_view text, uint32_t fallback) -> uint32_t
{
    if (text.empty()) return fallback;
    if (text == "all") return ~uint32_t{};

    auto mask = uint32_t{};
    auto start = size_t{};

    while (start <= text.size()) {
        const auto end = std::min(text.find(',', start), text.size());
        const auto token = text.substr(start, end - start);

        for (const auto& entry : cat_names) {
            if (token == entry.name) mask |= static_cast<uint32_t>(entry.cat);
        }
        start = end + 1;
    }

    return mask != 0 ? mask : fallback;
}

// MARK: - logger

class Logger {
public:

    Logger()
    {
        _configure();
        _open_sinks();
        _thread = std::thread{[this]() { this->_run(); }};
    }

    ~Logger()
    {
        {
            const auto lock = std::lock_guard{_mutex};
            _stop = true;
        }
        _wake.notify_all();
        if (_thread.joinable()) _thread.join();

        _drain(); // Anything that landed between the last drain and the join.

        if (_file) std::fclose(_file);
    }

    auto submit(const Record& record) noexcept -> void
    {
        if (!_ring.push(record)) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    auto flush() -> void
    {
        const auto target = _ring.enqueued();
        _wake.notify_all();

        // Bounded: a flush must never become a hang in a host's shutdown path.
        for (auto attempt = 0; attempt < 200; ++attempt) {
            if (_ring.dequeued() >= target) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    auto dropped() const noexcept -> uint64_t { return _dropped.load(std::memory_order_relaxed); }

    auto set_process_tag(const char* tag) -> void { _tag.store(tag, std::memory_order_relaxed); }
    auto process_tag() const noexcept -> const char* { return _tag.load(std::memory_order_relaxed); }

private:

    auto _configure() -> void
    {
        // Debug builds are the probing builds, so the layer is useful with no setup at
        // all in a DAW where exporting an environment variable is awkward.
#ifdef NDEBUG
        auto level = Level::warn;
#else
        auto level = Level::debug;
#endif
        auto cats = ~uint32_t{};

        level = level_from(env_value("TINYPLUG_LOG"), level);
        cats = cats_from(env_value("TINYPLUG_LOG_CATS"), cats);

        detail::filter_level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
        detail::filter_cats.store(cats, std::memory_order_relaxed);

        _to_syslog = env_value("TINYPLUG_LOG_SYSLOG") != "0";
        _file_path = env_value("TINYPLUG_LOG_FILE");

        // Explicit wins; otherwise avoid the double-logging described above.
        const auto stderr_env = env_value("TINYPLUG_LOG_STDERR");
        if (!stderr_env.empty()) {
            _to_stderr = stderr_env != "0";
        }
        else {
#if TINY_PLATFORM_APPLE
            _to_stderr = !_to_syslog || has_controlling_terminal();
#else
            _to_stderr = true;
#endif
        }
    }

    auto _open_sinks() -> void
    {
        if (!_file_path.empty()) {
            // Append mode, and one write per drain, so several processes (an AUv3
            // extension and its host, a distributable VST3's two halves) can share a file.
            _file = std::fopen(_file_path.c_str(), "ab");
        }

#if TINY_PLATFORM_APPLE
        for (auto i = size_t{}; i < cat_names.size(); ++i) {
            _os_logs[i] = os_log_create(subsystem, cat_names[i].name);
        }
#endif
    }

    auto _run() -> void
    {
        auto lock = std::unique_lock{_mutex};

        while (!_stop) {
            _wake.wait_for(lock, std::chrono::milliseconds{20});

            lock.unlock();
            _drain();
            lock.lock();
        }
    }

    auto _drain() -> void
    {
        auto record = Record{};
        auto wrote = false;

        _batch.clear();

        while (_ring.pop(record)) {
            format_line(_line, record);
            _emit_immediate(record, _line);

            _batch.append(_line);
            _batch.push_back('\n');
            wrote = true;

            if (_batch.size() > 32 * 1024) break; // Bound the burst; the next tick takes the rest.
        }

        if (!wrote) {
            _report_drops();
            return;
        }

        if (_to_stderr) {
            std::fwrite(_batch.data(), 1, _batch.size(), stderr);
            std::fflush(stderr);
        }
        if (_file) {
            std::fwrite(_batch.data(), 1, _batch.size(), _file);
            std::fflush(_file);
        }

        _report_drops();
    }

    // Sinks that take one line at a time rather than a batch.
    auto _emit_immediate(const Record& record, const std::string& line) -> void
    {
        if (!_to_syslog) return;

#if TINY_PLATFORM_APPLE
        auto handle = static_cast<os_log_t>(OS_LOG_DEFAULT);
        for (auto i = size_t{}; i < cat_names.size(); ++i) {
            if (cat_names[i].cat == record.cat && _os_logs[i]) handle = _os_logs[i];
        }

        // Mapped so the lifecycle/latency narrative is persisted and visible with a bare
        // `log show` / `log stream`: only OS_LOG_TYPE_DEFAULT and above are kept to disk.
        // The noisy tiers stay behind --info and --debug, which is what those flags are for.
        const auto type = record.level >= Level::error ? OS_LOG_TYPE_ERROR
                        : record.level >= Level::info  ? OS_LOG_TYPE_DEFAULT
                        : record.level >= Level::debug ? OS_LOG_TYPE_INFO
                                                       : OS_LOG_TYPE_DEBUG;
        os_log_with_type(handle, type, "%{public}s", line.c_str());
#elif TINY_PLATFORM_WINDOWS
        (void)record;
        OutputDebugStringA(line.c_str());
        OutputDebugStringA("\n");
#else
        (void)record;
        (void)line;
#endif
    }

    // A drop means the ring overflowed, which invalidates any ordering conclusion drawn
    // from the trace — so say so in the trace itself.
    auto _report_drops() -> void
    {
        const auto total = _dropped.load(std::memory_order_relaxed);
        if (total == _reported_drops) return;

        auto notice = std::array<char, 128>{};
        std::snprintf(notice.data(), notice.size(),
                      "*** tiny::log dropped %llu record(s) — ring overflow ***\n",
                      static_cast<unsigned long long>(total - _reported_drops));
        _reported_drops = total;

        if (_to_stderr) std::fwrite(notice.data(), 1, std::strlen(notice.data()), stderr);
        if (_file) std::fwrite(notice.data(), 1, std::strlen(notice.data()), _file);
    }

    Ring _ring{};
    std::atomic<uint64_t> _dropped{};
    uint64_t _reported_drops{};

    std::thread _thread{};
    std::mutex _mutex{};
    std::condition_variable _wake{};
    bool _stop{};

    std::atomic<const char*> _tag{"tinyplug"};

    std::string _line{};
    std::string _batch{};

    bool _to_stderr{true};
    bool _to_syslog{true};
    std::string _file_path{};
    std::FILE* _file{};

#if TINY_PLATFORM_APPLE
    std::array<os_log_t, cat_names.size()> _os_logs{};
#endif
};

auto logger() -> Logger&
{
    static auto instance = Logger{};
    return instance;
}

std::atomic<bool> g_ready{false};
std::once_flag g_once{};

auto ensure_ready() -> void
{
    std::call_once(g_once, []() {
        logger();
        g_ready.store(true, std::memory_order_release);
    });
}

thread_local auto t_role = Thread_role::unknown;
thread_local auto t_id = uint32_t{};

} // namespace

// MARK: - filters

namespace detail {

#ifdef NDEBUG
std::atomic<uint8_t> filter_level{static_cast<uint8_t>(Level::warn)};
#else
std::atomic<uint8_t> filter_level{static_cast<uint8_t>(Level::debug)};
#endif
std::atomic<uint32_t> filter_cats{~uint32_t{}};

} // namespace detail

// MARK: - api

auto init() -> void
{
    ensure_ready();
}

auto flush() -> void
{
    if (!g_ready.load(std::memory_order_acquire)) return;
    logger().flush();
}

auto set_process_tag(const char* tag) -> void
{
    ensure_ready();
    logger().set_process_tag(tag);
}

auto set_thread_role(Thread_role role) -> void
{
    t_role = role;
}

auto set_level(Level level) -> void
{
    detail::filter_level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

auto set_categories(Cat cats) -> void
{
    detail::filter_cats.store(static_cast<uint32_t>(cats), std::memory_order_relaxed);
}

auto dropped() noexcept -> uint64_t
{
    if (!g_ready.load(std::memory_order_acquire)) return 0;
    return logger().dropped();
}

namespace detail {

auto submit(const Record& record) noexcept -> void
{
    // Only the very first record can reach the one-time construction, and `Probe` (and
    // `init`) get there off the audio thread in every wrapper.
    ensure_ready();
    logger().submit(record);
}

auto now_us() noexcept -> int64_t
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

auto this_process_id() noexcept -> uint32_t
{
    static const auto pid = []() -> uint32_t {
#if TINY_PLATFORM_WINDOWS
        return static_cast<uint32_t>(GetCurrentProcessId());
#else
        return static_cast<uint32_t>(getpid());
#endif
    }();
    return pid;
}

auto this_thread_id() noexcept -> uint32_t
{
    if (t_id == 0) {
        const auto hashed = std::hash<std::thread::id>{}(std::this_thread::get_id());
        t_id = static_cast<uint32_t>(hashed) | 1u;
    }
    return t_id;
}

auto this_thread_role() noexcept -> Thread_role
{
    return t_role;
}

auto process_tag() noexcept -> const char*
{
    if (!g_ready.load(std::memory_order_acquire)) return "tinyplug";
    return logger().process_tag();
}

auto copy_text(Arg& arg, std::string_view text) noexcept -> void
{
    arg.kind = Arg::Kind::inline_text;

    const auto count = std::min(text.size(), max_inline_text - 1);
    std::memcpy(arg.store.inline_text, text.data(), count);
    arg.store.inline_text[count] = '\0';
}

} // namespace detail

// MARK: - scope

Scope::Scope(Cat cat, Level level, const char* site, const char* tag, uint16_t instance, const char* name) noexcept
    : _site{site}
    , _tag{tag}
    , _name{name}
    , _instance{instance}
    , _cat{cat}
    , _level{level}
    , _live{enabled(cat, level)}
{
    if (!_live) return;

    _entered_us = detail::now_us();
    write(_cat, _level, _site, _tag, _instance, "-> {}", _name);
}

Scope::~Scope()
{
    if (!_live) return;

    const auto elapsed_us = detail::now_us() - _entered_us;
    write(_cat, _level, _site, _tag, _instance, "<- {} ({} us)", _name, elapsed_us);
}

} // namespace tiny::log
