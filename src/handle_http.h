#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

class RTSPClient;
class EpollLoop;
class BufferPool;

void handle_http_request(int client_fd, sockaddr_in client_addr, EpollLoop *loop, BufferPool &pool);
