#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// logger.h – Thread-safe, printf-style structured logger
//
// Usage:
//   LOG(aegis::LogLevel::INFO, "connected to %s:%d", host, port);
//
// Each line:  [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [file:line] message
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <mutex>
#include <string>

namespace aegis {

enum class LogLevel { TRACE = 0, DEBUG, INFO, WARN, ERROR, FATAL };

class Logger {
public:
    static Logger& instance();

    // Thread-safe; reconfiguration is live immediately.
    void setLevel(LogLevel level);

    // Redirect output to a file; empty path restores stderr.
    void setFile(const std::string& path);

    // The format attribute indices count `this` as param 1.
    void log(LogLevel level, const char* file, int line, const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

private:
    Logger();
    ~Logger();

    std::mutex mu_;
    LogLevel   level_{LogLevel::INFO};
    FILE*      out_{nullptr};  // nullptr → stderr
    bool       owned_{false};  // true if we fopen'd and must fclose
};

} // namespace aegis

// LOG(level, fmt, ...) – the format string is always present, so we can use
// the standard C99 variadic macro without the GCC ##__VA_ARGS__ extension,
// keeping -Wpedantic clean.
#define LOG(level, ...) \
    ::aegis::Logger::instance().log((level), __FILE__, __LINE__, __VA_ARGS__)
