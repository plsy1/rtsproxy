#pragma once

#include <string>

enum class LogLevel
{
    INFO,
    WARN,
    ERROR,
    DEBUG
};

class Logger
{
public:
    static void log(LogLevel level, const std::string &msg);
    static void info(const std::string &msg);
    static void warn(const std::string &msg);
    static void error(const std::string &msg);
    static void debug(const std::string &msg);

private:
    static std::string logLevelToString(LogLevel level);
    static std::string getCurrentTime();
};