#pragma once

#include <string>
#include <string_view>

namespace hadisplay {

enum class LogLevel {
    Info = 0,
    Warn,
    Error,
};

struct LogConfig {
    std::string path = "hadisplay.log";
    LogLevel min_level = LogLevel::Info;
    long max_bytes = 512L * 1024L;
};

void log_init(const LogConfig& config = {});
void log_shutdown();

void log_message(LogLevel level, std::string_view message);

inline void log_info(std::string_view message) { log_message(LogLevel::Info, message); }
inline void log_warn(std::string_view message) { log_message(LogLevel::Warn, message); }
inline void log_error(std::string_view message) { log_message(LogLevel::Error, message); }

}  // namespace hadisplay
