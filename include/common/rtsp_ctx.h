#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>

struct Media
{
    std::string type;
    int port;
    std::string protocol;
    std::string trackID;
    std::vector<std::string> formats;
    std::map<std::string, std::string> attributes;
    std::map<std::string, int> bandwidth;
};

struct sdpCtx
{
    std::string session_name;
    std::string version;
    std::string origin;
    std::string time;
    std::map<std::string, int> session_bandwidth;
    std::vector<Media> media_streams;
};

struct rtspCtx
{
    uint16_t server_rtsp_port;
    uint16_t server_rtp_port;
    uint16_t server_rtcp_port;

    std::string session_id;
    std::string server_ip;
    std::string path;
    std::string rtsp_url;

    sdpCtx sdp;

    rtspCtx() = default;
};