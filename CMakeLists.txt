cmake_minimum_required(VERSION 3.16)
project(binance_lob_capture VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the build type" FORCE)
endif()

# Build options
option(ENABLE_SANITIZERS "Enable Address and Undefined Behavior Sanitizers" OFF)
option(ENABLE_LTO "Enable Link Time Optimization (IPO)" OFF)

if(ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "IPO/LTO is not supported: ${ipo_error}")
    endif()
endif()

# Compiler warnings and options
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# simdjson via FetchContent
find_package(simdjson CONFIG QUIET)
if(NOT simdjson_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v3.10.1
    )
    FetchContent_MakeAvailable(simdjson)
endif()

# Boost and OpenSSL dependencies
find_package(Threads REQUIRED)
find_package(OpenSSL QUIET)
find_package(Boost QUIET COMPONENTS system)

# Static core library: lobcore
add_library(lobcore STATIC
    src/decimal.cpp
    src/csv.cpp
    src/envelope.cpp
    src/lob.cpp
    src/sync.cpp
    src/processor.cpp
    src/replay.cpp
)
target_include_directories(lobcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(TARGET simdjson)
    target_link_libraries(lobcore PUBLIC simdjson)
endif()

# Unit tests target (plain asserts, zero external network dependency)
add_executable(unit_tests
    tests/unit_tests.cpp
)
target_link_libraries(unit_tests PRIVATE lobcore)
if(TARGET simdjson)
    target_link_libraries(unit_tests PRIVATE simdjson)
endif()

# Main capture executable
add_executable(binance_capture
    src/main.cpp
)
target_link_libraries(binance_capture PRIVATE lobcore Threads::Threads)
if(TARGET simdjson)
    target_link_libraries(binance_capture PRIVATE simdjson)
endif()

if(OpenSSL_FOUND AND Boost_FOUND)
    target_link_libraries(binance_capture PRIVATE
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()

if(WIN32)
    target_link_libraries(binance_capture PRIVATE ws2_32 mswsock crypt32)
endif()
