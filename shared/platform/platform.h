#pragma once

#if defined(__APPLE__)
    #include <TargetConditionals.h>
    #if TARGET_OS_OSX
        #ifndef PLATFORM_MACOS
        #define PLATFORM_MACOS 1
        #endif
    #elif TARGET_OS_IPHONE
        #ifndef PLATFORM_IOS
        #define PLATFORM_IOS 1
        #endif
    #endif
#elif defined(_WIN32)
    #ifndef PLATFORM_WINDOWS
    #define PLATFORM_WINDOWS 1
    #endif
#else
    #error "Unsupported platform."
#endif

struct Platform {
    enum class Type {
        macos, ios, windows
    };

    static constexpr auto resolved = Type{
#if PLATFORM_MACOS
        Type::macos
#elif PLATFORM_IOS
        Type::ios
#elif PLATFORM_WINDOWS
        Type::windows
#endif
    };

    static constexpr auto is_apple = bool{
        Platform::resolved == Type::macos || Platform::resolved == Type::ios
    };
};