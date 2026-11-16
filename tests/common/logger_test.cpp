// logger_test.cpp
#include <gtest/gtest.h>
#include "common/logger.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>  // getpid()

using namespace aegis;

// Build a unique temp-file path for this process and test name.
static std::string tmpPath(const char* tag) {
    char buf[256];
    snprintf(buf, sizeof(buf), "/tmp/aegis_logger_%s_%d.log", tag, static_cast<int>(getpid()));
    return buf;
}

static std::string readAll(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ─── LogToFile ───────────────────────────────────────────────────────────────
TEST(LoggerTest, LogToFile) {
    const std::string path = tmpPath("log_to_file");

    Logger::instance().setLevel(LogLevel::DEBUG);
    Logger::instance().setFile(path);
    LOG(LogLevel::INFO, "test message %d", 42);
    Logger::instance().setFile("");  // back to stderr

    const std::string content = readAll(path);
    std::remove(path.c_str());

    EXPECT_NE(content.find("test message 42"), std::string::npos)
        << "logged message not found in: " << content;
    EXPECT_NE(content.find("INFO"), std::string::npos);
    // Timestamp format sanity check.
    EXPECT_NE(content.find('['), std::string::npos);
}

// ─── LevelFiltering ──────────────────────────────────────────────────────────
TEST(LoggerTest, LevelFiltering) {
    const std::string path = tmpPath("level_filter");

    Logger::instance().setLevel(LogLevel::WARN);
    Logger::instance().setFile(path);
    LOG(LogLevel::DEBUG, "this should not appear");
    LOG(LogLevel::INFO,  "this should not appear either");
    LOG(LogLevel::WARN,  "this SHOULD appear");
    Logger::instance().setFile("");

    const std::string content = readAll(path);
    std::remove(path.c_str());

    // Reset to default for other tests.
    Logger::instance().setLevel(LogLevel::INFO);

    EXPECT_EQ(content.find("this should not appear"), std::string::npos)
        << "DEBUG/INFO message leaked through WARN filter";
    EXPECT_NE(content.find("this SHOULD appear"), std::string::npos)
        << "WARN message missing from: " << content;
}
