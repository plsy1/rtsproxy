#pragma once

#include <string>

class ServerConfig
{
public:
    static void setPort(int p);
    static void enableNat();
    static void setRtpBufferSize(int size);
    static void setUdpPacketSize(int size);
    static void setStunPort(int port);
    static void setStunHost(std::string host);
    static void setJsonPath(std::string path);
    static int getPort();
    static bool isNatEnabled();
    static int getRtpBufferSize();
    static int getUdpPacketSize();
    static int getStunPort();
    static std::string getStunHost();
    static std::string getJsonPath();
    static void printUsage(const std::string &program_name);

private:
    static int port;
    static bool enable_nat;
    static int rtp_buffer_size;
    static int udp_packet_size;
    static int stun_server_port;
    static std::string stun_server_host;
    static std::string json_path;
};