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

namespace
{

struct Placeholder
{
    const char *token;
    size_t length;
    const char *group;
};

// The documented wildcard vocabulary. Everything else in a match pattern is a
// literal, so it has to be escaped before it reaches std::regex.
constexpr Placeholder kPlaceholders[] = {
    {"{number}", 8, "(\\d+)"},
    {"{word}", 6, "(\\w+)"},
    {"{any}", 5, "(.*?)"},
};

const Placeholder *placeholder_at(const std::string &s, size_t pos)
{
    for (const auto &ph : kPlaceholders) {
        if (s.compare(pos, ph.length, ph.token) == 0)
            return &ph;
    }
    return nullptr;
}

} // namespace

std::string URLRewriter::simplifyToRegex(const std::string &match_pattern)
{
    // Match patterns describe URL query strings, which are full of regex
    // metacharacters ('?', '.', '+', '('). Escaping everything that is not a
    // placeholder keeps a natural pattern like "?playseek={number}" from
    // compiling to an invalid regex.
    static const std::string metacharacters = "\\^$.|?*+()[]{}";

    std::string regex_pattern;
    regex_pattern.reserve(match_pattern.size() * 2);

    size_t pos = 0;
    while (pos < match_pattern.size()) {
        if (const Placeholder *ph = placeholder_at(match_pattern, pos)) {
            regex_pattern += ph->group;
            pos += ph->length;
            continue;
        }
        char c = match_pattern[pos++];
        if (metacharacters.find(c) != std::string::npos)
            regex_pattern += '\\';
        regex_pattern += c;
    }
    return regex_pattern;
}

std::string URLRewriter::expandReplacement(const std::string &replacement)
{
    // std::regex_replace only understands $1/$2 backreferences, but the
    // documented wildcard syntax is {number}/{word}/{any}, and that is what the
    // shipped config.json writes in its replacement strings. Bind each
    // placeholder to the capture group at the same ordinal so both spellings
    // work.
    std::string expanded;
    expanded.reserve(replacement.size());

    int group = 0;
    size_t pos = 0;
    while (pos < replacement.size()) {
        if (const Placeholder *ph = placeholder_at(replacement, pos)) {
            expanded += '$';
            expanded += std::to_string(++group);
            pos += ph->length;
            continue;
        }
        expanded += replacement[pos++];
    }
    return expanded;
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
        time_t timestamp;
        try {
            size_t parsed = 0;
            long long value = std::stoll(time_str, &parsed);
            if (parsed != time_str.size()) return time_str;
            timestamp = static_cast<time_t>(value);
        } catch (...) {
            return time_str;
        }
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
            if (!template_obj.is_object() ||
                !template_obj.contains("action") || !template_obj["action"].is_string() ||
                !template_obj.contains("match") || !template_obj["match"].is_string()) {
                Logger::warn("[REWRITE] Skipping template without a string action/match");
                continue;
            }

            std::string action = template_obj["action"];
            std::string match_pattern = template_obj["match"];
            std::string regex_pattern = simplifyToRegex(match_pattern);
            try {
                std::regex rgx(regex_pattern);
                if (action == "remove") {
                    processed_url = std::regex_replace(processed_url, rgx, "");
                } else if (action == "replace") {
                    if (!template_obj.contains("replacement") || !template_obj["replacement"].is_string()) {
                        Logger::warn("[REWRITE] Skipping replace template without a replacement: " + match_pattern);
                        continue;
                    }
                    std::string replacement = expandReplacement(template_obj["replacement"]);
                    processed_url = std::regex_replace(processed_url, rgx, replacement);
                } else if (action == "timeshift") {
                    if (!template_obj.contains("shift_hours") || !template_obj["shift_hours"].is_number_integer()) {
                        Logger::warn("[REWRITE] Skipping timeshift template without shift_hours: " + match_pattern);
                        continue;
                    }
                    int shift_hours = template_obj["shift_hours"];
                    std::smatch match;
                    if (std::regex_search(processed_url, match, rgx) && match.size() >= 3) {
                        std::string new_start = shiftTime(match[1].str(), shift_hours);
                        std::string new_end = shiftTime(match[2].str(), shift_hours);

                        // Splice by capture-group offset rather than by searching
                        // for the timestamp text, which would also hit a digit
                        // belonging to the pattern's own literal part. The later
                        // group goes first so the earlier edit cannot shift the
                        // offset that is still needed.
                        processed_url.replace(match.position(2), match.length(2), new_end);
                        processed_url.replace(match.position(1), match.length(1), new_start);
                    }
                }
            } catch (const std::regex_error &e) {
                // One malformed rule must not fail the whole request.
                Logger::error("[REWRITE] Skipping template '" + match_pattern + "': " + e.what());
                continue;
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
