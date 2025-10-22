#ifndef SOCKETCTX_H
#define SOCKETCTX_H

#include <functional> 

struct SocketCtx
{
    int fd;
    std::function<void(uint32_t)> handler;
};

#endif