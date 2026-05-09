#include "../include/statistics.h"
#include "../include/rtsp_client.h"
#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/buffer_pool.h"
#include "../include/common/socket_ctx.h"
#include "../include/common/rtsp_ctx.h"
#include "../include/stun_client.h"
#include "../include/utils.h"
#include "../include/rtsp_parser.h"
#include "../include/socket_helper.h"
#include "../include/port_pool.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <sys/timerfd.h>

RTSPClient::RTSPClient(EpollLoop *loop, BufferPool &pool, const sockaddr_in &client_addr, int client_fd, const rtspCtx &ctx)
    : loop(loop),
      buffer_pool_(pool),
      client_ctx_(std::make_unique<SocketCtx>(client_fd, [this](uint32_t event)
                                              { handle_client(event); })),
      start_time_(std::chrono::steady_clock::now()),
      client_addr_(client_addr),
      client_fd_(client_fd, loop),
      ctx(ctx),
      rtp_pipeline_(std::make_unique<RtpPipeline>())
{
    loop->remove(client_fd);

    loop->set(client_ctx_.get(), client_fd, EPOLLRDHUP | EPOLLHUP | EPOLLERR);

    send_http_response();
    init_rtp_rtcp_sockets();
    if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "stun")
    {
        StunClient::send_stun_mapping_request(rtp_fd_);
    }
    else
    {
        is_init_ok = true;
        send_rtsp_option();
    }
}

void RTSPClient::set_on_closed_callback(ClosedCallback cb)
{
    on_closed_callback_ = std::move(cb);
}

RTSPClient::~RTSPClient()
{
    for (auto &packet : send_queue_)
    {
        buffer_pool_.release(std::move(packet.data));
    }
    send_queue_.clear();

    if (rtp_port_ != 0) {
        PortPool::getInstance().release_pair(rtp_port_);
    }
}

void RTSPClient::connect_server()
{
    rtsp_fd_ = create_nonblocking_tcp(ctx.server_ip, ctx.server_rtsp_port, ServerConfig::getHttpUpstreamInterface());

    if (rtsp_fd_ < 0)
    {
        Logger::error("[RTSP] Connect to upstream failed.");
        if (on_closed_callback_) on_closed_callback_();
        return;
    }

    rtsp_ctx_ = std::make_unique<SocketCtx>(
        rtsp_fd_,
        [this](uint32_t event)
        { handle_rtsp(event); });

    loop->set(rtsp_ctx_.get(), rtsp_fd_, EPOLLOUT);

    state_ = RtspState::CONNECTING;
}

void RTSPClient::handle_rtsp(uint32_t event)
{
    if (event & EPOLLIN)
    {
        on_rtsp_readable();
    }
    if (event & EPOLLOUT)
    {
        on_rtsp_writable();
    }
}

void RTSPClient::handle_rtp(uint32_t event)
{
    if (event & EPOLLIN)
    {
        on_rtp_readable();
    }
    if (event & EPOLLOUT)
    {
        on_rtp_writable();
    }
}

void RTSPClient::handle_rtcp(uint32_t event)
{
    if (event & EPOLLIN)
    {
        on_rtcp_readable();
    }
    if (event & EPOLLOUT)
    {
        on_rtcp_writable();
    }
}

void RTSPClient::handle_client(uint32_t event)
{
    if (event & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        on_client_closed();
        return;
    }
    if (event & EPOLLIN)
    {
        on_client_readable();
    }
    if (event & EPOLLOUT)
    {
        on_client_writable();
    }
}

void RTSPClient::handle_timer(uint32_t event)
{
    if (event & EPOLLIN)
    {
        uint64_t expirations;
        read(timer_fd_, &expirations, sizeof(expirations));
        push_request_into_queue(RtspMethod::GET_PARAMETER, "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path);
        build_and_send_request();

        // if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte")
        // {
        //     send_zte_heartbeat();
        // }
    }
}

void RTSPClient::on_rtsp_writable()
{
    if (state_ == RtspState::CONNECTING)
    {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(rtsp_fd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        {
            Logger::error("[RTSP] Connect to upstream failed.");
            on_closed_callback_();
            return;
        }
        Logger::debug("[RTSP] Connection to upstream established.");
        state_ = RtspState::CONNECTED;

        struct sockaddr_in local_addr;
        socklen_t addr_len = sizeof(local_addr);
        if (getsockname(rtsp_fd_, (struct sockaddr *)&local_addr, &addr_len) == 0) {
            local_ip_ = inet_ntoa(local_addr.sin_addr);
            local_tcp_port_ = ntohs(local_addr.sin_port);
            Logger::debug("[RTSP] Local IP: " + local_ip_ + ", Local TCP Port: " + std::to_string(local_tcp_port_));
        }
    }

    ssize_t n = send(rtsp_fd_, req_buf_.data() + tcp_send_offset_,
                     req_buf_.size() - tcp_send_offset_, 0);
    if (n > 0)
    {
        tcp_send_offset_ += n;
        if (tcp_send_offset_ == req_buf_.size())
        {
            loop->set(rtsp_ctx_.get(), rtsp_fd_, EPOLLIN);
            tcp_send_offset_ = 0;
        }
    }
    else
    {
        Logger::error("[RTSP] RTSP control message send failed.");

        on_closed_callback_();
        return;
    }
}

void RTSPClient::on_rtsp_readable()
{
    while (true)
    {
        ssize_t n = recv(rtsp_fd_, rtsp_buf, sizeof(rtsp_buf), 0);
        if (n > 0)
        {
            resp_buf_.append(rtsp_buf, n);
            while (!resp_buf_.empty())
            {
                if (resp_buf_[0] == '$')
                {
                    if (resp_buf_.size() < 4)
                        break;
                    uint16_t len = ntohs(*reinterpret_cast<const uint16_t *>(resp_buf_.data() + 2));
                    if (resp_buf_.size() < static_cast<size_t>(len) + 4)
                        break;

                    uint8_t channel = static_cast<uint8_t>(resp_buf_[1]);
                    upstream_est_.addBytes(len + 4);
                    Statistics::getInstance().addUpstreamBytes(len + 4);
                    handle_interleaved_packet(channel, reinterpret_cast<const uint8_t *>(resp_buf_.data() + 4), len);
                    resp_buf_.erase(0, 4 + len);
                    continue;
                }

                size_t end = resp_buf_.find("\r\n\r\n");
                if (end == std::string::npos)
                    break;

                std::string header = resp_buf_.substr(0, end + 4);
                int content_length = rtspParser::get_content_length(header);
                if (resp_buf_.size() < end + 4 + content_length)
                    break;

                std::string body = resp_buf_.substr(end + 4, content_length);
                resp_buf_.erase(0, end + 4 + content_length);

                rtspParser::parse_session_id(header, ctx);
                int status = rtspParser::parse_status_code(header);

                if (status == -1)
                {
                    std::string cseq = rtspParser::extract_header_value(header, "CSeq");
                    if (!cseq.empty())
                    {
                        Logger::debug("[RTSP] Received request from server, responding with 200 OK (CSeq: " + cseq + ")");
                        std::string resp = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n\r\n";
                        send(rtsp_fd_, resp.data(), resp.size(), 0);
                        continue;
                    }
                }

                if (status == 461 && current_request_.method == RtspMethod::SETUP && !setup_retry_with_tcp_)
                {
                    Logger::warn("[RTSP] Upstream rejected UDP SETUP (461). Retrying with TCP Interleaved...");
                    setup_retry_with_tcp_ = true;
                    send_rtsp_setup();
                    continue;
                }

                if (status != 200)
                {
                    Logger::error("[RTSP] Connection to upstream refused. Status: " + std::to_string(status) + ", Header: " + header);
                    on_closed_callback_();
                    return;
                }

                if (current_request_.method == RtspMethod::OPTIONS)
                {
                    send_rtsp_describe();
                }
                else if (current_request_.method == RtspMethod::DESCRIBE)
                {
                    ctx.content_base = rtspParser::extract_header_value(header, "Content-Base");
                    send_rtsp_setup(body);
                }
                else if (current_request_.method == RtspMethod::SETUP)
                {
                    if (rtspParser::parse_server_ports(header, ctx) != 0)
                    {
                        Logger::error("Can't parser server port");
                        on_closed_callback_();
                        return;
                    }

                    if (header.find("interleaved=") != std::string::npos)
                    {
                        is_tcp_mode_ = true;
                        interleaved_rtp_channel_ = static_cast<uint8_t>(ctx.server_rtp_port);
                        interleaved_rtcp_channel_ = static_cast<uint8_t>(ctx.server_rtcp_port);
                        Logger::debug("[RTSP] SETUP done (TCP Interleaved), Channels: " +
                                     std::to_string(interleaved_rtp_channel_) + "-" +
                                     std::to_string(interleaved_rtcp_channel_));
                    }
                    else
                    {
                        is_tcp_mode_ = false;
                        Logger::debug(std::string("[RTSP] SETUP done (UDP), server port: " +
                                                 std::to_string(ctx.server_rtp_port) + "-" +
                                                 std::to_string(ctx.server_rtcp_port)));
                        init_rtp_rtcp_server_addr();
                        if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte")
                        {
                            send_zte_heartbeat();
                        }
                        else
                        {
                            send_rtp_trigger();
                        }
                    }
                    send_rtsp_play();
                }
                else if (current_request_.method == RtspMethod::PLAY)
                {
                    Logger::debug(std::string("[RTSP] Streaming Start: " + ctx.rtsp_url));
                    rtp_pipeline_->reset();
                    init_timer_fd();
                    state_ = RtspState::STREAMING;
                }
            }
        }
        else if (n == 0)
        {
            Logger::debug("[RTSP] Server closed connection");
            on_closed_callback_();
            return;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        else
        {
            Logger::warn("[RTSP] Receive failed");
            on_closed_callback_();
            return;
        }
    }
}

void RTSPClient::on_rtp_writable()
{
}

void RTSPClient::on_rtp_readable()
{
    auto buf = buffer_pool_.acquire();
    ssize_t n = recvfrom(rtp_fd_, buf.get(), buffer_pool_.get_buffer_size(), 0, nullptr, nullptr);

    if (n <= 0)
    {
        buffer_pool_.release(std::move(buf));
        return;
    }

    upstream_est_.addBytes(n);
    Statistics::getInstance().addUpstreamBytes(n);
    size_t recv_len = static_cast<size_t>(n);

    if (rtp_pipeline_->process(buf.get(), recv_len))
    {
        size_t payload_off = 0;
        if (RtpPipeline::get_payload_offset(buf.get(), recv_len, payload_off))
        {
            if (send_queue_.size() > 512) {
                auto &old = send_queue_.front();
                if (old.data) buffer_pool_.release(std::move(old.data));
                send_queue_.pop_front();
            }
            send_queue_.push_back(Packet{std::move(buf), recv_len, payload_off});
        }
        else
        {
            buffer_pool_.release(std::move(buf));
        }
    }
    else if (!is_init_ok)
    {
        if (ServerConfig::isNatEnabled() == true)
        {
            if (StunClient::extract_stun_mapping_from_response(buf.get(), recv_len, nat_wan_ip, nat_wan_port) == 0)
            {
                Logger::debug("[RTP] Extract STUN mapping success: " + nat_wan_ip + ":" + std::to_string(nat_wan_port));
            };
            loop->set(rtp_ctx_.get(), rtp_fd_, EPOLLIN);
        }
        buffer_pool_.release(std::move(buf));
        is_init_ok = true;
        send_rtsp_option();
    }
    else
    {
        buffer_pool_.release(std::move(buf));
    }

    if (loop && client_fd_ >= 0 && client_ctx_)
        loop->set(client_ctx_.get(), client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT);
}

void RTSPClient::on_rtcp_writable()
{
}

void RTSPClient::on_rtcp_readable()
{
}

void RTSPClient::handle_interleaved_packet(uint8_t channel, const uint8_t *data, size_t len)
{
    if (channel != interleaved_rtp_channel_)
        return;

    auto buf = buffer_pool_.acquire();
    size_t max_buf_size = buffer_pool_.get_buffer_size();
    size_t actual_len = std::min(len, max_buf_size);
    memcpy(buf.get(), data, actual_len);
    if (rtp_pipeline_->process(buf.get(), actual_len))
    {
        size_t payload_off = 0;
        if (RtpPipeline::get_payload_offset(buf.get(), actual_len, payload_off))
        {
            if (send_queue_.size() > 512) {
                auto &old = send_queue_.front();
                if (old.data) buffer_pool_.release(std::move(old.data));
                send_queue_.pop_front();
            }
            send_queue_.push_back(Packet{std::move(buf), actual_len, payload_off});
        }
        else
        {
            buffer_pool_.release(std::move(buf));
        }
    }
    else
    {
        buffer_pool_.release(std::move(buf));
    }

    if (loop && client_fd_ >= 0 && client_ctx_)
        loop->set(client_ctx_.get(), client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT | EPOLLIN);
}

void RTSPClient::on_client_writable()
{
    while (!send_queue_.empty())
    {
        auto &packet = send_queue_.front();
        ssize_t n = send(client_fd_, packet.data.get() + packet.offset, packet.length - packet.offset, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
            {
                on_client_closed();
                return;
            }
        }

        Statistics::getInstance().addDownstreamBytes(n);
        downstream_est_.addBytes(n);
        packet.offset += n;

        if (packet.offset == packet.length)
        {
            buffer_pool_.release(std::move(packet.data));
            send_queue_.pop_front();
        }
        else
        {
            break;
        }
    }

    if (send_queue_.empty())
        loop->set(client_ctx_.get(), client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLIN);
}

void RTSPClient::on_client_readable()
{
}

void RTSPClient::on_client_closed()
{
    if (is_closed_) return;
    is_closed_ = true;
    if (on_closed_callback_)
        on_closed_callback_();
}

void RTSPClient::push_request_into_queue(RtspMethod method, const std::string &uri, const std::string &extra_headers, const std::string &body)
{
    RtspRequest req{method, uri, extra_headers, body, cseq_++};
    request_queue_.push(req);
}

void RTSPClient::build_and_send_request()
{
    if (!request_queue_.empty())
    {
        current_request_ = request_queue_.front();
        request_queue_.pop();

        req_buf_.clear();
        req_buf_ += RtspMethodToString(current_request_.method) + " " + current_request_.uri + " RTSP/1.0\r\n";
        req_buf_ += "CSeq: " + std::to_string(current_request_.cseq) + "\r\n";
        if (!ctx.session_id.empty())
            req_buf_ += "Session: " + ctx.session_id + "\r\n";
        req_buf_ += current_request_.headers;
        if (!current_request_.body.empty())
            req_buf_ += "Content-Length: " + std::to_string(current_request_.body.size()) + "\r\n\r\n" + current_request_.body;
        else
            req_buf_ += "\r\n";

        tcp_send_offset_ = 0;
        loop->set(rtsp_ctx_.get(), rtsp_fd_, EPOLLOUT);
    }
}

void RTSPClient::init_rtp_rtcp_sockets()
{
    if (bind_udp_pair_from_pool(rtp_fd_.get_ref(), rtcp_fd_.get_ref(), rtp_port_, ServerConfig::getHttpUpstreamInterface()) < 0)
    {
        Logger::error("[RTP] Failed to bind RTP/RTCP sockets from pool");
        if (on_closed_callback_) on_closed_callback_();
        return;
    }


    rtp_ctx_ = std::make_unique<SocketCtx>(
        rtp_fd_,
        [this](uint32_t event)
        { handle_rtp(event); });

    rtcp_ctx_ = std::make_unique<SocketCtx>(
        rtcp_fd_,
        [this](uint32_t event)
        { handle_rtcp(event); });

    loop->set(rtp_ctx_.get(), rtp_fd_, EPOLLIN);
    loop->set(rtcp_ctx_.get(), rtcp_fd_, EPOLLIN);
}

void RTSPClient::init_rtp_rtcp_server_addr()
{
    server_rtp_addr_.sin_family = AF_INET;
    server_rtp_addr_.sin_port = htons(ctx.server_rtp_port);
    inet_pton(AF_INET, ctx.server_ip.c_str(), &server_rtp_addr_.sin_addr);

    server_rtcp_addr_.sin_family = AF_INET;
    server_rtcp_addr_.sin_port = htons(ctx.server_rtcp_port);
    inet_pton(AF_INET, ctx.server_ip.c_str(), &server_rtcp_addr_.sin_addr);
}

void RTSPClient::send_rtp_trigger()
{
    char dummy = 0;
    ssize_t n = sendto(rtp_fd_, &dummy, 1, 0,
                       (struct sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
    if (n < 0)
    {
        Logger::error("[RTP] Trigger send failed");
    }
}

void RTSPClient::send_zte_heartbeat()
{
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

    uint16_t udp_port = rtp_port_;
    uint16_t tcp_port = local_tcp_port_;

    payload[16] = (udp_port >> 8) & 0xFF;
    payload[17] = udp_port & 0xFF;
    payload[18] = (tcp_port >> 8) & 0xFF;
    payload[19] = tcp_port & 0xFF;

    ssize_t n = sendto(rtp_fd_, payload, sizeof(payload), 0,
                       (struct sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
    if (n < 0)
    {
        Logger::error("[RTP] ZTE heartbeat send failed");
    }
    else
    {
        Logger::debug("[RTP] ZTE heartbeat sent to " + ctx.server_ip + ":" + std::to_string(ctx.server_rtp_port));
    }
}

void RTSPClient::init_timer_fd()
{
    using namespace std::chrono;

    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    itimerspec its{};
    auto interval = seconds(20);

    its.it_value.tv_sec = interval.count();
    its.it_interval.tv_sec = interval.count();

    timerfd_settime(timer_fd_, 0, &its, nullptr);

    timer_ctx = std::make_unique<SocketCtx>(
        timer_fd_,
        [this](uint32_t event)
        { handle_timer(event); });

    loop->set(timer_ctx.get(), timer_fd_, EPOLLIN);
}

void RTSPClient::send_http_response()
{
    auto buf = buffer_pool_.acquire();

    const char *response_header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: video/mp2t\r\n"
        "Connection: close\r\n"
        "\r\n";

    size_t len = strlen(response_header);
    memcpy(buf.get(), response_header, len);

    send_queue_.push_back(Packet{std::move(buf), len, 0});
}


std::string RTSPClient::RtspMethodToString(RtspMethod method)
{
    switch (method)
    {
    case RtspMethod::OPTIONS:
        return "OPTIONS";
    case RtspMethod::DESCRIBE:
        return "DESCRIBE";
    case RtspMethod::SETUP:
        return "SETUP";
    case RtspMethod::PLAY:
        return "PLAY";
    case RtspMethod::PAUSE:
        return "PAUSE";
    case RtspMethod::TEARDOWN:
        return "TEARDOWN";
    case RtspMethod::GET_PARAMETER:
        return "GET_PARAMETER";
    case RtspMethod::SET_PARAMETER:
        return "SET_PARAMETER";
    default:
        return "";
    }
}

void RTSPClient::send_rtsp_option()
{
    connect_server();
    push_request_into_queue(RtspMethod::OPTIONS, "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path, "", "");
    build_and_send_request();
}

void RTSPClient::send_rtsp_describe()
{
    std::string headers = "Accept: application/sdp\r\n";
    if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte") {
        headers += "User-Agent: HMTL RTSP 1.0; CTC/2.0\r\n";
        headers += "x-NAT: " + local_ip_ + ":" + std::to_string(local_tcp_port_) + "\r\n";
        headers += "Timeshift: 1\r\n";
        headers += "x-BurstSize: 1048576\r\n";
    }
    push_request_into_queue(RtspMethod::DESCRIBE, "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path, headers, "");
    build_and_send_request();
}

void RTSPClient::send_rtsp_setup(const std::string &sdp_data)
{
    if (!sdp_data.empty())
    {
        rtspParser::SDP::parseSDP(sdp_data, ctx);
    }

    std::string track;

    for (const auto &media : ctx.sdp.media_streams)
    {

        if (std::find(media.formats.begin(), media.formats.end(), "33") != media.formats.end())
        {
            track = media.trackID;
            break;
        }
    }

    if (track.empty())
    {
        Logger::error("[RTSP] Unsupported video format, no track with format 33 found!");
        on_closed_callback_();
        return;
    }

    int port1 = nat_wan_port ? nat_wan_port : rtp_port_;
    int port2 = port1 + 1;

    std::string header;
    if (setup_retry_with_tcp_)
    {
        header = "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n";
        Logger::debug("[RTSP] SETUP with TCP Interleaved mode");
    }
    else if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte")
    {
        header = "Transport: MP2T/RTP/UDP;unicast;client_address=" + local_ip_ +
                 ";client_port=" + std::to_string(port1) + "-" + std::to_string(port2) +
                 ";mode=PLAY\r\n";
        header += "User-Agent: HMTL RTSP 1.0; CTC/2.0\r\n";
        header += "x-NAT: " + local_ip_ + ":" + std::to_string(local_tcp_port_) + "\r\n";
        Logger::debug("[RTSP] ZTE SETUP with client port: " + std::to_string(port1) + "-" + std::to_string(port2));
    }
    else
    {
        header = "Transport: RTP/AVP;unicast;client_port=" +
                 std::to_string(port1) + "-" + std::to_string(port2) + "\r\n";
        Logger::debug("[RTSP] SETUP with client port: " + std::to_string(port1) + "-" + std::to_string(port2));
    }

    std::string base_url;
    if (!ctx.content_base.empty()) {
        base_url = ctx.content_base;
    } else {
        base_url = "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path;
        size_t query_pos = base_url.find('?');
        if (query_pos != std::string::npos) {
            base_url = base_url.substr(0, query_pos);
        }
    }

    if (!base_url.empty() && base_url.back() != '/' && !track.empty() && track[0] != '*') {
        base_url += "/";
    }
    
    std::string url = base_url + track;
    push_request_into_queue(RtspMethod::SETUP, url, header, "");

    build_and_send_request();
}

void RTSPClient::send_rtsp_play()
{
    std::string base_url;
    if (!ctx.content_base.empty()) {
        base_url = ctx.content_base;
    } else {
        base_url = "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path;
    }

    std::string header = "Range: npt=0.000-\r\n";
    if (ServerConfig::isNatEnabled() && ServerConfig::getNatMethod() == "zte") {
        // Only use clock=end- for live streams (usually no query params like tvdr)
        if (ctx.path.find('?') == std::string::npos) {
            header = "Range: clock=end-\r\n";
        }
        header += "User-Agent: HMTL RTSP 1.0; CTC/2.0\r\n";
        header += "x-BurstSize: 1048576\r\n";
        header += "Scale: 1.0\r\n";
    }
    
    push_request_into_queue(RtspMethod::PLAY, base_url, header);
    build_and_send_request();
}

//////////////////////////////
// FdGuard
//////////////////////////////

RTSPClient::FdGuard::FdGuard() = default;

RTSPClient::FdGuard::FdGuard(int fd, EpollLoop *loop) : fd_(fd), loop_(loop) {}

RTSPClient::FdGuard::~FdGuard()
{
    if (fd_ >= 0)
    {
        if (loop_)
            loop_->remove(fd_);
        close(fd_);
    }
}

RTSPClient::FdGuard::FdGuard(FdGuard &&other) noexcept
    : fd_(other.fd_), loop_(other.loop_)
{
    other.fd_ = -1;
    other.loop_ = nullptr;
}

RTSPClient::FdGuard &RTSPClient::FdGuard::operator=(FdGuard &&other) noexcept
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

int &RTSPClient::FdGuard::get_ref() { return fd_; }
int RTSPClient::FdGuard::get() const { return fd_; }
RTSPClient::FdGuard::operator int() const { return fd_; }
json RTSPClient::get_info() const
{
    json info;
    info["type"] = "http-proxy";
    info["transport"] = is_tcp_mode_ ? "TCP" : "UDP";
    
    char addr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr_.sin_addr, addr, INET_ADDRSTRLEN);
    info["downstream"] = std::string(addr) + ":" + std::to_string(ntohs(client_addr_.sin_port));
    
    info["upstream"] = ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port);
    
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    info["proxy"] = std::to_string(duration);
    info["upstream_bandwidth"] = (uint64_t)upstream_est_.getBandwidth();
    info["downstream_bandwidth"] = (uint64_t)downstream_est_.getBandwidth();
    
    return info;
}
