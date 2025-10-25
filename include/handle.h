#pragma once


#include <netinet/in.h>

class RTSPClient;
class EpollLoop;
class BufferPool;

void handle_http_request(int client_fd, sockaddr_in client_addr, EpollLoop *loop, BufferPool &pool);
