#pragma once

#include <netinet/in.h>
#include <string>

class EpollLoop;
class BufferPool;

/**
 * Called when the epoll loop fires on a newly-accepted RTSP client fd.
 * Reads the first RTSP request line, then hands the connection to
 * RTSPMitmClient.
 */
void handle_rtsp_request(int client_fd, sockaddr_in client_addr,
                         EpollLoop *loop, BufferPool &pool);
