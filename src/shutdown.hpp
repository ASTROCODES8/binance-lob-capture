#pragma once

#include <atomic>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace lob {

namespace detail {
inline std::atomic<bool> g_stop_requested{false};

#ifdef _WIN32
inline BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stop_requested.store(true, std::memory_order_release);
            return TRUE;
        default:
            return FALSE;
    }
}
#else
inline void posix_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop_requested.store(true, std::memory_order_release);
    }
}
#endif
} // namespace detail

/**
 * @brief Installs OS console/signal interrupt handlers for clean shutdown.
 */
inline void install_signal_handler() {
#ifdef _WIN32
    SetConsoleCtrlHandler(detail::console_ctrl_handler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = detail::posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

/**
 * @brief Checks if a shutdown signal (Ctrl+C, SIGINT, SIGTERM) was received.
 */
[[nodiscard]] inline bool is_stop_requested() noexcept {
    return detail::g_stop_requested.load(std::memory_order_acquire);
}

/**
 * @brief Programmatically signals the application to initiate graceful shutdown.
 */
inline void request_stop() noexcept {
    detail::g_stop_requested.store(true, std::memory_order_release);
}

/**
 * @brief Resets the shutdown state flag (primarily for unit test re-entry).
 */
inline void reset_stop() noexcept {
    detail::g_stop_requested.store(false, std::memory_order_release);
}

} // namespace lob
