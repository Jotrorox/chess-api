#pragma once

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace slog {

// C++17-compatible replacement for std::source_location (introduced in C++20).
struct SourceLocation {
    const char* file_name;
    std::uint_least32_t line;
    const char* function_name;
};

enum class Level : std::uint8_t {
    trace = 0,
    debug,
    info,
    warning,
    error,
    critical,
    off
};

constexpr std::string_view to_string(Level level) noexcept {
    switch (level) {
        case Level::trace:    return "TRACE";
        case Level::debug:    return "DEBUG";
        case Level::info:     return "INFO ";
        case Level::warning:  return "WARN ";
        case Level::error:    return "ERROR";
        case Level::critical: return "FATAL";
        case Level::off:      return "OFF  ";
    }

    return "UNKWN";
}

inline std::optional<Level> level_from_string(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char character : value) {
        if (!std::isspace(static_cast<unsigned char>(character))) {
            normalized += static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))
            );
        }
    }

    if (normalized == "trace") return Level::trace;
    if (normalized == "debug") return Level::debug;
    if (normalized == "info") return Level::info;
    if (normalized == "warning" || normalized == "warn") return Level::warning;
    if (normalized == "error") return Level::error;
    if (normalized == "critical" || normalized == "fatal") return Level::critical;
    if (normalized == "off") return Level::off;

    return std::nullopt;
}

namespace detail {

template <typename... Args>
std::string build_message(Args&&... args) {
    std::ostringstream stream;
    ((stream << std::forward<Args>(args)), ...);
    return stream.str();
}

inline std::string_view filename_only(std::string_view path) noexcept {
    const auto slash = path.find_last_of("/\\");

    if (slash == std::string_view::npos) {
        return path;
    }

    return path.substr(slash + 1);
}

inline std::tm local_time(std::time_t time) noexcept {
    std::tm result{};

#if defined(_WIN32)
    ::localtime_s(&result, &time);
#else
    ::localtime_r(&time, &result);
#endif

    return result;
}

inline std::string make_timestamp() {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto time = system_clock::to_time_t(now);
    const auto milliseconds_part =
        duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    const std::tm local = local_time(time);

    std::ostringstream stream;
    stream << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
           << '.'
           << std::setfill('0')
           << std::setw(3)
           << milliseconds_part.count();

    return stream.str();
}

inline std::string_view color_for(Level level) noexcept {
    switch (level) {
        case Level::trace:    return "\033[90m";     // Gray
        case Level::debug:    return "\033[36m";     // Cyan
        case Level::info:     return "\033[32m";     // Green
        case Level::warning:  return "\033[33m";     // Yellow
        case Level::error:    return "\033[31m";     // Red
        case Level::critical: return "\033[1;31m";   // Bold red
        case Level::off:      return "";
    }

    return "";
}

inline constexpr std::string_view color_reset = "\033[0m";

} // namespace detail

class Logger final {
public:
    static Logger& instance() noexcept {
        static Logger logger;
        return logger;
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    void set_level(Level level) noexcept {
        minimum_level_.store(level, std::memory_order_relaxed);
    }

    [[nodiscard]]
    Level level() const noexcept {
        return minimum_level_.load(std::memory_order_relaxed);
    }

    void enable_console(bool enabled) noexcept {
        console_enabled_.store(enabled, std::memory_order_relaxed);
    }

    void enable_colors(bool enabled) noexcept {
        colors_enabled_.store(enabled, std::memory_order_relaxed);
    }

    void enable_source_location(bool enabled) noexcept {
        source_location_enabled_.store(enabled, std::memory_order_relaxed);
    }

    void enable_thread_id(bool enabled) noexcept {
        thread_id_enabled_.store(enabled, std::memory_order_relaxed);
    }

    void flush_after_each_message(bool enabled) noexcept {
        flush_enabled_.store(enabled, std::memory_order_relaxed);
    }

    /*
     * Opens a log file.
     *
     * path:
     *     Destination file.
     *
     * append:
     *     true  -> preserve existing contents
     *     false -> truncate existing contents
     *
     * max_size_bytes:
     *     When non-zero, the logger rotates the file after it reaches
     *     approximately this size. The previous file is renamed to:
     *
     *         application.log.1
     */
    bool set_file(
        const std::filesystem::path& path,
        bool append = true,
        std::uintmax_t max_size_bytes = 0
    ) noexcept {
        try {
            std::scoped_lock lock(mutex_);

            close_file_unlocked();

            file_path_ = path;
            max_file_size_ = max_size_bytes;

            const auto parent = path.parent_path();

            if (!parent.empty()) {
                std::error_code error;
                std::filesystem::create_directories(parent, error);

                if (error) {
                    file_path_.reset();
                    max_file_size_ = 0;
                    return false;
                }
            }

            const auto mode =
                std::ios::out |
                (append ? std::ios::app : std::ios::trunc);

            file_.open(path, mode);

            if (!file_.is_open()) {
                file_path_.reset();
                max_file_size_ = 0;
                return false;
            }

            return true;
        } catch (...) {
            return false;
        }
    }

    void disable_file() noexcept {
        try {
            std::scoped_lock lock(mutex_);
            close_file_unlocked();
        } catch (...) {
            // Logging configuration should not terminate the application.
        }
    }

    template <typename... Args>
    void log(
        Level message_level,
        const SourceLocation& location,
        Args&&... args
    ) noexcept {
        try {
            if (!should_log(message_level)) {
                return;
            }

            const std::string message =
                detail::build_message(std::forward<Args>(args)...);

            write(message_level, message, location);
        } catch (...) {
            // A logger should generally never crash the application.
        }
    }

    void flush() noexcept {
        try {
            std::scoped_lock lock(mutex_);

            std::clog.flush();

            if (file_.is_open()) {
                file_.flush();
            }
        } catch (...) {
            // Ignore logging I/O errors.
        }
    }

private:
    Logger() = default;

    ~Logger() {
        flush();
    }

    [[nodiscard]]
    bool should_log(Level message_level) const noexcept {
        const Level minimum =
            minimum_level_.load(std::memory_order_relaxed);

        return minimum != Level::off &&
               static_cast<std::uint8_t>(message_level) >=
                   static_cast<std::uint8_t>(minimum);
    }

    void write(
        Level message_level,
        std::string_view message,
        const SourceLocation& location
    ) {
        const std::string line =
            format_line(message_level, message, location);

        std::scoped_lock lock(mutex_);

        if (console_enabled_.load(std::memory_order_relaxed)) {
            write_console_unlocked(message_level, line);
        }

        if (file_.is_open()) {
            rotate_file_if_needed_unlocked(line.size() + 1);

            if (file_.is_open()) {
                file_ << line << '\n';

                if (flush_enabled_.load(std::memory_order_relaxed)) {
                    file_.flush();
                }
            }
        }
    }

    [[nodiscard]]
    std::string format_line(
        Level message_level,
        std::string_view message,
        const SourceLocation& location
    ) const {
        std::ostringstream stream;

        stream << detail::make_timestamp()
               << " [" << to_string(message_level) << ']';

        if (thread_id_enabled_.load(std::memory_order_relaxed)) {
            stream << " [thread " << std::this_thread::get_id() << ']';
        }

        if (source_location_enabled_.load(std::memory_order_relaxed)) {
            stream << " ["
                   << detail::filename_only(location.file_name)
                   << ':'
                   << location.line
                   << ']';

            const std::string_view function = location.function_name;

            if (!function.empty()) {
                stream << " [" << function << ']';
            }
        }

        stream << " | " << message;

        return stream.str();
    }

    void write_console_unlocked(
        Level message_level,
        std::string_view line
    ) {
        if (colors_enabled_.load(std::memory_order_relaxed)) {
            std::clog << detail::color_for(message_level)
                      << line
                      << detail::color_reset
                      << '\n';
        } else {
            std::clog << line << '\n';
        }

        if (flush_enabled_.load(std::memory_order_relaxed)) {
            std::clog.flush();
        }
    }

    void rotate_file_if_needed_unlocked(
        std::uintmax_t upcoming_message_size
    ) {
        if (!file_path_ ||
            max_file_size_ == 0 ||
            !file_.is_open()) {
            return;
        }

        std::error_code error;
        const std::uintmax_t current_size =
            std::filesystem::file_size(*file_path_, error);

        if (error ||
            current_size + upcoming_message_size < max_file_size_) {
            return;
        }

        file_.flush();
        file_.close();

        std::filesystem::path rotated_path = *file_path_;
        rotated_path += ".1";

        std::filesystem::remove(rotated_path, error);
        error.clear();

        std::filesystem::rename(
            *file_path_,
            rotated_path,
            error
        );

        /*
         * Even if renaming failed, attempt to reopen the original file.
         * Truncation is used after a successful rotation.
         */
        const auto mode = error
            ? std::ios::out | std::ios::app
            : std::ios::out | std::ios::trunc;

        file_.open(*file_path_, mode);
    }

    void close_file_unlocked() noexcept {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }

        file_path_.reset();
        max_file_size_ = 0;
    }

    std::mutex mutex_;
    std::ofstream file_;
    std::optional<std::filesystem::path> file_path_;

    std::atomic<Level> minimum_level_{Level::info};
    std::atomic_bool console_enabled_{true};
    std::atomic_bool colors_enabled_{true};
    std::atomic_bool source_location_enabled_{true};
    std::atomic_bool thread_id_enabled_{true};
    std::atomic_bool flush_enabled_{false};

    std::uintmax_t max_file_size_{0};
};

} // namespace slog

/*
 * Capture source information at the call site without C++20's
 * std::source_location or __VA_OPT__. The immediately invoked generic lambda
 * also permits an empty argument list (for example, SLOG_INFO()).
 */
#define SLOG_LOG(level, ...)                                              \
    [slog_location = ::slog::SourceLocation{__FILE__, __LINE__, __func__}] \
    (auto&&... slog_args) noexcept {                                      \
        ::slog::Logger::instance().log(                                   \
            (level),                                                      \
            slog_location,                                                \
            std::forward<decltype(slog_args)>(slog_args)...               \
        );                                                               \
    }(__VA_ARGS__)

#define SLOG_TRACE(...) SLOG_LOG(::slog::Level::trace, __VA_ARGS__)
#define SLOG_DEBUG(...) SLOG_LOG(::slog::Level::debug, __VA_ARGS__)
#define SLOG_INFO(...) SLOG_LOG(::slog::Level::info, __VA_ARGS__)
#define SLOG_WARNING(...) SLOG_LOG(::slog::Level::warning, __VA_ARGS__)
#define SLOG_ERROR(...) SLOG_LOG(::slog::Level::error, __VA_ARGS__)
#define SLOG_CRITICAL(...) SLOG_LOG(::slog::Level::critical, __VA_ARGS__)
