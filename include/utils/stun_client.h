#pragma once

#include <string>
#include <cstdint>

class StunClient
{
public:
    static int send_stun_mapping_request(int s);
    static int extract_stun_mapping_from_response(unsigned char *rsp, size_t rsp_len, std::string &out_pub_ip, uint16_t &out_pub_port);
private:
    static void gen_tid(unsigned char tid[12]);
    static void put16(unsigned char *p, uint16_t v);
    static void put32(unsigned char *p, uint32_t v);
};
