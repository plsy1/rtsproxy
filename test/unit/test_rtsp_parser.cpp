// Unit tests for rtspParser (src/protocol/rtsp_parser.cpp).
//
// Scope: the pure parsing helpers -- status line, header extraction,
// Content-Length, Session, Transport server_port/interleaved, RTSP URL split
// and the SDP body parser.
//
// NOTE ON HOSTNAMES: rtspParser::parse_url falls back to DNSResolver for any
// host that is not a numeric IPv4 literal. Every URL below therefore uses a
// dotted-quad from the TEST-NET-1 documentation range (192.0.2.0/24) so the
// tests never touch the network and never block on a resolver.

#include "test_harness.h"

#include "common/rtsp_ctx.h"
#include "core/logger.h"
#include "protocol/rtsp_parser.h"

#include <map>
#include <string>

namespace
{

// The parser logs to stdout on some error paths; push the threshold above
// every defined level so the harness output stays readable.
void silence_logger()
{
    Logger::setLogLevel(static_cast<LogLevel>(100));
}

std::string attr_of(const Media &m, const std::string &key)
{
    auto it = m.attributes.find(key);
    return it == m.attributes.end() ? std::string("<missing>") : it->second;
}

int bw_of(const std::map<std::string, int> &m, const std::string &key)
{
    auto it = m.find(key);
    return it == m.end() ? -1 : it->second;
}

size_t sz(size_t v) { return v; }

// ---------------------------------------------------------------- status line

void test_parse_status_code()
{
    SUITE("parse_status_code");

    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0 200 OK\r\n"), 200);
    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0 401 Unauthorized\r\n"), 401);
    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0 302 Found\r\nLocation: x\r\n\r\n"), 302);
    // extra whitespace between version and code is tolerated by scanf
    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0   503 Service Unavailable"), 503);
    // no reason phrase
    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0 454"), 454);

    // malformed / non-RTSP inputs must yield the -1 sentinel, never a stale or
    // uninitialised code.
    CHECK_EQ(rtspParser::parse_status_code(""), -1);
    CHECK_EQ(rtspParser::parse_status_code("HTTP/1.1 200 OK\r\n"), -1);
    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0"), -1);
    CHECK_EQ(rtspParser::parse_status_code("RTSP/1.0 OK 200"), -1);
    CHECK_EQ(rtspParser::parse_status_code("OPTIONS rtsp://192.0.2.10/ RTSP/1.0"), -1);
    // status line is case sensitive per RFC 2326 -- lowercase is not a response
    CHECK_EQ(rtspParser::parse_status_code("rtsp/1.0 200 OK"), -1);
}

// ------------------------------------------------------------ header lookup

void test_extract_header_value()
{
    SUITE("extract_header_value");

    const std::string resp =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 3\r\n"
        "Content-Type: application/sdp\r\n"
        "Session: 12345678;timeout=60\r\n"
        "Content-Length: 42\r\n"
        "\r\n";

    CHECK_EQ(rtspParser::extract_header_value(resp, "CSeq"), std::string("3"));
    CHECK_EQ(rtspParser::extract_header_value(resp, "Content-Type"),
             std::string("application/sdp"));
    CHECK_EQ(rtspParser::extract_header_value(resp, "Session"),
             std::string("12345678;timeout=60"));

    // header names are case insensitive in both directions
    CHECK_EQ(rtspParser::extract_header_value(resp, "session"),
             std::string("12345678;timeout=60"));
    CHECK_EQ(rtspParser::extract_header_value(resp, "CONTENT-LENGTH"), std::string("42"));
    const std::string odd_case =
        "RTSP/1.0 200 OK\r\ncSeQ: 7\r\nsession: abc\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(odd_case, "CSeq"), std::string("7"));
    CHECK_EQ(rtspParser::extract_header_value(odd_case, "Session"), std::string("abc"));

    // absent header yields the empty string
    CHECK_EQ(rtspParser::extract_header_value(resp, "Transport"), std::string(""));

    // leading spaces/tabs after the colon are stripped, the value is not
    // otherwise altered.
    const std::string padded = "RTSP/1.0 200 OK\r\nCSeq:\t \t9\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(padded, "CSeq"), std::string("9"));

    // header on the very first line of the buffer is still found
    const std::string first_line = "Session: only\r\nCSeq: 1\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(first_line, "Session"), std::string("only"));

    // last header without a terminating CRLF returns the tail
    const std::string unterminated = "RTSP/1.0 200 OK\r\nCSeq: 12";
    CHECK_EQ(rtspParser::extract_header_value(unterminated, "CSeq"), std::string("12"));

    // empty value
    const std::string empty_val = "RTSP/1.0 200 OK\r\nSession:\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(empty_val, "Session"), std::string(""));

    SUITE("extract_header_value / line anchoring");

    // A *different* header whose name merely ends with the requested name must
    // not satisfy the lookup.
    const std::string x_only = "RTSP/1.0 200 OK\r\nX-Session: nope\r\nCSeq: 1\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(x_only, "Session"), std::string(""));

    // ... and when both exist, the real one wins regardless of order.
    const std::string x_first =
        "RTSP/1.0 200 OK\r\nX-Session: nope\r\nSession: real\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(x_first, "Session"), std::string("real"));
    const std::string x_last =
        "RTSP/1.0 200 OK\r\nSession: real\r\nX-Session: nope\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(x_last, "Session"), std::string("real"));

    // "session:" appearing inside another header's *value* must not match.
    const std::string in_value =
        "RTSP/1.0 200 OK\r\nX-Debug: session:bogus\r\nCSeq: 1\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(in_value, "Session"), std::string(""));
    const std::string in_value_then_real =
        "RTSP/1.0 200 OK\r\nX-Debug: session:bogus\r\nSession: real\r\n\r\n";
    CHECK_EQ(rtspParser::extract_header_value(in_value_then_real, "Session"),
             std::string("real"));

    // ... including inside the body that follows the blank line.
    const std::string in_body =
        "RTSP/1.0 200 OK\r\nContent-Length: 20\r\n\r\na=tool: session:xyz\r\n";
    CHECK_EQ(rtspParser::extract_header_value(in_body, "Session"), std::string(""));

    // RTSP mandates CRLF; a bare-LF message is not a valid header block and
    // must not be mined for values.
    const std::string lf_only = "RTSP/1.0 200 OK\nSession: 42\n\n";
    CHECK_EQ(rtspParser::extract_header_value(lf_only, "Session"), std::string(""));
}

void test_replace_request_uri()
{
    SUITE("replace_request_uri");

    const std::string request =
        "DESCRIBE rtsp://proxy.example/old/path?old=1 RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "Accept: application/sdp\r\n\r\n";
    const std::string location =
        "rtsp://124.132.240.33:554/live/ch.sdp?playtype=1&time=20260730055417+08"
        "&profilecode=&AuthInfo=XpUHKW8KQXs8yP0uyna3%2Bu8XDxkg%3D%3D";

    CHECK_EQ(rtspParser::replace_request_uri(request, location),
             "DESCRIBE " + location + " RTSP/1.0\r\n"
             "CSeq: 7\r\n"
             "Accept: application/sdp\r\n\r\n");

    // Malformed/incomplete messages are left untouched.
    CHECK_EQ(rtspParser::replace_request_uri("DESCRIBE /x", location),
             std::string("DESCRIBE /x"));
}

// ------------------------------------------------------------ Content-Length

void test_get_content_length()
{
    SUITE("get_content_length");

    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 314\r\n\r\n"),
             314);
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\ncontent-length: 0\r\n\r\n"),
             0);
    // trailing horizontal whitespace before the CRLF is tolerated
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 7 \t\r\n\r\n"),
             7);
    // no header at all means no body
    CHECK_EQ(rtspParser::get_content_length("RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"), 0);

    // malformed values must be rejected with -1, not silently truncated
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 123abc\r\n\r\n"),
             -1);
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 12 3\r\n\r\n"),
             -1);
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: abc\r\n\r\n"),
             -1);
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 0x10\r\n\r\n"),
             -1);
    // negative
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: -1\r\n\r\n"),
             -1);
    // above INT_MAX but representable in long long
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 2147483648\r\n\r\n"),
             -1);
    // beyond long long entirely (stoll throws out_of_range)
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n"),
             -1);
    // INT_MAX itself is still accepted
    CHECK_EQ(rtspParser::get_content_length(
                 "RTSP/1.0 200 OK\r\nContent-Length: 2147483647\r\n\r\n"),
             2147483647);
}

// ------------------------------------------------------------------- Session

void test_parse_session_id()
{
    SUITE("parse_session_id");

    rtspCtx ctx{};
    CHECK_EQ(rtspParser::parse_session_id(
                 "RTSP/1.0 200 OK\r\nCSeq: 2\r\nSession: 12345678\r\n\r\n", ctx),
             0);
    CHECK_EQ(ctx.session_id, std::string("12345678"));

    // the ;timeout= parameter must be stripped from the id
    rtspCtx ctx2{};
    CHECK_EQ(rtspParser::parse_session_id(
                 "RTSP/1.0 200 OK\r\nSession: A1B2C3D4;timeout=60\r\n\r\n", ctx2),
             0);
    CHECK_EQ(ctx2.session_id, std::string("A1B2C3D4"));

    // multiple parameters
    rtspCtx ctx3{};
    CHECK_EQ(rtspParser::parse_session_id(
                 "RTSP/1.0 200 OK\r\nSession: 99;timeout=30;foo=bar\r\n\r\n", ctx3),
             0);
    CHECK_EQ(ctx3.session_id, std::string("99"));

    // missing header -> failure, ctx untouched
    rtspCtx ctx4{};
    ctx4.session_id = "previous";
    CHECK_EQ(rtspParser::parse_session_id("RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n", ctx4), -1);
    CHECK_EQ(ctx4.session_id, std::string("previous"));

    // present but empty -> failure
    rtspCtx ctx5{};
    CHECK_EQ(rtspParser::parse_session_id("RTSP/1.0 200 OK\r\nSession:\r\n\r\n", ctx5), -1);

    // an X-Session header must not be mistaken for the session id
    rtspCtx ctx6{};
    CHECK_EQ(rtspParser::parse_session_id(
                 "RTSP/1.0 200 OK\r\nX-Session: decoy\r\n\r\n", ctx6),
             -1);
}

// ----------------------------------------------------------------- Transport

void test_parse_server_ports()
{
    SUITE("parse_server_ports / UDP");

    rtspCtx ctx{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP;unicast;client_port=5000-5001;server_port=6970-6971\r\n"
                 "Session: 1\r\n\r\n",
                 ctx),
             0);
    CHECK_EQ(ctx.server_rtp_port, 6970);
    CHECK_EQ(ctx.server_rtcp_port, 6971);

    // trailing parameters after the range must not confuse the second stoi
    rtspCtx ctx2{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP;unicast;server_port=6970-6971;ssrc=1234ABCD\r\n\r\n",
                 ctx2),
             0);
    CHECK_EQ(ctx2.server_rtp_port, 6970);
    CHECK_EQ(ctx2.server_rtcp_port, 6971);

    // out-of-range port numbers are rejected
    rtspCtx ctx3{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP;server_port=0-1\r\n\r\n", ctx3),
             -1);
    rtspCtx ctx4{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP;server_port=70000-70001\r\n\r\n",
                 ctx4),
             -1);

    // no Transport header at all
    rtspCtx ctx5{};
    CHECK_EQ(rtspParser::parse_server_ports("RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n", ctx5), -1);

    // Transport present but carries neither server_port nor interleaved
    rtspCtx ctx6{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP;unicast;client_port=5000-5001\r\n\r\n",
                 ctx6),
             -1);

    SUITE("parse_server_ports / TCP interleaved");

    rtspCtx tcp{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n",
                 tcp),
             0);
    CHECK_EQ(tcp.server_rtp_port, 0);
    CHECK_EQ(tcp.server_rtcp_port, 1);

    rtspCtx tcp2{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;ssrc=DEADBEEF\r\n\r\n",
                 tcp2),
             0);
    CHECK_EQ(tcp2.server_rtp_port, 2);
    CHECK_EQ(tcp2.server_rtcp_port, 3);

    // channel numbers are single bytes
    rtspCtx tcp3{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP/TCP;interleaved=256-257\r\n\r\n",
                 tcp3),
             -1);

    // server_port takes precedence when both are present
    rtspCtx both{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP;server_port=6970-6971;interleaved=4-5\r\n\r\n",
                 both),
             0);
    CHECK_EQ(both.server_rtp_port, 6970);

    SUITE("parse_server_ports / dash is bounded to its own parameter");

    // The '-' that separates the RTP and RTCP ports is only looked for inside
    // the server_port parameter (up to the next ';'), so a dash living in a
    // later parameter can neither be used as the separator nor make the parse
    // throw.
    rtspCtx w1{};
    w1.server_rtp_port = 1111;
    w1.server_rtcp_port = 2222;
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP;unicast;server_port=6970;mode=\"play-record\"\r\n\r\n",
                 w1),
             -1);
    CHECK_EQ(w1.server_rtp_port, 1111);
    CHECK_EQ(w1.server_rtcp_port, 2222);

    // A single-port server_port must not pair with the dash of a later
    // parameter, and must leave the context untouched.
    rtspCtx w2{};
    const std::string cross =
        "RTSP/1.0 200 OK\r\n"
        "Transport: RTP/AVP;unicast;server_port=6970;client_port=9000-9001\r\n\r\n";
    CHECK_EQ(rtspParser::parse_server_ports(cross, w2), -1);
    CHECK_EQ(static_cast<int>(w2.server_rtp_port), 0);
    CHECK_EQ(static_cast<int>(w2.server_rtcp_port), 0);

    // The interleaved branch is bounded the same way.
    rtspCtx w3{};
    const std::string cross_tcp =
        "RTSP/1.0 200 OK\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0;ssrc=1;x=7-9\r\n\r\n";
    CHECK_EQ(rtspParser::parse_server_ports(cross_tcp, w3), -1);
    CHECK_EQ(static_cast<int>(w3.server_rtcp_port), 0);

    // mode="play-record" before the range must not be mistaken for it either.
    rtspCtx w4{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\n"
                 "Transport: RTP/AVP;unicast;mode=\"play-record\";server_port=6970-6971\r\n\r\n",
                 w4),
             0);
    CHECK_EQ(w4.server_rtp_port, 6970);
    CHECK_EQ(w4.server_rtcp_port, 6971);

    // Junk inside the parameter is rejected instead of being truncated away.
    rtspCtx w5{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP;server_port=6970-69x1\r\n\r\n", w5),
             -1);
    rtspCtx w6{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP;server_port=-6971\r\n\r\n", w6),
             -1);
    rtspCtx w7{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP/TCP;interleaved=0-1x\r\n\r\n", w7),
             -1);

    // Padding around the numbers is still tolerated.
    rtspCtx w8{};
    CHECK_EQ(rtspParser::parse_server_ports(
                 "RTSP/1.0 200 OK\r\nTransport: RTP/AVP;server_port=6970-6971 \r\n\r\n", w8),
             0);
    CHECK_EQ(w8.server_rtcp_port, 6971);
}

// ---------------------------------------------------------------------- URL

void test_parse_url()
{
    SUITE("parse_url / literal IPv4");

    rtspCtx a{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.10:8554/live/ch0", a), 0);
    CHECK_EQ(a.server_ip, std::string("192.0.2.10"));
    CHECK_EQ(a.server_rtsp_port, 8554);
    CHECK_EQ(a.path, std::string("/live/ch0"));
    CHECK_EQ(a.rtsp_url, std::string("rtsp://192.0.2.10:8554/live/ch0"));

    // default RTSP port when none is given
    rtspCtx b{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.20/stream1", b), 0);
    CHECK_EQ(b.server_ip, std::string("192.0.2.20"));
    CHECK_EQ(b.server_rtsp_port, 554);
    CHECK_EQ(b.path, std::string("/stream1"));

    // no path at all -> "/"
    rtspCtx c{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.30:5540", c), 0);
    CHECK_EQ(c.server_rtsp_port, 5540);
    CHECK_EQ(c.path, std::string("/"));

    // bare trailing slash
    rtspCtx d{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.30/", d), 0);
    CHECK_EQ(d.path, std::string("/"));

    // query string stays part of the path
    rtspCtx e{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.40:554/cam?chan=1&sub=0", e), 0);
    CHECK_EQ(e.path, std::string("/cam?chan=1&sub=0"));

    SUITE("parse_url / rejections");

    rtspCtx f{};
    CHECK_EQ(rtspParser::parse_url("http://192.0.2.10/x", f), -1);
    // the (cleaned) input is recorded even on the failure path
    CHECK_EQ(f.rtsp_url, std::string("http://192.0.2.10/x"));

    rtspCtx g{};
    CHECK_EQ(rtspParser::parse_url("", g), -1);
    rtspCtx h{};
    CHECK_EQ(rtspParser::parse_url("rtsp://", h), -1);
    rtspCtx i{};
    CHECK_EQ(rtspParser::parse_url("rtsp:///path", i), -1);
    rtspCtx j{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.10:abc/x", j), -1);
    rtspCtx k{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.10:8554x/x", k), -1);
    rtspCtx l{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.10:0/x", l), -1);
    rtspCtx m{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.10:99999/x", m), -1);

    SUITE("parse_url / backslash and %5C stripping");

    // Backslashes are removed outright (they are neither converted to '/' nor
    // left in place), which is what keeps a CDN-mangled URL routable.
    rtspCtx n{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.50:554/live\\/ch0", n), 0);
    CHECK_EQ(n.path, std::string("/live/ch0"));
    CHECK_EQ(n.rtsp_url, std::string("rtsp://192.0.2.50:554/live/ch0"));

    rtspCtx o{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.50/live%5C/ch1", o), 0);
    CHECK_EQ(o.path, std::string("/live/ch1"));

    // lowercase escape is handled too
    rtspCtx p{};
    CHECK_EQ(rtspParser::parse_url("rtsp://192.0.2.50/live%5c/ch2", p), 0);
    CHECK_EQ(p.path, std::string("/live/ch2"));

    // several of them, mixed
    rtspCtx q{};
    CHECK_EQ(rtspParser::parse_url("rtsp:%5C/%5c/192.0.2.60:554/a\\b%5Cc", q), 0);
    CHECK_EQ(q.server_ip, std::string("192.0.2.60"));
    CHECK_EQ(q.path, std::string("/abc"));

    SUITE("parse_url / IPv6 literal");

    // A bracketed IPv6 literal is rejected: the first ':' inside the brackets
    // is taken as the port separator. Documented, not fixed here.
    rtspCtx r{};
    XCHECK_EQ(rtspParser::parse_url("rtsp://[2001:db8::1]:554/live", r), 0,
              "bracketed IPv6 literals are not understood (first ':' read as the "
              "port separator)");
}

// ---------------------------------------------------------------------- SDP

void test_sdp()
{
    SUITE("SDP::parseSDP / session level");

    const std::string sdp =
        "v=0\r\n"
        "o=- 2890844526 1 IN IP4 192.0.2.10\r\n"
        "s=Media Presentation\r\n"
        "t=0 0\r\n"
        "a=range:npt=0-\r\n"
        "b=AS:4096\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "b=AS:2048\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:trackID=1\r\n"
        "a=recvonly\r\n"
        "m=audio 0 RTP/AVP 97 98\r\n"
        "a=rtpmap:97 MPEG4-GENERIC/16000\r\n"
        "a=control:trackID=2\r\n";

    rtspCtx ctx{};
    rtspParser::SDP::parseSDP(sdp, ctx);

    CHECK_EQ(ctx.sdp.version, std::string("0"));
    CHECK_EQ(ctx.sdp.origin, std::string("- 2890844526 1 IN IP4 192.0.2.10"));
    CHECK_EQ(ctx.sdp.session_name, std::string("Media Presentation"));
    CHECK_EQ(ctx.sdp.time, std::string("0 0"));

    SUITE("SDP::parseSDP / m= lines");

    CHECK_EQ(sz(ctx.sdp.media_streams.size()), sz(2));
    if (ctx.sdp.media_streams.size() == 2)
    {
        const Media &v = ctx.sdp.media_streams[0];
        const Media &a = ctx.sdp.media_streams[1];

        CHECK_EQ(v.type, std::string("video"));
        CHECK_EQ(v.port, 0);
        CHECK_EQ(v.protocol, std::string("RTP/AVP"));
        CHECK_EQ(sz(v.formats.size()), sz(1));
        CHECK_EQ(v.formats.front(), std::string("96"));

        CHECK_EQ(a.type, std::string("audio"));
        CHECK_EQ(a.protocol, std::string("RTP/AVP"));
        CHECK_EQ(sz(a.formats.size()), sz(2));
        if (a.formats.size() == 2)
        {
            CHECK_EQ(a.formats[0], std::string("97"));
            CHECK_EQ(a.formats[1], std::string("98"));
        }

        SUITE("SDP::parseSDP / a= binds to most recent m=");

        CHECK_EQ(attr_of(v, "rtpmap"), std::string("96 H264/90000"));
        CHECK_EQ(attr_of(v, "fmtp"), std::string("96 packetization-mode=1"));
        CHECK_EQ(attr_of(v, "control"), std::string("trackID=1"));
        CHECK_EQ(v.trackID, std::string("trackID=1"));

        CHECK_EQ(attr_of(a, "rtpmap"), std::string("97 MPEG4-GENERIC/16000"));
        CHECK_EQ(attr_of(a, "control"), std::string("trackID=2"));
        CHECK_EQ(a.trackID, std::string("trackID=2"));

        // the audio section must not inherit the video attributes
        CHECK_EQ(sz(a.attributes.count("fmtp")), sz(0));

        // a session-level a= that appears before any m= is dropped: sdpCtx has
        // nowhere to keep it, and it must not leak into the first media section.
        CHECK_EQ(sz(v.attributes.count("range")), sz(0));
        CHECK_EQ(sz(a.attributes.count("range")), sz(0));

        // property attributes (no ':') are kept with an empty value
        CHECK_EQ(sz(v.attributes.count("recvonly")), sz(1));
        CHECK_EQ(attr_of(v, "recvonly"), std::string(""));

        SUITE("SDP::parseSDP / b= bandwidth");

        // every b= line lands in the session map...
        CHECK_EQ(sz(ctx.sdp.session_bandwidth.count("AS")), sz(1));
        // ... including media-level ones, which overwrite the session value.
        XCHECK_EQ(bw_of(ctx.sdp.session_bandwidth, "AS"), 4096,
                  "media-level b= is written to sdpCtx.session_bandwidth and "
                  "clobbers the session-level value");
        XCHECK_EQ(sz(v.bandwidth.count("AS")), sz(1),
                  "Media::bandwidth is never populated; media-level b= is not "
                  "bound to its m= section");
    }

    SUITE("SDP::parseSDP / property attributes");

    // The other common RFC 4566 flag attributes, and a value attribute whose
    // value itself contains colons, all land in the map.
    rtspCtx flags{};
    rtspParser::SDP::parseSDP(
        "v=0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=sendonly\r\n"
        "a=rtcp-mux\r\n"
        "a=control:rtsp://192.0.2.10:554/live/trackID=1\r\n"
        "a=\r\n",
        flags);
    CHECK_EQ(sz(flags.sdp.media_streams.size()), sz(1));
    if (flags.sdp.media_streams.size() == 1)
    {
        const Media &f = flags.sdp.media_streams[0];
        CHECK_EQ(attr_of(f, "sendonly"), std::string(""));
        CHECK_EQ(attr_of(f, "rtcp-mux"), std::string(""));
        // only the first ':' separates key from value
        CHECK_EQ(attr_of(f, "control"),
                 std::string("rtsp://192.0.2.10:554/live/trackID=1"));
        CHECK_EQ(f.trackID, std::string("rtsp://192.0.2.10:554/live/trackID=1"));
        // a bare "a=" has no key and is not stored
        CHECK_EQ(sz(f.attributes.count("")), sz(0));
        CHECK_EQ(sz(f.attributes.size()), sz(3));
    }

    SUITE("SDP::parseSDP / robustness");

    // Bare-LF SDP (common in the wild) parses fine; the trailing \r of CRLF
    // bodies is trimmed rather than kept in the value.
    rtspCtx lf{};
    rtspParser::SDP::parseSDP("v=0\ns=lf body\nm=video 0 RTP/AVP 96\n", lf);
    CHECK_EQ(lf.sdp.session_name, std::string("lf body"));
    CHECK_EQ(sz(lf.sdp.media_streams.size()), sz(1));

    // A non-numeric media port drops the whole m= section, and the a= lines
    // that follow are discarded instead of crashing or attaching elsewhere.
    rtspCtx bad{};
    rtspParser::SDP::parseSDP("v=0\r\nm=video xyz RTP/AVP 96\r\na=control:trackID=1\r\n", bad);
    CHECK_EQ(sz(bad.sdp.media_streams.size()), sz(0));

    // A malformed b= value is ignored rather than stored as 0.
    rtspCtx badbw{};
    rtspParser::SDP::parseSDP("v=0\r\nb=AS:notanumber\r\n", badbw);
    CHECK_EQ(sz(badbw.sdp.session_bandwidth.count("AS")), sz(0));

    // Empty body is a no-op.
    rtspCtx empty{};
    rtspParser::SDP::parseSDP("", empty);
    CHECK_EQ(sz(empty.sdp.media_streams.size()), sz(0));
    CHECK_EQ(empty.sdp.version, std::string(""));

    // Blank lines and unknown line types are skipped.
    rtspCtx mixed{};
    rtspParser::SDP::parseSDP("v=0\r\n\r\nz=0 0\r\nc=IN IP4 0.0.0.0\r\ns=ok\r\n", mixed);
    CHECK_EQ(mixed.sdp.version, std::string("0"));
    CHECK_EQ(mixed.sdp.session_name, std::string("ok"));
}

} // namespace

int main()
{
    silence_logger();

    test_parse_status_code();
    test_extract_header_value();
    test_replace_request_uri();
    test_get_content_length();
    test_parse_session_id();
    test_parse_server_ports();
    test_parse_url();
    test_sdp();

    return tst::summary();
}
