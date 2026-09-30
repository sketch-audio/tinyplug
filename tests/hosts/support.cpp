#include "support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

// Under MSVC's ASan every module's heap goes through the one runtime DLL, so a malloc hook sees the
// plug-in's allocations too. Replacing operator new can't: with the static CRT each DLL has its own.
#if defined(_WIN32) && defined(__SANITIZE_ADDRESS__)
#define TINY_HOST_MALLOC_HOOK 1
extern "C" int __sanitizer_install_malloc_and_free_hooks(void (*)(const volatile void*, size_t), void (*)(const volatile void*));
extern "C" void __sanitizer_print_stack_trace();
#else
#define TINY_HOST_MALLOC_HOOK 0
#endif

namespace tiny::hosts {

namespace {

std::atomic<long> g_trapped{0};

#if TINY_HOST_MALLOC_HOOK
// The armed thread's id, not a thread_local: the hook runs on every thread, including the loader's
// workers, which start before the executable's TLS exists and would read a null slot.
std::atomic<DWORD> g_armed_thread{0};

struct Module_range {
    uintptr_t begin{}, end{};
    explicit Module_range(const wchar_t* name)
    {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(name));
        if (!base) return;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
        begin = base;
        end = base + nt->OptionalHeader.SizeOfImage;
    }
    auto contains(const void* p) const -> bool { const auto a = reinterpret_cast<uintptr_t>(p); return a >= begin && a < end; }
};

// The macOS trap replaces operator new, so it sees what code asks for and never the OS's own
// bookkeeping. The hook sees every heap call, including ntdll's: a contended critical section (MSVC's
// debug STL takes a global one in every vector::clear) allocates inside RtlEnterCriticalSection.
// Matching macOS: an allocation whose first caller outside the ASan runtime is ntdll isn't counted.
// A plug-in's own HeapAlloc still is: that export forwards, so its return address is the plug-in's.
auto from_the_os() -> bool
{
    static const auto ntdll = Module_range{L"ntdll.dll"};
    static const auto asan = Module_range{L"clang_rt.asan_dynamic-x86_64.dll"};
    void* frames[24];
    const auto n = CaptureStackBackTrace(1, 24, frames, nullptr); // Skips this frame; on_malloc is the next.
    for (auto i = USHORT{1}; i < n; ++i) {
        if (asan.contains(frames[i])) continue;
        return ntdll.contains(frames[i]);
    }
    return false;
}

auto on_malloc(const volatile void*, size_t) -> void
{
    const auto self = GetCurrentThreadId();
    if (g_armed_thread.load(std::memory_order_relaxed) != self) return;
    if (from_the_os()) return;
    if (g_trapped.fetch_add(1, std::memory_order_relaxed) < 3) {
        g_armed_thread.store(0, std::memory_order_relaxed); // Symbolizing allocates.
        std::fputs("host: allocation on the audio thread:\n", stderr);
        __sanitizer_print_stack_trace();
        g_armed_thread.store(self, std::memory_order_relaxed);
    }
}
auto on_free(const volatile void*) -> void {}
const auto g_hooks_installed = __sanitizer_install_malloc_and_free_hooks(&on_malloc, &on_free);
#else
thread_local bool t_armed = false;
#endif

} // namespace

auto arm_trap(bool on) -> void
{
#if TINY_HOST_MALLOC_HOOK
    g_armed_thread.store(on ? GetCurrentThreadId() : 0, std::memory_order_relaxed);
#else
    t_armed = on;
#endif
}
auto trapped_allocations() -> long { return g_trapped.load(); }

auto pump_main(std::chrono::microseconds duration) -> void
{
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
#elif defined(_WIN32)
    const auto deadline = std::chrono::steady_clock::now() + duration;
    for (;;) {
        auto msg = MSG{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left.count()), QS_ALLINPUT);
    }
#else
    std::this_thread::sleep_for(duration);
#endif
}

auto parse_options(int argc, char** argv) -> Options
{
    // Unbuffered: under ctest stdout is a pipe, and a host killed for hanging would otherwise take
    // the name of the scenario that hung with it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    // A DAW's resolution, not the default 15.6 ms tick: a host's short sleeps (the render loop's
    // pacing, the Direct Data timer) would otherwise all round up to it.
    timeBeginPeriod(1);
#endif
    auto options = Options{};
    for (auto i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) options.seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) options.chaos_seconds = std::strtod(argv[++i], nullptr);
        else options.bundle = argv[i];
    }
    return options;
}

auto bundle_name(const std::string& path) -> std::string
{
    auto p = std::filesystem::path{path};
    if (!p.has_filename()) p = p.parent_path(); // A trailing separator.
    return p.stem().string();
}

Library::Library(const std::string& path)
{
    auto binary = std::filesystem::path{path};
    if (std::filesystem::is_directory(binary)) {
#if defined(_WIN32)
        binary = binary / "Contents" / "x86_64-win" / (bundle_name(path) + binary.extension().string());
#else
        binary = binary / "Contents" / "MacOS" / bundle_name(path);
#endif
    }
#if defined(_WIN32)
    _handle = LoadLibraryW(binary.wstring().c_str());
    if (!_handle) throw std::runtime_error("LoadLibrary failed for " + binary.string() + ", error " + std::to_string(GetLastError()));
#else
    _handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!_handle) throw std::runtime_error(std::string{"dlopen failed: "} + dlerror());
#endif
}

auto Library::symbol(const char* name) const -> void*
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(_handle), name));
#else
    return dlsym(_handle, name);
#endif
}

} // namespace tiny::hosts

#if !TINY_HOST_MALLOC_HOOK
// The process's operator new: counts, when armed, then allocates normally. On Windows without ASan
// this sees only the host's own allocations; the plug-in DLL has its own CRT.
auto operator new(std::size_t n) -> void*
{
    if (tiny::hosts::t_armed) {
        if (tiny::hosts::g_trapped.fetch_add(1, std::memory_order_relaxed) < 3) {
#if defined(_WIN32)
            static constexpr char header[] = "host: allocation on the audio thread\n";
            (void)_write(2, header, sizeof(header) - 1);
#else
            // The first few get a stack, written straight to stderr: backtrace_symbols_fd doesn't allocate.
            void* frames[32];
            const auto n_frames = backtrace(frames, 32);
            static constexpr char header[] = "host: allocation on the audio thread:\n";
            (void)!write(STDERR_FILENO, header, sizeof(header) - 1);
            backtrace_symbols_fd(frames, n_frames, STDERR_FILENO);
#endif
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
#endif
