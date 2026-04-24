#pragma once

#include <functional>

/**
 * Common interface for all proxy client types (RTSPClient, RTSPMitmClient, …).
 * EpollLoop stores IClient pointers, avoiding a dependency on concrete types.
 */
class IClient
{
public:
    using ClosedCallback = std::function<void()>;

    virtual ~IClient() = default;
    virtual void set_on_closed_callback(ClosedCallback cb) = 0;
};
