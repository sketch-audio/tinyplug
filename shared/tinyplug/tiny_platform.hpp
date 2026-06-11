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
#else
    #error "Unsupported platform."
#endif