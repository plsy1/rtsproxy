#pragma once

#include <string>
#include <fstream>
#include <mutex>

enum class LogLevel
{
    ERROR = 0,
    WARN = 1,
    INFO = 2,
    DEBUG = 3
};

class Logger
{
public:
    static void setLogLevel(LogLevel level);
    static void log(LogLevel level, const std::string &msg);
    static void info(const std::string &msg);
    static void warn(const std::string &msg);
    static void error(const std::string &msg);
    static void debug(const std::string &msg);
    static void flush();

    static void setLogFile(const std::string &path, size_t maxLines = 10000);

private:
    static std::mutex logMutex;
    static std::ofstream logFile;
    static std::string logFilePath;
    static size_t maxLogLines;
    static size_t currentLogLines;

    static LogLevel currentLevel;
    static std::string logLevelToString(LogLevel level);
    static std::string getCurrentTime();
};