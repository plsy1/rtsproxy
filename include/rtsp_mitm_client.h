#pragma once

#include "../include/common/rtsp_ctx.h"
#include "../include/iclient.h"
#include <string>
#include <queue>
#include <deque>
#include <vector>
#include <map>
#include <netinet/in.h>
#include <unistd.h>
#include <functional>
#include <memory>

class EpollLoop;
class BufferPool;
class SocketCtx;

/**
 * RTSPMitmClient — RTSP man-in-the-middle proxy.
 *
 * Downstream (client) speaks real RTSP to us.
 * We forward every request to the real upstream server, patch the
 * Transport header so the RTP/RTCP ports point back to us, relay the
 * upstream responses back to the client, and forward RTP/RTCP packets
 * in both directions.
 */
class RTSPMitmClient : public IClient
{
public:
    explicit RTSPMitmClient(EpollLoop *loop, BufferPool &pool,
                            const sockaddr_in &client_addr, int client_fd,
                            const std::string &first_request);
    ~RTSPMitmClient() override;

    void set_on_closed_callback(ClosedCallback cb) override;

private:
    /* ------------------------------------------------------------------ */
    /* State machine                                                        */
    /* ------------------------------------------------------------------ */
    enum class State
    {
        WAIT_UPSTREAM_CONNECT, // TCP connect to upstream in progress
        IDLE,                  // Connected, waiting for next client request
        WAIT_UPSTREAM_RESP,    // Forwarded a request, waiting for response
        STREAMING,             // PLAY done, forwarding RTP
    };

    /* ------------------------------------------------------------------ */
    /* RAII fd wrapper (identical pattern to RTSPClient)                   */
    /* ------------------------------------------------------------------ */
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
    /* ------------------------------------------------------------------ */
    /* Internal helpers                                                     */
    /* ------------------------------------------------------------------ */
    void connect_upstream();

    // Patch the SETUP request's Transport header: replace client_port with
    // our local RTP/RTCP port pair.
    std::string patch_transport_for_upstream(const std::string &req);
    // Patch the SETUP response's Transport header: replace server_port and
    // client_port fields seen by the client.
    std::string patch_transport_for_client(const std::string &resp);

    // Extract client_port from Transport header in a SETUP request.
    bool extract_client_port(const std::string &req,
                             uint16_t &rtp_port, uint16_t &rtcp_port);

    // Allocate local UDP sockets for RTP and RTCP relay.
    bool init_relay_sockets();

    void init_timer_fd();

    // Rewrite the RTSP request URI from the proxy-format URL
    // (rtsp://proxy-host:port/real-host:port/path) to the upstream URL
    // (rtsp://real-host:port/path) before forwarding to the server.
    std::string rewrite_request_for_upstream(const std::string &req);

    // Patch any response from upstream before sending to client.
    // Handles Transport, Content-Base, RTP-Info and SDP.
    std::string patch_response_for_client(const std::string &resp);

    // Send accumulated data from a queue over a TCP fd.
    // Returns false on unrecoverable error.
    bool drain_tcp_queue(std::deque<std::string> &q, FdGuard &fd,
                         std::unique_ptr<SocketCtx> &ctx, uint32_t wait_events);

    /* epoll handlers */
    void handle_downstream(uint32_t events);
    void handle_upstream(uint32_t events);
    void handle_rtp_from_upstream(uint32_t events);
    void handle_rtcp_from_upstream(uint32_t events);
    void handle_rtp_from_client(uint32_t events);
    void handle_rtcp_from_client(uint32_t events);
    void handle_timer(uint32_t events);

    void on_downstream_readable();
    void on_downstream_writable();
    void on_downstream_closed();
    void on_upstream_readable();
    void on_upstream_writable();

    void close_all();

private:
    EpollLoop *loop_;
    BufferPool &pool_;
    ClosedCallback on_closed_;

    /* downstream (the RTSP client that connected to us) */
    FdGuard downstream_fd_;
    std::unique_ptr<SocketCtx> downstream_ctx_;
    sockaddr_in client_addr_;

    /* upstream (the real RTSP server) */
    FdGuard upstream_fd_;
    std::unique_ptr<SocketCtx> upstream_ctx_;

    /* RTP relay sockets (our side, between upstream and downstream) */
    FdGuard rtp_us_fd_;   // receives RTP from upstream server
    FdGuard rtcp_us_fd_;  // receives RTCP from upstream server
    FdGuard rtp_ds_fd_;   // receives RTCP/RTP from downstream client (for RTCP SR)
    FdGuard rtcp_ds_fd_;

    std::unique_ptr<SocketCtx> rtp_us_ctx_;
    std::unique_ptr<SocketCtx> rtcp_us_ctx_;
    std::unique_ptr<SocketCtx> rtp_ds_ctx_;
    std::unique_ptr<SocketCtx> rtcp_ds_ctx_;
    std::unique_ptr<SocketCtx> timer_ctx_;
    FdGuard timer_fd_;

    /* Local port numbers for the relay sockets */
    uint16_t local_rtp_port_{0};   // our RTP relay port (facing upstream)
    uint16_t local_rtcp_port_{0};  // our RTCP relay port (facing upstream)

    /* The actual client RTP/RTCP endpoints (where we forward RTP to) */
    sockaddr_in client_rtp_addr_{};
    sockaddr_in client_rtcp_addr_{};

    /* The upstream server RTP/RTCP endpoints */
    sockaddr_in server_rtp_addr_{};
    sockaddr_in server_rtcp_addr_{};

    State state_{State::WAIT_UPSTREAM_CONNECT};

    rtspCtx ctx_; // parsed URL info for upstream

    // Per-request buffers
    std::string downstream_recv_buf_; // accumulate full RTSP request from client
    std::string upstream_recv_buf_;   // accumulate full RTSP response from server

    // Send queues (raw RTSP text)
    std::deque<std::string> to_upstream_q_;
    std::deque<std::string> to_downstream_q_;

    // TCP send progress for simple head-of-queue item
    size_t upstream_send_offset_{0};
    size_t downstream_send_offset_{0};

    // Whether the relay sockets have been set up (after SETUP)
    bool relay_ready_{false};
    bool closed_{false};

    // Track whether we forwarded a PLAY so we can correctly detect the response
    bool pending_play_{false};

    // URI rewriting: when the client uses the proxy-path format
    // rtsp://proxy:port/real-host:port/path, we store the prefix to
    // replace so every forwarded request uses the real upstream URI.
    std::string proxy_uri_prefix_;   // e.g. "rtsp://10.1.0.6:8555/112.245.125.44:1554"
    std::string upstream_uri_base_;  // e.g. "rtsp://112.245.125.44:1554"

    // Removed rtp_buf_ in favor of BufferPool for consistency with RTSPClient
};
