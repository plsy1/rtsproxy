#pragma once

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>

struct SocketCtx
{
    int fd;
    std::function<void(uint32_t)> handler;

    SocketCtx() : fd(-1), handler(nullptr)
    {
    }

    SocketCtx(int fd, std::function<void(uint32_t)> handler)
        : fd(fd), handler(std::move(handler))
    {
    }

    ~SocketCtx()
    {
    }
};