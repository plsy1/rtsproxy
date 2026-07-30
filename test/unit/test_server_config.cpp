#include "test_harness.h"

#include "core/server_config.h"
#include "core/logger.h"
#include "utils/url_rewriter.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{

void reset()
{
    ServerConfig::setPort(8554);
    ServerConfig::setNatEnabled(false);
    ServerConfig::setNatMethod("stun");
    ServerConfig::setBufferPoolCount(8192);
    ServerConfig::setBufferPoolBlockSize(2048);
    ServerConfig::setStunPort(19302);
    ServerConfig::setStunHost("stun.l.google.com");
    ServerConfig::setToken("");
    ServerConfig::setUpstreamRoutes({});
    ServerConfig::setListenInterface("");
    ServerConfig::setLogFile("");
    ServerConfig::setLogLines(10000);
    ServerConfig::setStripPadding(false);
    ServerConfig::setWaitKeyframe(false);
    ServerConfig::setWatchdogEnabled(false);
    ServerConfig::setDaemonEnabled(false);
    ServerConfig::setBlacklist({});
}

void test_accessors()
{
    SUITE("server config accessors");
    reset();

    ServerConfig::setPort(9554);
    ServerConfig::setNatEnabled(true);
    ServerConfig::setNatMethod("zte");
    ServerConfig::setBufferPoolCount(4096);
    ServerConfig::setBufferPoolBlockSize(4096);
    ServerConfig::setStunPort(3478);
    ServerConfig::setStunHost("stun.example");
    ServerConfig::setToken("secret");
    ServerConfig::setListenInterface("br-lan");
    ServerConfig::setLogFile("/tmp/rtsproxy.log");
    ServerConfig::setLogLines(1234);
    ServerConfig::setStripPadding(true);
    ServerConfig::setWaitKeyframe(true);
    ServerConfig::setWatchdogEnabled(true);
    ServerConfig::setDaemonEnabled(true);
    ServerConfig::setBlacklist({"192.0.2.0/24"});
    ServerConfig::addBlacklist("198.51.100.1");

    CHECK_EQ(ServerConfig::getPort(), 9554);
    CHECK(ServerConfig::isNatEnabled());
    CHECK_EQ(ServerConfig::getNatMethod(), std::string("zte"));
    CHECK_EQ(ServerConfig::getBufferPoolCount(), 4096);
    CHECK_EQ(ServerConfig::getBufferPoolBlockSize(), 4096);
    CHECK_EQ(ServerConfig::getStunPort(), 3478);
    CHECK_EQ(ServerConfig::getStunHost(), std::string("stun.example"));
    CHECK_EQ(ServerConfig::getToken(), std::string("secret"));
    CHECK_EQ(ServerConfig::getListenInterface(), std::string("br-lan"));
    CHECK_EQ(ServerConfig::getLogFile(), std::string("/tmp/rtsproxy.log"));
    CHECK_EQ(ServerConfig::getLogLines(), size_t(1234));
    CHECK(ServerConfig::isStripPadding());
    CHECK(ServerConfig::isWaitKeyframe());
    CHECK(ServerConfig::isWatchdogEnabled());
    CHECK(ServerConfig::isDaemonEnabled());
    CHECK_EQ(ServerConfig::getBlacklist().size(), size_t(2));
}

void test_validation()
{
    SUITE("server config validation");

    bool bad_port = false;
    try { ServerConfig::setPort(0); }
    catch (const std::out_of_range &) { bad_port = true; }
    CHECK(bad_port);

    bool bad_method = false;
    try { ServerConfig::setNatMethod("invalid"); }
    catch (const std::invalid_argument &) { bad_method = true; }
    CHECK(bad_method);
}

void test_upstream_routes()
{
    SUITE("upstream CIDR routes");

    ServerConfig::setUpstreamRoutes({
        "0.0.0.0/0,wan",
        "10.0.0.0/8,eth1",
        "10.1.2.0/24,eth2"
    });
    CHECK_EQ(ServerConfig::getUpstreamInterface("10.1.2.99", "fallback"),
             std::string("eth2"));
    CHECK_EQ(ServerConfig::getUpstreamInterface("10.9.0.1", "fallback"),
             std::string("eth1"));
    CHECK_EQ(ServerConfig::getUpstreamInterface("203.0.113.5", "fallback"),
             std::string("wan"));

    ServerConfig::setUpstreamRoutes({"192.0.2.0/24,iptv"});
    CHECK_EQ(ServerConfig::getUpstreamInterface("198.51.100.1", "legacy0"),
             std::string("legacy0"));

    bool rejected = false;
    try { ServerConfig::addUpstreamRoute("192.0.2.0/99,eth0"); }
    catch (const std::invalid_argument &) { rejected = true; }
    CHECK(rejected);
    CHECK_EQ(ServerConfig::getUpstreamRoutes().size(), size_t(1));
}

void test_toml_config()
{
    SUITE("TOML configuration");
    reset();

    const std::string path = "/tmp/rtsproxy_server_config_test.toml";
    {
        std::ofstream out(path);
        out <<
            "[settings]\n"
            "port = 10554\n"
            "enable_nat = true\n"
            "nat_method = \"zte\"\n"
            "log_level = \"debug\"\n"
            "\n"
            "[security]\n"
            "blacklist = [\n"
            "  \"127.0.0.0/8\",\n"
            "  \"192.0.2.0/24\", # comments are allowed\n"
            "]\n"
            "\n"
            "[[upstream_routes]]\n"
            "cidr = \"192.0.2.0/24\"\n"
            "interface = \"eth9\"\n"
            "\n"
            "[[rewrite_rules]]\n"
            "action = \"replace\"\n"
            "match = \"/old\"\n"
            "replacement = \"/new\"\n";
    }

    CHECK(ServerConfig::loadTomlFile(path));
    CHECK_EQ(ServerConfig::getPort(), 10554);
    CHECK(ServerConfig::isNatEnabled());
    CHECK_EQ(ServerConfig::getNatMethod(), std::string("zte"));
    CHECK_EQ(ServerConfig::getBlacklist().size(), size_t(2));
    CHECK_EQ(ServerConfig::getUpstreamInterface("192.0.2.5", ""),
             std::string("eth9"));
    std::string rewritten;
    CHECK(URLRewriter::rewrite_path("/tv/host/old?x=1", rewritten));
    CHECK_EQ(rewritten, std::string("rtsp://host/new?x=1"));

    {
        std::ofstream out(path);
        out << "[settings]\nunknown = 1\n";
    }
    CHECK(!ServerConfig::loadTomlFile(path));
    ::unlink(path.c_str());
}

} // namespace

int main()
{
    Logger::setLogLevel(LogLevel::ERROR);
    test_accessors();
    test_validation();
    test_upstream_routes();
    test_toml_config();
    reset();
    return tst::summary();
}
