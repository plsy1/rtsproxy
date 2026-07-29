// Unit tests for StunClient.
//
// The RTP socket that carries a STUN exchange is a media socket: it accepts
// datagrams from anywhere. Whatever comes back sets the public address the
// proxy then advertises upstream, so every response has to be tied to the
// request that was actually sent. These tests drive the real code over loopback
// -- a second UDP socket plays the STUN server -- so the transaction ID under
// test is the one the client genuinely put on the wire.
//
//   meson setup build -Dtests=true && meson test -C build stun_client

#include "test_harness.h"

#include "utils/stun_client.h"
#include "core/server_config.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t kMagicCookie = 0x2112A442;
constexpr uint16_t kBindingRequest = 0x0001;
constexpr uint16_t kBindingSuccess = 0x0101;
constexpr uint16_t kBindingError = 0x0111;
constexpr uint16_t kAttrMappedAddr = 0x0001;
constexpr uint16_t kAttrXorMappedAddr = 0x0020;

// A UDP socket bound to an ephemeral loopback port, used as the far end.
struct LoopbackSocket
{
    int fd = -1;
    uint16_t port = 0;

    LoopbackSocket()
    {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            return;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
        {
            close(fd);
            fd = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (getsockname(fd, (sockaddr *)&bound, &len) == 0)
            port = ntohs(bound.sin_port);
    }

    ~LoopbackSocket()
    {
        if (fd >= 0)
            close(fd);
    }

    LoopbackSocket(const LoopbackSocket &) = delete;
    LoopbackSocket &operator=(const LoopbackSocket &) = delete;
};

void put16(unsigned char *p, uint16_t v) { uint16_t n = htons(v); std::memcpy(p, &n, 2); }
void put32(unsigned char *p, uint32_t v) { uint32_t n = htonl(v); std::memcpy(p, &n, 4); }

using TransactionId = std::vector<unsigned char>;

// Read the request the client just sent and hand back its transaction ID.
// Returns an empty vector if nothing arrived or the request is malformed.
TransactionId receive_request(const LoopbackSocket &server)
{
    unsigned char req[64];
    sockaddr_in from{};
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(server.fd, req, sizeof(req), MSG_DONTWAIT,
                         (sockaddr *)&from, &flen);
    if (n != 20)
        return {};

    uint16_t type = 0;
    uint32_t cookie = 0;
    std::memcpy(&type, req + 0, 2);
    std::memcpy(&cookie, req + 4, 4);
    if (ntohs(type) != kBindingRequest || ntohl(cookie) != kMagicCookie)
        return {};

    return TransactionId(req + 8, req + 20);
}

// Build a STUN message carrying one address attribute.
std::vector<unsigned char> make_response(uint16_t msg_type, uint32_t cookie,
                                         const TransactionId &tid,
                                         uint16_t attr_type,
                                         const char *ip, uint16_t port)
{
    uint32_t addr = ntohl(inet_addr(ip));
    uint16_t wire_port = port;
    uint32_t wire_addr = addr;
    if (attr_type == kAttrXorMappedAddr)
    {
        wire_port = port ^ static_cast<uint16_t>(kMagicCookie >> 16);
        wire_addr = addr ^ kMagicCookie;
    }

    std::vector<unsigned char> msg(20 + 4 + 8, 0);
    put16(msg.data() + 0, msg_type);
    put16(msg.data() + 2, 12); // attribute section length
    put32(msg.data() + 4, cookie);
    std::memcpy(msg.data() + 8, tid.data(), tid.size());

    put16(msg.data() + 20, attr_type);
    put16(msg.data() + 22, 8);
    msg[24] = 0x00;            // reserved
    msg[25] = 0x01;            // family: IPv4
    put16(msg.data() + 26, wire_port);
    put32(msg.data() + 28, wire_addr);
    return msg;
}

// Send a fresh request and return the transaction ID the server observed.
TransactionId issue_request(int client_fd, const LoopbackSocket &server)
{
    if (StunClient::send_stun_mapping_request(client_fd) != 0)
        return {};
    return receive_request(server);
}

int extract(int fd, std::vector<unsigned char> &msg, std::string &ip, uint16_t &port)
{
    ip.clear();
    port = 0;
    return StunClient::extract_stun_mapping_from_response(fd, msg.data(), msg.size(), ip, port);
}

/* ------------------------------------------------------------------ */

void test_happy_path(const LoopbackSocket &server)
{
    SUITE("extract: a well-formed XOR-MAPPED-ADDRESS response is accepted");

    LoopbackSocket client;
    CHECK(client.fd >= 0);

    TransactionId tid = issue_request(client.fd, server);
    CHECK_EQ((int)tid.size(), 12);

    auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                             kAttrXorMappedAddr, "203.0.113.7", 41000);
    std::string ip;
    uint16_t port = 0;
    CHECK_EQ(extract(client.fd, msg, ip, port), 0);
    CHECK_EQ(ip, std::string("203.0.113.7"));
    CHECK_EQ(port, 41000);
}

void test_plain_mapped_address(const LoopbackSocket &server)
{
    SUITE("extract: the legacy MAPPED-ADDRESS attribute also works");

    LoopbackSocket client;
    TransactionId tid = issue_request(client.fd, server);
    CHECK_EQ((int)tid.size(), 12);

    auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                             kAttrMappedAddr, "198.51.100.9", 5060);
    std::string ip;
    uint16_t port = 0;
    CHECK_EQ(extract(client.fd, msg, ip, port), 0);
    CHECK_EQ(ip, std::string("198.51.100.9"));
    CHECK_EQ(port, 5060);
}

void test_wrong_transaction_id(const LoopbackSocket &server)
{
    SUITE("extract: a response with someone else's transaction ID is rejected");

    LoopbackSocket client;
    TransactionId tid = issue_request(client.fd, server);
    CHECK_EQ((int)tid.size(), 12);

    // An off-path attacker who sprays the media port knows the magic cookie --
    // it is a constant -- but not the 96-bit transaction ID.
    TransactionId forged(12, 0xAB);
    CHECK(forged != tid);

    auto msg = make_response(kBindingSuccess, kMagicCookie, forged,
                             kAttrXorMappedAddr, "6.6.6.6", 1234);
    std::string ip;
    uint16_t port = 0;
    CHECK_EQ(extract(client.fd, msg, ip, port), -1);
    CHECK_EQ(ip, std::string(""));
    CHECK_EQ(port, 0);
}

void test_wrong_message_type(const LoopbackSocket &server)
{
    SUITE("extract: only a Binding Success Response carries a mapping");

    {
        LoopbackSocket client;
        TransactionId tid = issue_request(client.fd, server);
        auto msg = make_response(kBindingError, kMagicCookie, tid,
                                 kAttrXorMappedAddr, "6.6.6.6", 1234);
        std::string ip;
        uint16_t port = 0;
        CHECK_EQ(extract(client.fd, msg, ip, port), -1);
        CHECK_EQ(port, 0);
    }

    {
        // A reflected copy of our own request must not be read as an answer.
        LoopbackSocket client;
        TransactionId tid = issue_request(client.fd, server);
        auto msg = make_response(kBindingRequest, kMagicCookie, tid,
                                 kAttrXorMappedAddr, "6.6.6.6", 1234);
        std::string ip;
        uint16_t port = 0;
        CHECK_EQ(extract(client.fd, msg, ip, port), -1);
        CHECK_EQ(port, 0);
    }
}

void test_wrong_cookie(const LoopbackSocket &server)
{
    SUITE("extract: the magic cookie is still required");

    LoopbackSocket client;
    TransactionId tid = issue_request(client.fd, server);
    auto msg = make_response(kBindingSuccess, 0xDEADBEEF, tid,
                             kAttrXorMappedAddr, "6.6.6.6", 1234);
    std::string ip;
    uint16_t port = 0;
    CHECK_EQ(extract(client.fd, msg, ip, port), -1);
    CHECK_EQ(port, 0);
}

void test_no_request_in_flight(const LoopbackSocket &server)
{
    SUITE("extract: a socket that never asked accepts nothing");

    LoopbackSocket other;
    TransactionId tid = issue_request(other.fd, server);
    CHECK_EQ((int)tid.size(), 12);

    // Perfectly valid message, but addressed at a socket with no outstanding
    // request -- which is what an unsolicited datagram looks like.
    LoopbackSocket quiet;
    auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                             kAttrXorMappedAddr, "6.6.6.6", 1234);
    std::string ip;
    uint16_t port = 0;
    CHECK_EQ(extract(quiet.fd, msg, ip, port), -1);
    CHECK_EQ(port, 0);
}

void test_response_is_single_use(const LoopbackSocket &server)
{
    SUITE("extract: one request accepts exactly one answer");

    LoopbackSocket client;
    TransactionId tid = issue_request(client.fd, server);
    auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                             kAttrXorMappedAddr, "203.0.113.7", 41000);

    std::string ip;
    uint16_t port = 0;
    CHECK_EQ(extract(client.fd, msg, ip, port), 0);
    CHECK_EQ(port, 41000);

    // Replaying it must not work: the exchange is over.
    auto replay = make_response(kBindingSuccess, kMagicCookie, tid,
                                kAttrXorMappedAddr, "6.6.6.6", 1234);
    CHECK_EQ(extract(client.fd, replay, ip, port), -1);
    CHECK_EQ(port, 0);
}

void test_truncated_and_lying_lengths(const LoopbackSocket &server)
{
    SUITE("extract: malformed messages are rejected without reading past the end");

    {
        LoopbackSocket client;
        TransactionId tid = issue_request(client.fd, server);
        auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                                 kAttrXorMappedAddr, "203.0.113.7", 41000);
        msg.resize(19); // shorter than a STUN header
        std::string ip;
        uint16_t port = 0;
        CHECK_EQ(extract(client.fd, msg, ip, port), -1);
    }

    {
        // Attribute claims 4 KiB of value inside a 32-byte datagram.
        LoopbackSocket client;
        TransactionId tid = issue_request(client.fd, server);
        auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                                 kAttrXorMappedAddr, "203.0.113.7", 41000);
        put16(msg.data() + 2, 4096);
        put16(msg.data() + 22, 4092);
        std::string ip;
        uint16_t port = 0;
        CHECK_EQ(extract(client.fd, msg, ip, port), -1);
        CHECK_EQ(port, 0);
    }

    {
        // Header present, no attributes at all.
        LoopbackSocket client;
        TransactionId tid = issue_request(client.fd, server);
        auto msg = make_response(kBindingSuccess, kMagicCookie, tid,
                                 kAttrXorMappedAddr, "203.0.113.7", 41000);
        msg.resize(20);
        put16(msg.data() + 2, 0);
        std::string ip;
        uint16_t port = 0;
        CHECK_EQ(extract(client.fd, msg, ip, port), -1);
    }
}

void test_transaction_ids_differ(const LoopbackSocket &server)
{
    SUITE("gen_tid: back-to-back requests use different transaction IDs");

    // The old implementation re-seeded srand() from time(nullptr) on every
    // call, so two requests issued within the same second drew an identical
    // transaction ID -- which would make the check above worthless.
    LoopbackSocket a, b, c;
    TransactionId ta = issue_request(a.fd, server);
    TransactionId tb = issue_request(b.fd, server);
    TransactionId tc = issue_request(c.fd, server);

    CHECK_EQ((int)ta.size(), 12);
    CHECK_EQ((int)tb.size(), 12);
    CHECK_EQ((int)tc.size(), 12);
    CHECK(ta != tb);
    CHECK(tb != tc);
    CHECK(ta != tc);
}

} // namespace

int main()
{
    LoopbackSocket server;
    if (server.fd < 0 || server.port == 0)
    {
        std::printf("FATAL: could not bind a loopback UDP socket\n");
        return 1;
    }

    // Point the client at our stand-in server. sendto() to loopback needs no
    // listener, but binding one lets the test read back the real request.
    ServerConfig::setStunHost("127.0.0.1");
    ServerConfig::setStunPort(server.port);

    test_happy_path(server);
    test_plain_mapped_address(server);
    test_wrong_transaction_id(server);
    test_wrong_message_type(server);
    test_wrong_cookie(server);
    test_no_request_in_flight(server);
    test_response_is_single_use(server);
    test_truncated_and_lying_lengths(server);
    test_transaction_ids_differ(server);

    return tst::summary();
}
