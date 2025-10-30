#pragma once

#include "../include/common/rtsp_ctx.h"

#include <string>
#include <queue>
#include <vector>
#include <map>
#include <netinet/in.h>
#include <functional>

class EpollLoop;
class BufferPool;
class SocketCtx;
class Packet;

class RTSPClient
{
public:
    explicit RTSPClient(EpollLoop *loop, BufferPool &pool, const sockaddr_in &client_addr, int client_fd, const std::string &rtsp_url);
    ~RTSPClient();

    using ClosedCallback = std::function<void()>;

    void set_on_closed_callback(ClosedCallback cb);

private:
    enum class RtspState
    {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        IDLE,
        WAIT_RESPONSE,
        STREAMING
    };

    enum class RtspMethod
    {
        OPTIONS,
        DESCRIBE,
        SETUP,
        PLAY,
        PAUSE,
        TEARDOWN,
        GET_PARAMETER,
        SET_PARAMETER
    };

    struct RtspRequest
    {
        RtspMethod method;
        std::string uri;
        std::string headers;
        std::string body;
        int cseq;
    };

private:
    void init_rtp_rtcp_sockets();
    void init_rtp_rtcp_server_addr();
    void init_timer_fd();
    void connect_server();
    void push_request_into_queue(RtspMethod method, const std::string &uri, const std::string &extra_headers = "", const std::string &body = "");
    void build_and_send_request();
    void send_rtp_trigger();
    void send_http_response();
    bool get_rtp_payload_offset(uint8_t *buf, size_t &recv_len, size_t &payload_offset);
    uint16_t get_random_rtp_port();
    bool bind_udp_socket(int &fd, uint16_t &port);

    void handle_rtsp(uint32_t event);
    void handle_rtp(uint32_t event);
    void handle_rtcp(uint32_t event);
    void handle_client(uint32_t event);
    void handle_timer(uint32_t event);

    void on_rtsp_writable();
    void on_rtsp_readable();
    void on_rtp_writable();
    void on_rtp_readable();
    void on_rtcp_writable();
    void on_rtcp_readable();
    void on_client_writable();
    void on_client_readable();
    void on_client_closed();

    void send_rtsp_option();
    void send_rtsp_describe();
    void send_rtsp_setup();
    void send_rtsp_play();
    
    std::string RtspMethodToString(RtspMethod method);

private:
    EpollLoop *loop;
    BufferPool &buffer_pool_;
    SocketCtx *rtsp_ctx_ = nullptr;
    SocketCtx *rtp_ctx_ = nullptr;
    SocketCtx *rtcp_ctx_ = nullptr;
    SocketCtx *client_ctx_ = nullptr;
    SocketCtx *timer_ctx = nullptr;
    ClosedCallback on_closed_callback_;

    sockaddr_in client_addr_;
    sockaddr_in server_rtp_addr_;
    sockaddr_in server_rtcp_addr_;

    uint8_t cseq_ = 1;
    uint16_t rtp_port_;
    uint16_t nat_wan_port = 0;
    bool is_init_ok = false;

    size_t tcp_send_offset_ = 0;
    size_t payload_offset = 0;

    int client_fd_ = -1;
    int rtsp_fd_ = -1;
    int rtp_fd_ = -1;
    int rtcp_fd_ = -1;
    int timer_fd = -1;

    RtspState state_ = RtspState::DISCONNECTED;

    std::string nat_wan_ip;
    std::string req_buf_;
    std::string resp_buf_;
    char rtsp_buf[2048];

    rtspCtx ctx;

    RtspRequest current_request_;
    std::queue<RtspRequest> request_queue_;
    std::deque<Packet> send_queue_;
};
