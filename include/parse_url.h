#pragma once

#include <string>
#include "../include/3rd/json.hpp"

class ParseURL
{
public:
    static bool parse_http_url(const std::string &url, std::string &rtsp_url);
    static bool load_json(const std::string jsonPath);
private:
    static nlohmann::json parseConfig;
    static bool jsonLoaded;

    static std::vector<std::string> split(const std::string &str, char delimiter);
    static std::string join(const std::vector<std::string> &parts, const std::string &delimiter);

    static std::string simplifyToRegex(const std::string &match_pattern);
    static std::string shiftTime(const std::string &time_str, int shift_hours);
    static void shiftTimeInString(std::string &input, const std::string &regex_pattern, int shift_hours);

    static bool parse_rtp_url(const std::string &url, std::string &rtsp_url);
    static bool parse_tv_url(const std::string &url, std::string &rtsp_url);
};
