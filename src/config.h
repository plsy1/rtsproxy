#pragma once

#include <string>
#include <iostream>

class ServerConfig
{
public:
    static void setPort(int p);
    static void enableNat();
    static void setRtpBufferSize(int size);
    static void setUdpPacketSize(int size);

    static int getPort();
    static bool isNatEnabled();
    static int getRtpBufferSize();
    static int getUdpPacketSize();

    static void printUsage(const std::string &program_name);
    
private:
    static int port;
    static bool enable_nat;
    static int rtp_buffer_size;
    static int udp_packet_size;
};