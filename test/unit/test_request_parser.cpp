// Unit tests for RequestParser (src/protocol/request_parser.cpp).
//
// RequestParser::parse() is the very first thing that touches attacker
// controlled bytes: it splits the request line, sanitizes it for logging,
// strips backslash escapes, extracts query parameters, checks the auth token
// and derives the upstream RTSP URL. Everything here is pure string handling,
// so no sockets and no DNS are involved -- upstream hosts are always written
// as literal dotted-quad addresses so that nothing can ever try to resolve.
//
// Build and run:
//   meson setup build -Dtests=true && meson test -C build request_parser

#include "test_harness.h"

#include "protocol/request_parser.h"
#include "core/server_config.h"

#include <string>

namespace
{

RequestInfo P(const std::string &request_line)
{
    return RequestParser::parse(request_line);
}

// Convenience: value of a query parameter, or a sentinel when absent.
std::string param(const RequestInfo &info, const std::string &key)
{
    auto it = info.params.find(key);
    return it == info.params.end() ? std::string("<absent>") : it->second;
}

bool has_param(const RequestInfo &info, const std::string &key)
{
    return info.params.find(key) != info.params.end();
}

const std::string NPOS_OK = "<none>";

// Returns NPOS_OK when the byte is absent, otherwise a description, so a
// failure message shows which byte leaked instead of just "false".
std::string first_raw_byte(const std::string &s, char needle)
{
    size_t p = s.find(needle);
    if (p == std::string::npos)
        return NPOS_OK;
    return "raw byte at offset " + std::to_string(p);
}

} // namespace

int main()
{
    // ------------------------------------------------------------------
    SUITE("request line: HTTP");
    // ------------------------------------------------------------------
    ServerConfig::setToken("");
    {
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live HTTP/1.1\r\n");
        CHECK_EQ(i.method, "GET");
        CHECK_EQ(i.raw_uri, "/rtp/192.0.2.10:5540/live");
        CHECK_EQ(i.version, "HTTP/1.1");
        CHECK_EQ(i.is_http, true);
        // The CRLF terminator must never end up inside a parsed field.
        CHECK_EQ(first_raw_byte(i.version, '\r'), NPOS_OK);
        CHECK_EQ(first_raw_byte(i.version, '\n'), NPOS_OK);
    }
    {
        // Headers following the request line are ignored, and extra tokens on
        // the request line itself are dropped rather than merged into version.
        RequestInfo i = P("GET /status HTTP/1.0 junk\r\nHost: 192.0.2.10\r\n\r\n");
        CHECK_EQ(i.method, "GET");
        CHECK_EQ(i.raw_uri, "/status");
        CHECK_EQ(i.version, "HTTP/1.0");
        CHECK_EQ(i.is_http, true);
    }

    // ------------------------------------------------------------------
    SUITE("request line: RTSP");
    // ------------------------------------------------------------------
    {
        RequestInfo i = P("OPTIONS rtsp://192.0.2.10:554/live RTSP/1.0\r\n");
        CHECK_EQ(i.method, "OPTIONS");
        CHECK_EQ(i.raw_uri, "rtsp://192.0.2.10:554/live");
        CHECK_EQ(i.version, "RTSP/1.0");
        CHECK_EQ(i.is_http, false);
    }
    {
        RequestInfo i = P("DESCRIBE /rtp/192.0.2.10:5540/live RTSP/1.0\r\n");
        CHECK_EQ(i.is_http, false);
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:5540/live");
    }
    {
        // is_http is a prefix test on the version token, so anything that is
        // not literally "HTTP/..." is treated as RTSP.
        CHECK_EQ(P("GET /x SIP/2.0").is_http, false);
        CHECK_EQ(P("GET /x XHTTP/1.1").is_http, false);
    }

    // ------------------------------------------------------------------
    SUITE("request line: malformed input");
    // ------------------------------------------------------------------
    {
        RequestInfo i = P("");
        CHECK_EQ(i.method, "");
        CHECK_EQ(i.raw_uri, "");
        CHECK_EQ(i.version, "");
        CHECK_EQ(i.upstream_url, "");
        // Bailing out early must never hand back an authorized request.
        CHECK_EQ(i.is_authorized, false);
    }
    {
        // Only two tokens -> the chained extraction fails on the version, so
        // parse() bails out early. Nothing downstream may be derived from it.
        RequestInfo i = P("GET /rtp/192.0.2.10:554/live");
        CHECK_EQ(i.version, "");
        CHECK_EQ(i.upstream_url, "");
        CHECK_EQ(i.is_authorized, false);
        CHECK_EQ(i.clean_uri, "");
        CHECK_EQ(i.params.empty(), true);
        // The first two tokens were already written by the chained >> before it
        // failed, so they survive the early return.
        std::printf("  [ NOTE ] truncated request line leaves method=%s raw_uri=%s\n",
                    tst::show(i.method).c_str(), tst::show(i.raw_uri).c_str());
    }
    {
        // ...and the early return still sanitizes what the extraction managed to
        // write, so no raw byte reaches the caller (and its log line).
        RequestInfo i = P("GET /a\x1b[31mBOOM");
        CHECK_EQ(first_raw_byte(i.raw_uri, '\x1b'), NPOS_OK);
        CHECK_EQ(i.raw_uri, "/a\\x1b[31mBOOM");
        CHECK_EQ(first_raw_byte(P("GET /a\\b").raw_uri, '\\'), NPOS_OK);
        CHECK_EQ(P("GET /a\\b").raw_uri, "/ab");
        // The method token is escaped on this path as well.
        CHECK_EQ(P("\x1bGET /a").method, "\\x1bGET");
        // %5C removal happens here too, and the early return is still taken.
        RequestInfo j = P("GET /rtp%5C/192.0.2.10:5540/live");
        CHECK_EQ(j.raw_uri, "/rtp/192.0.2.10:5540/live");
        CHECK_EQ(j.upstream_url, "");
        CHECK_EQ(j.clean_uri, "");
    }
    {
        // Leading whitespace is skipped by the stream extraction.
        RequestInfo i = P("   GET   /status   HTTP/1.1");
        CHECK_EQ(i.method, "GET");
        CHECK_EQ(i.raw_uri, "/status");
        CHECK_EQ(i.version, "HTTP/1.1");
    }

    // ------------------------------------------------------------------
    SUITE("sanitize_input: control bytes cannot reach the log");
    // ------------------------------------------------------------------
    {
        // ESC is the terminal-injection primitive: a raw 0x1b in the URI would
        // let a request repaint the operator's terminal via the log line.
        RequestInfo i = P("GET /a\x1b[31mBOOM HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/a\\x1b[31mBOOM");
        CHECK_EQ(first_raw_byte(i.raw_uri, '\x1b'), NPOS_OK);
    }
    {
        // Same treatment for the method and version tokens.
        RequestInfo i = P("\x1bG\aET /x \x1bHTTP/1.1"); // \a == BEL == 0x07
        CHECK_EQ(i.method, "\\x1bG\\x07ET");
        CHECK_EQ(first_raw_byte(i.method, '\x1b'), NPOS_OK);
        CHECK_EQ(i.version, "\\x1bHTTP/1.1");
        // The escaped version no longer starts with "HTTP/", so it is not HTTP.
        CHECK_EQ(i.is_http, false);
    }
    {
        // A NUL byte inside the URI is not whitespace, so it stays in the token
        // and must come out escaped rather than truncating the string.
        std::string uri = std::string("/a") + '\0' + "b";
        RequestInfo i = P("GET " + uri + " HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/a\\x00b");
    }
    {
        // DEL (0x7f) is above the printable range and must be escaped too;
        // 0x20 (space) and 0x7e (~) are the inclusive boundaries that stay.
        RequestInfo i = P("GET /a\x7f~ HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/a\\x7f~");
    }
    {
        // A bare CR inside the URI is whitespace to the extractor, so it splits
        // the token -- what matters is that no raw CR survives anywhere.
        RequestInfo i = P("GET /a\rInjected: x HTTP/1.1");
        CHECK_EQ(first_raw_byte(i.method, '\r'), NPOS_OK);
        CHECK_EQ(first_raw_byte(i.raw_uri, '\r'), NPOS_OK);
        CHECK_EQ(first_raw_byte(i.version, '\r'), NPOS_OK);
        CHECK_EQ(i.raw_uri, "/a");
    }
    {
        // High-bit / UTF-8 bytes are escaped byte by byte.
        RequestInfo i = P("GET /\xc3\xa9 HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/\\xc3\\xa9");
    }

    // ------------------------------------------------------------------
    SUITE("URI cleanup: backslash and %5C stripping");
    // ------------------------------------------------------------------
    {
        RequestInfo i = P("GET /rtp\\/192.0.2.10:5540/live HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/rtp/192.0.2.10:5540/live");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:5540/live");
    }
    {
        RequestInfo i = P("GET /rtp%5C/192.0.2.10:5540/live HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/rtp/192.0.2.10:5540/live");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:5540/live");
    }
    {
        // Lowercase percent-escape is handled as well.
        RequestInfo i = P("GET /rtp%5c/192.0.2.10:5540/live HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/rtp/192.0.2.10:5540/live");
    }
    {
        // Removal loops until no occurrence remains, so a split escape that
        // re-forms after the first removal is still stripped.
        RequestInfo i = P("GET /a%5%5CC%5Cb HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/ab");
        CHECK_EQ(first_raw_byte(i.raw_uri, '\\'), NPOS_OK);
    }
    {
        // Multiple backslashes anywhere, including the query string.
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/a\\b\\c?k=v\\w HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/rtp/192.0.2.10:5540/abc?k=vw");
        CHECK_EQ(param(i, "k"), "vw");
    }
    {
        // Double-encoding is left alone: %255C is not a backslash yet.
        RequestInfo i = P("GET /a%255Cb HTTP/1.1");
        CHECK_EQ(i.raw_uri, "/a%255Cb");
    }

    // ------------------------------------------------------------------
    SUITE("query parameters");
    // ------------------------------------------------------------------
    ServerConfig::setToken("");
    {
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live?a=1&b=two&c= HTTP/1.1");
        CHECK_EQ(i.params.size(), static_cast<size_t>(3));
        CHECK_EQ(param(i, "a"), "1");
        CHECK_EQ(param(i, "b"), "two");
        CHECK_EQ(param(i, "c"), "");
        CHECK_EQ(i.clean_uri, "/rtp/192.0.2.10:5540/live?a=1&b=two&c=");
    }
    {
        // A value containing '=' keeps everything after the first '='.
        RequestInfo i = P("GET /x?sig=a=b=c HTTP/1.1");
        CHECK_EQ(param(i, "sig"), "a=b=c");
    }
    {
        // A valueless fragment is not a parameter but is preserved in the URI.
        RequestInfo i = P("GET /x?flag&a=1 HTTP/1.1");
        CHECK_EQ(has_param(i, "flag"), false);
        CHECK_EQ(param(i, "a"), "1");
        CHECK_EQ(i.clean_uri, "/x?flag&a=1");
    }
    {
        // No query string at all: clean_uri is the raw URI and params is empty.
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live HTTP/1.1");
        CHECK_EQ(i.params.empty(), true);
        CHECK_EQ(i.clean_uri, "/rtp/192.0.2.10:5540/live");
    }
    {
        // Later duplicates win in the params map.
        RequestInfo i = P("GET /x?a=1&a=2 HTTP/1.1");
        CHECK_EQ(param(i, "a"), "2");
    }

    // ------------------------------------------------------------------
    SUITE("authorization: no token configured");
    // ------------------------------------------------------------------
    ServerConfig::setToken("");
    {
        // Empty configured token disables auth entirely.
        CHECK_EQ(P("GET /rtp/192.0.2.10:5540/live HTTP/1.1").is_authorized, true);
        CHECK_EQ(P("GET /status HTTP/1.1").is_authorized, true);
        CHECK_EQ(P("DESCRIBE /rtp/192.0.2.10:5540/live RTSP/1.0").is_authorized, true);
    }
    {
        // A token supplied when none is configured is still stripped from the
        // URI so it can never be forwarded upstream.
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live?token=whatever HTTP/1.1");
        CHECK_EQ(i.is_authorized, true);
        CHECK_EQ(i.clean_uri, "/rtp/192.0.2.10:5540/live");
        CHECK_EQ(param(i, "token"), "whatever");
    }

    // ------------------------------------------------------------------
    SUITE("authorization: token configured");
    // ------------------------------------------------------------------
    ServerConfig::setToken("s3cr3t");
    {
        // Correct token -> authorized, and removed from clean_uri (and from the
        // derived upstream URL) so the secret never leaks to the upstream.
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live?token=s3cr3t HTTP/1.1");
        CHECK_EQ(i.is_authorized, true);
        CHECK_EQ(i.clean_uri, "/rtp/192.0.2.10:5540/live");
        CHECK_EQ(i.clean_uri.find("token"), std::string::npos);
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:5540/live");
        // The raw URI is untouched and still carries the token.
        CHECK_EQ(i.raw_uri, "/rtp/192.0.2.10:5540/live?token=s3cr3t");
    }
    {
        // The token is removed from the middle of the query string and the
        // remaining parameters keep their order and separators.
        RequestInfo i = P("GET /tv/192.0.2.10:554/ch?a=1&token=s3cr3t&b=2 HTTP/1.1");
        CHECK_EQ(i.is_authorized, true);
        CHECK_EQ(i.clean_uri, "/tv/192.0.2.10:554/ch?a=1&b=2");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:554/ch?a=1&b=2");
    }
    {
        // Wrong token -> not authorized.
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live?token=wrong HTTP/1.1");
        CHECK_EQ(i.is_authorized, false);
        CHECK_EQ(param(i, "token"), "wrong");
        // It is still stripped from clean_uri, which is the safe direction.
        CHECK_EQ(i.clean_uri, "/rtp/192.0.2.10:5540/live");
    }
    {
        // Missing token -> not authorized, URI untouched.
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live HTTP/1.1");
        CHECK_EQ(i.is_authorized, false);
        CHECK_EQ(i.clean_uri, "/rtp/192.0.2.10:5540/live");
    }
    {
        // Prefix/suffix of the real token must not authorize.
        CHECK_EQ(P("GET /x?token=s3cr3 HTTP/1.1").is_authorized, false);
        CHECK_EQ(P("GET /x?token=s3cr3tt HTTP/1.1").is_authorized, false);
        CHECK_EQ(P("GET /x?token= HTTP/1.1").is_authorized, false);
    }
    {
        // The parameter name is matched exactly: "TOKEN" is a normal parameter,
        // so it neither authorizes nor gets stripped.
        RequestInfo i = P("GET /x?TOKEN=s3cr3t HTTP/1.1");
        CHECK_EQ(i.is_authorized, false);
        CHECK_EQ(i.clean_uri, "/x?TOKEN=s3cr3t");
    }
    {
        // Any occurrence of the correct token authorizes, even alongside a
        // wrong one.
        CHECK_EQ(P("GET /x?token=wrong&token=s3cr3t HTTP/1.1").is_authorized, true);
    }
    {
        // A token carrying an escape byte is sanitized before comparison, so it
        // cannot match -- injection attempts fail closed.
        RequestInfo i = P("GET /x?token=s3cr3t\x1b HTTP/1.1");
        CHECK_EQ(i.is_authorized, false);
        CHECK_EQ(param(i, "token"), "s3cr3t\\x1b");
    }

    // ------------------------------------------------------------------
    SUITE("upstream_url derivation");
    // ------------------------------------------------------------------
    ServerConfig::setToken("");
    {
        RequestInfo i = P("GET /rtp/192.0.2.10:5540/live HTTP/1.1");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:5540/live");
    }
    {
        RequestInfo i = P("GET /tv/192.0.2.20:554/channel1 HTTP/1.1");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.20:554/channel1");
    }
    {
        // A /tv/ URL with a query string goes through the (empty by default)
        // rewrite template list and comes out unchanged.
        RequestInfo i = P("GET /tv/192.0.2.20:554/ch?start=1&end=2 HTTP/1.1");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.20:554/ch?start=1&end=2");
    }
    {
        // Paths that match neither prefix produce no upstream at all.
        CHECK_EQ(P("GET / HTTP/1.1").upstream_url, "");
        CHECK_EQ(P("GET /status HTTP/1.1").upstream_url, "");
        CHECK_EQ(P("GET /api/logs?n=10 HTTP/1.1").upstream_url, "");
    }
    {
        // Prefix with nothing after it is rejected instead of yielding "rtsp://".
        CHECK_EQ(P("GET /rtp/ HTTP/1.1").upstream_url, "");
        CHECK_EQ(P("GET /tv/ HTTP/1.1").upstream_url, "");
    }
    {
        // Derivation happens after backslash stripping and token removal.
        ServerConfig::setToken("s3cr3t");
        RequestInfo i = P("GET /rtp%5C/192.0.2.10:5540/live?token=s3cr3t HTTP/1.1");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.10:5540/live");
        ServerConfig::setToken("");
    }

    // ------------------------------------------------------------------
    SUITE("upstream_url: prefix is anchored at the start of the path");
    // ------------------------------------------------------------------
    ServerConfig::setToken("");
    {
        // "/rtp/" inside a query parameter value on an unrelated endpoint must
        // not select the upstream host, and the query itself is left alone.
        RequestInfo i = P("GET /index?x=/rtp/192.0.2.1:554/s HTTP/1.1");
        CHECK_EQ(i.clean_uri, "/index?x=/rtp/192.0.2.1:554/s");
        CHECK_EQ(i.upstream_url, "");
    }
    {
        // A path that merely contains the prefix deeper down is not a stream.
        CHECK_EQ(P("GET /static/rtp/192.0.2.1:554/s HTTP/1.1").upstream_url, "");
        CHECK_EQ(P("GET /a/tv/192.0.2.1:554/s HTTP/1.1").upstream_url, "");
    }
    {
        // A genuine /tv/ path that happens to contain "/rtp/" further along is
        // no longer truncated to the /rtp/ suffix.
        RequestInfo i = P("GET /tv/192.0.2.20:554/a/rtp/x HTTP/1.1");
        CHECK_EQ(i.upstream_url, "rtsp://192.0.2.20:554/a/rtp/x");
    }
    {
        // An absolute URI keeps working: RTSP clients send one on every request
        // after the redirect, so the scheme and authority are skipped.
        CHECK_EQ(P("DESCRIBE rtsp://192.0.2.9:8554/rtp/192.0.2.10:5540/live RTSP/1.0").upstream_url,
                 "rtsp://192.0.2.10:5540/live");
        CHECK_EQ(P("PLAY rtsp://192.0.2.9:8554/tv/192.0.2.20:554/ch?a=1 RTSP/1.0").upstream_url,
                 "rtsp://192.0.2.20:554/ch?a=1");
        // ...but only when the prefix really starts the path.
        CHECK_EQ(P("DESCRIBE rtsp://192.0.2.9:8554/x/rtp/192.0.2.10:5540/live RTSP/1.0").upstream_url,
                 "");
        // An authority with no path at all must not be mistaken for one.
        CHECK_EQ(P("OPTIONS rtsp://192.0.2.9:8554 RTSP/1.0").upstream_url, "");
    }
    {
        // A "://" that only appears inside the query string must not be taken
        // for a scheme separator and move the anchor into the query.
        CHECK_EQ(P("GET /index?u=http://h/rtp/192.0.2.1:554/s HTTP/1.1").upstream_url, "");
    }

    ServerConfig::setToken("");
    return tst::summary();
}
