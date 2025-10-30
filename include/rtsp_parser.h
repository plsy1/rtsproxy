#pragma once

#include <string>
#include <map>
#include <vector>

struct rtspCtx;
struct Media;

class rtspParser
{
public:
    explicit rtspParser();
    ~rtspParser();

public:
    class SDP
    {
    public:
        static void parseSDP(const std::string &sdp_data,rtspCtx &ctx);

    private:
        static void trim(std::string &str);
        static void parseMedia(const std::string &line,rtspCtx &ctx);
        static void parseAttribute(const std::string &line, Media &media);
        static void parseBandwidth(const std::string &line,rtspCtx &ctx);
    };

public:
    static int parse_server_ports(const std::string &resp, rtspCtx &ctx);
    static int parse_status_code(const std::string &resp);
    static int parse_session_id(const std::string &resp, rtspCtx &ctx);
    static int parse_url(const std::string &url, rtspCtx &ctx);

private:
};
