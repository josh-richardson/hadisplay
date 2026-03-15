#include "logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace hadisplay {
namespace {

struct LogState {
    std::mutex mutex;
    std::FILE* file = nullptr;
    LogLevel min_level = LogLevel::Info;
    std::string path;
};

LogState& state() {
    static LogState s;
    return s;
}

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

void rotate_if_needed(const std::string& path, long max_bytes) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || static_cast<long>(size) < max_bytes) {
        return;
    }

    const std::string old_path = path + ".1";
    std::filesystem::rename(path, old_path, ec);
}

}  // namespace

void log_init(const LogConfig& config) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    if (s.file != nullptr) {
        std::fclose(s.file);
        s.file = nullptr;
    }

    s.path = config.path;
    s.min_level = config.min_level;

    rotate_if_needed(s.path, config.max_bytes);

    s.file = std::fopen(s.path.c_str(), "a");
    if (s.file != nullptr) {
        std::setvbuf(s.file, nullptr, _IOLBF, 0);
    }
}

void log_shutdown() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    if (s.file != nullptr) {
        std::fclose(s.file);
        s.file = nullptr;
    }
}

void log_message(LogLevel level, std::string_view message) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    if (level < s.min_level || s.file == nullptr) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&time, &local_tm);

    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_tm);

    std::fprintf(s.file, "[%s] [%s] %.*s\n",
                 timestamp,
                 level_tag(level),
                 static_cast<int>(message.size()),
                 message.data());
}

}  // namespace hadisplay
