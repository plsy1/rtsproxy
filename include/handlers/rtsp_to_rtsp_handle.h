#pragma once

#include "protocol/request_parser.h"
#include "core/epoll_loop.h"
#include "core/buffer_pool.h"
#include <netinet/in.h>
#include <string>

class RtspToRtspHandle
{
public:
    /**
     * Handles RTSP-to-RTSP (MITM) proxy requests.
     * @return true if handled, false otherwise.
     */
    static bool dispatch(int client_fd, const sockaddr_in &client_addr, const RequestInfo &info, const std::string &raw_request, EpollLoop *loop, BufferPool &pool);
};
