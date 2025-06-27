include(FetchContent)

FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG main
    # 'FIND_PACKAGE_ARGS' will skip download if
    # the target is already available in the system
    FIND_PACKAGE_ARGS NAMES clap
)

FetchContent_Declare(
    clap-helpers
    GIT_REPOSITORY https://github.com/free-audio/clap-helpers.git
    GIT_TAG main
    FIND_PACKAGE_ARGS NAMES clap-helpers
)

# Suppress a CLAP dependency (dev) warning.
if(WIN32)
    cmake_policy(SET CMP0177 NEW)
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "No dev warnings")
endif()

FetchContent_MakeAvailable(clap clap-helpers)