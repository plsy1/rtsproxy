#include "core/proxy_server.h"

int main(int argc, char *argv[])
{
    ProxyServer server;
    return server.run(argc, argv);
}