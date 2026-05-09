#include "../include/statistics.h"
#include "../include/rtsp_mitm_client.h"
#include "../include/http_parser.h"
#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/buffer_pool.h"
#include "../include/common/socket_ctx.h"
#include "../include/common/rtsp_ctx.h"
#include "../include/rtsp_parser.h"
#include "../include/blacklist_checker.h"
#include "../include/socket_helper.h"
#include "../include/stun_client.h"
#include "../include/port_pool.h"
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
                               const RtspMitmConfig &config,
                               const std::string &first_request)
    : loop_(loop),
      pool_(pool),
      downstream_fd_(client_fd, loop),
      downstream_ctx_(std::make_unique<SocketCtx>(
          client_fd,
          [this](uint32_t ev) { handle_downstream(ev); })),
      client_addr_(client_addr),
      rtp_us_fd_(-1, loop),
      rtcp_us_fd_(-1, loop),
      rtp_ds_fd_(-1, loop),
      rtcp_ds_fd_(-1, loop),
      timer_fd_(-1, loop),
      ctx_(config.ctx),
      proxy_uri_prefix_(config.proxy_uri_prefix),
      upstream_uri_base_(config.upstream_uri_base),
      rtp_pipeline_(std::make_unique<RtpPipeline>())
{
    // Remove the simple EPOLLIN watch that was set by the accept handler;
    // we will re-register it ourselves.
    loop_->remove(client_fd);

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
    for (auto &packet : to_downstream_q_) {
        if (packet.data) {
            pool_.release(std::move(packet.data));
        }
    }
    to_downstream_q_.clear();

    if (local_rtp_us_port_ != 0) {
        PortPool::getInstance().release_pair(local_rtp_us_port_);
    }
    if (local_rtp_ds_port_ != 0) {
        PortPool::getInstance().release_pair(local_rtp_ds_port_);
    }
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
                                          ServerConfig::getMitmUpstreamInterface());
    if (upstream_fd_ < 0)
    {
        throw std::runtime_error("Failed to connect to upstream " + ctx_.server_ip +
                                ":" + std::to_string(ctx_.server_rtsp_port));
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
    std::string transport = rtspParser::extract_header_value(req, "Transport");
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

bool RTSPMitmClient::extract_interleaved_channels(const std::string &req,
                                                 uint8_t &rtp_chan, uint8_t &rtcp_chan)
{
    std::string transport = rtspParser::extract_header_value(req, "Transport");
    if (transport.empty())
        return false;

    // interleaved=0-1
    std::regex re(R"(interleaved=(\d+)-(\d+))");
    std::smatch m;
    if (!std::regex_search(transport, m, re))
        return false;

    rtp_chan = static_cast<uint8_t>(std::stoi(m[1]));
    rtcp_chan = static_cast<uint8_t>(std::stoi(m[2]));
    return true;
}

bool RTSPMitmClient::init_relay_sockets()
{
    // 1. Allocate UPSTREAM-facing sockets (bound to mitm interface)
    if (bind_udp_pair_from_pool(rtp_us_fd_.get_ref(), rtcp_us_fd_.get_ref(), 
                                local_rtp_us_port_, ServerConfig::getMitmUpstreamInterface()) < 0)
    {
        Logger::error("[MITM] Failed to bind upstream-facing UDP sockets");
        return false;
    }
    local_rtcp_us_port_ = local_rtp_us_port_ + 1;

    // 2. Allocate DOWNSTREAM-facing sockets (NOT bound to mitm interface, uses default route)
    if (bind_udp_pair_from_pool(rtp_ds_fd_.get_ref(), rtcp_ds_fd_.get_ref(), 
                                local_rtp_ds_port_) < 0)
    {
        Logger::error("[MITM] Failed to bind downstream-facing UDP sockets");
        return false;
    }
    local_rtcp_ds_port_ = local_rtp_ds_port_ + 1;

    // Register all for EPOLLIN
    rtp_us_ctx_ = std::make_unique<SocketCtx>(
        rtp_us_fd_,
        [this](uint32_t ev) { handle_rtp_from_upstream(ev); });
    rtcp_us_ctx_ = std::make_unique<SocketCtx>(
        rtcp_us_fd_,
        [this](uint32_t ev) { handle_rtcp_from_upstream(ev); });
    rtp_ds_ctx_ = std::make_unique<SocketCtx>(
        rtp_ds_fd_,
        [this](uint32_t ev) { handle_rtp_from_client(ev); });
    rtcp_ds_ctx_ = std::make_unique<SocketCtx>(
        rtcp_ds_fd_,
        [this](uint32_t ev) { handle_rtcp_from_client(ev); });

    loop_->set(rtp_us_ctx_.get(), rtp_us_fd_, EPOLLIN);
    loop_->set(rtcp_us_ctx_.get(), rtcp_us_fd_, EPOLLIN);
    loop_->set(rtp_ds_ctx_.get(), rtp_ds_fd_, EPOLLIN);
    loop_->set(rtcp_ds_ctx_.get(), rtcp_ds_fd_, EPOLLIN);


    Logger::debug("[MITM] Relay ports: US=" + std::to_string(local_rtp_us_port_) + 
                 ", DS=" + std::to_string(local_rtp_ds_port_));
    return true;
}

/* ========================================================================= */
/* Response patching for client (MITM)                                        */
/* ========================================================================= */

std::string RTSPMitmClient::patch_response_for_client(const std::string &resp)
{
    std::string result = resp;

    // Dynamically get local IP/port that the client connected to
    struct sockaddr_in local_addr{};
    socklen_t addr_len = sizeof(local_addr);
    getsockname(downstream_fd_, (struct sockaddr *)&local_addr, &addr_len);
    std::string proxy_ip = inet_ntoa(local_addr.sin_addr);
    uint16_t proxy_port = ntohs(local_addr.sin_port);

    // 1. Rewrite Transport header (if present)
    std::string transport = rtspParser::extract_header_value(result, "Transport");
    if (!transport.empty())
    {
        Logger::debug("[MITM] Original Transport: " + transport);

        if (!ds_transport_protocol_.empty()) {
            size_t pos = transport.find("MP2T/RTP/UDP");
            if (pos != std::string::npos) {
                transport.replace(pos, 12, ds_transport_protocol_);
            }
        }

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
            
            if (!is_upstream_tcp_)
            {
                if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte")
                {
                    send_zte_heartbeat();
                }
                else
                {
                    send_rtp_trigger();
                }
            }
        }

        // Rewrite client_port back to original client ports.
        std::regex cp_re(R"(client_port=\d+-\d+)");

        if (is_downstream_tcp_)
        {
            // If downstream requested TCP, ensure the response says so.
            if (transport.find("RTP/AVP/TCP") == std::string::npos)
            {
                size_t pos = transport.find("RTP/AVP");
                if (pos != std::string::npos)
                    transport.replace(pos, 7, "RTP/AVP/TCP");
            }
            // Remove server_port and client_port, replace with interleaved
            transport = std::regex_replace(transport, cp_re, "");
            transport = std::regex_replace(transport, sp_re, "");
            
            // Clean up double semicolons or trailing semicolons
            while (transport.find(";;") != std::string::npos) transport.replace(transport.find(";;"), 2, ";");
            if (!transport.empty() && transport.back() == ';') transport.pop_back();

            if (transport.find("interleaved=") == std::string::npos)
            {
                transport += ";interleaved=" + std::to_string(ds_interleaved_rtp_) + "-" +
                             std::to_string(ds_interleaved_rtcp_);
            }
        }
        else
        {
            std::string client_rtp_str = std::to_string(ntohs(client_rtp_addr_.sin_port));
            std::string client_rtcp_str = std::to_string(ntohs(client_rtcp_addr_.sin_port));
            transport = std::regex_replace(
                transport, cp_re,
                "client_port=" + client_rtp_str + "-" + client_rtcp_str);

            // Rewrite server_port to OUR downstream-facing relay ports.
            transport = std::regex_replace(
                transport, sp_re,
                "server_port=" + std::to_string(local_rtp_ds_port_) + "-" +
                    std::to_string(local_rtcp_ds_port_));
        }

        // Rewrite source to our proxy IP
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
        std::string val = rtspParser::extract_header_value(result, h_name);
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
    std::string transport = rtspParser::extract_header_value(req, "Transport");
    if (transport.empty())
        return req;

    if (transport.find("MP2T/RTP/UDP") != std::string::npos) {
        ds_transport_protocol_ = "MP2T/RTP/UDP";
    } else if (transport.find("RTP/AVP/TCP") != std::string::npos) {
        ds_transport_protocol_ = "RTP/AVP/TCP";
    } else {
        ds_transport_protocol_ = "RTP/AVP";
    }

    if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte")
    {
        std::string new_transport = "MP2T/RTP/UDP;unicast;client_address=" + local_ip_ +
                                    ";client_port=" + std::to_string(local_rtp_us_port_) + "-" +
                                    std::to_string(local_rtcp_us_port_) + ";mode=PLAY";
        return replace_header(req, "Transport", new_transport);
    }

    uint16_t rtp_port = local_rtp_us_port_;
    uint16_t rtcp_port = local_rtcp_us_port_;

    if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "stun" && nat_wan_port_us_ != 0)
    {
        rtp_port = nat_wan_port_us_;
        rtcp_port = nat_wan_port_us_ + 1;
    }

    std::regex re(R"(client_port=\d+-\d+)");
    std::string new_transport = transport;

    if (transport.find("RTP/AVP/TCP") != std::string::npos || transport.find("interleaved=") != std::string::npos)
    {
        // Convert TCP request to UDP for upstream
        size_t pos = new_transport.find("RTP/AVP/TCP");
        if (pos != std::string::npos)
            new_transport.replace(pos, 11, "RTP/AVP");

        std::regex int_re(R"(interleaved=\d+-\d+;?)");
        new_transport = std::regex_replace(new_transport, int_re, "");
        if (new_transport.back() == ';') new_transport.pop_back();

        new_transport += ";client_port=" + std::to_string(rtp_port) + "-" +
                         std::to_string(rtcp_port);
    }
    else
    {
        new_transport = std::regex_replace(
            transport, re,
            "client_port=" + std::to_string(rtp_port) + "-" +
                std::to_string(rtcp_port));
    }

    return replace_header(req, "Transport", new_transport);
}

std::string RTSPMitmClient::patch_transport_for_upstream_tcp(const std::string &req)
{
    std::string transport = rtspParser::extract_header_value(req, "Transport");
    if (transport.empty()) return req;

    // Force TCP interleaved mode for upstream
    std::string new_transport = "RTP/AVP/TCP;unicast;interleaved=0-1";
    
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
        Logger::debug("[MITM] Upstream connection closed");
        close_all();
    }
}

void RTSPMitmClient::handle_rtp_from_upstream(uint32_t /*events*/)
{
    if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "stun" && state_ == State::WAIT_STUN)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        auto buf = pool_.acquire();
        ssize_t n = recvfrom(rtp_us_fd_, buf.get(), pool_.get_buffer_size(), 0,
                             (sockaddr *)&src, &slen);
        if (n > 0)
        {
            std::string wan_ip;
            uint16_t wan_port = 0;
            if (StunClient::extract_stun_mapping_from_response(buf.get(), n, wan_ip, wan_port) == 0)
            {
                nat_wan_port_us_ = wan_port;
                Logger::debug("[MITM] STUN mapped public port for RTP: " + std::to_string(nat_wan_port_us_));
            }
            else
            {
                Logger::warn("[MITM] Failed to parse STUN response for RTP, fallback to local port");
                nat_wan_port_us_ = local_rtp_us_port_;
            }
            
            state_ = State::IDLE;
            process_pending_setup();
        }
        pool_.release(std::move(buf));
        return;
    }

    while (true)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        auto buf = pool_.acquire();
        ssize_t n = recvfrom(rtp_us_fd_, buf.get(), pool_.get_buffer_size(), 0,
                             (sockaddr *)&src, &slen);
        if (n <= 0) {
            pool_.release(std::move(buf));
            break;
        }

        // Packet from upstream server -> send to downstream client
        // if (src.sin_port == server_rtp_addr_.sin_port &&
        //     src.sin_addr.s_addr == server_rtp_addr_.sin_addr.s_addr)
        {
            upstream_est_.addBytes(n);
            Statistics::getInstance().addUpstreamBytes(n);
            size_t actual_n = static_cast<size_t>(n);

            if (!rtp_pipeline_->process(buf.get(), actual_n)) {
                pool_.release(std::move(buf));
                continue;
            }
            n = static_cast<ssize_t>(actual_n);
            
            if (n > 0) {
                if (is_downstream_tcp_)
                {
                    send_interleaved_downstream(ds_interleaved_rtp_, buf.get(), n);
                }
                else if (client_rtp_addr_.sin_port != 0)
                {
                    sendto(rtp_ds_fd_, buf.get(), n, 0,
                           (sockaddr *)&client_rtp_addr_, sizeof(client_rtp_addr_));
                    downstream_est_.addBytes(n);
                    Statistics::getInstance().addDownstreamBytes(n);
                }
            }
        }
        pool_.release(std::move(buf));
    }
}

void RTSPMitmClient::handle_rtcp_from_upstream(uint32_t /*events*/)
{
    while (true)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        auto buf = pool_.acquire();
        ssize_t n = recvfrom(rtcp_us_fd_, buf.get(), pool_.get_buffer_size(), 0,
                             (sockaddr *)&src, &slen);
        if (n <= 0) {
            pool_.release(std::move(buf));
            break;
        }

        // Packet from upstream server -> send to downstream client
        // if (src.sin_port == server_rtcp_addr_.sin_port &&
        //     src.sin_addr.s_addr == server_rtcp_addr_.sin_addr.s_addr)
        {
            upstream_est_.addBytes(n);
            Statistics::getInstance().addUpstreamBytes(n);
            if (is_downstream_tcp_)
            {
                send_interleaved_downstream(ds_interleaved_rtcp_, buf.get(), n);
            }
            else if (client_rtcp_addr_.sin_port != 0)
            {
                sendto(rtcp_ds_fd_, buf.get(), n, 0,
                       (sockaddr *)&client_rtcp_addr_, sizeof(client_rtcp_addr_));
                downstream_est_.addBytes(n);
                Statistics::getInstance().addDownstreamBytes(n);
            }
        }
        pool_.release(std::move(buf));
    }
}

void RTSPMitmClient::send_interleaved_downstream(uint8_t channel, const uint8_t *data, size_t len)
{
    if (closed_ || downstream_fd_ < 0) return;

    size_t pool_block_size = pool_.get_buffer_size();
    if (len + 4 > pool_block_size) {
        Logger::warn("[MITM] Packet too large, truncated (" + std::to_string(len + 4) + " > " + std::to_string(pool_block_size) + ")");
        len = pool_block_size - 4;
    }

    auto buf = pool_.acquire();
    buf[0] = '$';
    buf[1] = static_cast<uint8_t>(channel);
    uint16_t nlen = htons(static_cast<uint16_t>(len));
    memcpy(buf.get() + 2, &nlen, 2);
    memcpy(buf.get() + 4, data, len);

    // Prevent memory exhaustion by limiting queue size (Drop oldest if full)
    if (to_downstream_q_.size() > 2048)
    {
        auto &old_packet = to_downstream_q_.front();
        if (old_packet.data) pool_.release(std::move(old_packet.data));
        to_downstream_q_.pop_front();
    }

    to_downstream_q_.push_back(Packet{std::move(buf), len + 4, 0});
    downstream_est_.addBytes(len + 4);
    
    // We need to trigger EPOLLOUT to drain the queue
    loop_->set(downstream_ctx_.get(), downstream_fd_,
               EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLIN | EPOLLOUT);
}

void RTSPMitmClient::handle_interleaved_from_client(uint8_t channel, const uint8_t *data, size_t len)
{
    // Client sent something (usually RTCP) over TCP interleaved.
    // Relay it to upstream via UDP.
    if (channel == ds_interleaved_rtp_)
    {
        if (server_rtp_addr_.sin_port != 0)
        {
            sendto(rtp_us_fd_, data, len, 0,
                   (sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
            Statistics::getInstance().addUpstreamBytes(len);
        }
    }
    else if (channel == ds_interleaved_rtcp_)
    {
        if (server_rtcp_addr_.sin_port != 0)
        {
            sendto(rtcp_us_fd_, data, len, 0,
                   (sockaddr *)&server_rtcp_addr_, sizeof(server_rtcp_addr_));
            Statistics::getInstance().addUpstreamBytes(len);
        }
    }
}

void RTSPMitmClient::handle_interleaved_from_upstream(uint8_t channel, const uint8_t *data, size_t len)
{
    // Relay RTP/RTCP from upstream (TCP) to downstream
    if (channel == us_interleaved_rtp_)
    {
        upstream_est_.addBytes(len);
        Statistics::getInstance().addUpstreamBytes(len);

        auto buf = pool_.acquire();
        size_t n = std::min(len, pool_.get_buffer_size());
        memcpy(buf.get(), data, n);

        if (!rtp_pipeline_->process(buf.get(), n)) {
            pool_.release(std::move(buf));
            return;
        }

        if (n > 0) {
            if (is_downstream_tcp_) {
                send_interleaved_downstream(ds_interleaved_rtp_, buf.get(), n);
            } else if (client_rtp_addr_.sin_port != 0) {
                sendto(rtp_ds_fd_, buf.get(), n, 0, (sockaddr *)&client_rtp_addr_, sizeof(client_rtp_addr_));
                Statistics::getInstance().addDownstreamBytes(n);
            }
        }
        pool_.release(std::move(buf));
    }
    else if (channel == us_interleaved_rtcp_)
    {
        upstream_est_.addBytes(len);
        if (is_downstream_tcp_)
            send_interleaved_downstream(ds_interleaved_rtcp_, data, len);
        else if (client_rtcp_addr_.sin_port != 0) {
            sendto(rtcp_ds_fd_, data, len, 0, (sockaddr *)&client_rtcp_addr_, sizeof(client_rtcp_addr_));
            Statistics::getInstance().addUpstreamBytes(len);
        }
    }
}

void RTSPMitmClient::handle_rtp_from_client(uint32_t /*events*/)
{
    while (true)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        auto buf = pool_.acquire();
        ssize_t n = recvfrom(rtp_ds_fd_, buf.get(), pool_.get_buffer_size(), 0,
                             (sockaddr *)&src, &slen);
        if (n <= 0) {
            pool_.release(std::move(buf));
            break;
        }

        // Packet from downstream client -> send to upstream server
        if (src.sin_addr.s_addr == client_addr_.sin_addr.s_addr)
        {
            if (client_rtp_addr_.sin_port != src.sin_port) {
                client_rtp_addr_.sin_port = src.sin_port;
            }

            if (server_rtp_addr_.sin_port != 0)
            {
                sendto(rtp_us_fd_, buf.get(), n, 0,
                       (sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
                Statistics::getInstance().addUpstreamBytes(n);
            }
        }
        pool_.release(std::move(buf));
    }
}

void RTSPMitmClient::handle_rtcp_from_client(uint32_t /*events*/)
{
    while (true)
    {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        auto buf = pool_.acquire();
        ssize_t n = recvfrom(rtcp_ds_fd_, buf.get(), pool_.get_buffer_size(), 0,
                             (sockaddr *)&src, &slen);
        if (n <= 0) {
            pool_.release(std::move(buf));
            break;
        }

        // Packet from downstream client -> send to upstream server
        if (src.sin_addr.s_addr == client_addr_.sin_addr.s_addr)
        {
            if (client_rtcp_addr_.sin_port != src.sin_port) {
                client_rtcp_addr_.sin_port = src.sin_port;
            }

            if (server_rtcp_addr_.sin_port != 0)
            {
                sendto(rtcp_us_fd_, buf.get(), n, 0,
                       (sockaddr *)&server_rtcp_addr_, sizeof(server_rtcp_addr_));
                Statistics::getInstance().addUpstreamBytes(n);
            }
        }
        pool_.release(std::move(buf));
    }
}

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

    // if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte")
    // {
    //     send_zte_heartbeat();
    // }
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

    // Wait for a complete RTSP message (ends with \r\n\r\n) or Interleaved packet ($)
    while (!downstream_recv_buf_.empty())
    {
        if (downstream_recv_buf_[0] == '$')
        {
            if (downstream_recv_buf_.size() < 4)
                break;
            
            uint16_t len = ntohs(*reinterpret_cast<const uint16_t *>(downstream_recv_buf_.data() + 2));
            if (downstream_recv_buf_.size() < static_cast<size_t>(len) + 4)
                break;

            uint8_t channel = static_cast<uint8_t>(downstream_recv_buf_[1]);
            handle_interleaved_from_client(channel, reinterpret_cast<const uint8_t *>(downstream_recv_buf_.data() + 4), len);
            downstream_recv_buf_.erase(0, 4 + len);
            continue;
        }

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
            // Detect if client wants TCP interleaved
            if (extract_interleaved_channels(req, ds_interleaved_rtp_, ds_interleaved_rtcp_))
            {
                is_downstream_tcp_ = true;
                Logger::debug("[MITM] Downstream using TCP interleaved: " +
                             std::to_string(ds_interleaved_rtp_) + "-" +
                             std::to_string(ds_interleaved_rtcp_));
            }
            else
            {
                is_downstream_tcp_ = false;
                // Remember the client's original RTP/RTCP ports for UDP.
                uint16_t crtp = 0, crtcp = 0;
                if (extract_client_port(req, crtp, crtcp))
                {
                    client_rtp_addr_.sin_family = AF_INET;
                    client_rtp_addr_.sin_port = htons(crtp);
                    client_rtp_addr_.sin_addr = client_addr_.sin_addr;

                    client_rtcp_addr_.sin_family = AF_INET;
                    client_rtcp_addr_.sin_port = htons(crtcp);
                    client_rtcp_addr_.sin_addr = client_addr_.sin_addr;

                    Logger::debug("[MITM] Client RTP ports: " +
                                 std::to_string(crtp) + "-" + std::to_string(crtcp));
                }
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

            if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "stun")
            {
                StunClient::send_stun_mapping_request(rtp_us_fd_);
                state_ = State::WAIT_STUN;
                pending_setup_req_ = req;
                Logger::debug("[MITM] Pausing SETUP for STUN mapping...");
                break;
            }

            last_setup_req_ = req; // Store for potential TCP fallback
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
        auto &packet = to_downstream_q_.front();
        ssize_t n = send(downstream_fd_,
                         packet.data.get() + packet.offset,
                         packet.length - packet.offset, 0);
        if (n > 0)
        {
            Statistics::getInstance().addDownstreamBytes(n);
            packet.offset += n;
            if (packet.offset == packet.length)
            {
                pool_.release(std::move(packet.data));
                to_downstream_q_.pop_front();
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
    Logger::debug("[MITM] Downstream client disconnected");
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
        Logger::debug("[MITM] Connected to upstream " + ctx_.server_ip +
                     ":" + std::to_string(ctx_.server_rtsp_port));
        state_ = State::IDLE;

        struct sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        if (getsockname(upstream_fd_, (struct sockaddr *)&local_addr, &addr_len) == 0) {
            local_ip_ = inet_ntoa(local_addr.sin_addr);
            local_tcp_port_ = ntohs(local_addr.sin_port);
            Logger::debug("[MITM] Local IP (facing upstream): " + local_ip_ + ", Local TCP Port: " + std::to_string(local_tcp_port_));
        }

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
            Logger::debug("[MITM] Upstream closed connection");
            close_all();
        }
        return;
    }
    buf[n] = 0;
    upstream_recv_buf_.append(buf, n);

    // Forward complete RTSP responses or handle Interleaved packets
    while (!upstream_recv_buf_.empty())
    {
        if (upstream_recv_buf_[0] == '$')
        {
            if (upstream_recv_buf_.size() < 4)
                break;
            
            uint16_t len = ntohs(*reinterpret_cast<const uint16_t *>(upstream_recv_buf_.data() + 2));
            if (upstream_recv_buf_.size() < static_cast<size_t>(len) + 4)
                break;

            uint8_t channel = static_cast<uint8_t>(upstream_recv_buf_[1]);
            handle_interleaved_from_upstream(channel, reinterpret_cast<const uint8_t *>(upstream_recv_buf_.data() + 4), len);
            upstream_recv_buf_.erase(0, 4 + len);
            continue;
        }

        size_t end = upstream_recv_buf_.find("\r\n\r\n");
        if (end == std::string::npos)
            break;

        // A response may have a body (SDP). Read Content-Length.
        size_t body_len = 0;
        std::string cl_val = rtspParser::extract_header_value(
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

        int status = rtspParser::parse_status_code(resp);

        // -----------------------------------------------------------
        // Auto-fallback to TCP if UDP is not supported (Status 461)
        // -----------------------------------------------------------
        if (status == 461 && !setup_retry_with_tcp_ && !last_setup_req_.empty())
        {
            Logger::warn("[MITM] Upstream rejected UDP Transport (461). Retrying with TCP Interleaved...");
            setup_retry_with_tcp_ = true;
            std::string retry_req = patch_transport_for_upstream_tcp(last_setup_req_);
            
            // Rewrite URI if needed
            retry_req = rewrite_request_for_upstream(retry_req);
            
            to_upstream_q_.push_back(retry_req);
            on_upstream_writable(); // Trigger immediate send
            continue; // Consume the 461 response, don't send to client yet
        }

        // If it's a 200 OK for SETUP, check if it's TCP interleaved
        if (status == 200 && resp.find("interleaved=") != std::string::npos)
        {
            if (extract_interleaved_channels(resp, us_interleaved_rtp_, us_interleaved_rtcp_))
            {
                is_upstream_tcp_ = true;
                Logger::debug("[MITM] Upstream confirmed TCP interleaved: " +
                             std::to_string(us_interleaved_rtp_) + "-" +
                             std::to_string(us_interleaved_rtcp_));
            }
        }

        // Apply robust response patching (Transport, Content-Base, etc.)
        resp = patch_response_for_client(resp);

        // Detect PLAY response -> start streaming state
        if (status == 200 && pending_play_)
        {
            pending_play_ = false;
            Logger::debug(std::string("[MITM] Streaming Start: " + ctx_.rtsp_url));
            rtp_pipeline_->reset();
            init_timer_fd();
            state_ = State::STREAMING;
            
            if (!is_upstream_tcp_) {
                if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte") {
                    send_zte_heartbeat();
                } else {
                    send_rtp_trigger();
                }
            }
        }

        // Convert RTSP response string to Packet
        auto buf = pool_.acquire();
        memcpy(buf.get(), resp.data(), resp.size());
        to_downstream_q_.push_back(Packet{std::move(buf), resp.size(), 0});
        loop_->set(downstream_ctx_.get(), downstream_fd_,
                   EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT | EPOLLIN);
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
void RTSPMitmClient::send_rtp_trigger()
{
    if (server_rtp_addr_.sin_port == 0) return;
    
    char dummy = 0;
    // Trigger RTP
    sendto(rtp_us_fd_, &dummy, 1, 0, (struct sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
    // Trigger RTCP
    if (server_rtcp_addr_.sin_port != 0) {
        sendto(rtcp_us_fd_, &dummy, 1, 0, (struct sockaddr *)&server_rtcp_addr_, sizeof(server_rtcp_addr_));
    }
    Logger::debug("[MITM] Sent RTP/RTCP trigger packets to upstream");
}

void RTSPMitmClient::send_zte_heartbeat()
{
    if (!(ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte"))
        return;

    uint8_t payload[84];
    memset(payload, 0, sizeof(payload));
    memcpy(payload, "ZXV10STB", 8);
    payload[8] = 0x7f;
    payload[9] = 0xff;
    payload[10] = 0xff;
    payload[11] = 0xff;

    struct in_addr addr;
    if (inet_pton(AF_INET, local_ip_.c_str(), &addr) == 1) {
        memcpy(payload + 12, &addr.s_addr, 4);
    }

    uint16_t udp_port = local_rtp_us_port_;
    uint16_t tcp_port = local_tcp_port_;

    payload[16] = (udp_port >> 8) & 0xFF;
    payload[17] = udp_port & 0xFF;
    payload[18] = (tcp_port >> 8) & 0xFF;
    payload[19] = tcp_port & 0xFF;

    ssize_t n = sendto(rtp_us_fd_, payload, sizeof(payload), 0,
                       (struct sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
    if (n < 0)
    {
        Logger::error("[MITM] ZTE heartbeat send failed");
    }
    else
    {
        Logger::debug("[MITM] ZTE heartbeat sent to " + ctx_.server_ip + ":" + std::to_string(ntohs(server_rtp_addr_.sin_port)));
    }
}

void RTSPMitmClient::process_pending_setup()
{
    std::string req = pending_setup_req_;
    pending_setup_req_.clear();

    last_setup_req_ = req;
    req = patch_transport_for_upstream(req);
    req = rewrite_request_for_upstream(req);

    to_upstream_q_.push_back(req);
    loop_->set(upstream_ctx_.get(), upstream_fd_, EPOLLIN | EPOLLOUT);
}

json RTSPMitmClient::get_info() const
{
    json info;
    info["type"] = "mitm";
    info["transport"] = is_downstream_tcp_ ? "TCP" : "UDP";
    
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr_.sin_addr, addr, INET_ADDRSTRLEN);
    info["downstream"] = std::string(addr) + ":" + std::to_string(ntohs(client_addr_.sin_port));
    
    if (server_rtp_addr_.sin_port != 0) {
        inet_ntop(AF_INET, &server_rtp_addr_.sin_addr, addr, INET_ADDRSTRLEN);
        info["upstream"] = std::string(addr) + ":" + std::to_string(ntohs(server_rtp_addr_.sin_port));
    } else {
        info["upstream"] = "Connecting...";
    }
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    info["proxy"] = std::to_string(duration);
    info["upstream_bandwidth"] = (uint64_t)upstream_est_.getBandwidth();
    info["downstream_bandwidth"] = (uint64_t)downstream_est_.getBandwidth();
    return info;
}

RtspMitmConfig RTSPMitmClient::resolve_upstream(const std::string &first_request, int client_fd)
{
    RtspMitmConfig config;
    std::string url;
    std::istringstream ss(first_request);
    std::string method, uri, version;
    ss >> method >> uri >> version;

    if (uri.rfind("rtsp://", 0) != 0)
    {
        throw std::runtime_error("First request does not contain a valid rtsp:// URI: " + uri);
    }

    config.ctx.rtsp_url = uri;
    if (rtspParser::parse_url(uri, config.ctx) != 0)
    {
        throw std::runtime_error("Failed to parse RTSP URL: " + uri);
    }

    std::string path = config.ctx.path;
    std::string rewritten_rtsp_url;
    bool is_prefixed = false;

    if (path.find("/rtp/") == 0 || path.find("/tv/") == 0) {
        if (httpParser::parse_http_url(path, rewritten_rtsp_url)) {
            is_prefixed = true;
        }
    }

    if (is_prefixed)
    {
        rtspCtx real_ctx;
        if (rtspParser::parse_url(rewritten_rtsp_url, real_ctx) == 0) {
            size_t path_pos = uri.find(real_ctx.path);
            if (path_pos != std::string::npos) {
                config.proxy_uri_prefix = uri.substr(0, path_pos);
                config.upstream_uri_base = "rtsp://" + real_ctx.server_ip + ":" + std::to_string(real_ctx.server_rtsp_port);
                config.ctx = real_ctx;
            }
        }
    }
    else if (path.size() > 1)
    {
        std::string stripped = path.substr(1);
        size_t slash = stripped.find('/');
        std::string first_seg = (slash != std::string::npos) ? stripped.substr(0, slash) : stripped;
        std::string real_path = (slash != std::string::npos) ? stripped.substr(slash) : "/";

        size_t colon = first_seg.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon < first_seg.size() - 1)
        {
            std::string real_host = first_seg.substr(0, colon);
            std::string port_str  = first_seg.substr(colon + 1);
            bool all_digits = !port_str.empty() && std::all_of(port_str.begin(), port_str.end(), ::isdigit);
            if (all_digits)
            {
                config.proxy_uri_prefix  = "rtsp://" + config.ctx.server_ip + ":" + std::to_string(config.ctx.server_rtsp_port) + "/" + first_seg;
                config.upstream_uri_base = "rtsp://" + real_host + ":" + port_str;
                config.ctx.server_ip        = real_host;
                config.ctx.server_rtsp_port = static_cast<uint16_t>(std::stoi(port_str));
                config.ctx.path             = real_path;
                config.ctx.rtsp_url         = config.upstream_uri_base + real_path;
            }
        }
    }

    if (BlacklistChecker::is_blacklisted(config.ctx.server_ip))
    {
        throw std::runtime_error("Connection to upstream " + config.ctx.server_ip + " is forbidden by blacklist.");
    }

    struct sockaddr_in local_addr{};
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(client_fd, (struct sockaddr *)&local_addr, &addr_len) == 0)
    {
        std::string proxy_ip = inet_ntoa(local_addr.sin_addr);
        uint16_t proxy_port = ntohs(local_addr.sin_port);
        if ((config.ctx.server_ip == "127.0.0.1" || config.ctx.server_ip == "localhost" || config.ctx.server_ip == proxy_ip) &&
            config.ctx.server_rtsp_port == proxy_port)
        {
            throw std::runtime_error("Recursive connection detected: URL points back to proxy itself.");
        }
    }

    return config;
}
