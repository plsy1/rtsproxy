#include "../include/rtsp_mitm_client.h"
#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/buffer_pool.h"
#include "../include/common/socket_ctx.h"
#include "../include/common/rtsp_ctx.h"
#include "../include/rtsp_parser.h"
#include "../include/socket_helper.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sys/timerfd.h>
#include <chrono>
#include <regex>
#include <sstream>

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

static std::string extract_header_value(const std::string &msg,
                                        const std::string &header_name)
{
    // Search case-insensitively for "Header-Name:"
    std::string lower_msg = msg;
    std::string lower_hdr = header_name;
    std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(), ::tolower);
    std::transform(lower_hdr.begin(), lower_hdr.end(), lower_hdr.begin(), ::tolower);

    size_t pos = lower_msg.find(lower_hdr + ":");
    if (pos == std::string::npos)
        return {};
    pos += header_name.size() + 1;
    while (pos < msg.size() && (msg[pos] == ' ' || msg[pos] == '\t'))
        ++pos;
    size_t end = msg.find("\r\n", pos);
    return msg.substr(pos, end - pos);
}

static std::string replace_header(const std::string &msg,
                                  const std::string &header_name,
                                  const std::string &new_value)
{
    std::string result = msg;
    std::string lower_result = result;
    std::string lower_hdr = header_name;
    std::transform(lower_result.begin(), lower_result.end(), lower_result.begin(), ::tolower);
    std::transform(lower_hdr.begin(), lower_hdr.end(), lower_hdr.begin(), ::tolower);

    size_t pos = lower_result.find(lower_hdr + ":");
    if (pos == std::string::npos)
        return msg;

    size_t end = result.find("\r\n", pos);
    if (end == std::string::npos)
        return msg;

    result.replace(pos, end - pos, header_name + ": " + new_value);
    return result;
}

/* ========================================================================= */
/* Constructor / Destructor                                                   */
/* ========================================================================= */

RTSPMitmClient::RTSPMitmClient(EpollLoop *loop, BufferPool &pool,
                               const sockaddr_in &client_addr, int client_fd,
                               const std::string &first_request)
    : loop_(loop),
      pool_(pool),
      downstream_fd_(client_fd, loop),
      downstream_ctx_(std::make_unique<SocketCtx>(
          client_fd,
          [this](uint32_t ev) { handle_downstream(ev); })),
      client_addr_(client_addr)
{
    // Remove the simple EPOLLIN watch that was set by the accept handler;
    // we will re-register it ourselves.
    loop_->remove(client_fd);

    // Peek at the first bytes to extract the RTSP URL from the request line.
    // The first request was already recv'd and passed in as first_request.
    // We need to figure out where the upstream server is.
    //
    // The client sends something like:
    //   OPTIONS rtsp://192.168.1.100:554/stream RTSP/1.0
    //
    std::string url;
    std::istringstream ss(first_request);
    std::string method, uri, version;
    ss >> method >> uri >> version;

    if (uri.rfind("rtsp://", 0) != 0)
    {
        Logger::error("[MITM] First request does not contain a valid rtsp:// URI: " + uri);
        if (on_closed_) on_closed_();
        return;
    }

    ctx_.rtsp_url = uri;
    if (rtspParser::parse_url(uri, ctx_) != 0)
    {
        Logger::error("[MITM] Failed to parse RTSP URL: " + uri);
        if (on_closed_) on_closed_();
        return;
    }

    // ---------------------------------------------------------------
    // Proxy-path URL format:
    //   rtsp://proxy-host:port/real-host:port/actual/path
    //
    // After parse_url, ctx_.server_ip  = proxy-host
    //                  ctx_.path       = /real-host:port/actual/path
    //
    // Detect this by checking whether the first path segment looks like
    // "host:port" (contains a colon and only valid hostname/IP chars).
    // ---------------------------------------------------------------
    {
        std::string path = ctx_.path; // e.g. "/112.245.125.44:1554/iptv/..."
        if (path.size() > 1)
        {
            std::string stripped = path.substr(1); // remove leading '/'
            size_t slash = stripped.find('/');
            std::string first_seg = (slash != std::string::npos)
                                        ? stripped.substr(0, slash)
                                        : stripped;
            std::string real_path = (slash != std::string::npos)
                                        ? stripped.substr(slash)
                                        : "/";

            size_t colon = first_seg.rfind(':');
            if (colon != std::string::npos && colon > 0 && colon < first_seg.size() - 1)
            {
                std::string real_host = first_seg.substr(0, colon);
                std::string port_str  = first_seg.substr(colon + 1);
                bool all_digits = !port_str.empty() &&
                                  std::all_of(port_str.begin(), port_str.end(), ::isdigit);
                if (all_digits)
                {
                    // Store proxy prefix and real upstream base for URI rewriting.
                    proxy_uri_prefix_  = "rtsp://" + ctx_.server_ip + ":" +
                                         std::to_string(ctx_.server_rtsp_port) +
                                         "/" + first_seg; // proxy addr + /real-host:port
                    upstream_uri_base_ = "rtsp://" + real_host + ":" + port_str;

                    // Update ctx_ to point at the real upstream server.
                    ctx_.server_ip        = real_host;
                    ctx_.server_rtsp_port = static_cast<uint16_t>(std::stoi(port_str));
                    ctx_.path             = real_path;
                    ctx_.rtsp_url         = upstream_uri_base_ + real_path;

                    Logger::info("[MITM] Proxy-path URL detected. Upstream: " +
                                 ctx_.server_ip + ":" +
                                 std::to_string(ctx_.server_rtsp_port) +
                                 ctx_.path);
                }
            }
        }
    }

    // Stash the first request so it gets forwarded once the upstream TCP
    // connection is established.
    downstream_recv_buf_ = first_request;

    // Register downstream fd for HUP/ERR only; we will not read more until
    // upstream is ready.
    loop_->set(downstream_ctx_.get(), client_fd,
               EPOLLRDHUP | EPOLLHUP | EPOLLERR);

    connect_upstream();
}

RTSPMitmClient::~RTSPMitmClient()
{
}

void RTSPMitmClient::set_on_closed_callback(ClosedCallback cb)
{
    on_closed_ = std::move(cb);
}

/* ========================================================================= */
/* Connect to upstream                                                        */
/* ========================================================================= */

void RTSPMitmClient::connect_upstream()
{
    upstream_fd_ = create_nonblocking_tcp(ctx_.server_ip, ctx_.server_rtsp_port,
                                          ServerConfig::getInterface());
    if (upstream_fd_ < 0)
    {
        Logger::error("[MITM] Failed to connect to upstream " + ctx_.server_ip +
                      ":" + std::to_string(ctx_.server_rtsp_port));
        close_all();
        return;
    }

    upstream_ctx_ = std::make_unique<SocketCtx>(
        upstream_fd_,
        [this](uint32_t ev) { handle_upstream(ev); });

    // EPOLLOUT fires when non-blocking connect completes.
    loop_->set(upstream_ctx_.get(), upstream_fd_, EPOLLOUT);
    state_ = State::WAIT_UPSTREAM_CONNECT;
}

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

bool RTSPMitmClient::extract_client_port(const std::string &req,
                                         uint16_t &rtp_port, uint16_t &rtcp_port)
{
    std::string transport = extract_header_value(req, "Transport");
    if (transport.empty())
        return false;

    // client_port=NNNN-MMMM
    std::regex re(R"(client_port=(\d+)-(\d+))");
    std::smatch m;
    if (!std::regex_search(transport, m, re))
        return false;

    rtp_port = static_cast<uint16_t>(std::stoi(m[1]));
    rtcp_port = static_cast<uint16_t>(std::stoi(m[2]));
    return true;
}

bool RTSPMitmClient::init_relay_sockets()
{
    // Allocate our RTP/RTCP relay sockets (facing upstream).
    if (bind_udp_socket_with_retry(rtp_us_fd_.get_ref(), local_rtp_port_, 5,
                                   ServerConfig::getInterface()) < 0)
    {
        Logger::error("[MITM] Failed to bind upstream-facing RTP socket");
        return false;
    }
    local_rtcp_port_ = local_rtp_port_ + 1;
    if (bind_udp_socket(rtcp_us_fd_.get_ref(), local_rtcp_port_,
                        ServerConfig::getInterface()) < 0)
    {
        Logger::error("[MITM] Failed to bind upstream-facing RTCP socket");
        return false;
    }

    rtp_us_ctx_ = std::make_unique<SocketCtx>(
        rtp_us_fd_,
        [this](uint32_t ev) { handle_rtp_from_upstream(ev); });
    rtcp_us_ctx_ = std::make_unique<SocketCtx>(
        rtcp_us_fd_,
        [this](uint32_t ev) { handle_rtcp_from_upstream(ev); });

    loop_->set(rtp_us_ctx_.get(), rtp_us_fd_, EPOLLIN);
    loop_->set(rtcp_us_ctx_.get(), rtcp_us_fd_, EPOLLIN);

    // Optimize UDP buffers using ServerConfig values
    // rtp_buffer_size in config is number of packets, so we multiply by packet size.
    int total_buf_size = ServerConfig::getRtpBufferSize() * ServerConfig::getUdpPacketSize();
    if (total_buf_size < 1024 * 1024) total_buf_size = 2 * 1024 * 1024; // Min 2MB

    setsockopt(rtp_us_fd_, SOL_SOCKET, SO_RCVBUF, &total_buf_size, sizeof(total_buf_size));
    setsockopt(rtp_us_fd_, SOL_SOCKET, SO_SNDBUF, &total_buf_size, sizeof(total_buf_size));
    setsockopt(rtcp_us_fd_, SOL_SOCKET, SO_RCVBUF, &total_buf_size, sizeof(total_buf_size));
    setsockopt(rtcp_us_fd_, SOL_SOCKET, SO_SNDBUF, &total_buf_size, sizeof(total_buf_size));

    Logger::info("[MITM] Relay RTP ports: " + std::to_string(local_rtp_port_) +
                 "-" + std::to_string(local_rtcp_port_) + " (Buffers: " + 
                 std::to_string(total_buf_size / 1024) + " KB)");
    return true;
}

/* ========================================================================= */
/* Response patching for client (MITM)                                        */
/* ========================================================================= */

std::string RTSPMitmClient::patch_response_for_client(const std::string &resp)
{
    std::string result = resp;
    std::string proxy_ip = "10.1.0.6"; // Should ideally be dynamic from ServerConfig or socket
    uint16_t proxy_port = 8555;      // Should ideally be dynamic

    // 1. Rewrite Transport header (if present)
    std::string transport = extract_header_value(result, "Transport");
    if (!transport.empty())
    {
        Logger::debug("[MITM] Original Transport: " + transport);

        // Parse server_port from upstream response and remember it.
        std::regex sp_re(R"(server_port=(\d+)-(\d+))");
        std::smatch sm;
        if (std::regex_search(transport, sm, sp_re))
        {
            uint16_t srv_rtp = static_cast<uint16_t>(std::stoi(sm[1]));
            uint16_t srv_rtcp = static_cast<uint16_t>(std::stoi(sm[2]));
            server_rtp_addr_.sin_family = AF_INET;
            server_rtp_addr_.sin_port = htons(srv_rtp);
            inet_pton(AF_INET, ctx_.server_ip.c_str(), &server_rtp_addr_.sin_addr);
            server_rtcp_addr_.sin_family = AF_INET;
            server_rtcp_addr_.sin_port = htons(srv_rtcp);
            inet_pton(AF_INET, ctx_.server_ip.c_str(), &server_rtcp_addr_.sin_addr);
            
            // Send a trigger packet to upstream now that we know its port.
            char dummy = 0;
            sendto(rtp_us_fd_, &dummy, 1, 0, (struct sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
        }

        // Rewrite client_port back to original client ports.
        std::regex cp_re(R"(client_port=\d+-\d+)");
        std::string client_rtp_str = std::to_string(ntohs(client_rtp_addr_.sin_port));
        std::string client_rtcp_str = std::to_string(ntohs(client_rtcp_addr_.sin_port));
        transport = std::regex_replace(
            transport, cp_re,
            "client_port=" + client_rtp_str + "-" + client_rtcp_str);

        // Rewrite server_port to OUR relay ports.
        transport = std::regex_replace(
            transport, sp_re,
            "server_port=" + std::to_string(local_rtp_port_) + "-" +
                std::to_string(local_rtcp_port_));

        // Rewrite source=<upstream-ip> to proxy-ip
        std::regex src_re(R"(source=[0-9.]+)");
        transport = std::regex_replace(transport, src_re, "source=" + proxy_ip);

        Logger::debug("[MITM] Patched Transport: " + transport);
        result = replace_header(result, "Transport", transport);
    }

    // 2. Rewrite Content-Base and RTP-Info (URIs pointing to upstream)
    // We need to be flexible with ports because the server might use 9820 or other ports.
    // Regex matches rtsp://<upstream-ip>:<any-port>/
    std::string upstream_pattern = "rtsp://" + ctx_.server_ip + "(:[0-9]+)?/";
    std::regex uri_re(upstream_pattern);
    
    auto patch_uri_header = [&](const std::string& h_name) {
        std::string val = extract_header_value(result, h_name);
        if (!val.empty()) {
            std::smatch m;
            if (std::regex_search(val, m, uri_re)) {
                // The match is the upstream base. We want to prefix it.
                // But wait, the proxy format is rtsp://proxy/real-host:port/path
                // So we replace rtsp://real-host:port/ with rtsp://proxy/real-host:port/
                std::string matched = m.str(); // e.g. "rtsp://112.245.125.44:53364/"
                std::string host_port = matched.substr(7, matched.size() - 8); // "112.245.125.44:53364"
                std::string replacement = "rtsp://" + proxy_ip + ":" + std::to_string(proxy_port) + "/" + host_port + "/";
                val = std::regex_replace(val, uri_re, replacement);
                result = replace_header(result, h_name, val);
            }
        }
    };
    patch_uri_header("Content-Base");
    patch_uri_header("RTP-Info");

    // 3. Rewrite SDP (c= and o= lines)
    size_t sdp_pos = result.find("\r\n\r\n");
    if (sdp_pos != std::string::npos)
    {
        std::string headers = result.substr(0, sdp_pos + 4);
        std::string sdp     = result.substr(sdp_pos + 4);
        if (!sdp.empty() && headers.find("application/sdp") != std::string::npos)
        {
            // Replace upstream IP in c= and o= lines
            std::regex ip_re(R"(IP4 [0-9.]+)");
            sdp = std::regex_replace(sdp, ip_re, "IP4 " + proxy_ip);

            // Update Content-Length
            result = headers + sdp;
            result = replace_header(result, "Content-Length", std::to_string(sdp.size()));
        }
    }

    return result;
}

std::string RTSPMitmClient::patch_transport_for_upstream(const std::string &req)
{
    // Replace client_port=X-Y with our local relay port pair.
    std::string transport = extract_header_value(req, "Transport");
    if (transport.empty())
        return req;

    std::regex re(R"(client_port=\d+-\d+)");
    std::string new_transport = std::regex_replace(
        transport, re,
        "client_port=" + std::to_string(local_rtp_port_) + "-" +
            std::to_string(local_rtcp_port_));

    return replace_header(req, "Transport", new_transport);
}

// Remove old patch_transport_for_client implementation since it's merged into patch_response_for_client
// (I will just delete it in the next chunk or here)


/* ========================================================================= */
/* URI rewriting for proxy-path format                                        */
/* ========================================================================= */

std::string RTSPMitmClient::rewrite_request_for_upstream(const std::string &req)
{
    // If no URI rewriting is needed (plain RTSP proxy / explicit-proxy mode)
    // just return the request as-is.
    if (proxy_uri_prefix_.empty())
        return req;

    // Rewrite only the first line: "METHOD <uri> RTSP/1.0\r\n"
    size_t crlf = req.find("\r\n");
    if (crlf == std::string::npos)
        return req;

    std::string first_line = req.substr(0, crlf); // "METHOD rtsp://proxy/real-host:port/path"
    std::string rest       = req.substr(crlf);    // everything from \r\n onward

    // Find the URI token (second word)
    size_t sp1 = first_line.find(' ');
    if (sp1 == std::string::npos)
        return req;
    size_t sp2 = first_line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos)
        return req;

    std::string method   = first_line.substr(0, sp1);
    std::string uri      = first_line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string rtsp_ver = first_line.substr(sp2 + 1);

    // Replace the proxy prefix with the real upstream base.
    // e.g. "rtsp://10.1.0.6:8555/112.245.125.44:1554/iptv/..."
    //   -> "rtsp://112.245.125.44:1554/iptv/..."
    if (uri.rfind(proxy_uri_prefix_, 0) == 0)
    {
        uri = upstream_uri_base_ + uri.substr(proxy_uri_prefix_.size());
    }

    return method + " " + uri + " " + rtsp_ver + rest;
}

/* ========================================================================= */
/* Timer (keepalive GET_PARAMETER)                                            */
/* ========================================================================= */

void RTSPMitmClient::init_timer_fd()
{
    using namespace std::chrono;
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    itimerspec its{};
    auto interval = seconds(20);
    its.it_value.tv_sec = interval.count();
    its.it_interval.tv_sec = interval.count();
    timerfd_settime(timer_fd_, 0, &its, nullptr);
    timer_ctx_ = std::make_unique<SocketCtx>(
        timer_fd_,
        [this](uint32_t ev) { handle_timer(ev); });
    loop_->set(timer_ctx_.get(), timer_fd_, EPOLLIN);
}

/* ========================================================================= */
/* Epoll handlers                                                             */
/* ========================================================================= */

void RTSPMitmClient::handle_downstream(uint32_t events)
{
    if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        on_downstream_closed();
        return;
    }
    if (events & EPOLLIN)
        on_downstream_readable();
    if (events & EPOLLOUT)
        on_downstream_writable();
}

void RTSPMitmClient::handle_upstream(uint32_t events)
{
    if (events & EPOLLOUT)
        on_upstream_writable();
    if (events & EPOLLIN)
        on_upstream_readable();
    if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        Logger::info("[MITM] Upstream connection closed");
        close_all();
    }
}

void RTSPMitmClient::handle_rtp_from_upstream(uint32_t /*events*/)
{
    while (true)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        // Use full 64KB capacity to avoid truncation (crucial for some streams)
        ssize_t n = recvfrom(rtp_us_fd_, rtp_relay_buf_, sizeof(rtp_relay_buf_), 0,
                             (sockaddr *)&src, &slen);
        if (n <= 0)
            break;

        if (src.sin_port == server_rtp_addr_.sin_port &&
            src.sin_addr.s_addr == server_rtp_addr_.sin_addr.s_addr)
        {
            if (client_rtp_addr_.sin_port != 0)
            {
                sendto(rtp_us_fd_, rtp_relay_buf_, n, 0,
                       (sockaddr *)&client_rtp_addr_, sizeof(client_rtp_addr_));
            }
        }
        else if (src.sin_addr.s_addr == client_addr_.sin_addr.s_addr)
        {
            if (client_rtp_addr_.sin_port != src.sin_port) {
                client_rtp_addr_.sin_port = src.sin_port;
            }

            if (server_rtp_addr_.sin_port != 0)
            {
                sendto(rtp_us_fd_, rtp_relay_buf_, n, 0,
                       (sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
            }
        }
    }
}

void RTSPMitmClient::handle_rtcp_from_upstream(uint32_t /*events*/)
{
    while (true)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(rtcp_us_fd_, rtp_relay_buf_, sizeof(rtp_relay_buf_), 0,
                             (sockaddr *)&src, &slen);
        if (n <= 0)
            break;

        if (src.sin_port == server_rtcp_addr_.sin_port &&
            src.sin_addr.s_addr == server_rtcp_addr_.sin_addr.s_addr)
        {
            if (client_rtcp_addr_.sin_port != 0)
            {
                sendto(rtcp_us_fd_, rtp_relay_buf_, n, 0,
                       (sockaddr *)&client_rtcp_addr_, sizeof(client_rtcp_addr_));
            }
        }
        else if (src.sin_addr.s_addr == client_addr_.sin_addr.s_addr)
        {
            if (client_rtcp_addr_.sin_port != src.sin_port) {
                client_rtcp_addr_.sin_port = src.sin_port;
            }

            if (server_rtcp_addr_.sin_port != 0)
            {
                sendto(rtcp_us_fd_, rtp_relay_buf_, n, 0,
                       (sockaddr *)&server_rtcp_addr_, sizeof(server_rtcp_addr_));
            }
        }
    }
}

void RTSPMitmClient::handle_rtp_from_client(uint32_t /*events*/) {}
void RTSPMitmClient::handle_rtcp_from_client(uint32_t /*events*/) {}

void RTSPMitmClient::handle_timer(uint32_t /*events*/)
{
    uint64_t exp;
    read(timer_fd_, &exp, sizeof(exp));
    // Send a GET_PARAMETER to upstream as keepalive.
    std::string ka = "GET_PARAMETER " + ctx_.rtsp_url + " RTSP/1.0\r\n"
                     "CSeq: 99\r\n"
                     "Session: " + ctx_.session_id + "\r\n"
                     "\r\n";
    to_upstream_q_.push_back(ka);
    loop_->set(upstream_ctx_.get(), upstream_fd_, EPOLLIN | EPOLLOUT);
}

/* ========================================================================= */
/* Readable / Writable callbacks                                              */
/* ========================================================================= */

void RTSPMitmClient::on_downstream_readable()
{
    char buf[8192];
    ssize_t n = recv(downstream_fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            on_downstream_closed();
        return;
    }
    buf[n] = 0;
    downstream_recv_buf_.append(buf, n);

    // Wait for a complete RTSP message (ends with \r\n\r\n).
    while (true)
    {
        size_t end = downstream_recv_buf_.find("\r\n\r\n");
        if (end == std::string::npos)
            break;

        // Include the trailing \r\n\r\n
        std::string req = downstream_recv_buf_.substr(0, end + 4);
        downstream_recv_buf_ = downstream_recv_buf_.substr(end + 4);

        // Check if this is a SETUP request — we need to prepare relay sockets.
        bool is_setup = (req.find("SETUP ") == 0);
        bool is_play  = (req.find("PLAY ") == 0);
        if (is_setup)
        {
            // Remember the client's original RTP/RTCP ports.
            uint16_t crtp = 0, crtcp = 0;
            if (extract_client_port(req, crtp, crtcp))
            {
                client_rtp_addr_.sin_family = AF_INET;
                client_rtp_addr_.sin_port = htons(crtp);
                client_rtp_addr_.sin_addr = client_addr_.sin_addr;

                client_rtcp_addr_.sin_family = AF_INET;
                client_rtcp_addr_.sin_port = htons(crtcp);
                client_rtcp_addr_.sin_addr = client_addr_.sin_addr;

                Logger::info("[MITM] Client RTP ports: " +
                             std::to_string(crtp) + "-" + std::to_string(crtcp));
            }

            if (!relay_ready_)
            {
                if (!init_relay_sockets())
                {
                    close_all();
                    return;
                }
                relay_ready_ = true;
            }

            req = patch_transport_for_upstream(req);
        }

        if (is_play)
        {
            pending_play_ = true;
        }
        else
        {
            pending_play_ = false;
        }

        // Rewrite URI from proxy-path format to real upstream URI.
        req = rewrite_request_for_upstream(req);

        to_upstream_q_.push_back(req);
        loop_->set(upstream_ctx_.get(), upstream_fd_, EPOLLIN | EPOLLOUT);
    }
}

void RTSPMitmClient::on_downstream_writable()
{
    while (!to_downstream_q_.empty())
    {
        auto &msg = to_downstream_q_.front();
        ssize_t n = send(downstream_fd_,
                         msg.data() + downstream_send_offset_,
                         msg.size() - downstream_send_offset_, 0);
        if (n > 0)
        {
            downstream_send_offset_ += n;
            if (downstream_send_offset_ == msg.size())
            {
                to_downstream_q_.pop_front();
                downstream_send_offset_ = 0;
            }
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        else
        {
            on_downstream_closed();
            return;
        }
    }

    // Once the queue is empty, go back to only waiting for incoming data.
    if (to_downstream_q_.empty())
    {
        loop_->set(downstream_ctx_.get(), downstream_fd_,
                   EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLIN);
    }
}

void RTSPMitmClient::on_downstream_closed()
{
    Logger::info("[MITM] Downstream client disconnected");
    close_all();
}

void RTSPMitmClient::on_upstream_writable()
{
    if (state_ == State::WAIT_UPSTREAM_CONNECT)
    {
        // Check whether the non-blocking connect succeeded.
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(upstream_fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        {
            Logger::error("[MITM] Upstream connect failed");
            close_all();
            return;
        }
        Logger::info("[MITM] Connected to upstream " + ctx_.server_ip +
                     ":" + std::to_string(ctx_.server_rtsp_port));
        state_ = State::IDLE;

        // Now that we are connected, forward the first request that was
        // stashed in downstream_recv_buf_.
        if (!downstream_recv_buf_.empty())
        {
            // The stashed buffer is the raw first request.
            // Apply URI rewriting before forwarding.
            std::string first = rewrite_request_for_upstream(downstream_recv_buf_);
            to_upstream_q_.push_back(first);
            downstream_recv_buf_.clear();
        }

        // Also enable reading from the downstream client now.
        loop_->set(downstream_ctx_.get(), downstream_fd_,
                   EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLIN);
    }

    // Drain the upstream send queue.
    while (!to_upstream_q_.empty())
    {
        auto &msg = to_upstream_q_.front();
        ssize_t n = send(upstream_fd_,
                         msg.data() + upstream_send_offset_,
                         msg.size() - upstream_send_offset_, 0);
        if (n > 0)
        {
            upstream_send_offset_ += n;
            if (upstream_send_offset_ == msg.size())
            {
                to_upstream_q_.pop_front();
                upstream_send_offset_ = 0;
                state_ = State::WAIT_UPSTREAM_RESP;
            }
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        else
        {
            Logger::error("[MITM] Upstream send failed");
            close_all();
            return;
        }
    }

    // Decide what to watch for next on the upstream fd.
    uint32_t upstream_events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    if (!to_upstream_q_.empty())
        upstream_events |= EPOLLOUT;
    loop_->set(upstream_ctx_.get(), upstream_fd_, upstream_events);
}

void RTSPMitmClient::on_upstream_readable()
{
    char buf[8192];
    ssize_t n = recv(upstream_fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        {
            Logger::info("[MITM] Upstream closed connection");
            close_all();
        }
        return;
    }
    buf[n] = 0;
    upstream_recv_buf_.append(buf, n);

    // Forward complete RTSP responses back to the downstream client.
    while (true)
    {
        size_t end = upstream_recv_buf_.find("\r\n\r\n");
        if (end == std::string::npos)
            break;

        // A DESCRIBE response may have a body (SDP).  Read Content-Length.
        size_t body_len = 0;
        std::string cl_val = extract_header_value(
            upstream_recv_buf_.substr(0, end + 4), "Content-Length");
        if (!cl_val.empty())
        {
            try { body_len = std::stoul(cl_val); } catch (...) {}
        }

        size_t total = end + 4 + body_len;
        if (upstream_recv_buf_.size() < total)
            break; // wait for the full body

        std::string resp = upstream_recv_buf_.substr(0, total);
        upstream_recv_buf_ = upstream_recv_buf_.substr(total);

        // Check if this is a response to our injected keepalive (CSeq: 99).
        if (resp.find("CSeq: 99\r\n") != std::string::npos)
        {
            Logger::debug("[MITM] Consumed keepalive response from upstream");
            continue;
        }

        // Parse session id if present.
        rtspParser::parse_session_id(resp, ctx_);

        // Apply robust response patching (Transport, Content-Base, etc.)
        resp = patch_response_for_client(resp);

        // Detect PLAY response -> start streaming state.
        int status = rtspParser::parse_status_code(resp);
        if (status == 200 && pending_play_)
        {
            pending_play_ = false;
            if (state_ != State::STREAMING)
            {
                state_ = State::STREAMING;
                Logger::info("[MITM] Streaming started: " + ctx_.rtsp_url +
                             " -> " + std::string(inet_ntoa(client_addr_.sin_addr)) +
                             ":" + std::to_string(ntohs(client_addr_.sin_port)));
                init_timer_fd();
            }
        }

        to_downstream_q_.push_back(resp);
        loop_->set(downstream_ctx_.get(), downstream_fd_,
                   EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT);
    }
}

/* ========================================================================= */
/* Teardown                                                                   */
/* ========================================================================= */

void RTSPMitmClient::close_all()
{
    if (closed_)
        return;
    closed_ = true;
    if (on_closed_)
        on_closed_();
}

/* ========================================================================= */
/* FdGuard implementation                                                     */
/* ========================================================================= */

RTSPMitmClient::FdGuard::FdGuard() = default;
RTSPMitmClient::FdGuard::FdGuard(int fd, EpollLoop *loop) : fd_(fd), loop_(loop) {}

RTSPMitmClient::FdGuard::~FdGuard()
{
    if (fd_ >= 0)
    {
        if (loop_)
            loop_->remove(fd_);
        close(fd_);
    }
}

RTSPMitmClient::FdGuard::FdGuard(FdGuard &&other) noexcept
    : fd_(other.fd_), loop_(other.loop_)
{
    other.fd_ = -1;
    other.loop_ = nullptr;
}

RTSPMitmClient::FdGuard &RTSPMitmClient::FdGuard::operator=(FdGuard &&other) noexcept
{
    if (this != &other)
    {
        if (fd_ >= 0)
        {
            if (loop_)
                loop_->remove(fd_);
            close(fd_);
        }
        fd_ = other.fd_;
        loop_ = other.loop_;
        other.fd_ = -1;
        other.loop_ = nullptr;
    }
    return *this;
}

int &RTSPMitmClient::FdGuard::get_ref() { return fd_; }
int RTSPMitmClient::FdGuard::get() const { return fd_; }
RTSPMitmClient::FdGuard::operator int() const { return fd_; }
