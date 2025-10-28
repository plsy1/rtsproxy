#include "../include/server_config.h"
#include "../include/logger.h"
#include <iostream>
#include <signal.h>
#include <dirent.h>
#include <fstream>
#include <unistd.h>

int ServerConfig::port = 8554;
bool ServerConfig::enable_nat = false;
int ServerConfig::rtp_buffer_size = 4096;
int ServerConfig::udp_packet_size = 1500;
int ServerConfig::stun_server_port = 19302;
std::string ServerConfig::stun_server_host = "stun.l.google.com";
std::string ServerConfig::json_path = "config.json";
std::string ServerConfig::auth_token = "";

void ServerConfig::setPort(int p)
{
    port = p;
}

void ServerConfig::enableNat()
{
    enable_nat = true;
    Logger::info("[CONFIG] NAT traversal enabled");
}

void ServerConfig::setRtpBufferSize(int size)
{
    rtp_buffer_size = size;
}

void ServerConfig::setUdpPacketSize(int size)
{
    udp_packet_size = size;
}

void ServerConfig::setStunPort(int port)
{
    stun_server_port = port;
}

void ServerConfig::setStunHost(std::string host)
{
    stun_server_host = host;
}

void ServerConfig::setJsonPath(std::string path)
{
    json_path = path;
}

void ServerConfig::setToken(std::string token)
{
    auth_token = token;
}

int ServerConfig::getPort()
{
    return port;
}

bool ServerConfig::isNatEnabled()
{
    return enable_nat;
}

int ServerConfig::getRtpBufferSize()
{
    return rtp_buffer_size;
}

int ServerConfig::getUdpPacketSize()
{
    return udp_packet_size;
}

int ServerConfig::getStunPort()
{
    return stun_server_port;
}

std::string ServerConfig::getStunHost()
{
    return stun_server_host;
}

std::string ServerConfig::getJsonPath()
{
    return json_path;
}

std::string ServerConfig::getToken()
{
    return auth_token;
}

void ServerConfig::printUsage(const std::string &program_name)
{
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -p, --port            <port>  Set server port (default: " << port << ")" << std::endl;
    std::cout << "  -n, --enable-nat              Enable NAT (default: " << (enable_nat ? "enabled" : "disabled") << ")" << std::endl;
    std::cout << "  -r, --rtp-buffer-size <size>  Set RTP buffer size (default: " << rtp_buffer_size << ")" << std::endl;
    std::cout << "  -u, --udp-packet-size <size>  Set UDP packet size (default: " << udp_packet_size << ")" << std::endl;
    std::cout << "  -t, --set-auth-token  <token> Set auth token (default: " << "no auth required" << ")" << std::endl;
    std::cout << "  -j, --set-json-path   <path>  Set JSON file path (default: " << json_path << ")" << std::endl;
    std::cout << "  -d, --daemon                  Run rtsproxy in the background" << std::endl;
    std::cout << "  -k, --kill                    Kill the running rtsproxy instance" << std::endl;
    std::cout << "      --set-stun-host,  <port>  Set STUN server host (default: " << stun_server_host << ")" << std::endl;
    std::cout << "      --set-stun-port,  <port>  Set STUN server port (default: " << stun_server_port << ")" << std::endl;
}

void ServerConfig::kill_previous_instance()
{
    pid_t current_pid = getpid();

    DIR *dir = opendir("/proc");
    if (!dir)
    {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type == DT_DIR && isdigit(entry->d_name[0]))
        {
            pid_t pid = atoi(entry->d_name);
            if (pid == current_pid)
                continue;

            std::string cmdline_path = "/proc/" + std::to_string(pid) + "/cmdline";
            std::ifstream cmdline_file(cmdline_path);
            if (cmdline_file)
            {
                std::string cmdline;
                std::getline(cmdline_file, cmdline);

                if (cmdline.find("rtsproxy") != std::string::npos)
                {
                    kill(pid, SIGTERM);
                    break;
                }
            }
        }
    }
    closedir(dir);
}
