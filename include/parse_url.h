#pragma once

#include <string>
#include "../include/3rd/json.hpp"

class ParseURL
{
public:
    static bool parse_http_url(const std::string &url, std::string &host, int &port, std::string &path);
    static bool load_json(const std::string jsonPath);

private:
    static nlohmann::json iptvData;
    static bool jsonLoaded;

    static std::vector<std::string> split(const std::string &str, char delimiter);
    static std::string join(const std::vector<std::string> &parts, const std::string &delimiter);

    static bool parse_rtp_url(const std::string &url, std::string &host, int &port, std::string &path);
    static bool parse_tv_url(const std::string &url, std::string &host, int &port, std::string &path);
};
