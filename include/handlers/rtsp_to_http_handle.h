#pragma once

#include "protocol/request_parser.h"
#include "core/epoll_loop.h"
#include "core/buffer_pool.h"
#include <netinet/in.h>

class RtspToHttpHandle
{
public:
    /**
     * Handles RTSP-over-HTTP streaming requests.
     * @return true if handled, false otherwise.
     */
    static bool dispatch(int client_fd, const sockaddr_in &client_addr, const RequestInfo &info, EpollLoop *loop, BufferPool &pool);
};
