#pragma once

#include "core/iclient.h"
#include "protocol/rtsp_parser.h"
#include "common/rtsp_ctx.h"
#include "protocol/rtp_pipeline.h"
#include "core/buffer_pool.h"
#include <string>
#include <memory>
#include <queue>
#include <deque>
#include <chrono>
#include <netinet/in.h>

class EpollLoop;
class SocketCtx;

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

class RTSPToHttpClient : public IClient
{
public:
    RTSPToHttpClient(EpollLoop *loop, BufferPool &pool, const sockaddr_in &client_addr, int client_fd, const rtspCtx &ctx);
    ~RTSPToHttpClient() override;

    void set_on_closed_callback(ClosedCallback cb) override;
    json get_info() const override;
    bool is_closed() const override { return is_closed_; }

private:
    enum class RtspState
    {
        INIT,
        CONNECTING,
        CONNECTED,
        STREAMING
    };

    struct RtspRequest
    {
        RtspMethod method;
        std::string uri;
        std::string headers;
        std::string body;
        int cseq;
    };

    class FdGuard
    {
    public:
        FdGuard();
        FdGuard(int fd, EpollLoop *loop = nullptr);
        ~FdGuard();
        FdGuard(const FdGuard &) = delete;
        FdGuard &operator=(const FdGuard &) = delete;
        FdGuard(FdGuard &&other) noexcept;
        FdGuard &operator=(FdGuard &&other) noexcept;
        int &get_ref();
        int get() const;
        operator int() const;

    private:
        int fd_{-1};
        EpollLoop *loop_{nullptr};
    };

private:
    void connect_server();
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

    void push_request_into_queue(RtspMethod method, const std::string &uri, const std::string &extra_headers = "", const std::string &body = "");
    void build_and_send_request();
    void init_rtp_rtcp_sockets();
    void init_rtp_rtcp_server_addr();
    void send_rtp_trigger();
    void send_zte_heartbeat();
    void init_timer_fd();
    void send_http_response();
    void send_rtsp_option();
    void send_rtsp_describe();
    void send_rtsp_setup(const std::string &sdp_data = "");
    void send_rtsp_play();
    void handle_interleaved_packet(uint8_t channel, const uint8_t *data, size_t len);

    static std::string RtspMethodToString(RtspMethod method);

private:
    EpollLoop *loop_;
    BufferPool &buffer_pool_;
    ClosedCallback on_closed_callback_;
    std::chrono::steady_clock::time_point start_time_;
    sockaddr_in client_addr_;
    FdGuard client_fd_;
    rtspCtx ctx;
    std::unique_ptr<RtpPipeline> rtp_pipeline_;

    std::unique_ptr<SocketCtx> client_ctx_;
    std::unique_ptr<SocketCtx> rtsp_ctx_;
    std::unique_ptr<SocketCtx> rtp_ctx_;
    std::unique_ptr<SocketCtx> rtcp_ctx_;
    std::unique_ptr<SocketCtx> timer_ctx_;

    FdGuard rtsp_fd_;
    FdGuard rtp_fd_;
    FdGuard rtcp_fd_;
    FdGuard timer_fd_;

    RtspState state_{RtspState::INIT};
    int cseq_{1};
    std::string req_buf_;
    std::string resp_buf_;
    size_t tcp_send_offset_{0};
    char rtsp_buf[4096];

    std::queue<RtspRequest> request_queue_;
    RtspRequest current_request_;

    uint16_t rtp_port_{0};
    sockaddr_in server_rtp_addr_{};
    sockaddr_in server_rtcp_addr_{};

    bool is_closed_{false};
    bool is_init_ok{false};
    bool is_tcp_mode_{false};
    uint8_t interleaved_rtp_channel_{0};
    uint8_t interleaved_rtcp_channel_{1};
    bool setup_retry_with_tcp_{false};

    std::string local_ip_;
    uint16_t local_tcp_port_{0};
    std::string nat_wan_ip;
    uint16_t nat_wan_port{0};

    std::deque<Packet> send_queue_;
    mutable BandwidthEstimator upstream_est_;
    mutable BandwidthEstimator downstream_est_;
};
