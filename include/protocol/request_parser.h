#pragma once

#include <string>
#include <map>
#include <vector>

struct RequestInfo
{
    std::string method;
    std::string raw_uri;
    std::string version;
    std::string clean_uri; // URI without token parameter
    std::string upstream_url; // Resolved upstream RTSP URL
    std::map<std::string, std::string> params;
    bool is_authorized = false;
    bool is_http = true;
};

class RequestParser
{
public:
    /**
     * Parses the first line of an HTTP or RTSP request and extracts metadata.
     */
    static RequestInfo parse(const std::string &request_data);

private:
    static std::vector<std::string> split(const std::string &str, char delimiter);
};
