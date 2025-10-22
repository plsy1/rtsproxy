#pragma once

#include <arpa/inet.h>
#include <iostream>

struct RTSPClientInfo
{
    std::string rtsp_url;
    int client_fd;
    sockaddr_in client_addr;
};