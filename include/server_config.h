#pragma once

#include <string>

class ServerConfig
{
public:
    static void setPort(int p);
    static void enableNat();
    static void setBufferPoolCount(int count);
    static void setBufferPoolBlockSize(int size);
    static void setStunPort(int port);
    static void setStunHost(std::string host);
    static void setNatMethod(const std::string &method);
    static void setJsonPath(std::string path);
    static void setToken(std::string token);
    static void setHttpUpstreamInterface(std::string iface);
    static void setMitmUpstreamInterface(std::string iface);
    static void setListenInterface(std::string iface);
    static void setLogFile(std::string path);
    static void setLogLines(size_t lines);

    static bool isNatEnabled();
    static std::string getNatMethod();
    static int getPort();
    static int getBufferPoolCount();
    static int getBufferPoolBlockSize();
    static int getStunPort();
    static std::string getStunHost();
    static std::string getJsonPath();
    static std::string getToken();
    static std::string getHttpUpstreamInterface();
    static std::string getMitmUpstreamInterface();
    static std::string getListenInterface();
    static std::string getLogFile();
    static size_t getLogLines();
    static void printUsage(const std::string &program_name);

    static void kill_previous_instance();

private:
    static int port;
    static bool enable_nat;
    static std::string nat_method;
    static int buffer_pool_count;
    static int buffer_pool_block_size;
    static int stun_server_port;
    static std::string stun_server_host;
    static std::string json_path;
    static std::string auth_token;
    static std::string http_upstream_interface;
    static std::string mitm_upstream_interface;
    static std::string listen_interface;
    static std::string log_file_path;
    static size_t log_file_lines;
};