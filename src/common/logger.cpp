// logger.cpp – Logger singleton implementation
#include "common/logger.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace aegis {

// ─── Helpers ─────────────────────────────────────────────────────────────────

static const char* levelName(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::TRACE: return "TRACE";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "?????";
}

// ─── Singleton ───────────────────────────────────────────────────────────────

Logger& Logger::instance() {
    // Guaranteed safe from C++11 onwards (§6.7 static init).
    static Logger inst;
    return inst;
}

Logger::Logger()  = default;

Logger::~Logger() {
    if (owned_ && out_ != nullptr) {
        fclose(out_);
    }
}

// ─── Configuration ───────────────────────────────────────────────────────────

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lk(mu_);
    level_ = level;
}

void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (owned_ && out_ != nullptr) {
        fclose(out_);
        out_   = nullptr;
        owned_ = false;
    }
    if (!path.empty()) {
        out_   = fopen(path.c_str(), "a");
        owned_ = (out_ != nullptr);
    }
    // If path is empty or fopen failed, out_==nullptr → writes go to stderr.
}

// ─── Logging ─────────────────────────────────────────────────────────────────

void Logger::log(LogLevel level, const char* file, int line,
                 const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(mu_);
    if (level < level_) return;

    // Wall-clock timestamp with millisecond resolution.
    struct timeval tv {};
    gettimeofday(&tv, nullptr);

    struct tm tm_info {};
    localtime_r(&tv.tv_sec, &tm_info);

    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);
    const int ms = static_cast<int>(tv.tv_usec / 1000);

    // Strip directory prefix from __FILE__ for compact output.
    const char* base = strrchr(file, '/');
    base = (base != nullptr) ? base + 1 : file;

    FILE* dest = (out_ != nullptr) ? out_ : stderr;

    fprintf(dest, "[%s.%03d] [%s] [%s:%d] ",
            timebuf, ms, levelName(level), base, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(dest, fmt, args);
    va_end(args);

    fputc('\n', dest);
    fflush(dest);

    if (level == LogLevel::FATAL) {
        std::abort();
    }
}

} // namespace aegis
