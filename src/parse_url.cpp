#include "parse_url.h"
#include "../include/3rd/json.hpp"
#include "../include/logger.h"
#include <fstream>
#include <regex>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>

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

using json = nlohmann::json;

bool ParseURL::jsonLoaded = false;
nlohmann::json ParseURL::iptvData;

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
        ifs >> iptvData;
    }
    catch (...)
    {
        return false;
    }
    jsonLoaded = true;
    return true;
}

bool ParseURL::parse_tv_url(const std::string &url, std::string &host, int &port, std::string &path)
{
    if (!jsonLoaded)
    {
        return false;
    }
    // url: /tv/:ChannelID?tvdr=...
    std::regex tv_regex(R"(^/tv/([^?]+)(?:\?tvdr=(\d{14}-\d{14}))?)");
    std::smatch match;
    if (!std::regex_match(url, match, tv_regex))
    {
        return false;
    }

    std::string channelID = match[1];
    std::string tvdrParam = match.size() > 2 && match[2].matched ? match[2].str() : "";

    json channel;
    bool found = false;
    for (const auto &c : iptvData)
    {
        if (c.contains("ChannelID") && c["ChannelID"] == channelID)
        {
            channel = c;
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    std::string rtspUrl;
    if (!tvdrParam.empty())
    {
        std::regex tvdr_regex(R"((\d{14})-(\d{14}))");
        std::smatch tvdrMatch;
        if (!std::regex_match(tvdrParam, tvdrMatch, tvdr_regex))
            return false;

        std::string start = tvdrMatch[1];
        std::string end = tvdrMatch[2];

        rtspUrl = channel["uni_playback"];
        rtspUrl = std::regex_replace(rtspUrl, std::regex("\\{utc:YmdHMS\\}"), start);
        rtspUrl = std::regex_replace(rtspUrl, std::regex("\\{utcend:YmdHMS\\}"), end);
    }
    else
    {
        rtspUrl = channel["uni_live"];
    }

    std::regex rtsp_regex(R"(rtsp://([^:/]+)(?::(\d+))?/(.+))");
    std::smatch rtspMatch;
    if (!std::regex_match(rtspUrl, rtspMatch, rtsp_regex))
        return false;

    host = rtspMatch[1];
    port = rtspMatch[2].matched ? std::stoi(rtspMatch[2]) : 554;
    path = rtspMatch[3];

    return true;
}

bool ParseURL::parse_rtp_url(const std::string &url, std::string &host, int &port, std::string &path)
{
    std::string s = url.substr(5);
    size_t colon = s.find(':');
    size_t slash = s.find('/');
    if (colon == std::string::npos || slash == std::string::npos)
    {
        return false;
    }

    host = s.substr(0, colon);
    port = std::stoi(s.substr(colon + 1, slash - colon - 1));
    path = s.substr(slash + 1);

    return true;
}

bool ParseURL::parse_http_url(const std::string &url, std::string &host, int &port, std::string &path)
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
        return parse_rtp_url(clean_url, host, port, path);
    }
    else if (clean_url.find("/tv/") == 0)
    {
        return parse_tv_url(clean_url, host, port, path);
    }

    return false;
}