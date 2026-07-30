// Unit tests for URLRewriter (src/utils/url_rewriter.cpp).
//
// URLRewriter is entirely static and has no ServerConfig / socket / DNS
// dependencies -- rewrite_path() is pure string work -- so the only global
// state that has to be pinned per test group is the template list itself
// (set_replace_templates) and the process timezone (shiftTime() goes through
// mktime()/localtime() for the 14-char form). Both are set explicitly below so
// the groups are order-independent.
//
// simplifyToRegex() and shiftTime() are private, so they are exercised through
// rewrite_path() with purpose-built templates.
//
// Build and run:
//   meson setup build -Dtests=true && meson test -C build url_rewriter

#include "test_harness.h"

#include "utils/url_rewriter.h"
#include "core/logger.h"

#include <cstdlib>
#include <ctime>
#include <string>

using json = nlohmann::json;

namespace
{

// Convenience: run rewrite_path with a fresh output string.
std::string rewrite(const std::string &url, bool &ok)
{
    std::string out;
    ok = URLRewriter::rewrite_path(url, out);
    return out;
}

// Same, but only the produced URL matters.
std::string rewrite(const std::string &url)
{
    bool ok = false;
    return rewrite(url, ok);
}

void set_templates(const char *json_text)
{
    URLRewriter::set_replace_templates(json::parse(json_text));
}

void no_templates()
{
    URLRewriter::set_replace_templates(json::array());
}

// ---------------------------------------------------------------------------

void test_prefix_handling()
{
    SUITE("rewrite_path: /rtp/ and /tv/ prefix handling");
    no_templates();

    bool ok = false;

    // The two accepted prefixes are stripped and replaced with "rtsp://".
    CHECK_EQ(rewrite("/rtp/239.1.1.1:5000", ok), std::string("rtsp://239.1.1.1:5000"));
    CHECK(ok);
    CHECK_EQ(rewrite("/tv/192.0.2.10:554/live/1", ok), std::string("rtsp://192.0.2.10:554/live/1"));
    CHECK(ok);

    // Query strings survive the prefix strip untouched when no template matches.
    CHECK_EQ(rewrite("/rtp/239.1.1.1:5000?a=1"), std::string("rtsp://239.1.1.1:5000?a=1"));

    // Anything else is rejected.
    std::string out = "SENTINEL";
    CHECK(!URLRewriter::rewrite_path("/foo/bar", out));
    CHECK_EQ(out, std::string("SENTINEL")); // rtsp_url is left untouched on failure

    // The prefix must sit at offset 0, not merely appear somewhere.
    out = "SENTINEL";
    CHECK(!URLRewriter::rewrite_path("/x/rtp/239.1.1.1:5000", out));
    CHECK_EQ(out, std::string("SENTINEL"));

    // Matching is case sensitive.
    CHECK(!URLRewriter::rewrite_path("/TV/192.0.2.10", out));

    // A bare prefix carries no upstream path and is rejected.
    CHECK(!URLRewriter::rewrite_path("/tv/", out));
    CHECK(!URLRewriter::rewrite_path("/rtp/", out));
    CHECK(!URLRewriter::rewrite_path("/tv", out));

    // One character past the prefix is enough.
    CHECK_EQ(rewrite("/tv/x"), std::string("rtsp://x"));
    CHECK_EQ(rewrite("/rtp/x"), std::string("rtsp://x"));
}

void test_template_gating()
{
    SUITE("rewrite_path: when templates are applied at all");

    set_templates(R"([{"action":"remove","match":"/live/{number}"}])");

    // Templates only run for /tv/ URLs that carry a query string -- this is the
    // documented design ("playback links usually have query params").
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7?x=1"), std::string("rtsp://192.0.2.10?x=1"));
    // No '?' -> the rule is skipped even though it would have matched.
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7"), std::string("rtsp://192.0.2.10/live/7"));
    // /rtp/ never runs templates, query string or not.
    CHECK_EQ(rewrite("/rtp/192.0.2.10/live/7?x=1"), std::string("rtsp://192.0.2.10/live/7?x=1"));

    // An empty array and a null json are both "no rules".
    no_templates();
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7?x=1"), std::string("rtsp://192.0.2.10/live/7?x=1"));
    URLRewriter::set_replace_templates(json());
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7?x=1"), std::string("rtsp://192.0.2.10/live/7?x=1"));
}

void test_placeholder_expansion()
{
    SUITE("simplifyToRegex: {number} / {word} / {any} expansion");

    // Single placeholder, capture group is usable from the replacement.
    set_templates(R"([{"action":"replace","match":"seek={number}","replacement":"S=$1"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?seek=1234"), std::string("rtsp://192.0.2.10/c?S=1234"));

    set_templates(R"([{"action":"replace","match":"id={word}","replacement":"W=$1"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?id=abc"), std::string("rtsp://192.0.2.10/c?W=abc"));

    // {any} expands to a lazy group, so it stops at the following literal.
    set_templates(R"([{"action":"replace","match":"p={any}z","replacement":"A=[$1]"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?p=helloz&q=2"),
             std::string("rtsp://192.0.2.10/c?A=[hello]&q=2"));

    // --- regression: two ADJACENT placeholders ---------------------------
    // The erase length must be the placeholder's own length and the advance
    // must be the replacement's length (5). If the advance is too large the
    // second placeholder is stepped over and survives verbatim, which then
    // makes std::regex reject "{number}" as a bad quantifier.
    set_templates(R"([{"action":"replace","match":"seek={number}{number}","replacement":"S=$1|$2"}])");
    bool ok = false;
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?seek=1234", ok), std::string("rtsp://192.0.2.10/c?S=123|4"));
    CHECK(ok);

    set_templates(R"([{"action":"replace","match":"n={number}{number}{number}","replacement":"[$1][$2][$3]"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?n=12345"), std::string("rtsp://192.0.2.10/c?[123][4][5]"));

    set_templates(R"([{"action":"replace","match":"m={word}{number}n","replacement":"[$1][$2]"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?m=abc12n"), std::string("rtsp://192.0.2.10/c?[abc1][2]"));

    // --- regression: placeholder immediately followed by a literal --------
    // If the erase length is short/long the neighbouring literal is either
    // swallowed or leftover placeholder text is kept.
    set_templates(R"([{"action":"replace","match":"seek={number}s","replacement":"T=$1"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?seek=99s&z=1", ok), std::string("rtsp://192.0.2.10/c?T=99&z=1"));
    CHECK(ok);

    set_templates(R"([{"action":"replace","match":"id={word}-","replacement":"W=$1;"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?id=abc-1"), std::string("rtsp://192.0.2.10/c?W=abc;1"));

    set_templates(R"([{"action":"replace","match":"q={any}!","replacement":"A=$1"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?q=xy!&r=3"), std::string("rtsp://192.0.2.10/c?A=xy&r=3"));

    // '/' is escaped to "\/" (harmless in ECMAScript, but the escape loop must
    // not corrupt the pattern or drop following characters).
    set_templates(R"([{"action":"replace","match":"/live/{number}","replacement":"/L$1"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7?x=1"), std::string("rtsp://192.0.2.10/L7?x=1"));

    set_templates(R"([{"action":"remove","match":"/a/b/c/"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/a/b/c/d?x=1"), std::string("rtsp://192.0.2.10d?x=1"));
}

void test_actions()
{
    SUITE("rewrite_path: remove / replace actions");

    // remove: the shipped "delete redundant field" rule.
    set_templates(R"([{"action":"remove","match":"/{number}_Uni.sdp"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/PLTV/224/3221226109/1_Uni.sdp?a=1"),
             std::string("rtsp://192.0.2.10/PLTV/224/3221226109?a=1"));

    // remove with no placeholders at all.
    set_templates(R"([{"action":"remove","match":"/iptv/import"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/iptv/import/live?x=1"),
             std::string("rtsp://192.0.2.10/live?x=1"));

    // replace, literal -> literal.
    set_templates(R"([{"action":"replace","match":"/iptv/import","replacement":"/iptv"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/iptv/import/live?x=1"),
             std::string("rtsp://192.0.2.10/iptv/live?x=1"));

    // Every occurrence is rewritten, not just the first.
    set_templates(R"([{"action":"remove","match":"&drop={number}"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?a=1&drop=1&b=2&drop=22"),
             std::string("rtsp://192.0.2.10/c?a=1&b=2"));

    // Templates are applied in array order, each seeing the previous result.
    set_templates(R"([
        {"action":"replace","match":"/iptv/import","replacement":"/iptv"},
        {"action":"remove","match":"/{number}_Uni.sdp"}
    ])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/iptv/import/1_Uni.sdp?a=1"),
             std::string("rtsp://192.0.2.10/iptv?a=1"));

    // An unknown action is a silent no-op (no throw, request still succeeds).
    set_templates(R"([{"action":"nosuchaction","match":"/live/{number}"}])");
    bool ok = false;
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7?x=1", ok), std::string("rtsp://192.0.2.10/live/7?x=1"));
    CHECK(ok);
}

void test_timeshift()
{
    SUITE("timeshift action + shiftTime()");

    // Pin the timezone: the 14-char branch round-trips through
    // mktime()/localtime(), which are locale/TZ dependent.
    setenv("TZ", "UTC", 1);
    tzset();

    // --- 14-char YYYYmmddHHMMSS form ---
    set_templates(R"([{"action":"timeshift","match":"playseek={number}-{number}","shift_hours":8}])");
    bool ok = false;
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=20240101120000-20240101130000", ok),
             std::string("rtsp://192.0.2.10/c?playseek=20240101200000-20240101210000"));
    CHECK(ok);

    // Negative shift, rolling back over a month/year boundary.
    set_templates(R"([{"action":"timeshift","match":"playseek={number}-{number}","shift_hours":-8}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=20240101020000-20240101030000"),
             std::string("rtsp://192.0.2.10/c?playseek=20231231180000-20231231190000"));

    // shift_hours == 0 is an identity transform.
    set_templates(R"([{"action":"timeshift","match":"playseek={number}-{number}","shift_hours":0}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=20240101120000-20240101130000"),
             std::string("rtsp://192.0.2.10/c?playseek=20240101120000-20240101130000"));

    // --- epoch form (<= 10 chars) : converted to 14-char UTC wall clock ---
    // 1704110400 == 2024-01-01T12:00:00Z, 1704114000 == 13:00:00Z; +1h each.
    set_templates(R"([{"action":"timeshift","match":"playseek={number}-{number}","shift_hours":1}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=1704110400-1704114000"),
             std::string("rtsp://192.0.2.10/c?playseek=20240101130000-20240101140000"));

    // Tiny epoch values still go down the epoch branch.
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=0-60"),
             std::string("rtsp://192.0.2.10/c?playseek=19700101010000-19700101010100"));

    // --- lengths that are neither 14 nor <= 10 are returned verbatim ---
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=202401011200-202401011300"),
             std::string("rtsp://192.0.2.10/c?playseek=202401011200-202401011300"));
    // 15+ digits likewise.
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=202401011200001-202401011300001"),
             std::string("rtsp://192.0.2.10/c?playseek=202401011200001-202401011300001"));

    // A timeshift rule with a single capture group needs match.size() >= 3 and
    // is therefore a silent no-op -- documented here so the guard is not
    // "fixed" by accident.
    set_templates(R"([{"action":"timeshift","match":"t={number}","shift_hours":1}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?t=1704110400", ok),
             std::string("rtsp://192.0.2.10/c?t=1704110400"));
    CHECK(ok);

    // A pattern that simply does not occur leaves the URL alone.
    set_templates(R"([{"action":"timeshift","match":"playseek={number}-{number}","shift_hours":8}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?other=1"), std::string("rtsp://192.0.2.10/c?other=1"));

    // --- splicing is done by capture-group offset --------------------------
    // A digit inside the pattern's own literal text ('s2=') must survive: only
    // the two captured timestamps are rewritten.
    set_templates(R"([{"action":"timeshift","match":"s2={number}-{number}","shift_hours":1}])");
    std::string got = rewrite("/tv/192.0.2.10/c?s2=2-3", ok);
    CHECK(ok);
    CHECK_EQ(got, std::string("rtsp://192.0.2.10/c?s2=19700101010002-19700101010003"));

    // Both groups grow from 10 to 14 characters, so the second one has to be
    // spliced first for the first one's offset to still be valid.
    set_templates(R"([{"action":"timeshift","match":"playseek={number}-{number}","shift_hours":1}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=0-60&z=9"),
             std::string("rtsp://192.0.2.10/c?playseek=19700101010000-19700101010100&z=9"));

    // timeshift uses regex_search, so only the first occurrence is shifted.
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?playseek=0-60&playseek=0-60"),
             std::string("rtsp://192.0.2.10/c?playseek=19700101010000-19700101010100&playseek=0-60"));
}

void test_regex_metacharacters()
{
    SUITE("simplifyToRegex: regex metacharacters in match patterns stay literal");

    // A perfectly natural config pattern such as "?playseek={number}" has to be
    // read as a literal '?' rather than compiled as the invalid regex
    // "?playseek=(\d+)".
    set_templates(R"([{"action":"remove","match":"?playseek={number}"}])");

    bool ok = false;
    std::string out = "SENTINEL";
    bool threw = false;
    try {
        ok = URLRewriter::rewrite_path("/tv/192.0.2.10/c?playseek=100&b=2", out);
    } catch (...) {
        threw = true;
    }
    // Whatever else happens, the std::regex_error must not escape rewrite_path.
    CHECK(!threw);
    CHECK(ok);
    CHECK_EQ(out, std::string("rtsp://192.0.2.10/c&b=2"));

    // An unbalanced '(' is no longer a broken regex either.
    set_templates(R"([{"action":"remove","match":"(live"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/(live?x=1"), std::string("rtsp://192.0.2.10/?x=1"));

    // '.' is a literal dot, so it no longer matches an arbitrary character.
    set_templates(R"([{"action":"replace","match":"id=1.2","replacement":"OK"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?id=1x2"), std::string("rtsp://192.0.2.10/c?id=1x2"));
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?id=1.2"), std::string("rtsp://192.0.2.10/c?OK"));

    // The rest of the metacharacter set behaves the same way.
    set_templates(R"([{"action":"remove","match":"^a$b|c*d+e[f]g"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?^a$b|c*d+e[f]g&x=1"),
             std::string("rtsp://192.0.2.10/c?&x=1"));

    // Braces that do not spell one of the three documented placeholders are
    // literal braces, not a regex repetition.
    set_templates(R"([{"action":"remove","match":"{foo}"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/{foo}/x?a=1"), std::string("rtsp://192.0.2.10//x?a=1"));

    // A backslash matches a backslash instead of starting an escape sequence.
    set_templates(R"([{"action":"replace","match":"a\\d","replacement":"OK"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?a1&x=1"), std::string("rtsp://192.0.2.10/c?a1&x=1"));
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?a\\d&x=1"), std::string("rtsp://192.0.2.10/c?OK&x=1"));
}

void test_replacement_placeholders()
{
    SUITE("expandReplacement: placeholders inside the 'replacement' string");

    // A placeholder in the replacement means the same as "$1".
    set_templates(R"([{"action":"replace","match":"seek={number}","replacement":"S={number}"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?seek=1234"), std::string("rtsp://192.0.2.10/c?S=1234"));

    // Binding is by ordinal, not by wildcard kind: the first placeholder in the
    // replacement is $1 even when it names a different wildcard than the group
    // that captured the text.
    set_templates(R"([{"action":"replace","match":"a={number}-{word}","replacement":"x={word}/{number}"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?a=12-ab"), std::string("rtsp://192.0.2.10/c?x=12/ab"));

    // Three placeholders bind to $1/$2/$3 in order.
    set_templates(R"([{"action":"replace","match":"t={number}-{word}-{any};","replacement":"[{any}][{number}][{word}]"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?t=12-ab-xy;&z=1"),
             std::string("rtsp://192.0.2.10/c?[12][ab][xy]&z=1"));

    // $1/$2 keep working and may be mixed in; the ordinal is counted over
    // placeholders only, so "{number}" below is still the first one, i.e. $1.
    set_templates(R"([{"action":"replace","match":"p={number}-{number}","replacement":"[$2][{number}]"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?p=1-2"), std::string("rtsp://192.0.2.10/c?[2][1]"));

    // Braces that are not a documented placeholder are copied verbatim.
    set_templates(R"([{"action":"replace","match":"k={word}","replacement":"{foo}=$1"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?k=abc"), std::string("rtsp://192.0.2.10/c?{foo}=abc"));

    // "remove" ignores the replacement field entirely.
    set_templates(R"([{"action":"remove","match":"d={number}","replacement":"{number}"}])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?d=5&x=1"), std::string("rtsp://192.0.2.10/c?&x=1"));
}

void test_malformed_templates()
{
    SUITE("rewrite_path: an unusable template is skipped, not fatal");

    // Since every non-placeholder character is escaped, a match pattern can no
    // longer produce an invalid regex; what is still rejectable is a template
    // with the wrong shape. One of those must not cost the request the rules
    // that would have handled it.

    // "replace" without a "replacement".
    set_templates(R"([
        {"action":"replace","match":"/iptv/import"},
        {"action":"remove","match":"/{number}_Uni.sdp"}
    ])");
    bool ok = false;
    CHECK_EQ(rewrite("/tv/192.0.2.10/iptv/import/1_Uni.sdp?a=1", ok),
             std::string("rtsp://192.0.2.10/iptv/import?a=1"));
    CHECK(ok);

    // "timeshift" without a "shift_hours", and with a non-integer one.
    set_templates(R"([
        {"action":"timeshift","match":"t={number}-{number}"},
        {"action":"timeshift","match":"t={number}-{number}","shift_hours":"8"},
        {"action":"remove","match":"&drop=1"}
    ])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/c?t=0-60&drop=1", ok), std::string("rtsp://192.0.2.10/c?t=0-60"));
    CHECK(ok);

    // Entries missing a string action/match, and entries that are not objects at
    // all, are skipped instead of throwing.
    set_templates(R"([
        {"match":"/live/{number}"},
        {"action":"remove"},
        {"action":5,"match":"/live/{number}"},
        {"action":"remove","match":7},
        "not-an-object",
        null,
        {"action":"remove","match":"/live/{number}"}
    ])");
    CHECK_EQ(rewrite("/tv/192.0.2.10/live/7?x=1", ok), std::string("rtsp://192.0.2.10?x=1"));
    CHECK(ok);
}

void test_shipped_config_chain()
{
    SUITE("default replace_templates chain");

    // The four built-in rules.
    set_templates(R"([
        {"action":"remove","match":"/{number}_Uni.sdp"},
        {"action":"replace","match":"/iptv/import","replacement":"/iptv"},
        {"action":"replace","match":"tvdr={number}-{number}","replacement":"tvdr={number}GMT-{number}GMT"},
        {"action":"timeshift","match":"tvdr={number}GMT-{number}GMT","shift_hours":-8}
    ])");

    setenv("TZ", "UTC", 1);
    tzset();

    // A live URL with no query string is untouched by the chain.
    CHECK_EQ(rewrite("/tv/192.0.2.10/PLTV/224/3221226109/1_Uni.sdp"),
             std::string("rtsp://192.0.2.10/PLTV/224/3221226109/1_Uni.sdp"));

    // With a query string the remove + replace rules do fire.
    CHECK_EQ(rewrite("/tv/192.0.2.10/iptv/import/1_Uni.sdp?a=1"),
             std::string("rtsp://192.0.2.10/iptv?a=1"));

    // The playback chain annotates both timestamps with "GMT" and the timeshift
    // rule that follows then shifts them by -8h.
    bool ok = false;
    CHECK_EQ(rewrite("/tv/192.0.2.10/PLTV/c?tvdr=20240101120000-20240101130000", ok),
             std::string("rtsp://192.0.2.10/PLTV/c?tvdr=20240101040000GMT-20240101050000GMT"));
    CHECK(ok);

    // Same chain with epoch timestamps: 1704110400 == 2024-01-01T12:00:00Z.
    CHECK_EQ(rewrite("/tv/192.0.2.10/PLTV/c?tvdr=1704110400-1704114000"),
             std::string("rtsp://192.0.2.10/PLTV/c?tvdr=20240101040000GMT-20240101050000GMT"));

    // All four rules firing on one URL.
    CHECK_EQ(rewrite("/tv/192.0.2.10/iptv/import/PLTV/1_Uni.sdp?tvdr=20240101120000-20240101130000"),
             std::string("rtsp://192.0.2.10/iptv/PLTV?tvdr=20240101040000GMT-20240101050000GMT"));

    // A URL the playback rules do not apply to is still carried through.
    CHECK_EQ(rewrite("/tv/192.0.2.10/PLTV/c?other=1"), std::string("rtsp://192.0.2.10/PLTV/c?other=1"));
}

} // namespace

int main()
{
    Logger::setLogLevel(LogLevel::ERROR);

    test_prefix_handling();
    test_template_gating();
    test_placeholder_expansion();
    test_actions();
    test_timeshift();
    test_regex_metacharacters();
    test_replacement_placeholders();
    test_malformed_templates();
    test_shipped_config_chain();

    return tst::summary();
}
