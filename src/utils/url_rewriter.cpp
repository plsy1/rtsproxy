#include "utils/url_rewriter.h"
#include "core/logger.h"
#include <regex>
#include <vector>
#include <ctime>

using json = nlohmann::json;

nlohmann::json URLRewriter::replaceTemplates;

void URLRewriter::set_replace_templates(const nlohmann::json &templates)
{
    replaceTemplates = templates;
}

std::string URLRewriter::simplifyToRegex(const std::string &match_pattern)
{
    std::string regex_pattern = match_pattern;
    size_t pos = 0;
    while ((pos = regex_pattern.find("{number}", pos)) != std::string::npos) {
        regex_pattern.replace(pos, 8, "(\\d+)");
        pos += 6;
    }
    pos = 0;
    while ((pos = regex_pattern.find("{word}", pos)) != std::string::npos) {
        regex_pattern.replace(pos, 7, "(\\w+)");
        pos += 6;
    }
    pos = 0;
    while ((pos = regex_pattern.find("{any}", pos)) != std::string::npos) {
        regex_pattern.replace(pos, 6, "(.*?)");
        pos += 5;
    }
    pos = 0;
    while ((pos = regex_pattern.find("/", pos)) != std::string::npos) {
        regex_pattern.replace(pos, 1, "\\/");
        pos += 2;
    }
    return regex_pattern;
}

std::string URLRewriter::shiftTime(const std::string &time_str, int shift_hours)
{
    if (time_str.length() == 14) {
        struct tm timeinfo = {};
        if (strptime(time_str.c_str(), "%Y%m%d%H%M%S", &timeinfo) == nullptr) return time_str;
        time_t time_epoch = mktime(&timeinfo);
        time_epoch += shift_hours * 3600;
        struct tm *new_timeinfo = localtime(&time_epoch);
        char buffer[16];
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", new_timeinfo);
        return std::string(buffer);
    } else if (time_str.length() <= 10) {
        time_t timestamp = std::stoll(time_str);
        timestamp += shift_hours * 3600;
        struct tm *new_timeinfo = gmtime(&timestamp);
        char buffer[16];
        strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", new_timeinfo);
        return std::string(buffer);
    }
    return time_str;
}

bool URLRewriter::rewrite_path(const std::string &url, std::string &rtsp_url)
{
    std::string processed_url = url;
    bool is_tv = (url.find("/tv/") == 0);
    bool is_rtp = (url.find("/rtp/") == 0);

    if (!is_tv && !is_rtp) return false;

    // Apply templates for TV URLs (playback links usually have query params)
    if (is_tv && url.find('?') != std::string::npos) {
        for (const auto &template_obj : replaceTemplates) {
            std::string action = template_obj["action"];
            std::string match_pattern = template_obj["match"];
            std::string regex_pattern = simplifyToRegex(match_pattern);
            try {
                std::regex rgx(regex_pattern);
                if (action == "remove") {
                    processed_url = std::regex_replace(processed_url, rgx, "");
                } else if (action == "replace") {
                    std::string replacement = template_obj["replacement"];
                    processed_url = std::regex_replace(processed_url, rgx, replacement);
                } else if (action == "timeshift") {
                    int shift_hours = template_obj["shift_hours"];
                    std::smatch match;
                    if (std::regex_search(processed_url, match, rgx) && match.size() >= 3) {
                        std::string start_time = match[1];
                        std::string end_time = match[2];
                        std::string new_start = shiftTime(start_time, shift_hours);
                        std::string new_end = shiftTime(end_time, shift_hours);
                        
                        std::string full_match = match[0];
                        size_t pos1 = full_match.find(start_time);
                        if (pos1 != std::string::npos) full_match.replace(pos1, start_time.length(), new_start);
                        size_t pos2 = full_match.find(end_time, pos1 + new_start.length());
                        if (pos2 != std::string::npos) full_match.replace(pos2, end_time.length(), new_end);
                        
                        processed_url.replace(match.position(0), match.length(0), full_match);
                    }
                }
            } catch (const std::regex_error &e) {
                Logger::error(std::string("Regex error: ") + e.what());
                return false;
            }
        }
    }

    // Strip prefix and prepend rtsp://
    if (is_tv) {
        if (processed_url.length() <= 4) return false;
        rtsp_url = "rtsp://" + processed_url.substr(4);
    } else { // is_rtp
        if (processed_url.length() <= 5) return false;
        rtsp_url = "rtsp://" + processed_url.substr(5);
    }

    return true;
}

std::vector<std::string> URLRewriter::split(const std::string &str, char delimiter)
{
    std::vector<std::string> tokens;
    size_t start = 0, end = str.find(delimiter);
    while (end != std::string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    tokens.push_back(str.substr(start));
    return tokens;
}

std::string URLRewriter::join(const std::vector<std::string> &parts, const std::string &delimiter)
{
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) result += delimiter;
        result += parts[i];
    }
    return result;
}
