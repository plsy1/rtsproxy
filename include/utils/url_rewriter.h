#pragma once

#include <string>
#include <vector>
#include "3rd/json.hpp"

class URLRewriter
{
public:
    /**
     * Set the rewriting templates (usually called once at startup).
     */
    static void set_replace_templates(const nlohmann::json &templates);

    /**
     * Centralized entry point for rewriting /rtp/ or /tv/ paths into upstream RTSP URLs.
     */
    static bool rewrite_path(const std::string &url, std::string &rtsp_url);

private:
    static nlohmann::json replaceTemplates;
    
    static std::vector<std::string> split(const std::string &str, char delimiter);
    static std::string join(const std::vector<std::string> &parts, const std::string &delimiter);
    static std::string simplifyToRegex(const std::string &match_pattern);
    static std::string shiftTime(const std::string &time_str, int shift_hours);
};
