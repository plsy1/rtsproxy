#pragma once

#include <netinet/in.h>
#include <cstdint>
#include <string>

class EpollLoop;
class BufferPool;

class MasterHandle
{
public:
    /**
     * Entry point for every new connection.
     * Identifies the protocol and dispatches to HTTPHandle or RTSPHandle.
     */
    static void handle(int client_fd, sockaddr_in client_addr, EpollLoop *loop,
                       BufferPool &pool, std::string &request_buffer,
                       uint32_t events);
};
