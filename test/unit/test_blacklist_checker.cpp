// Unit tests for BlacklistChecker (src/utils/blacklist_checker.cpp).
//
// NOTE ON DNS: BlacklistChecker::is_blacklisted() falls through to
// DNSResolver::resolve_ipv4() (a synchronous getaddrinfo) whenever the raw host
// string does not match any blacklist entry. Every host used below is therefore
// either
//   * a literal dotted-quad IPv4 address  -> getaddrinfo parses it numerically,
//     never touching the network, or
//   * a syntactically invalid DNS name containing an empty label ("a..b")
//     -> rejected by the resolver's name encoder before any query is sent.
// For the invalid-name group the blacklist deliberately contains no IP-shaped
// entries at all, so the outcome cannot depend on what any resolver returns.
//
// match_cidr()/match_wildcard() are private, so they are exercised through
// is_blacklisted().

#include "test_harness.h"

#include "core/server_config.h"
#include "utils/blacklist_checker.h"

#include <fcntl.h>
#include <unistd.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace
{

// ServerConfig is process-wide static state; every group sets it explicitly.
void set_bl(std::initializer_list<const char *> entries)
{
    std::vector<std::string> v;
    for (const char *e : entries)
        v.emplace_back(e);
    ServerConfig::setBlacklist(v);
}

bool bl(const char *host) { return BlacklistChecker::is_blacklisted(host); }

} // namespace

int main()
{
    // ---------------------------------------------------------------- //
    SUITE("empty blacklist short-circuits to false");
    // ---------------------------------------------------------------- //
    set_bl({});
    CHECK_EQ(bl("192.0.2.10"), false);
    CHECK_EQ(bl("127.0.0.1"), false);
    CHECK_EQ(bl("0.0.0.0"), false);
    CHECK_EQ(bl("255.255.255.255"), false);
    CHECK_EQ(bl(""), false);

    // ---------------------------------------------------------------- //
    SUITE("exact literal-IP entries");
    // ---------------------------------------------------------------- //
    set_bl({"192.0.2.10", "198.51.100.5"});
    CHECK_EQ(bl("192.0.2.10"), true);
    CHECK_EQ(bl("198.51.100.5"), true);
    CHECK_EQ(bl("192.0.2.11"), false);
    CHECK_EQ(bl("198.51.100.50"), false); // prefix of an entry is not a match
    CHECK_EQ(bl("8.51.100.5"), false);    // suffix of an entry is not a match
    CHECK_EQ(bl("192.0.2.1"), false);
    // NOTE: a non-canonical spelling of a listed address ("192.0.2.010") is
    // deliberately not asserted on. The string compare misses it, but the
    // resolver pass then re-checks whatever getaddrinfo() canonicalises it to,
    // and that differs between libcs (BSD reads "010" as decimal 10 and the
    // entry matches; glibc's inet_aton reads it as octal 8 and it does not).

    // ---------------------------------------------------------------- //
    SUITE("CIDR /8");
    // ---------------------------------------------------------------- //
    set_bl({"10.0.0.0/8"});
    CHECK_EQ(bl("10.0.0.0"), true);        // network address
    CHECK_EQ(bl("10.1.2.3"), true);
    CHECK_EQ(bl("10.255.255.255"), true);  // last address in range
    CHECK_EQ(bl("9.255.255.255"), false);  // one below
    CHECK_EQ(bl("11.0.0.0"), false);       // one above

    // ---------------------------------------------------------------- //
    SUITE("CIDR /12");
    // ---------------------------------------------------------------- //
    set_bl({"172.16.0.0/12"});
    CHECK_EQ(bl("172.15.255.255"), false); // just outside, below
    CHECK_EQ(bl("172.16.0.0"), true);      // first in range
    CHECK_EQ(bl("172.20.30.40"), true);
    CHECK_EQ(bl("172.31.255.255"), true);  // last in range
    CHECK_EQ(bl("172.32.0.0"), false);     // just outside, above

    // ---------------------------------------------------------------- //
    SUITE("CIDR /16");
    // ---------------------------------------------------------------- //
    set_bl({"192.168.0.0/16"});
    CHECK_EQ(bl("192.167.255.255"), false);
    CHECK_EQ(bl("192.168.0.0"), true);
    CHECK_EQ(bl("192.168.255.255"), true);
    CHECK_EQ(bl("192.169.0.0"), false);

    // ---------------------------------------------------------------- //
    SUITE("CIDR /24");
    // ---------------------------------------------------------------- //
    set_bl({"198.51.100.0/24"});
    CHECK_EQ(bl("198.51.99.255"), false);
    CHECK_EQ(bl("198.51.100.0"), true);
    CHECK_EQ(bl("198.51.100.255"), true);
    CHECK_EQ(bl("198.51.101.0"), false);

    // ---------------------------------------------------------------- //
    SUITE("CIDR /32");
    // ---------------------------------------------------------------- //
    set_bl({"203.0.113.7/32"});
    CHECK_EQ(bl("203.0.113.7"), true);
    CHECK_EQ(bl("203.0.113.6"), false);
    CHECK_EQ(bl("203.0.113.8"), false);

    // ---------------------------------------------------------------- //
    SUITE("CIDR /0 (must be well-defined, no shift-by-32 UB)");
    // ---------------------------------------------------------------- //
    set_bl({"0.0.0.0/0"});
    CHECK_EQ(bl("0.0.0.0"), true);
    CHECK_EQ(bl("8.8.8.8"), true);
    CHECK_EQ(bl("255.255.255.255"), true);
    // The base address is irrelevant once the prefix length is 0.
    set_bl({"203.0.113.7/0"});
    CHECK_EQ(bl("10.1.2.3"), true);

    // ---------------------------------------------------------------- //
    SUITE("CIDR misc: host bits in base, bad base, bad prefix");
    // ---------------------------------------------------------------- //
    // Host bits set in the base address are masked off, so the whole block
    // still matches.
    set_bl({"172.16.5.9/12"});
    CHECK_EQ(bl("172.16.0.0"), true);
    CHECK_EQ(bl("172.31.255.255"), true);
    CHECK_EQ(bl("172.32.0.0"), false);

    // Unparseable base address -> never matches (and must not crash).
    set_bl({"999.0.0.0/8"});
    CHECK_EQ(bl("10.1.2.3"), false);
    CHECK_EQ(bl("192.0.2.10"), false);

    // Prefix length out of range / not a number -> entry is inert.
    set_bl({"10.0.0.0/33"});
    CHECK_EQ(bl("10.1.2.3"), false);
    set_bl({"10.0.0.0/-1"});
    CHECK_EQ(bl("10.1.2.3"), false);
    set_bl({"10.0.0.0/8x"});
    CHECK_EQ(bl("10.1.2.3"), false);
    set_bl({"10.0.0.0/"});
    CHECK_EQ(bl("10.1.2.3"), false);
    set_bl({"10.0.0.0/0x8"});
    CHECK_EQ(bl("10.1.2.3"), false);

    // BUG: std::stoi accepts "-0" as 0, and the (bits < 0) guard does not
    // reject it, so a malformed prefix silently becomes /0 and blacklists the
    // entire IPv4 space.
    set_bl({"10.0.0.0/-0"});
    XCHECK_EQ(bl("8.8.8.8"), false,
              "blacklist_checker.cpp:57 stoi accepts \"-0\"; malformed prefix "
              "degrades to /0 and matches every IPv4 address");

    // ---------------------------------------------------------------- //
    SUITE("wildcard: leading '*' (suffix match)");
    // ---------------------------------------------------------------- //
    set_bl({"*.2.10"});
    CHECK_EQ(bl("192.0.2.10"), true);
    CHECK_EQ(bl("198.51.2.10"), true);
    CHECK_EQ(bl("192.0.2.11"), false);
    CHECK_EQ(bl("192.0.12.10"), false); // the literal '.' before 2 must match
    set_bl({"*192.0.2.10"});            // pattern longer than the host
    CHECK_EQ(bl("0.2.10"), false);

    // ---------------------------------------------------------------- //
    SUITE("wildcard: trailing '*' (prefix match)");
    // ---------------------------------------------------------------- //
    set_bl({"192.0.*"});
    CHECK_EQ(bl("192.0.2.10"), true);
    CHECK_EQ(bl("192.0.113.9"), true);
    CHECK_EQ(bl("192.1.2.10"), false);
    CHECK_EQ(bl("10.192.0.1"), false); // prefix must be anchored at the start

    // ---------------------------------------------------------------- //
    SUITE("wildcard: bare '*' matches everything");
    // ---------------------------------------------------------------- //
    set_bl({"*"});
    CHECK_EQ(bl("192.0.2.10"), true);
    CHECK_EQ(bl("10.0.0.1"), true);

    // ---------------------------------------------------------------- //
    SUITE("wildcard: '*' in the middle is unsupported");
    // ---------------------------------------------------------------- //
    // Documented limitation: only a leading or trailing '*' is honoured. A '*'
    // anywhere else falls through to a literal string comparison, which a real
    // host can never satisfy, so nothing matches.
    set_bl({"192.*.2.10"});
    CHECK_EQ(bl("192.0.2.10"), false);
    CHECK_EQ(bl("192.168.2.10"), false);
    set_bl({"10.*.*.1"});
    CHECK_EQ(bl("10.0.0.1"), false);

    // ---------------------------------------------------------------- //
    SUITE("pattern kind dispatch: '/' wins over '*'");
    // ---------------------------------------------------------------- //
    // A pattern containing a '/' is always treated as CIDR, even if it also
    // contains a '*', so the '*' is never expanded.
    set_bl({"10.0.*.0/8"});
    CHECK_EQ(bl("10.1.2.3"), false); // "10.0.*.0" is not a valid base address

    // ---------------------------------------------------------------- //
    SUITE("host comparison is case-sensitive");
    // ---------------------------------------------------------------- //
    // Hosts below contain an empty DNS label, so the resolver rejects them
    // locally and no query is ever emitted; the blacklist holds no IP-shaped
    // entries, so a resolver result could not change the answer either.
    set_bl({"CAM..BLOCKED"});
    CHECK_EQ(bl("CAM..BLOCKED"), true); // identical spelling matches
    // DNS names are case-insensitive (RFC 4343), so an entry must block every
    // case variant of the same name; today it does not, which is a blacklist
    // bypass.
    XCHECK(bl("cam..blocked"),
           "blacklist_checker.cpp:26 exact match uses case-sensitive ==; "
           "changing hostname case bypasses the blacklist");
    XCHECK(bl("Cam..Blocked"),
           "blacklist_checker.cpp:26 exact match is case-sensitive");

    set_bl({"*.EVIL..BLOCKED"});
    CHECK_EQ(bl("host.EVIL..BLOCKED"), true);
    XCHECK(bl("host.evil..blocked"),
           "blacklist_checker.cpp:86 wildcard suffix compare is case-sensitive; "
           "case variation bypasses *. patterns");

    // ---------------------------------------------------------------- //
    SUITE("several patterns of mixed kinds in one blacklist");
    // ---------------------------------------------------------------- //
    set_bl({"192.0.2.10", "10.0.0.0/8", "*.113.9", "172.16.*"});
    CHECK_EQ(bl("192.0.2.10"), true);  // exact
    CHECK_EQ(bl("10.9.9.9"), true);    // cidr
    CHECK_EQ(bl("203.0.113.9"), true); // suffix wildcard
    CHECK_EQ(bl("172.16.4.5"), true);  // prefix wildcard
    CHECK_EQ(bl("198.51.100.5"), false);
    CHECK_EQ(bl("11.9.9.9"), false);

    // Re-running with an empty list must forget everything (no hidden state).
    set_bl({});
    CHECK_EQ(bl("10.9.9.9"), false);

    // ---------------------------------------------------------------- //
    SUITE("is_loopback with no usable socket fd");
    // ---------------------------------------------------------------- //
    // Only the non-socket paths are exercised here: getsockname() must fail and
    // the function must report false rather than guessing.
    CHECK_EQ(BlacklistChecker::is_loopback("127.0.0.1", 554, -1), false);
    CHECK_EQ(BlacklistChecker::is_loopback("192.0.2.10", 8554, -1), false);
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0)
    {
        // A valid fd that is not a socket -> ENOTSOCK -> false.
        CHECK_EQ(BlacklistChecker::is_loopback("127.0.0.1", 554, devnull), false);
        ::close(devnull);
    }

    return tst::summary();
}
