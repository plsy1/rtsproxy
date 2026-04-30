#pragma once

#include <functional>
#include "3rd/json.hpp"

using json = nlohmann::json;

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
    virtual json get_info() const = 0;
    virtual bool is_closed() const = 0;
};
