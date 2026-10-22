#pragma once

#if defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_OSX
        #ifndef TINY_PLATFORM_MACOS
        #define TINY_PLATFORM_MACOS 1
        #endif
    #elif TARGET_OS_IOS
        #ifndef TINY_PLATFORM_IOS
        #define TINY_PLATFORM_IOS 1
        #endif
    #endif
    #if TINY_PLATFORM_MACOS || TINY_PLATFORM_IOS
        #ifndef TINY_PLATFORM_APPLE
        #define TINY_PLATFORM_APPLE 1
        #endif
    #endif
#elif defined(_WIN32)
    #ifndef TINY_PLATFORM_WINDOWS
    #define TINY_PLATFORM_WINDOWS 1
    #endif
#elif defined(__EMSCRIPTEN__)
    #ifndef TINY_PLATFORM_WASM
    #define TINY_PLATFORM_WASM 1
    #endif
#else
    #error "Unsupported platform."
#endif

// Whether this build can start threads: always natively; in wasm only with shared memory (-pthread), which the
// web format's nonisolated variant goes without.
#ifndef TINY_PLATFORM_THREADS
    #if TINY_PLATFORM_WASM && !defined(__EMSCRIPTEN_PTHREADS__)
    #define TINY_PLATFORM_THREADS 0
    #else
    #define TINY_PLATFORM_THREADS 1
    #endif
#endif
