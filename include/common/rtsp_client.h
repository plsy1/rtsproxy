#pragma once

#include <string>
#include <netinet/in.h>

struct RTSPClientCtx
{
    std::string rtsp_url;
    int client_fd;
    sockaddr_in client_addr;
};