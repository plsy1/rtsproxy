#include "../include/logger.h"
#include <iostream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

LogLevel Logger::currentLevel = LogLevel::INFO;
std::mutex Logger::logMutex;
std::ofstream Logger::logFile;
std::string Logger::logFilePath;
size_t Logger::maxLogLines = 10000; // Default 10000 lines
size_t Logger::currentLogLines = 0;

void Logger::setLogLevel(LogLevel level)
{
    currentLevel = level;
}

void Logger::setLogFile(const std::string &path, size_t maxLines)
{
    std::lock_guard<std::mutex> lock(logMutex);
    logFilePath = path;
    maxLogLines = maxLines;
    currentLogLines = 0;
    
    if (logFile.is_open()) {
        logFile.close();
    }
    
    if (!path.empty()) {
        size_t last_slash = path.find_last_of("/");
        if (last_slash != std::string::npos) {
            std::string dir = path.substr(0, last_slash);
            size_t pos = 1;
            while ((pos = dir.find('/', pos)) != std::string::npos) {
                mkdir(dir.substr(0, pos).c_str(), 0755);
                pos++;
            }
            mkdir(dir.c_str(), 0755);
        }

        logFile.open(path, std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "[LOGGER] Failed to open log file: " << path << std::endl;
        }
    }
}

void Logger::log(LogLevel level, const std::string &msg)
{
    if (level > currentLevel)
    {
        return;
    }
    std::string level_str = logLevelToString(level);
    std::string timestamp = getCurrentTime();
    std::string formatted_msg = "[" + timestamp + "] [" + level_str + "] " + msg + "\n";
    
    std::lock_guard<std::mutex> lock(logMutex);
    
    if (logFile.is_open()) {
        // Log rotation based on line count
        if (currentLogLines >= maxLogLines) {
            logFile.close();
            std::string old_path = logFilePath + ".1";
            std::rename(logFilePath.c_str(), old_path.c_str());
            logFile.open(logFilePath, std::ios::app);
            currentLogLines = 0;
        }
        
        if (logFile.is_open()) {
            logFile << formatted_msg;
            // Only flush for ERROR and WARN to reduce flash wear
            if (level == LogLevel::ERROR || level == LogLevel::WARN) {
                logFile.flush();
            }
            currentLogLines++;
        }
    }
    
    // Always output to console with color
    std::string color_code = "";
    switch (level) {
        case LogLevel::ERROR: color_code = "\033[31m"; break; // Red
        case LogLevel::WARN:  color_code = "\033[33m"; break; // Yellow
        case LogLevel::INFO:  color_code = "\033[32m"; break; // Green
        case LogLevel::DEBUG: color_code = "\033[36m"; break; // Cyan
    }

    if (!color_code.empty()) {
        std::cout << color_code << formatted_msg << "\033[0m";
    } else {
        std::cout << formatted_msg;
    }
    std::cout.flush();
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

void Logger::flush()
{
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile.is_open()) {
        logFile.flush();
    }
    std::cout.flush();
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