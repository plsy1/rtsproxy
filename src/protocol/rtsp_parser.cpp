#include "protocol/rtsp_parser.h"
#include "core/logger.h"
#include "common/rtsp_ctx.h"
#include "utils/dns_resolver.h"
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <limits>
#include <arpa/inet.h>

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

    try
    {
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
    catch (...)
    {
        Logger::error("[RTSP] Failed to parse media port: " + port_str);
    }
}

void rtspParser::SDP::parseAttribute(const std::string &line, Media &media)
{
    // RFC 4566 allows both "a=<key>:<value>" and the bare property form "a=<key>"
    std::string attribute = line.substr(2);
    size_t colon = attribute.find(':');
    std::string key = attribute.substr(0, colon);
    std::string value = (colon == std::string::npos) ? std::string() : attribute.substr(colon + 1);
    if (key.empty())
        return;

    media.attributes[key] = value;
    if (key == "control")
        media.trackID = value;
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

namespace
{

// A single Transport range endpoint: a decimal number, optionally padded with
// horizontal whitespace as some servers do.
bool parse_transport_number(const std::string &text, int &value)
{
    try
    {
        size_t parsed = 0;
        int number = std::stoi(text, &parsed);
        while (parsed < text.size() && (text[parsed] == ' ' || text[parsed] == '\t'))
            ++parsed;
        if (parsed != text.size())
            return false;
        value = number;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace

int rtspParser::parse_server_ports(const std::string &resp, rtspCtx &ctx)
{
    std::string transport_ = extract_header_value(resp, "Transport");
    if (transport_.empty())
        return -1;

    size_t sp_pos = transport_.find("server_port=");
    if (sp_pos != std::string::npos)
    {
        sp_pos += strlen("server_port=");
        // the range must be read from this parameter only, never from a later one
        size_t sp_end = transport_.find(';', sp_pos);
        if (sp_end == std::string::npos)
            sp_end = transport_.size();
        size_t dash = transport_.find('-', sp_pos);
        if (dash == std::string::npos || dash >= sp_end)
            return -1;

        int rtp_port = 0;
        int rtcp_port = 0;
        if (!parse_transport_number(transport_.substr(sp_pos, dash - sp_pos), rtp_port) ||
            !parse_transport_number(transport_.substr(dash + 1, sp_end - dash - 1), rtcp_port))
            return -1;
        if (rtp_port < 1 || rtp_port > 65535 ||
            rtcp_port < 1 || rtcp_port > 65535)
            return -1;
        ctx.server_rtp_port = static_cast<uint16_t>(rtp_port);
        ctx.server_rtcp_port = static_cast<uint16_t>(rtcp_port);
        return 0;
    }

    size_t int_pos = transport_.find("interleaved=");
    if (int_pos != std::string::npos)
    {
        int_pos += strlen("interleaved=");
        size_t int_end = transport_.find(';', int_pos);
        if (int_end == std::string::npos)
            int_end = transport_.size();
        size_t dash = transport_.find('-', int_pos);
        if (dash == std::string::npos || dash >= int_end)
            return -1;

        // For interleaved mode, we can store channels in port fields
        // The caller (RTSPToHttpClient) will check for "interleaved" in the string anyway
        int rtp_channel = 0;
        int rtcp_channel = 0;
        if (!parse_transport_number(transport_.substr(int_pos, dash - int_pos), rtp_channel) ||
            !parse_transport_number(transport_.substr(dash + 1, int_end - dash - 1), rtcp_channel))
            return -1;
        if (rtp_channel < 0 || rtp_channel > 255 ||
            rtcp_channel < 0 || rtcp_channel > 255)
            return -1;
        ctx.server_rtp_port = static_cast<uint16_t>(rtp_channel);
        ctx.server_rtcp_port = static_cast<uint16_t>(rtcp_channel);
        return 0;
    }

    return -1;
}

int rtspParser::get_content_length(const std::string &resp)
{
    std::string value = extract_header_value(resp, "Content-Length");
    if (value.empty())
        return 0;

    try
    {
        size_t parsed = 0;
        long long length = std::stoll(value, &parsed);
        while (parsed < value.size() &&
               (value[parsed] == ' ' || value[parsed] == '\t'))
            ++parsed;
        if (parsed != value.size() || length < 0 ||
            length > std::numeric_limits<int>::max())
            return -1;
        return static_cast<int>(length);
    }
    catch (...)
    {
        return -1;
    }
}

int rtspParser::parse_status_code(const std::string &resp)
{
    int code = -1;
    sscanf(resp.c_str(), "RTSP/%*s %d", &code);
    return code;
}

int rtspParser::parse_session_id(const std::string &resp, rtspCtx &ctx)
{
    std::string session = extract_header_value(resp, "Session");
    if (session.empty())
        return -1;
    size_t end = session.find(';');
    ctx.session_id = session.substr(0, end);
    return 0;
}

int rtspParser::parse_url(const std::string &url, rtspCtx &ctx)
{
    std::string clean_url = url;
    size_t pos;
    while ((pos = clean_url.find("%5C")) != std::string::npos) {
        clean_url.replace(pos, 3, "");
    }
    while ((pos = clean_url.find("%5c")) != std::string::npos) {
        clean_url.replace(pos, 3, "");
    }
    while ((pos = clean_url.find('\\')) != std::string::npos) {
        clean_url.replace(pos, 1, "");
    }

    ctx.rtsp_url = clean_url;
    if (clean_url.rfind("rtsp://", 0) != 0)
        return -1;

    size_t slash = clean_url.find('/', 7);
    std::string hostport = clean_url.substr(7, (slash == std::string::npos) ? std::string::npos : slash - 7);
    if (hostport.empty())
    {
        return -1;
    }
    size_t colon = hostport.find(':');

    try
    {
        if (colon != std::string::npos)
        {
            ctx.server_ip = hostport.substr(0, colon);
            size_t parsed = 0;
            int port = std::stoi(hostport.substr(colon + 1), &parsed);
            if (parsed != hostport.size() - colon - 1 || port < 1 || port > 65535)
                return -1;
            ctx.server_rtsp_port = static_cast<uint16_t>(port);
        }
        else
        {
            ctx.server_ip = hostport;
            ctx.server_rtsp_port = 554;
        }
    }
    catch (...)
    {
        Logger::error("[RTSP] Failed to parse port in URL: " + clean_url);
        return -1;
    }

    struct in_addr numeric_addr{};
    if (inet_pton(AF_INET, ctx.server_ip.c_str(), &numeric_addr) != 1)
    {
        auto addresses = DNSResolver::resolve_ipv4(ctx.server_ip);
        if (addresses.empty())
        {
            Logger::error("[RTSP] Failed to resolve host in URL: " + ctx.server_ip);
            return -1;
        }
        ctx.server_ip = addresses.front();
    }

    ctx.path = (slash != std::string::npos) ? clean_url.substr(slash) : "/";

    return 0;
}

std::string rtspParser::extract_header_value(const std::string &msg, const std::string &header_name)
{
    std::string lower_msg = msg;
    std::string lower_hdr = header_name;
    std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(), [](unsigned char c) { return std::tolower(c); });
    std::transform(lower_hdr.begin(), lower_hdr.end(), lower_hdr.begin(), [](unsigned char c) { return std::tolower(c); });

    const std::string needle = lower_hdr + ":";
    size_t pos = 0;
    while (true)
    {
        pos = lower_msg.find(needle, pos);
        if (pos == std::string::npos)
            return {};
        if (pos == 0 || (pos >= 2 && msg[pos - 2] == '\r' && msg[pos - 1] == '\n'))
            break;
        pos += needle.size();
    }
    pos += header_name.size() + 1;
    while (pos < msg.size() && (msg[pos] == ' ' || msg[pos] == '\t'))
        ++pos;
    size_t end = msg.find("\r\n", pos);
    if (end == std::string::npos)
        return msg.substr(pos);
    return msg.substr(pos, end - pos);
}

std::string rtspParser::replace_request_uri(const std::string &request, const std::string &uri)
{
    size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos)
        return request;

    size_t method_end = request.find(' ');
    if (method_end == std::string::npos || method_end >= line_end)
        return request;

    size_t version_start = request.find(' ', method_end + 1);
    if (version_start == std::string::npos || version_start >= line_end)
        return request;

    std::string result = request;
    result.replace(method_end + 1, version_start - method_end - 1, uri);
    return result;
}
