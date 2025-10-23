#pragma once

#include <string>
#include <arpa/inet.h>

struct RTSPClientInfo
{
    std::string rtsp_url;
    int client_fd;
    sockaddr_in client_addr;
};