// Unit tests for ServerConfig -- config-file half only.
//
// parseCommandLine() is deliberately NOT exercised: it drives getopt_long over
// the global `optind`, which would collide with the test runner's own argv and
// can call exit(0) on `-k`.  Everything here goes through loadFromFile().
//
// NO NETWORK: every host-shaped value below is a literal dotted quad from the
// TEST-NET-1 block (192.0.2.0/24).  ServerConfig::setStunHost only stores the
// string, but keeping the rule everywhere means a future refactor that starts
// resolving it cannot silently make this suite hang.
//
// ServerConfig is entirely static global state, so each group sets the fields
// it observes before asserting on them; the file is order-independent.

#include "test_harness.h"

#include "core/server_config.h"
#include "core/logger.h"
#include "utils/url_rewriter.h"
#include "3rd/json.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// temp-dir plumbing
// ---------------------------------------------------------------------------

static std::string g_dir;
static std::vector<std::string> g_files;

static bool make_temp_dir()
{
    const char *base = std::getenv("TMPDIR");
    std::string tmpl = (base && *base ? std::string(base) : std::string("/tmp"));
    if (tmpl.back() != '/')
        tmpl += '/';
    tmpl += "rtsproxy_cfg_XXXXXX";

    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char *made = mkdtemp(buf.data());
    if (!made)
        return false;
    g_dir = made;
    return true;
}

// Writes `body` to <tempdir>/<name> and returns the absolute path.  Nothing is
// ever written outside the directory mkdtemp handed us.
static std::string write_cfg(const std::string &name, const std::string &body)
{
    std::string path = g_dir + "/" + name;
    std::ofstream ofs(path, std::ios::trunc);
    ofs << body;
    ofs.close();
    g_files.push_back(path);
    return path;
}

static void cleanup_temp_dir()
{
    for (const auto &f : g_files)
        ::unlink(f.c_str());
    if (!g_dir.empty())
        ::rmdir(g_dir.c_str());
}

// ---------------------------------------------------------------------------
// Puts the static config back into its documented compiled-in default state so
// a group can assert "unchanged" against known values.
// ---------------------------------------------------------------------------

static void reset_to_defaults()
{
    ServerConfig::setPort(8554);
    ServerConfig::setNatEnabled(false);
    ServerConfig::setNatMethod("stun");
    ServerConfig::setBufferPoolCount(8192);
    ServerConfig::setBufferPoolBlockSize(2048);
    ServerConfig::setStunPort(19302);
    ServerConfig::setStunHost("192.0.2.1"); // literal IP stand-in, never resolved
    ServerConfig::setToken("");
    ServerConfig::setHttpUpstreamInterface("");
    ServerConfig::setMitmUpstreamInterface("");
    ServerConfig::setListenInterface("");
    ServerConfig::setLogFile("");
    ServerConfig::setLogLines(10000);
    ServerConfig::setStripPadding(false);
    ServerConfig::setWaitKeyframe(false);
    ServerConfig::setWatchdogEnabled(false);
    ServerConfig::setDaemonEnabled(false);
    ServerConfig::setBlacklist({});
}

// ---------------------------------------------------------------------------
// Outcome of one loadFromFile() call, including anything that escaped it.
// ---------------------------------------------------------------------------

struct LoadOutcome
{
    // Tri-state, so "it threw" is never mistaken for "it returned false":
    // "true" | "false" | "threw".
    std::string outcome = "threw";
    bool returned = false;
    bool threw = false;
    std::string kind;   // exception type that escaped, "" if none
    int json_id = -1;   // nlohmann exception id, -1 if not an nlohmann exception
};

static LoadOutcome load(const std::string &path)
{
    LoadOutcome o;
    try
    {
        o.returned = ServerConfig::loadFromFile(path);
        o.outcome = o.returned ? "true" : "false";
    }
    catch (const nlohmann::json::type_error &e)
    {
        o.threw = true;
        o.kind = "nlohmann::json::type_error";
        o.json_id = e.id;
    }
    catch (const nlohmann::json::parse_error &e)
    {
        o.threw = true;
        o.kind = "nlohmann::json::parse_error";
        o.json_id = e.id;
    }
    catch (const nlohmann::json::exception &e)
    {
        o.threw = true;
        o.kind = "nlohmann::json::exception";
        o.json_id = e.id;
    }
    catch (const std::out_of_range &)
    {
        o.threw = true;
        o.kind = "std::out_of_range";
    }
    catch (const std::invalid_argument &)
    {
        o.threw = true;
        o.kind = "std::invalid_argument";
    }
    catch (const std::exception &)
    {
        o.threw = true;
        o.kind = "std::exception";
    }
    return o;
}

// ---------------------------------------------------------------------------

static void test_full_settings_block()
{
    SUITE("loadFromFile: a complete settings block reaches every getter");

    reset_to_defaults();
    Logger::setLogLevel(LogLevel::ERROR);

    std::string path = write_cfg("full.json", R"({
        "settings": {
            "port": 9100,
            "nat_method": "zte",
            "enable_nat": true,
            "buffer_pool_count": 1024,
            "buffer_pool_block_size": 4096,
            "auth_token": "tok-abc",
            "log_file": "/var/log/rtsproxy-unit.log",
            "log_lines": 555,
            "log_level": "warn",
            "strip_padding": true,
            "wait_keyframe": true,
            "watchdog": true,
            "daemon": true,
            "http_interface": "eth0",
            "mitm_interface": "eth1",
            "listen_interface": "lo",
            "stun_host": "192.0.2.10",
            "stun_port": 3478
        }
    })");

    LoadOutcome o = load(path);
    CHECK(!o.threw);
    CHECK_EQ(o.returned, true);

    CHECK_EQ(ServerConfig::getPort(), 9100);
    CHECK_EQ(ServerConfig::getNatMethod(), std::string("zte"));
    CHECK_EQ(ServerConfig::isNatEnabled(), true);
    CHECK_EQ(ServerConfig::getBufferPoolCount(), 1024);
    CHECK_EQ(ServerConfig::getBufferPoolBlockSize(), 4096);
    CHECK_EQ(ServerConfig::getToken(), std::string("tok-abc"));
    CHECK_EQ(ServerConfig::getLogFile(), std::string("/var/log/rtsproxy-unit.log"));
    CHECK_EQ(ServerConfig::getLogLines(), static_cast<size_t>(555));
    CHECK_EQ(ServerConfig::isStripPadding(), true);
    CHECK_EQ(ServerConfig::isWaitKeyframe(), true);
    CHECK_EQ(ServerConfig::isWatchdogEnabled(), true);
    CHECK_EQ(ServerConfig::isDaemonEnabled(), true);
    CHECK_EQ(ServerConfig::getHttpUpstreamInterface(), std::string("eth0"));
    CHECK_EQ(ServerConfig::getMitmUpstreamInterface(), std::string("eth1"));
    CHECK_EQ(ServerConfig::getListenInterface(), std::string("lo"));
    CHECK_EQ(ServerConfig::getStunHost(), std::string("192.0.2.10"));
    CHECK_EQ(ServerConfig::getStunPort(), 3478);

    // "log_level" is routed into Logger, not into a ServerConfig getter.
    CHECK_EQ(static_cast<int>(Logger::getLogLevel()), static_cast<int>(LogLevel::WARN));

    // setLogFile() on ServerConfig only records the path; it must not have
    // handed it to Logger (that is main()'s job), so no file was created.
    std::ifstream probe("/var/log/rtsproxy-unit.log");
    CHECK(!probe.is_open());

    Logger::setLogLevel(LogLevel::ERROR);
}

static void test_partial_settings_leave_the_rest_alone()
{
    SUITE("loadFromFile: keys absent from settings keep their current value");

    reset_to_defaults();
    ServerConfig::setToken("preexisting");
    ServerConfig::setListenInterface("br-lan");

    std::string path = write_cfg("partial.json", R"({
        "settings": { "port": 7001 }
    })");

    LoadOutcome o = load(path);
    CHECK_EQ(o.returned, true);
    CHECK_EQ(ServerConfig::getPort(), 7001);
    CHECK_EQ(ServerConfig::getToken(), std::string("preexisting"));
    CHECK_EQ(ServerConfig::getListenInterface(), std::string("br-lan"));
    CHECK_EQ(ServerConfig::getBufferPoolCount(), 8192);

    // An empty object is valid and is a no-op.
    std::string empty = write_cfg("empty_obj.json", "{}");
    LoadOutcome o2 = load(empty);
    CHECK_EQ(o2.returned, true);
    CHECK_EQ(ServerConfig::getPort(), 7001);

    // "settings" present but not an object is ignored rather than fatal.
    std::string notobj = write_cfg("settings_not_object.json", R"({"settings": 42})");
    LoadOutcome o3 = load(notobj);
    CHECK_EQ(o3.returned, true);
    CHECK(!o3.threw);
    CHECK_EQ(ServerConfig::getPort(), 7001);
}

static void test_comments_are_tolerated()
{
    SUITE("loadFromFile: JSON comments are ignored (ignore_comments=true)");

    reset_to_defaults();

    std::string path = write_cfg("commented.json", R"({
        // a leading line comment
        "settings": {
            "port": 9200,           // trailing line comment
            /* block comment */
            "auth_token": "from-commented-file"
        }
        // trailing comment at the end of the object
    })");

    LoadOutcome o = load(path);
    CHECK(!o.threw);
    CHECK_EQ(o.returned, true);
    CHECK_EQ(ServerConfig::getPort(), 9200);
    CHECK_EQ(ServerConfig::getToken(), std::string("from-commented-file"));
}

static void test_blacklist()
{
    SUITE("loadFromFile: blacklist array reaches getBlacklist");

    reset_to_defaults();
    ServerConfig::setBlacklist({"seed.invalid"});

    // Dotted quads only: BlacklistChecker resolves non-numeric hosts via DNS.
    std::string path = write_cfg("blacklist.json", R"({
        "blacklist": ["192.0.2.20", "192.0.2.21", "198.51.100.7"]
    })");

    LoadOutcome o = load(path);
    CHECK_EQ(o.returned, true);
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(3));
    CHECK_EQ(ServerConfig::getBlacklist()[0], std::string("192.0.2.20"));
    CHECK_EQ(ServerConfig::getBlacklist()[1], std::string("192.0.2.21"));
    CHECK_EQ(ServerConfig::getBlacklist()[2], std::string("198.51.100.7"));

    // Non-string entries are skipped, not fatal.
    std::string mixed = write_cfg("blacklist_mixed.json", R"({
        "blacklist": ["192.0.2.30", 42, null, {"host": "x"}, "192.0.2.31"]
    })");
    LoadOutcome o2 = load(mixed);
    CHECK(!o2.threw);
    CHECK_EQ(o2.returned, true);
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(2));
    CHECK_EQ(ServerConfig::getBlacklist()[0], std::string("192.0.2.30"));
    CHECK_EQ(ServerConfig::getBlacklist()[1], std::string("192.0.2.31"));

    // An explicit empty array clears the list.
    std::string cleared = write_cfg("blacklist_empty.json", R"({"blacklist": []})");
    CHECK_EQ(load(cleared).returned, true);
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(0));

    // A non-array "blacklist" is ignored and leaves the current list intact.
    ServerConfig::setBlacklist({"192.0.2.40"});
    std::string notarr = write_cfg("blacklist_not_array.json",
                                   R"({"blacklist": "192.0.2.99"})");
    LoadOutcome o3 = load(notarr);
    CHECK(!o3.threw);
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(1));
    CHECK_EQ(ServerConfig::getBlacklist()[0], std::string("192.0.2.40"));
}

static void test_replace_templates_reach_url_rewriter()
{
    SUITE("loadFromFile: replace_templates array reaches URLRewriter");

    reset_to_defaults();

    // Baseline: install an empty template set, so the rewrite below is a pure
    // prefix strip. URLRewriter's template store is process-global too.
    std::string none = write_cfg("templates_none.json", R"({"replace_templates": []})");
    CHECK_EQ(load(none).returned, true);

    std::string out;
    CHECK_EQ(URLRewriter::rewrite_path("/tv/192.0.2.10:554/live?a=1", out), true);
    CHECK_EQ(out, std::string("rtsp://192.0.2.10:554/live?a=1"));

    // Now install a real template and observe it take effect.
    std::string path = write_cfg("templates.json", R"({
        "replace_templates": [
            { "action": "replace",
              "match": "/tv/192.0.2.10",
              "replacement": "/tv/192.0.2.11" }
        ]
    })");
    LoadOutcome o = load(path);
    CHECK(!o.threw);
    CHECK_EQ(o.returned, true);

    out.clear();
    CHECK_EQ(URLRewriter::rewrite_path("/tv/192.0.2.10:554/live?a=1", out), true);
    CHECK_EQ(out, std::string("rtsp://192.0.2.11:554/live?a=1"));

    // Templates only apply to /tv/ URLs carrying a query string.
    out.clear();
    CHECK_EQ(URLRewriter::rewrite_path("/tv/192.0.2.10:554/live", out), true);
    CHECK_EQ(out, std::string("rtsp://192.0.2.10:554/live"));

    // Leave the global rewriter empty for anyone running after us.
    CHECK_EQ(load(none).returned, true);
}

static void test_missing_file()
{
    SUITE("loadFromFile: a missing file returns false and changes nothing");

    reset_to_defaults();
    ServerConfig::setBlacklist({"192.0.2.50"});

    std::string missing = g_dir + "/definitely_not_here.json";
    LoadOutcome o = load(missing);

    CHECK(!o.threw);
    CHECK_EQ(o.returned, false);

    CHECK_EQ(ServerConfig::getPort(), 8554);
    CHECK_EQ(ServerConfig::getNatMethod(), std::string("stun"));
    CHECK_EQ(ServerConfig::isNatEnabled(), false);
    CHECK_EQ(ServerConfig::getBufferPoolCount(), 8192);
    CHECK_EQ(ServerConfig::getBufferPoolBlockSize(), 2048);
    CHECK_EQ(ServerConfig::getStunPort(), 19302);
    CHECK_EQ(ServerConfig::getToken(), std::string(""));
    CHECK_EQ(ServerConfig::getLogLines(), static_cast<size_t>(10000));
    CHECK_EQ(ServerConfig::isStripPadding(), false);
    CHECK_EQ(ServerConfig::isWaitKeyframe(), false);
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(1));
}

static void test_broken_json()
{
    SUITE("loadFromFile: syntactically broken JSON returns false, never throws");

    reset_to_defaults();
    ServerConfig::setPort(7777);

    std::string path = write_cfg("broken.json", R"({ "settings": { "port": )");
    LoadOutcome o = load(path);

    CHECK(!o.threw);
    CHECK_EQ(o.returned, false);
    CHECK_EQ(ServerConfig::getPort(), 7777);

    // A file that is not JSON at all behaves the same way.
    std::string garbage = write_cfg("garbage.json", "this is not json at all\n");
    LoadOutcome o2 = load(garbage);
    CHECK(!o2.threw);
    CHECK_EQ(o2.returned, false);
    CHECK_EQ(ServerConfig::getPort(), 7777);

    // An empty file is a parse error too (nlohmann requires a value).
    std::string blank = write_cfg("blank.json", "");
    LoadOutcome o3 = load(blank);
    CHECK(!o3.threw);
    CHECK_EQ(o3.returned, false);
    CHECK_EQ(ServerConfig::getPort(), 7777);
}

static void test_wrong_typed_value()
{
    SUITE("loadFromFile: a wrong-typed settings value");

    // `s["port"].get<int>()` on a JSON *string* raises nlohmann::json::type_error
    // (id 302, "type must be number, but is string"). loadFromFile has to contain
    // that and report it the way it reports a syntax error: return false, throw
    // nothing, name the offending key in the log.

    reset_to_defaults();
    ServerConfig::setPort(6001);
    ServerConfig::setBlacklist({});

    std::string path = write_cfg("wrong_type.json", R"({
        "blacklist": ["192.0.2.60"],
        "settings": { "port": "8554", "auth_token": "never-applied" }
    })");

    LoadOutcome o = load(path);

    CHECK(!o.threw);
    CHECK_EQ(o.kind, std::string(""));
    CHECK_EQ(o.outcome, std::string("false"));

    // A rejected file is applied all-or-nothing: the blacklist is staged and
    // only committed once the settings block has gone through.
    CHECK_EQ(ServerConfig::getPort(), 6001);
    CHECK_EQ(ServerConfig::getToken(), std::string(""));
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(0));

    // Same handling for a wrong-typed bool.
    reset_to_defaults();
    std::string boolpath = write_cfg("wrong_type_bool.json", R"({
        "settings": { "strip_padding": "yes" }
    })");
    LoadOutcome ob = load(boolpath);
    CHECK(!ob.threw);
    CHECK_EQ(ob.outcome, std::string("false"));
    CHECK_EQ(ServerConfig::isStripPadding(), false);
}

static void test_out_of_range_and_invalid_enum_values()
{
    SUITE("loadFromFile: values that are well-typed but rejected by the setters");

    reset_to_defaults();
    ServerConfig::setPort(6002);

    // setPort() throws std::out_of_range, which is not a JSON exception at all.
    std::string bad_port = write_cfg("bad_port.json", R"({
        "settings": { "port": 70000 }
    })");
    LoadOutcome o = load(bad_port);
    CHECK(!o.threw);
    CHECK_EQ(o.outcome, std::string("false"));
    CHECK_EQ(ServerConfig::getPort(), 6002);

    // setNatMethod() throws std::invalid_argument for anything but stun/zte.
    reset_to_defaults();
    std::string bad_method = write_cfg("bad_nat_method.json", R"({
        "settings": { "nat_method": "upnp" }
    })");
    LoadOutcome o2 = load(bad_method);
    CHECK(!o2.threw);
    CHECK_EQ(o2.outcome, std::string("false"));
    CHECK_EQ(ServerConfig::getNatMethod(), std::string("stun"));

    // Boundary values that are legal must be accepted without complaint.
    reset_to_defaults();
    std::string edges = write_cfg("edges.json", R"({
        "settings": {
            "port": 65535,
            "buffer_pool_count": 1,
            "buffer_pool_block_size": 64,
            "stun_port": 1
        }
    })");
    LoadOutcome o3 = load(edges);
    CHECK(!o3.threw);
    CHECK_EQ(o3.returned, true);
    CHECK_EQ(ServerConfig::getPort(), 65535);
    CHECK_EQ(ServerConfig::getBufferPoolCount(), 1);
    CHECK_EQ(ServerConfig::getBufferPoolBlockSize(), 64);
    CHECK_EQ(ServerConfig::getStunPort(), 1);
}

static void test_rejected_file_is_applied_all_or_nothing()
{
    SUITE("loadFromFile: a rejected file leaves no half-applied state behind");

    reset_to_defaults();
    ServerConfig::setPort(6003);
    ServerConfig::setToken("keep-me");
    ServerConfig::setBlacklist({"192.0.2.70"});

    // Every one of these is valid except nat_method, and "port" is applied
    // before it; none of them may survive the rejection.
    std::string path = write_cfg("mixed_valid_and_bad.json", R"({
        "blacklist": ["192.0.2.71", "192.0.2.72"],
        "settings": {
            "port": 9300,
            "nat_method": "upnp",
            "auth_token": "never-applied",
            "buffer_pool_count": 4096
        },
        "replace_templates": [
            { "action": "replace", "match": "/tv/192.0.2.80", "replacement": "/tv/192.0.2.81" }
        ]
    })");

    LoadOutcome o = load(path);
    CHECK(!o.threw);
    CHECK_EQ(o.outcome, std::string("false"));

    CHECK_EQ(ServerConfig::getPort(), 6003);
    CHECK_EQ(ServerConfig::getToken(), std::string("keep-me"));
    CHECK_EQ(ServerConfig::getBufferPoolCount(), 8192);
    CHECK_EQ(ServerConfig::getBlacklist().size(), static_cast<size_t>(1));
    CHECK_EQ(ServerConfig::getBlacklist()[0], std::string("192.0.2.70"));

    // replace_templates sits after the settings block, so it is not installed
    // either: the rewriter still has whatever the previous load left there.
    std::string out;
    CHECK_EQ(URLRewriter::rewrite_path("/tv/192.0.2.80:554/live?a=1", out), true);
    CHECK_EQ(out, std::string("rtsp://192.0.2.80:554/live?a=1"));

    // log_level is applied mid-block, so it has to be rolled back too.
    reset_to_defaults();
    Logger::setLogLevel(LogLevel::ERROR);
    std::string late = write_cfg("bad_after_log_level.json", R"({
        "settings": { "log_level": "debug", "stun_port": 0 }
    })");
    LoadOutcome o2 = load(late);
    CHECK(!o2.threw);
    CHECK_EQ(o2.outcome, std::string("false"));
    CHECK_EQ(static_cast<int>(Logger::getLogLevel()), static_cast<int>(LogLevel::ERROR));
    CHECK_EQ(ServerConfig::getStunPort(), 19302);
}

static void test_unusable_replace_templates_are_dropped()
{
    SUITE("loadFromFile: replace_templates entries that can never fire are dropped");

    reset_to_defaults();

    // Only the last entry is usable; the rest have no string action/match, so
    // rewrite_path could never apply them anyway.
    std::string path = write_cfg("templates_junk.json", R"({
        "replace_templates": [
            "not-an-object",
            42,
            { "action": "replace" },
            { "match": "/tv/192.0.2.90" },
            { "action": 5, "match": "/tv/192.0.2.90" },
            { "action": "replace",
              "match": "/tv/192.0.2.90",
              "replacement": "/tv/192.0.2.91" }
        ]
    })");

    LoadOutcome o = load(path);
    CHECK(!o.threw);
    CHECK_EQ(o.returned, true);

    std::string out;
    CHECK_EQ(URLRewriter::rewrite_path("/tv/192.0.2.90:554/live?a=1", out), true);
    CHECK_EQ(out, std::string("rtsp://192.0.2.91:554/live?a=1"));

    // Leave the global rewriter empty for anyone running after us.
    std::string none = write_cfg("templates_none2.json", R"({"replace_templates": []})");
    CHECK_EQ(load(none).returned, true);
}

static void test_json_path_accessor()
{
    SUITE("json path accessor");

    ServerConfig::setJsonPath("config.json");
    CHECK_EQ(ServerConfig::getJsonPath(), std::string("config.json"));

    std::string p = g_dir + "/somewhere.json";
    ServerConfig::setJsonPath(p);
    CHECK_EQ(ServerConfig::getJsonPath(), p);

    // setJsonPath must not itself touch the filesystem.
    std::ifstream probe(p);
    CHECK(!probe.is_open());

    ServerConfig::setJsonPath("config.json");
}

int main()
{
    if (!make_temp_dir())
    {
        std::printf("FATAL: could not create a temp directory\n");
        return 1;
    }
    std::printf("temp dir: %s\n\n", g_dir.c_str());

    // Keep the module's own logging out of the report.
    Logger::setLogLevel(LogLevel::ERROR);

    test_full_settings_block();
    test_partial_settings_leave_the_rest_alone();
    test_comments_are_tolerated();
    test_blacklist();
    test_replace_templates_reach_url_rewriter();
    test_missing_file();
    test_broken_json();
    test_wrong_typed_value();
    test_out_of_range_and_invalid_enum_values();
    test_rejected_file_is_applied_all_or_nothing();
    test_unusable_replace_templates_are_dropped();
    test_json_path_accessor();

    reset_to_defaults();
    cleanup_temp_dir();

    return tst::summary();
}
