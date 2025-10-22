#include "logs.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

void Logger::log(LogLevel level, const std::string &msg)
{
    std::string level_str = logLevelToString(level);
    std::string timestamp = getCurrentTime();
    std::cout << "[" << timestamp << "] [" << level_str << "] " << msg << std::endl;
}

void Logger::info(const std::string &msg)
{
    log(LogLevel::INFO, msg);
}

void Logger::warn(const std::string &msg)
{
    log(LogLevel::WARN, msg);
}

void Logger::error(const std::string &msg)
{
    log(LogLevel::ERROR, msg);
}

void Logger::debug(const std::string &msg)
{
    log(LogLevel::DEBUG, msg);
}

std::string Logger::logLevelToString(LogLevel level)
{
    switch (level)
    {
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::DEBUG:
        return "DEBUG";
    default:
        return "UNKNOWN";
    }
}

std::string Logger::getCurrentTime()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}