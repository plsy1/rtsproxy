#include "../include/rtsp_parser.h"
#include "../include/logger.h"
#include "../include/common/rtsp_ctx.h"
#include <unistd.h>
#include <cstring>
#include <sstream>

rtspParser::rtspParser() {}
rtspParser::~rtspParser() {}

void rtspParser::SDP::parseSDP(const std::string &sdp_data, rtspCtx &ctx)
{
    std::istringstream sdp_stream(sdp_data);
    std::string line;

    while (std::getline(sdp_stream, line))
    {
        trim(line);
        if (line.empty())
            continue;

        if (line.substr(0, 2) == "v=")
        {
            ctx.sdp.version = line.substr(2);
        }
        else if (line.substr(0, 2) == "o=")
        {
            ctx.sdp.origin = line.substr(2);
        }
        else if (line.substr(0, 2) == "s=")
        {
            ctx.sdp.session_name = line.substr(2);
        }
        else if (line.substr(0, 2) == "t=")
        {
            ctx.sdp.time = line.substr(2);
        }
        else if (line.substr(0, 2) == "m=")
        {
            parseMedia(line, ctx);
        }
        else if (line.substr(0, 2) == "b=")
        {
            parseBandwidth(line, ctx);
        }
        else if (line.substr(0, 2) == "a=")
        {
            if (!ctx.sdp.media_streams.empty())
            {
                parseAttribute(line, ctx.sdp.media_streams.back());
            }
        }
    }
}

void rtspParser::SDP::trim(std::string &str)
{
    const std::string whitespace = " \t\n\r";
    str.erase(0, str.find_first_not_of(whitespace));
    str.erase(str.find_last_not_of(whitespace) + 1);
}

void rtspParser::SDP::parseMedia(const std::string &line, rtspCtx &ctx)
{
    std::istringstream media_stream(line.substr(2));
    std::string type, port_str, protocol;
    std::getline(media_stream, type, ' ');
    std::getline(media_stream, port_str, ' ');
    std::getline(media_stream, protocol, ' ');

    int port = std::stoi(port_str);
    Media media;
    media.type = type;
    media.port = port;
    media.protocol = protocol;
    std::string format;
    while (std::getline(media_stream, format, ' '))
    {
        media.formats.push_back(format);
    }

    ctx.sdp.media_streams.push_back(media);
}

void rtspParser::SDP::parseAttribute(const std::string &line, Media &media)
{
    std::istringstream attr_stream(line.substr(2));
    std::string key, value;
    if (std::getline(attr_stream, key, ':') && std::getline(attr_stream, value))
    {
        media.attributes[key] = value;
        if (key == "control")
            media.trackID = value;
    }
}

void rtspParser::SDP::parseBandwidth(const std::string &line, rtspCtx &ctx)
{
    std::istringstream bandwidth_stream(line.substr(2));
    std::string type;
    int value;

    if (std::getline(bandwidth_stream, type, ':') && bandwidth_stream >> value)
    {
        ctx.sdp.session_bandwidth[type] = value;
    }
}

int rtspParser::parse_server_ports(const std::string &resp, rtspCtx &ctx)
{
    size_t pos = resp.find("Transport:");
    if (pos == std::string::npos)
        return -1;

    size_t end = resp.find("\r\n", pos);
    std::string transport_ = resp.substr(pos, end - pos);

    size_t sp_pos = transport_.find("server_port=");
    if (sp_pos != std::string::npos)
    {
        sp_pos += strlen("server_port=");
        size_t dash = transport_.find('-', sp_pos);
        if (dash != std::string::npos)
        {
            try
            {
                ctx.server_rtp_port = std::stoi(transport_.substr(sp_pos, dash - sp_pos));
                ctx.server_rtcp_port = std::stoi(transport_.substr(dash + 1));
                return 0;
            }
            catch (...)
            {
                return -1;
            }
        }
    }
    return -1;
}

int rtspParser::parse_status_code(const std::string &resp)
{
    int code = -1;
    sscanf(resp.c_str(), "RTSP/%*s %d", &code);
    return code;
}

int rtspParser::parse_session_id(const std::string &resp, rtspCtx &ctx)
{
    size_t pos = resp.find("Session:");
    if (pos == std::string::npos)
        return -1;
    pos += 8;
    while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t'))
        ++pos;
    size_t end = resp.find_first_of(";\r\n", pos);
    ctx.session_id = resp.substr(pos, end - pos);
    return 0;
}

int rtspParser::parse_url(const std::string &url, rtspCtx &ctx)
{
    if (!url.rfind("rtsp://", 0) == 0)
        return -1;

    size_t slash = url.find('/', 7);
    std::string hostport = url.substr(7, slash - 7);
    size_t colon = hostport.find(':');

    if (colon != std::string::npos)
    {
        ctx.server_ip = hostport.substr(0, colon);
        ctx.server_rtsp_port = std::stoi(hostport.substr(colon + 1));
    }
    else
    {
        ctx.server_ip = hostport;
        ctx.server_rtsp_port = 554;
    }

    ctx.path = (slash != std::string::npos) ? url.substr(slash) : "/";

    return 0;
}