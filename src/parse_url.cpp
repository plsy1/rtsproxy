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
    if (url.find("/rtp/") == 0)
    {
        return parse_rtp_url(url, host, port, path);
    }
    else if (url.find("/tv/") == 0)
    {
        return parse_tv_url(url, host, port, path);
    }

    return false;
}