#include "../include/http_parser.h"
#include "../include/server_config.h"
#include "../include/3rd/json.hpp"
#include "../include/logger.h"
#include <fstream>
#include <regex>
#include <vector>
#include <ctime>

using json = nlohmann::json;

nlohmann::json httpParser::parseConfig;

std::string httpParser::simplifyToRegex(const std::string &match_pattern)
{

    std::string regex_pattern = match_pattern;

    size_t pos = 0;
    while ((pos = regex_pattern.find("{number}", pos)) != std::string::npos)
    {
        regex_pattern.replace(pos, 8, "(\\d+)");
        pos += 6;
    }
    pos = 0;
    while ((pos = regex_pattern.find("{word}", pos)) != std::string::npos)
    {
        regex_pattern.replace(pos, 7, "(\\w+)");
        pos += 6;
    }
    pos = 0;
    while ((pos = regex_pattern.find("{any}", pos)) != std::string::npos)
    {
        regex_pattern.replace(pos, 6, "(.*?)");
        pos += 5;
    }
    pos = 0;
    while ((pos = regex_pattern.find("/", pos)) != std::string::npos)
    {
        regex_pattern.replace(pos, 1, "\\/");
        pos += 2;
    }

    return regex_pattern;
}

std::string httpParser::shiftTime(const std::string &time_str, int shift_hours)
{
    if (time_str.length() == 14)
    {
        struct tm timeinfo = {};
        if (strptime(time_str.c_str(), "%Y%m%d%H%M%S", &timeinfo) == nullptr)
        {
            return time_str;
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
        return time_str;
    }
}

void httpParser::shiftTimeInString(std::string &input, const std::string &regex_pattern, int shift_hours)
{

    std::regex rgx(regex_pattern);
    std::smatch match;

    if (!regex_search(input, match, rgx))
        return;

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

std::vector<std::string> httpParser::split(const std::string &str, char delimiter)
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

std::string httpParser::join(const std::vector<std::string> &parts, const std::string &delimiter)
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

bool httpParser::load_json(const std::string jsonPath)
{

    std::ifstream ifs(jsonPath);
    if (!ifs.is_open())
    {
        Logger::warn("[SERVER] Failed to open rewrite file.");
        return false;
    }

    try {
        parseConfig = json::parse(ifs, nullptr, true, true);
    } catch (const json::parse_error& e) {
        Logger::error("[CONFIG] JSON parse error: " + std::string(e.what()));
        return false;
    }

    if (parseConfig.contains("blacklist") && parseConfig["blacklist"].is_array())
    {
        std::vector<std::string> blacklist;
        for (const auto &item : parseConfig["blacklist"])
        {
            if (item.is_string())
            {
                blacklist.push_back(item.get<std::string>());
            }
        }
        ServerConfig::setBlacklist(blacklist);
        Logger::info("[CONFIG] Loaded " + std::to_string(blacklist.size()) + " items into blacklist");
    }

    // Load global settings
    if (parseConfig.contains("settings") && parseConfig["settings"].is_object())
    {
        const auto& s = parseConfig["settings"];
        if (s.contains("port")) ServerConfig::setPort(s["port"].get<int>());
        if (s.contains("nat_method")) ServerConfig::setNatMethod(s["nat_method"].get<std::string>());
        if (s.contains("enable_nat")) ServerConfig::setNatEnabled(s["enable_nat"].get<bool>());
        if (s.contains("buffer_pool_count")) ServerConfig::setBufferPoolCount(s["buffer_pool_count"].get<int>());
        if (s.contains("buffer_pool_block_size")) ServerConfig::setBufferPoolBlockSize(s["buffer_pool_block_size"].get<int>());
        if (s.contains("auth_token")) ServerConfig::setToken(s["auth_token"].get<std::string>());
        if (s.contains("log_file")) ServerConfig::setLogFile(s["log_file"].get<std::string>());
        if (s.contains("log_lines")) ServerConfig::setLogLines(s["log_lines"].get<size_t>());
        if (s.contains("log_level")) {
            std::string level = s["log_level"].get<std::string>();
            if (level == "error") Logger::setLogLevel(LogLevel::ERROR);
            else if (level == "warn") Logger::setLogLevel(LogLevel::WARN);
            else if (level == "info") Logger::setLogLevel(LogLevel::INFO);
            else if (level == "debug") Logger::setLogLevel(LogLevel::DEBUG);
        }
        if (s.contains("strip_padding")) ServerConfig::setStripPadding(s["strip_padding"].get<bool>());
        if (s.contains("wait_keyframe")) ServerConfig::setWaitKeyframe(s["wait_keyframe"].get<bool>());
        if (s.contains("watchdog")) ServerConfig::setWatchdogEnabled(s["watchdog"].get<bool>());
        if (s.contains("daemon")) ServerConfig::setDaemonEnabled(s["daemon"].get<bool>());
        if (s.contains("http_interface")) ServerConfig::setHttpUpstreamInterface(s["http_interface"].get<std::string>());
        if (s.contains("mitm_interface")) ServerConfig::setMitmUpstreamInterface(s["mitm_interface"].get<std::string>());
        if (s.contains("listen_interface")) ServerConfig::setListenInterface(s["listen_interface"].get<std::string>());
        if (s.contains("stun_host")) ServerConfig::setStunHost(s["stun_host"].get<std::string>());
        if (s.contains("stun_port")) ServerConfig::setStunPort(s["stun_port"].get<int>());
        
        Logger::info("[CONFIG] Global settings loaded from config file");
    }

    return true;
}

bool httpParser::parse_rtp_url(const std::string &url, std::string &rtsp_url)
{
    rtsp_url = url.substr(5);
    rtsp_url = "rtsp://" + rtsp_url;

    return true;
}

bool httpParser::parse_tv_url(const std::string &url, std::string &rtsp_url)
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

        Logger::debug("[SERVER] Playback address rewritten to: " + rtsp_url);
        return true;
    }
}

bool httpParser::parse_http_url(const std::string &url, std::string &rtsp_url)
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