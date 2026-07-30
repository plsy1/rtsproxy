#include "handlers/rtsp_to_rtsp_handle.h"
#include "clients/rtsp_to_rtsp_client.h"
#include "core/logger.h"
#include "protocol/rtsp_parser.h"
#include "utils/blacklist_checker.h"
#include <arpa/inet.h>

bool RtspToRtspHandle::dispatch(int client_fd, const sockaddr_in &client_addr, const RequestInfo &info, const std::string &raw_request, EpollLoop *loop, BufferPool &pool)
{
    if (info.is_http || info.upstream_url.empty()) {
        return false;
    }

    std::string client_host = std::string(inet_ntoa(client_addr.sin_addr)) + ":" + std::to_string(ntohs(client_addr.sin_port));
    
    try {
        // Resolve once, then audit the exact context the client will connect with:
        // a second parse would re-resolve DNS and could yield a different address.
        auto config = RTSPToRtspClient::resolve_upstream(raw_request);

        // Security Audit
        if (BlacklistChecker::is_blacklisted(config.ctx.server_ip)) {
            throw std::runtime_error("Upstream " + config.ctx.server_ip + " is blacklisted.");
        }
        if (BlacklistChecker::is_loopback(config.ctx.server_ip, config.ctx.server_rtsp_port, client_fd)) {
            throw std::runtime_error("Recursive connection detected.");
        }

        Logger::debug("[RTSP2RTSP] Dispatching session: " + client_host + " -> " + config.ctx.rtsp_url);

        auto client = std::make_unique<RTSPToRtspClient>(loop, pool, client_addr, client_fd, config, raw_request);
        loop->add_client_to_map(client_fd, std::move(client));

        auto client_ptr = loop->get_client_from_map(client_fd);
        if (client_ptr) {
            client_ptr->set_on_closed_callback([client_fd, loop, client_host]() {
                Logger::debug("[RTSP2RTSP] Client disconnected: " + client_host);
                loop->add_task([client_fd, loop]() {
                    loop->remove_client_from_map(client_fd);
                });
            });
        }
        return true;

    } catch (const std::exception &e) {
        Logger::error("[RTSP2RTSP] Initialization failed: " + std::string(e.what()));
        return false;
    }
}
