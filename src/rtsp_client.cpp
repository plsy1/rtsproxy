#include "../include/rtsp_client.h"
#include "../include/epoll_loop.h"
#include "../include/logger.h"
#include "../include/server_config.h"
#include "../include/buffer_pool.h"
#include "../include/common/rtsp_client.h"
#include "../include/common/socket_ctx.h"
#include "../include/stun_client.h"
#include "../include/utils.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sstream>
#include <cstring>
#include <sys/timerfd.h>
#include <unistd.h>

RTSPClient::RTSPClient(const RTSPClientCtx &info, EpollLoop *loop, BufferPool &pool)
    : loop(loop),
      client_fd_(info.client_fd),
      client_addr_(info.client_addr),
      rtsp_url(info.rtsp_url),
      buffer_pool_(pool)
{
    parse_url(info.rtsp_url);
    init_client_fd();
    client_rtp_port_ = get_random_rtp_port();
    init_rtp_rtcp_sockets();
    if (ServerConfig::isNatEnabled() == true)
    {
        StunClient::send_stun_mapping_request(rtp_fd_);
    }
    else
    {
        is_init_ok = true;
        connect_server();
        push_request_into_queue("OPTIONS", "", "");
    }
}

void RTSPClient::set_on_closed_callback(ClosedCallback cb)
{
    on_closed_callback_ = std::move(cb);
}

RTSPClient::~RTSPClient()
{

    if (sock_ctx_)
    {
        delete sock_ctx_;
        sock_ctx_ = nullptr;
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

    if (common_)
    {
        delete common_;
        common_ = nullptr;
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
    if (sockfd_ >= 0)
    {
        loop->remove(sockfd_);
        close(sockfd_);
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

void RTSPClient::parse_url(const std::string &url)
{
    if (!url.rfind("rtsp://", 0) == 0)
        return;

    size_t slash = url.find('/', 7);
    std::string hostport = url.substr(7, slash - 7);
    size_t colon = hostport.find(':');

    if (colon != std::string::npos)
    {
        server_ip_ = hostport.substr(0, colon);
        server_port_ = std::stoi(hostport.substr(colon + 1));
    }
    else
    {
        server_ip_ = hostport;
        server_port_ = 554;
    }

    path_ = (slash != std::string::npos) ? url.substr(slash) : "/";
}

bool RTSPClient::connect_server()
{
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0)
        return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_port_);
    inet_pton(AF_INET, server_ip_.c_str(), &addr.sin_addr);

    fcntl(sockfd_, F_SETFL, O_NONBLOCK);

    sock_ctx_ = new SocketCtx{sockfd_, std::bind(&RTSPClient::handle_tcp_control, this, std::placeholders::_1)};

    int res = connect(sockfd_, (struct sockaddr *)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS)
    {
        Logger::error("[RTSP] Connect to upstream failed.");
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    register_sockets_to_epoll(sock_ctx_, EPOLLOUT);

    state_ = RtspState::CONNECTING;
    return true;
}

void RTSPClient::handle_tcp_control(uint32_t event)
{
    if (event & EPOLLIN)
    {
        on_tcp_control_readable();
    }
    if (event & EPOLLOUT)
    {
        on_tcp_control_writable();
    }
}

void RTSPClient::handle_rtp_control(uint32_t event)
{
    if (event & EPOLLIN)
    {
        on_rtp_control_readable();
    }
    if (event & EPOLLOUT)
    {
        on_rtp_control_writable();
    }
}

void RTSPClient::handle_rtcp_control(uint32_t event)
{
    if (event & EPOLLIN)
    {
        on_rtcp_control_readable();
    }
    if (event & EPOLLOUT)
    {
        on_rtcp_control_writable();
    }
}

void RTSPClient::handle_client_control(uint32_t event)
{
    if (event & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        on_client_control_closed();
        return;
    }
    if (event & EPOLLIN)
    {
        on_client_control_readable();
    }
    if (event & EPOLLOUT)
    {
        on_client_control_writable();
    }
}

void RTSPClient::on_tcp_control_writable()
{
    // check if coonnect success
    if (state_ == RtspState::CONNECTING)
    {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
        {
            Logger::error("[RTSP] Connect to upstream failed.");
            if (on_closed_callback_)
                on_closed_callback_();
            return;
        }
        Logger::info("[RTSP] Connection to upstream established.");
        state_ = RtspState::IDLE;
        return;
    }

    ssize_t n = send(sockfd_, req_buf_.data() + tcp_send_offset_,
                     req_buf_.size() - tcp_send_offset_, 0);
    if (n > 0)
    {
        tcp_send_offset_ += n;
        if (tcp_send_offset_ == req_buf_.size())
        {
            loop->set(sock_ctx_, sockfd_, EPOLLIN);
            tcp_send_offset_ = 0;
        }
    }
    else
    {
        Logger::error("[RTSP] RTSP control message send failed.");
        if (on_closed_callback_)
            on_closed_callback_();
        return;
    }
}

void RTSPClient::on_tcp_control_readable()
{
    char buf[2048];
    while (true)
    {
        ssize_t n = recv(sockfd_, buf, sizeof(buf), 0);
        if (n > 0)
        {
            resp_buf_.append(buf, n);
            if (resp_buf_.find("\r\n\r\n") != std::string::npos)
            {
                process_response(resp_buf_);
                resp_buf_.clear();
            }
        }
        else if (n == 0)
        {
            Logger::info("[RTSP] Server closed connection");
            close(sockfd_);
            sockfd_ = -1;
            state_ = RtspState::DISCONNECTED;
            break;
        }
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }
        else
        {
            Logger::warn("[RTSP] Receive failed");
            break;
        }
    }
}

void RTSPClient::push_request_into_queue(const std::string &method,
                                         const std::string &extra_headers,
                                         const std::string &body)
{
    std::string uri = build_uri_for_method(method);
    RtspRequest req{method, uri, extra_headers, body, seq_++};
    request_queue_.push(req);
    build_and_send_request();
}

void RTSPClient::build_and_send_request()
{
    if (!request_queue_.empty())
    {
        current_request_ = request_queue_.front();
        request_queue_.pop();

        req_buf_.clear();
        req_buf_ += current_request_.method + " " + current_request_.uri + " RTSP/1.0\r\n";
        req_buf_ += "CSeq: " + std::to_string(current_request_.cseq) + "\r\n";
        if (!session_id_.empty())
            req_buf_ += "Session: " + session_id_ + "\r\n";
        req_buf_ += current_request_.headers;
        if (!current_request_.body.empty())
            req_buf_ += "Content-Length: " + std::to_string(current_request_.body.size()) + "\r\n\r\n" + current_request_.body;
        else
            req_buf_ += "\r\n";

        tcp_send_offset_ = 0;
        loop->set(sock_ctx_, sockfd_, EPOLLOUT);
    }
}

int RTSPClient::parse_status_code(const std::string &resp)
{
    int code = -1;
    sscanf(resp.c_str(), "RTSP/%*s %d", &code);
    return code;
}

void RTSPClient::parse_session(const std::string &resp)
{
    size_t pos = resp.find("Session:");
    if (pos == std::string::npos)
        return;
    pos += 8;
    while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t'))
        ++pos;
    size_t end = resp.find_first_of(";\r\n", pos);
    session_id_ = resp.substr(pos, end - pos);
}

void RTSPClient::process_response(const std::string &resp)
{
    int code = parse_status_code(resp);
    parse_session(resp);

    if (code == 200)
    {
        if (current_request_.method == "OPTIONS")
        {
            push_request_into_queue("DESCRIBE",
                                    "Accept: application/sdp\r\n", "");
        }
        else if (current_request_.method == "DESCRIBE")
        {
            auto tracks = parse_sdp_tracks(resp);
            for (const auto &t : tracks)
            {
                if (t.rfind("rtsp://", 0) == 0)
                    track_ = t;
                else
                    track_ = "rtsp://" + server_ip_ + ":" +
                             std::to_string(server_port_) + path_ + "/" + t;
            }

            std::ostringstream oss;
            oss << "Transport: RTP/AVP;unicast;client_port="
                << (nat_wan_port ? nat_wan_port : client_rtp_port_)
                << "-"
                << (nat_wan_port ? (nat_wan_port + 1) : (client_rtp_port_ + 1))
                << "\r\n";
            std::string header = oss.str();
            Logger::info(
                std::string("[RTSP] SETUP with client port: " +
                            std::to_string((nat_wan_port ? nat_wan_port : client_rtp_port_)) +
                            "-" +
                            std::to_string((nat_wan_port ? (nat_wan_port + 1) : (client_rtp_port_ + 1)))));
            push_request_into_queue("SETUP", header, "");
        }
        else if (current_request_.method == "SETUP")
        {

            parse_server_ports(resp);
            init_rtp_rtcp_server_addr();

            if (ServerConfig::isNatEnabled() == false)
            {
                send_rtp_trigger(); // public ip environment needed only, don't do this in nat environment
            }

            Logger::info(
                std::string("[RTSP] SETUP done, ready to PLAY, server port: " +
                            std::to_string(server_rtp_port_) + "-" +
                            std::to_string(server_rtcp_port_)));

            std::ostringstream play_header;

            play_header << "Range: npt=0.000-" << "\r\n";
            std::string header = play_header.str();
            push_request_into_queue("PLAY", header);
        }
        else if (current_request_.method == "PLAY")
        {
            Logger::info(std::string("[RTSP] Streaming Start: " + rtsp_url + " -> " + std::string(inet_ntoa(client_addr_.sin_addr)) + ":" +
                                     std::to_string(ntohs(client_addr_.sin_port))

                                         ));
            start_getparameter_timer();
        }

        else if (current_request_.method == "STREAMING")
        {
        }
    }
    else
    {
        Logger::error("[RTSP] Connection to upstream refused. Please verify if the URL is correct.");
        state_ = RtspState::IDLE;
    }
}

std::string RTSPClient::build_uri_for_method(const std::string &method) const
{
    if (method == "SETUP")
        return "rtsp://" + server_ip_ + ":" + std::to_string(server_port_) + path_ + track_;
    return "rtsp://" + server_ip_ + ":" + std::to_string(server_port_) + path_;
}

std::vector<std::string> RTSPClient::parse_sdp_tracks(const std::string &sdp)
{
    std::vector<std::string> tracks;
    std::istringstream ss(sdp);
    std::string line;
    while (std::getline(ss, line))
    {
        if (line.rfind("a=control:trackID", 0) == 0)
            tracks.push_back(line.substr(strlen("a=control:")));
    }
    return tracks;
}

void RTSPClient::parse_server_ports(const std::string &resp)
{
    size_t pos = resp.find("Transport:");
    if (pos == std::string::npos)
        return;

    size_t end = resp.find("\r\n", pos);
    transport_ = resp.substr(pos, end - pos);

    size_t sp_pos = transport_.find("server_port=");
    if (sp_pos != std::string::npos)
    {
        sp_pos += strlen("server_port=");
        size_t dash = transport_.find('-', sp_pos);
        if (dash != std::string::npos)
        {
            try
            {
                server_rtp_port_ = std::stoi(transport_.substr(sp_pos, dash - sp_pos));
                server_rtcp_port_ = std::stoi(transport_.substr(dash + 1));
            }
            catch (...)
            {
                server_rtp_port_ = server_rtcp_port_ = 0;
            }
        }
    }
}

bool RTSPClient::init_rtp_rtcp_sockets()
{
    rtp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_fd_ < 0)
        return false;

    sockaddr_in rtp_addr{};
    rtp_addr.sin_family = AF_INET;
    rtp_addr.sin_port = htons(client_rtp_port_);
    rtp_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(rtp_fd_, (struct sockaddr *)&rtp_addr, sizeof(rtp_addr)) < 0)
        return false;

    fcntl(rtp_fd_, F_SETFL, O_NONBLOCK);

    rtcp_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtcp_fd_ < 0)
        return false;

    sockaddr_in rtcp_addr{};
    rtcp_addr.sin_family = AF_INET;
    rtcp_addr.sin_port = htons(client_rtp_port_ + 1);
    rtcp_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(rtcp_fd_, (struct sockaddr *)&rtcp_addr, sizeof(rtcp_addr)) < 0)
        return false;

    fcntl(rtcp_fd_, F_SETFL, O_NONBLOCK);

    rtp_ctx_ = new SocketCtx{rtp_fd_, std::bind(&RTSPClient::handle_rtp_control, this, std::placeholders::_1)};
    rtcp_ctx_ = new SocketCtx{rtcp_fd_, std::bind(&RTSPClient::handle_rtcp_control, this, std::placeholders::_1)};

    register_sockets_to_epoll(rtp_ctx_, EPOLLIN);
    register_sockets_to_epoll(rtcp_ctx_, EPOLLIN);

    return true;
}

void RTSPClient::init_rtp_rtcp_server_addr()
{
    server_rtp_addr_.sin_family = AF_INET;
    server_rtp_addr_.sin_port = htons(server_rtp_port_);
    inet_pton(AF_INET, server_ip_.c_str(), &server_rtp_addr_.sin_addr);

    server_rtcp_addr_.sin_family = AF_INET;
    server_rtcp_addr_.sin_port = htons(server_rtcp_port_);
    inet_pton(AF_INET, server_ip_.c_str(), &server_rtcp_addr_.sin_addr);
}

void RTSPClient::register_sockets_to_epoll(SocketCtx *ctx, uint32_t events)
{

    loop->set(ctx, ctx->fd, events);
}

void RTSPClient::on_rtp_control_writable()
{
}

void RTSPClient::on_rtp_control_readable()
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
        connect_server();
        push_request_into_queue("OPTIONS", "", "");
    }
    else
    {
        // Unvalid rtp packet
        buffer_pool_.release(std::move(buf));
    }

    if (loop && client_fd_ >= 0 && common_)
        loop->set(common_, client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLOUT);
}

void RTSPClient::on_rtcp_control_writable()
{
}
void RTSPClient::on_rtcp_control_readable()
{
}

void RTSPClient::on_client_control_writable()
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
                on_client_control_closed();
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
        loop->set(common_, client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLIN);
}

void RTSPClient::on_client_control_readable()
{
}
void RTSPClient::on_client_control_closed()
{
    if (on_closed_callback_)
        on_closed_callback_();
}

void RTSPClient::send_rtp_trigger()
{
    if (rtp_fd_ < 0)
        return;

    char dummy = 0;
    ssize_t n = sendto(rtp_fd_, &dummy, 1, 0,
                       (struct sockaddr *)&server_rtp_addr_, sizeof(server_rtp_addr_));
    if (n < 0)
    {
        Logger::error("[RTP] Trigger send failed");
    }
    else
    {
        Logger::debug("[RTP] Trigger sent");
    }
}

void RTSPClient::on_timer_fd(uint32_t event)
{
    if (event & EPOLLIN)
    {
        uint64_t expirations;
        read(timer_fd, &expirations, sizeof(expirations));
        push_request_into_queue("GET_PARAMETER");
    }
}

bool RTSPClient::start_getparameter_timer()
{
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0)
    {
        Logger::error("[RTP] Keepalive timer create failed");
        return false;
    }

    struct itimerspec its{};
    its.it_value.tv_sec = 20;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 20;
    its.it_interval.tv_nsec = 0;

    if (timerfd_settime(timer_fd, 0, &its, nullptr) < 0)
    {
        Logger::error("[RTP] Keepalive timer  settime failed");
        close(timer_fd);
        timer_fd = -1;
        return false;
    }

    timer_ctx = new SocketCtx{
        timer_fd,
        std::bind(&RTSPClient::on_timer_fd, this, std::placeholders::_1)};
    loop->set(timer_ctx, timer_fd, EPOLLIN);

    return true;
}

void RTSPClient::init_client_fd()
{
    common_ = new SocketCtx{
        client_fd_,
        std::bind(&RTSPClient::handle_client_control, this, std::placeholders::_1)};
    loop->set(common_, client_fd_, EPOLLRDHUP | EPOLLHUP | EPOLLERR);

    send_http_response();
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

int RTSPClient::get_random_rtp_port()
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