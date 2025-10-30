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

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>

RTSPClient::RTSPClient(EpollLoop *loop, BufferPool &pool, const sockaddr_in &client_addr, int client_fd, const std::string &rtsp_url)
    : loop(loop),
      buffer_pool_(pool),
      client_addr_(client_addr),
      client_fd_(client_fd)
{

    ctx.rtsp_url = rtsp_url;

    if (rtspParser::parse_url(rtsp_url, ctx) != 0)
    {
        on_closed_callback_();
    }

    client_ctx_ = new SocketCtx{
        client_fd_,
        std::bind(&RTSPClient::handle_client, this, std::placeholders::_1)};
    loop->set(client_ctx_, client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR);

    send_http_response();

    rtp_port_ = get_random_rtp_port();
    init_rtp_rtcp_sockets();
    if (ServerConfig::isNatEnabled() == true)
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

    if (rtsp_ctx_)
    {
        delete rtsp_ctx_;
        rtsp_ctx_ = nullptr;
    }

    if (rtp_ctx_)
    {
        delete rtp_ctx_;
        rtp_ctx_ = nullptr;
    }

    if (rtcp_ctx_)
    {
        delete rtcp_ctx_;
        rtcp_ctx_ = nullptr;
    }

    if (client_ctx_)
    {
        delete client_ctx_;
        client_ctx_ = nullptr;
    }

    if (timer_ctx)
    {
        delete timer_ctx;
        timer_ctx = nullptr;
    }

    if (client_fd_ >= 0)
    {
        loop->remove(client_fd_);
        close(client_fd_);
    }
    if (rtsp_fd_ >= 0)
    {
        loop->remove(rtsp_fd_);
        close(rtsp_fd_);
    }
    if (rtp_fd_ >= 0)
    {
        loop->remove(rtp_fd_);
        close(rtp_fd_);
    }
    if (rtcp_fd_ >= 0)
    {
        loop->remove(rtcp_fd_);
        close(rtcp_fd_);
    }
    if (timer_fd >= 0)
    {
        loop->remove(timer_fd);
        close(timer_fd);
    }

    for (auto &packet : send_queue_)
    {
        buffer_pool_.release(std::move(packet.data));
    }
    send_queue_.clear();
}

void RTSPClient::connect_server()
{
    rtsp_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx.server_rtsp_port);
    inet_pton(AF_INET, ctx.server_ip.c_str(), &addr.sin_addr);

    fcntl(rtsp_fd_, F_SETFL, O_NONBLOCK);

    rtsp_ctx_ = new SocketCtx{rtsp_fd_, std::bind(&RTSPClient::handle_rtsp, this, std::placeholders::_1)};

    int res = connect(rtsp_fd_, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS)
    {
        Logger::error("[RTSP] Connect to upstream failed.");
        close(rtsp_fd_);
        rtsp_fd_ = -1;
        return;
    }

    loop->set(rtsp_ctx_, rtsp_fd_, EPOLLOUT);

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
        read(timer_fd, &expirations, sizeof(expirations));
        push_request_into_queue(RtspMethod::GET_PARAMETER, "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path);
        build_and_send_request();
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
        Logger::info("[RTSP] Connection to upstream established.");
        state_ = RtspState::CONNECTED;
        return;
    }

    ssize_t n = send(rtsp_fd_, req_buf_.data() + tcp_send_offset_,
                     req_buf_.size() - tcp_send_offset_, 0);
    if (n > 0)
    {
        tcp_send_offset_ += n;
        if (tcp_send_offset_ == req_buf_.size())
        {
            loop->set(rtsp_ctx_, rtsp_fd_, EPOLLIN);
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
            if (resp_buf_.find("\r\n\r\n") != std::string::npos)
            {
                rtspParser::parse_session_id(resp_buf_, ctx);

                if (rtspParser::parse_status_code(resp_buf_) != 200)
                {
                    Logger::error("[RTSP] Connection to upstream refused. Please verify if the URL is correct.");
                    on_closed_callback_();
                }

                if (current_request_.method == RtspMethod::OPTIONS)
                {
                    send_rtsp_describe();
                }

                else if (current_request_.method == RtspMethod::DESCRIBE)
                {

                    send_rtsp_setup();
                }

                else if (current_request_.method == RtspMethod::SETUP)
                {
                    if (rtspParser::parse_server_ports(resp_buf_, ctx) != 0)
                    {
                        Logger::error("Can't parser server port");
                        on_closed_callback_();
                    }

                    Logger::info(std::string("[RTSP] SETUP done, ready to PLAY, server port: " + std::to_string(ctx.server_rtp_port) + "-" + std::to_string(ctx.server_rtcp_port)));

                    init_rtp_rtcp_server_addr();
                    send_rtp_trigger();
                    send_rtsp_play();
                }
                else if (current_request_.method == RtspMethod::PLAY)
                {
                    Logger::info(std::string("[RTSP] Streaming Start: " + ctx.rtsp_url + " -> " + std::string(inet_ntoa(client_addr_.sin_addr)) + ":" + std::to_string(ntohs(client_addr_.sin_port))));
                    init_timer_fd();
                    state_ = RtspState::STREAMING;
                }

                resp_buf_.clear();
            }
        }
        else if (n == 0)
        {
            Logger::info("[RTSP] Server closed connection");
            on_closed_callback_();
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        else
        {
            Logger::warn("[RTSP] Receive failed");
            on_closed_callback_();
        }
    }
}

void RTSPClient::on_rtp_writable()
{
}

void RTSPClient::on_rtp_readable()
{
    auto buf = buffer_pool_.acquire();
    ssize_t n = recvfrom(rtp_fd_, buf.get(), 1500, 0, nullptr, nullptr);

    if (n <= 0)
    {
        buffer_pool_.release(std::move(buf));
        return;
    }

    size_t recv_len = static_cast<size_t>(n);

    if (get_rtp_payload_offset(buf.get(), recv_len, payload_offset))
    {
        send_queue_.push_back(Packet{std::move(buf), recv_len, payload_offset});
    }
    else if (!is_init_ok)
    {
        if (ServerConfig::isNatEnabled() == true)
        {
            if (StunClient::extract_stun_mapping_from_response(buf.get(), recv_len, nat_wan_ip, nat_wan_port) == 0)
            {
                Logger::info("[RTP] Extract STUN mapping success: " + nat_wan_ip + ":" + std::to_string(nat_wan_port));
            };
            loop->set(rtp_ctx_, rtp_fd_, EPOLLIN);
        }
        buffer_pool_.release(std::move(buf));
        is_init_ok = true;
        send_rtsp_option();
    }
    else
    {
        // Unvalid rtp packet
        buffer_pool_.release(std::move(buf));
    }

    if (loop && client_fd_ >= 0 && client_ctx_)
        loop->set(client_ctx_, client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT);
}

void RTSPClient::on_rtcp_writable()
{
}

void RTSPClient::on_rtcp_readable()
{
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
        loop->set(client_ctx_, client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLIN);
}

void RTSPClient::on_client_readable()
{
}

void RTSPClient::on_client_closed()
{
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
        req_buf_ += "Session: " + ctx.session_id + "\r\n";
        req_buf_ += current_request_.headers;
        if (!current_request_.body.empty())
            req_buf_ += "Content-Length: " + std::to_string(current_request_.body.size()) + "\r\n\r\n" + current_request_.body;
        else
            req_buf_ += "\r\n";

        tcp_send_offset_ = 0;
        loop->set(rtsp_ctx_, rtsp_fd_, EPOLLOUT);
    }
}

void RTSPClient::init_rtp_rtcp_sockets()
{
    rtp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in rtp_addr{};
    rtp_addr.sin_family = AF_INET;
    rtp_addr.sin_port = htons(rtp_port_);
    rtp_addr.sin_addr.s_addr = INADDR_ANY;
    bind(rtp_fd_, (struct sockaddr *)&rtp_addr, sizeof(rtp_addr));
    fcntl(rtp_fd_, F_SETFL, O_NONBLOCK);

    rtcp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in rtcp_addr{};
    rtcp_addr.sin_family = AF_INET;
    rtcp_addr.sin_port = htons(rtp_port_ + 1);
    rtcp_addr.sin_addr.s_addr = INADDR_ANY;
    bind(rtcp_fd_, (struct sockaddr *)&rtcp_addr, sizeof(rtcp_addr));
    fcntl(rtcp_fd_, F_SETFL, O_NONBLOCK);

    rtp_ctx_ = new SocketCtx{rtp_fd_, std::bind(&RTSPClient::handle_rtp, this, std::placeholders::_1)};
    rtcp_ctx_ = new SocketCtx{rtcp_fd_, std::bind(&RTSPClient::handle_rtcp, this, std::placeholders::_1)};

    loop->set(rtp_ctx_, rtp_fd_, EPOLLIN);
    loop->set(rtcp_ctx_, rtcp_fd_, EPOLLIN);
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

void RTSPClient::init_timer_fd()
{
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its{};
    its.it_value.tv_sec = 20;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 20;
    its.it_interval.tv_nsec = 0;

    timerfd_settime(timer_fd, 0, &its, nullptr);

    timer_ctx = new SocketCtx{
        timer_fd,
        std::bind(&RTSPClient::handle_timer, this, std::placeholders::_1)};
    loop->set(timer_ctx, timer_fd, EPOLLIN);
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

bool RTSPClient::get_rtp_payload_offset(uint8_t *buf, size_t &recv_len, size_t &payload_offset)
{
    if (unlikely(recv_len < 12 || (buf[0] & 0xC0) != 0x80))
    {
        return false;
    }

    uint8_t flags = buf[0];
    size_t payloadstart = 12 + (flags & 0x0F) * 4;

    // Check for extension header
    if (likely(flags & 0x10))
    {
        if (payloadstart + 4 > recv_len)
        {
            Logger::warn("[RTP] Malformed RTP packet: extension header truncated");
            return false;
        }

        uint16_t ext_len = ntohs(*reinterpret_cast<uint16_t *>(buf + payloadstart + 2));
        payloadstart += 4 + 4 * ext_len;
    }

    // Calculate payload length
    size_t payload_len = recv_len - payloadstart;

    // Adjust for padding
    if (unlikely(flags & 0x20))
    {
        payload_len -= buf[recv_len - 1];
    }

    if (payload_len <= 0 || payloadstart + payload_len > recv_len)
    {
        Logger::warn("[RTP] Malformed RTP packet: invalid payload length");
        return false;
    }

    payload_offset = recv_len - payload_len;

    return true;
}

uint16_t RTSPClient::get_random_rtp_port()
{
    static int initialized = 0;
    if (!initialized)
    {
        srand(time(NULL) ^ getpid());
        initialized = 1;
    }

    int port = 10000 + (rand() % 25000) * 2;
    return port;
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
    push_request_into_queue(RtspMethod::DESCRIBE, "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path, "Accept: application/sdp\r\n", "");
    build_and_send_request();
}

void RTSPClient::send_rtsp_setup()
{
    rtspParser::SDP::parseSDP(resp_buf_, ctx);

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
    }

    int port1 = nat_wan_port ? nat_wan_port : rtp_port_;
    int port2 = port1 + 1;

    std::string header = "Transport: RTP/AVP;unicast;client_port=" +
                         std::to_string(port1) + "-" + std::to_string(port2) + "\r\n";

    Logger::info("[RTSP] SETUP with client port: " + std::to_string(port1) + "-" + std::to_string(port2));

    std::string url = "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path + "/" + track;
    push_request_into_queue(RtspMethod::SETUP, url, header, "");

    build_and_send_request();
}

void RTSPClient::send_rtsp_play()
{
    std::string header = "Range: npt=0.000-\r\n";
    std::string url = "rtsp://" + ctx.server_ip + ":" + std::to_string(ctx.server_rtsp_port) + ctx.path;
    push_request_into_queue(RtspMethod::PLAY, url, header);
    build_and_send_request();
}