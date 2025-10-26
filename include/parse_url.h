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

    static bool parse_rtp_url(const std::string &url, std::string &host, int &port, std::string &path);
    static bool parse_tv_url(const std::string &url, std::string &host, int &port, std::string &path);
};
