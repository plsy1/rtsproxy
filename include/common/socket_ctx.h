#pragma once

#include <cstdint>
#include <functional>

struct SocketCtx
{
    int fd;
    std::function<void(uint32_t)> handler;
};