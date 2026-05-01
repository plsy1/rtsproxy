#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <deque>
#include <vector>

enum class LogLevel
{
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

class Logger
{
public:
    static void setLogLevel(LogLevel level);
    static LogLevel getLogLevel() { return currentLevel; }
    static void log(LogLevel level, const std::string &msg);
    static void info(const std::string &msg);
    static void warn(const std::string &msg);
    static void error(const std::string &msg);
    static void debug(const std::string &msg);
    static void flush();

    static void setLogFile(const std::string &path, size_t maxLines = 10000);
    static std::vector<std::string> getRecentLogs();

private:
    static std::mutex logMutex;
    static std::ofstream logFile;
    static std::string logFilePath;
    static size_t maxLogLines;
    static size_t currentLogLines;

    static std::deque<std::string> logBuffer;
    static const size_t maxBufferSize = 100;

    static LogLevel currentLevel;
    static std::string logLevelToString(LogLevel level);
    static std::string getCurrentTime();
};