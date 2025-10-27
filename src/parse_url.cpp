#include "parse_url.h"
#include "../include/3rd/json.hpp"
#include "../include/logger.h"
#include <fstream>
#include <regex>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>

using json = nlohmann::json;

bool ParseURL::jsonLoaded = false;
nlohmann::json ParseURL::parseConfig;

std::string ParseURL::simplifyToRegex(const std::string &match_pattern)
{
    std::string regex_pattern = match_pattern;

    regex_pattern = std::regex_replace(regex_pattern, std::regex("\\{number\\}"), "(\\d+)");
    regex_pattern = std::regex_replace(regex_pattern, std::regex("\\{word\\}"), "(\\w+)");
    regex_pattern = std::regex_replace(regex_pattern, std::regex("\\{any\\}"), "(.*?)");
    regex_pattern = std::regex_replace(regex_pattern, std::regex("\\/"), "\\/");

    return regex_pattern;
}

std::string ParseURL::shiftTime(const std::string &time_str, int shift_hours)
{
    if (time_str.length() == 14)
    {
        struct tm timeinfo = {};
        if (strptime(time_str.c_str(), "%Y%m%d%H%M%S", &timeinfo) == nullptr)
        {
            throw std::invalid_argument("Failed to parse time in yyyyMMddHHmmss format");
        }

        time_t time_epoch = mktime(&timeinfo);
        time_epoch += shift_hours * 3600;

        struct tm *new_timeinfo = localtime(&time_epoch);
        char buffer[16];
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", new_timeinfo);
        return std::string(buffer);
    }
    else if (time_str.length() <= 10)
    {
        time_t timestamp = std::stoll(time_str);
        timestamp += shift_hours * 3600;

        struct tm *new_timeinfo = gmtime(&timestamp);
        char buffer[16];
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", new_timeinfo);
        return std::string(buffer);
    }
    else
    {
        throw std::invalid_argument("Unsupported time format");
    }
}

void ParseURL::shiftTimeInString(std::string &input, const std::string &regex_pattern, int shift_hours)
{
    try
    {
        std::regex rgx(regex_pattern);
        std::smatch match;

        if (regex_search(input, match, rgx))
        {
            std::string start_time = match[1];
            std::string end_time = match[2];

            std::string new_start_time = shiftTime(start_time, shift_hours);
            std::string new_end_time = shiftTime(end_time, shift_hours);

            std::string replaced = match[0];
            size_t pos1 = replaced.find(start_time);
            if (pos1 != std::string::npos)
            {
                replaced.replace(pos1, start_time.length(), new_start_time);
            }
            size_t pos2 = replaced.find(end_time, pos1 + new_start_time.length());
            if (pos2 != std::string::npos)
            {
                replaced.replace(pos2, end_time.length(), new_end_time);
            }

            input.replace(match.position(0), match.length(0), replaced);
        }
        else
        {
        }
    }
    catch (const std::regex_error &e)
    {
        Logger::error(e.what());
    }
}

std::vector<std::string> ParseURL::split(const std::string &str, char delimiter)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    size_t end = str.find(delimiter);

    while (end != std::string::npos)
    {
        tokens.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    tokens.push_back(str.substr(start, end));

    return tokens;
}

std::string ParseURL::join(const std::vector<std::string> &parts, const std::string &delimiter)
{
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
        {
            result += delimiter;
        }
        result += parts[i];
    }
    return result;
}

bool ParseURL::load_json(const std::string jsonPath)
{

    std::ifstream ifs(jsonPath);
    if (!ifs.is_open())
    {
        Logger::debug("Failed to open json file.");
        return false;
    }

    try
    {
        ifs >> parseConfig;
    }
    catch (...)
    {
        return false;
    }
    jsonLoaded = true;
    return true;
}

bool ParseURL::parse_rtp_url(const std::string &url, std::string &rtsp_url)
{
    rtsp_url = url.substr(5);
    rtsp_url = "rtsp://" + rtsp_url;

    return true;
}

bool ParseURL::parse_tv_url(const std::string &url, std::string &rtsp_url)
{

    size_t query_pos = url.find('?');

    if (query_pos == std::string::npos)
    {
        // for live link
        rtsp_url = url.substr(4);
        rtsp_url = "rtsp://" + rtsp_url;
        return true;
    }
    else
    { // for playback link

        std::string replaced_url = url;
        for (const auto &template_obj : parseConfig["replace_templates"])
        {
            std::string action = template_obj["action"];
            std::string match_pattern = template_obj["match"];
            std::string description = template_obj["description"];
            std::string regex_pattern = simplifyToRegex(match_pattern);

            try
            {
                if (action == "remove")
                {
                    std::regex rgx(regex_pattern);
                    replaced_url = regex_replace(replaced_url, rgx, "");
                }
                else if (action == "replace")
                {
                    std::string replacement = template_obj["replacement"];
                    std::regex rgx(regex_pattern);
                    replaced_url = regex_replace(replaced_url, rgx, replacement);
                }
                else if (action == "timeshift")
                {
                    int shift_hours = template_obj["shift_hours"];
                    shiftTimeInString(replaced_url, regex_pattern, shift_hours);
                }
            }
            catch (const std::regex_error &e)
            {
                Logger::error(e.what());
                return false;
            }
        }

        rtsp_url = replaced_url.substr(4);
        rtsp_url = "rtsp://" + rtsp_url;

        Logger::debug(std::string("Playback redirect to: " + rtsp_url));
        return true;
    }
}

bool ParseURL::parse_http_url(const std::string &url, std::string &rtsp_url)
{
    std::string clean_url = url;
    size_t qpos = clean_url.find('?');

    if (qpos != std::string::npos)
    {
        std::string query_str = clean_url.substr(qpos + 1);
        clean_url = clean_url.substr(0, qpos);

        std::vector<std::string> params = split(query_str, '&');
        std::vector<std::string> filtered_params;

        for (const auto &param : params)
        {
            if (param.find("token=") != 0)
            {
                filtered_params.push_back(param);
            }
        }

        if (!filtered_params.empty())
        {
            clean_url += "?" + join(filtered_params, "&");
        }
    }

    if (clean_url.find("/rtp/") == 0)
    {
        return parse_rtp_url(clean_url, rtsp_url);
    }
    else if (clean_url.find("/tv/") == 0)
    {
        return parse_tv_url(clean_url, rtsp_url);
    }

    return false;
}