cmake_minimum_required(VERSION 3.16)
project(binance_lob_capture VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warnings and definitions
if(MSVC)
    add_compile_options(/W4 /permissive-)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Find or fetch simdjson
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

# Unit tests target (zero-external-dependency for core modules)
add_executable(unit_tests
    tests/test_fixed_point.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(TARGET simdjson)
    target_link_libraries(unit_tests PRIVATE simdjson)
endif()

# Main capture executable
if(OpenSSL_FOUND AND Boost_FOUND)
    add_executable(binance_capture
        src/main.cpp
    )
    target_include_directories(binance_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(binance_capture PRIVATE
        Boost::system
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
    )
    if(TARGET simdjson)
        target_link_libraries(binance_capture PRIVATE simdjson)
    endif()
    if(WIN32)
        target_link_libraries(binance_capture PRIVATE ws2_32 wsock32 crypt32)
    endif()
else()
    message(STATUS "Boost/OpenSSL not found yet; binance_capture target skipped, unit_tests is active.")
endif()
