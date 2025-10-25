#pragma once

#include <string>
#include <queue>
#include <vector>
#include <netinet/in.h>
#include <functional>

class EpollLoop;

enum class RtspState
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    IDLE,
    WAIT_RESPONSE,
    STREAMING
};

struct RtspRequest
{
    std::string method;
    std::string uri;
    std::string headers;
    std::string body;
    int cseq;
};

class BufferPool;
class SocketCtx;
class Packet;
struct RTSPClientCtx;

class RTSPClient
{
public:
    explicit RTSPClient(const RTSPClientCtx &info, EpollLoop *loop, BufferPool &pool);
    ~RTSPClient();

    using ClosedCallback = std::function<void()>;

    void set_on_closed_callback(ClosedCallback cb);

    void handle_tcp_control(uint32_t event);
    void handle_rtp_control(uint32_t event);
    void handle_rtcp_control(uint32_t event);
    void handle_client_control(uint32_t event);

private:
    ClosedCallback on_closed_callback_;
    void init_client_fd();
    bool connect_server();
    void push_request_into_queue(const std::string &method,
                                 const std::string &extra_headers = "",
                                 const std::string &body = "");
    void parse_url(const std::string &url);
    void build_and_send_request();
    std::string build_uri_for_method(const std::string &method) const;

    int parse_status_code(const std::string &resp);
    void process_response(const std::string &resp);
    void parse_session(const std::string &resp);
    void parse_server_ports(const std::string &resp);

    std::vector<std::string> parse_sdp_tracks(const std::string &sdp);

    bool init_rtp_rtcp_sockets();
    void init_rtp_rtcp_server_addr();
    void register_sockets_to_epoll(SocketCtx *ctx, uint32_t events);
    void send_rtp_trigger();
    bool start_getparameter_timer();
    void on_timer_fd(uint32_t event);
    void send_http_response();
    bool get_rtp_payload_offset(uint8_t *buf, size_t &recv_len, size_t &payload_offset);
    int get_random_rtp_port();

    void on_tcp_control_writable();
    void on_tcp_control_readable();
    void on_rtp_control_writable();
    void on_rtp_control_readable();
    void on_rtcp_control_writable();
    void on_rtcp_control_readable();
    void on_client_control_writable();
    void on_client_control_readable();
    void on_client_control_closed();

private:
    EpollLoop *loop;
    int client_fd_;
    int client_rtp_port_;
    sockaddr_in client_addr_;
    std::string rtsp_url;
    BufferPool &buffer_pool_;
    SocketCtx *sock_ctx_ = nullptr;
    SocketCtx *rtp_ctx_ = nullptr;
    SocketCtx *rtcp_ctx_ = nullptr;
    SocketCtx *common_ = nullptr;
    SocketCtx *timer_ctx = nullptr;
    int sockfd_ = -1;
    int rtp_fd_ = -1;
    int rtcp_fd_ = -1;
    int timer_fd = -1;
    int seq_ = 1;
    RtspState state_ = RtspState::DISCONNECTED;
    size_t tcp_send_offset_ = 0;
    std::string server_ip_;
    int server_port_;
    std::string path_;
    std::string track_;
    std::string session_id_;
    std::string req_buf_;
    std::string resp_buf_;
    RtspRequest current_request_;
    std::queue<RtspRequest> request_queue_;
    std::deque<Packet> send_queue_;
    int server_rtp_port_ = 0;
    int server_rtcp_port_ = 0;
    std::string transport_;
    struct sockaddr_in server_rtp_addr_{};
    struct sockaddr_in server_rtcp_addr_{};
    size_t payload_offset;
    std::string nat_wan_ip;
    int nat_wan_port = 0;
    bool is_init_ok = false;
};
