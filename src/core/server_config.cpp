#include "core/server_config.h"
#include "utils/url_rewriter.h"
#include "core/logger.h"
#include <iostream>
#include <signal.h>
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#include <getopt.h>
#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>
#include <map>
#include <algorithm>
#include <cctype>

int ServerConfig::port = 8554;
bool ServerConfig::enable_nat = false;
std::string ServerConfig::nat_method = "stun";
int ServerConfig::buffer_pool_count = 8192;
int ServerConfig::buffer_pool_block_size = 2048;
int ServerConfig::stun_server_port = 19302;
std::string ServerConfig::stun_server_host = "stun.l.google.com";
std::string ServerConfig::config_path = "";
std::string ServerConfig::auth_token = "";
std::vector<std::string> ServerConfig::upstream_routes = {};
std::string ServerConfig::listen_interface = "";
std::string ServerConfig::log_file_path = "";
size_t ServerConfig::log_file_lines = 10000;
bool ServerConfig::strip_padding = false;
bool ServerConfig::wait_keyframe = false;
bool ServerConfig::watchdog_enabled = false;
bool ServerConfig::daemon_enabled = false;
std::vector<std::string> ServerConfig::blacklist = {
    "127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12",
    "192.168.0.0/16", "169.254.0.0/16"
};

namespace
{
using TomlTable = std::map<std::string, std::string>;

std::string trim(const std::string &value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string strip_toml_comment(const std::string &line)
{
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];
        if (quoted && c == '\\' && !escaped)
        {
            escaped = true;
            continue;
        }
        if (c == '"' && !escaped) quoted = !quoted;
        if (c == '#' && !quoted) return line.substr(0, i);
        escaped = false;
    }
    return line;
}

std::string parse_toml_string(const std::string &raw)
{
    std::string value = trim(raw);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        throw std::invalid_argument("expected quoted string");
    std::string result;
    for (size_t i = 1; i + 1 < value.size(); ++i)
    {
        char c = value[i];
        if (c != '\\')
        {
            result.push_back(c);
            continue;
        }
        if (++i + 1 >= value.size())
            throw std::invalid_argument("unterminated string escape");
        switch (value[i])
        {
        case '\\': result.push_back('\\'); break;
        case '"': result.push_back('"'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        default: throw std::invalid_argument("unsupported string escape");
        }
    }
    return result;
}

long long parse_toml_integer(const std::string &raw)
{
    std::string value = trim(raw);
    if (value.empty()) throw std::invalid_argument("expected integer");
    size_t parsed = 0;
    long long result = std::stoll(value, &parsed);
    if (parsed != value.size())
        throw std::invalid_argument("invalid integer");
    return result;
}

bool parse_toml_bool(const std::string &raw)
{
    std::string value = trim(raw);
    if (value == "true") return true;
    if (value == "false") return false;
    throw std::invalid_argument("expected true or false");
}

std::vector<std::string> parse_toml_string_array(const std::string &raw)
{
    std::string value = trim(raw);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']')
        throw std::invalid_argument("expected string array");
    std::vector<std::string> result;
    size_t pos = 1;
    while (pos + 1 < value.size())
    {
        while (pos + 1 < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[pos])) || value[pos] == ','))
            ++pos;
        if (pos + 1 >= value.size()) break;
        if (value[pos] != '"') throw std::invalid_argument("array values must be strings");
        size_t end = pos + 1;
        bool escaped = false;
        for (; end < value.size(); ++end)
        {
            if (value[end] == '"' && !escaped) break;
            escaped = value[end] == '\\' && !escaped;
            if (value[end] != '\\') escaped = false;
        }
        if (end >= value.size()) throw std::invalid_argument("unterminated array string");
        result.push_back(parse_toml_string(value.substr(pos, end - pos + 1)));
        pos = end + 1;
    }
    return result;
}

void require_only(const TomlTable &table, const std::vector<std::string> &allowed,
                  const std::string &section)
{
    for (const auto &item : table)
    {
        if (std::find(allowed.begin(), allowed.end(), item.first) == allowed.end())
            throw std::invalid_argument("unknown key '" + item.first + "' in " + section);
    }
}
} // namespace

void ServerConfig::parseCommandLine(int argc, char *argv[])
{
    struct option long_options[] = {
        {"port", required_argument, nullptr, 'p'},
        {"help", no_argument, nullptr, 'h'},
        {"enable-nat", no_argument, nullptr, 'n'},
        {"nat-method", required_argument, nullptr, 0},
        {"buffer-pool-count", required_argument, nullptr, 'b'},
        {"buffer-pool-block-size", required_argument, nullptr, 's'},
        {"auth-token", required_argument, nullptr, 't'},
        {"upstream-route", required_argument, nullptr, 0},
        {"listen-interface", required_argument, nullptr, 'l'},
        {"config", required_argument, nullptr, 'c'},
        {"stun-port", required_argument, nullptr, 0},
        {"stun-host", required_argument, nullptr, 0},
        {"kill", no_argument, nullptr, 'k'},
        {"daemon", no_argument, nullptr, 'd'},
        {"watchdog", no_argument, nullptr, 'w'},
        {"log-file", required_argument, nullptr, 0},
        {"log-lines", required_argument, nullptr, 0},
        {"log-level", required_argument, nullptr, 0},
        {"strip-padding", no_argument, nullptr, 0},
        {"wait-keyframe", no_argument, nullptr, 0},
        {"clear-blacklist", no_argument, nullptr, 0},
        {"clear-rewrite-rules", no_argument, nullptr, 0},
        {"blacklist", required_argument, nullptr, 0},
        {"rewrite-remove", required_argument, nullptr, 0},
        {"rewrite-match", required_argument, nullptr, 0},
        {"rewrite-with", required_argument, nullptr, 0},
        {"timeshift-match", required_argument, nullptr, 0},
        {"timeshift-hours", required_argument, nullptr, 0},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    int longindex = -1;
    bool config_requested = false;
    bool help_requested = false;
    optind = 1;
    while ((opt = getopt_long(argc, argv, "hp:nb:s:t:c:l:kdw", long_options, &longindex)) != -1)
    {
        if (opt == 'h')
            help_requested = true;
        else if (opt == 'c')
        {
            setConfigPath(optarg);
            config_requested = true;
        }
    }

    if (help_requested)
    {
        printUsage(argv[0]);
        exit(0);
    }

    if (config_requested)
    {
        if (!loadTomlFile(getConfigPath()))
            throw std::runtime_error("TOML config '" + getConfigPath() + "' was rejected");
        return;
    }

    optind = 1;
    std::string replace_match;
    std::string timeshift_match;
    while ((opt = getopt_long(argc, argv, "hp:nb:s:t:c:l:kdw", long_options, &longindex)) != -1) {
        switch (opt) {
        case 'h': printUsage(argv[0]); exit(0);
        case 'p': setPort(std::stoi(optarg)); break;
        case 'n': setNatEnabled(true); break;
        case 'b': setBufferPoolCount(std::stoi(optarg)); break;
        case 's': setBufferPoolBlockSize(std::stoi(optarg)); break;
        case 't': setToken(optarg); break;
        case 'c': break;
        case 'l': setListenInterface(optarg); break;
        case 'k': kill_previous_instance(); exit(0);
        case 'd': setDaemonEnabled(true); break;
        case 'w': setWatchdogEnabled(true); break;
        case 0:
            if (longindex >= 0 && strcmp(long_options[longindex].name, "nat-method") == 0) setNatMethod(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "stun-host") == 0) setStunHost(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "stun-port") == 0) setStunPort(std::stoi(optarg));
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "upstream-route") == 0) addUpstreamRoute(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "log-file") == 0) setLogFile(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "log-lines") == 0) setLogLines(std::stoull(optarg));
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "log-level") == 0) {
                std::string level = optarg;
                if (level == "error") Logger::setLogLevel(LogLevel::ERROR);
                else if (level == "warn") Logger::setLogLevel(LogLevel::WARN);
                else if (level == "info") Logger::setLogLevel(LogLevel::INFO);
                else if (level == "debug") Logger::setLogLevel(LogLevel::DEBUG);
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "strip-padding") == 0) setStripPadding(true);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "wait-keyframe") == 0) setWaitKeyframe(true);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "clear-blacklist") == 0) setBlacklist({});
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "clear-rewrite-rules") == 0) URLRewriter::clear_templates();
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "blacklist") == 0) addBlacklist(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "rewrite-remove") == 0) URLRewriter::add_remove_rule(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "rewrite-match") == 0) replace_match = optarg;
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "rewrite-with") == 0) {
                if (replace_match.empty()) throw std::invalid_argument("--rewrite-with requires a preceding --rewrite-match");
                URLRewriter::add_replace_rule(replace_match, optarg);
                replace_match.clear();
            }
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "timeshift-match") == 0) timeshift_match = optarg;
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "timeshift-hours") == 0) {
                if (timeshift_match.empty()) throw std::invalid_argument("--timeshift-hours requires a preceding --timeshift-match");
                URLRewriter::add_timeshift_rule(timeshift_match, std::stoi(optarg));
                timeshift_match.clear();
            }
            break;
        default:
            printUsage(argv[0]);
            exit(EXIT_FAILURE);
        }
    }
}

void ServerConfig::setPort(int p)
{
    if (p < 1 || p > 65535)
        throw std::out_of_range("listen port must be in range 1..65535");
    port = p;
}

void ServerConfig::setNatEnabled(bool enable)
{
    enable_nat = enable;
}

void ServerConfig::setNatMethod(const std::string &method)
{
    if (method != "stun" && method != "zte")
        throw std::invalid_argument("nat_method must be 'stun' or 'zte'");
    nat_method = method;
}

void ServerConfig::setBufferPoolCount(int count)
{
    if (count < 1 || count > 262144)
        throw std::out_of_range("buffer_pool_count must be in range 1..262144");
    buffer_pool_count = count;
}

void ServerConfig::setBufferPoolBlockSize(int size)
{
    if (size < 64 || size > 65536)
        throw std::out_of_range("buffer_pool_block_size must be in range 64..65536");
    buffer_pool_block_size = size;
}

void ServerConfig::setStunPort(int port)
{
    if (port < 1 || port > 65535)
        throw std::out_of_range("STUN port must be in range 1..65535");
    stun_server_port = port;
}

void ServerConfig::setStunHost(std::string host)
{
    stun_server_host = host;
}

void ServerConfig::setConfigPath(std::string path)
{
    config_path = std::move(path);
}

void ServerConfig::setToken(std::string token)
{
    auth_token = token;
}

void ServerConfig::addUpstreamRoute(const std::string &rule)
{
    size_t comma = rule.find(',');
    if (comma == std::string::npos || comma == 0 || comma + 1 >= rule.size() ||
        rule.find(',', comma + 1) != std::string::npos)
        throw std::invalid_argument("upstream route must be CIDR,interface");

    std::string cidr = rule.substr(0, comma);
    std::string iface = rule.substr(comma + 1);
    size_t slash = cidr.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= cidr.size())
        throw std::invalid_argument("upstream route CIDR must include /prefix");

    in_addr network{};
    if (inet_pton(AF_INET, cidr.substr(0, slash).c_str(), &network) != 1)
        throw std::invalid_argument("upstream route contains invalid IPv4 network");
    std::string prefix_text = cidr.substr(slash + 1);
    if (prefix_text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("upstream route contains invalid prefix");
    int prefix = std::stoi(prefix_text);
    if (prefix < 0 || prefix > 32)
        throw std::invalid_argument("upstream route prefix must be in range 0..32");
    if (iface.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument("upstream route interface must not contain whitespace");

    upstream_routes.push_back(cidr + "," + iface);
}

void ServerConfig::setUpstreamRoutes(const std::vector<std::string> &rules)
{
    std::vector<std::string> old = upstream_routes;
    upstream_routes.clear();
    try
    {
        for (const auto &rule : rules)
            addUpstreamRoute(rule);
    }
    catch (...)
    {
        upstream_routes = std::move(old);
        throw;
    }
}

void ServerConfig::setListenInterface(std::string iface)
{
    listen_interface = iface;
}

void ServerConfig::setLogFile(std::string path)
{
    log_file_path = path;
}

void ServerConfig::setLogLines(size_t lines)
{
    log_file_lines = lines;
}
void ServerConfig::setBlacklist(const std::vector<std::string> &list)
{
    blacklist = list;
}
void ServerConfig::addBlacklist(const std::string &entry)
{
    if (!entry.empty()) blacklist.push_back(entry);
}
void ServerConfig::setStripPadding(bool enable)
{
    strip_padding = enable;
}
void ServerConfig::setWaitKeyframe(bool enable)
{
    wait_keyframe = enable;
}
void ServerConfig::setWatchdogEnabled(bool enable)
{
    watchdog_enabled = enable;
}
void ServerConfig::setDaemonEnabled(bool enable)
{
    daemon_enabled = enable;
}

int ServerConfig::getPort()
{
    return port;
}


bool ServerConfig::isNatEnabled()
{
    return enable_nat;
}

std::string ServerConfig::getNatMethod()
{
    return nat_method;
}

int ServerConfig::getBufferPoolCount()
{
    return buffer_pool_count;
}

int ServerConfig::getBufferPoolBlockSize()
{
    return buffer_pool_block_size;
}

int ServerConfig::getStunPort()
{
    return stun_server_port;
}

std::string ServerConfig::getStunHost()
{
    return stun_server_host;
}

std::string ServerConfig::getConfigPath()
{
    return config_path;
}

std::string ServerConfig::getToken()
{
    return auth_token;
}

std::string ServerConfig::getUpstreamInterface(const std::string &ip,
                                               const std::string &fallback)
{
    in_addr target{};
    if (inet_pton(AF_INET, ip.c_str(), &target) != 1)
        return fallback;

    int best_prefix = -1;
    std::string best_interface = fallback;
    uint32_t target_host = ntohl(target.s_addr);
    for (const auto &rule : upstream_routes)
    {
        size_t comma = rule.find(',');
        size_t slash = rule.find('/');
        if (comma == std::string::npos || slash == std::string::npos || slash > comma)
            continue;
        in_addr network{};
        if (inet_pton(AF_INET, rule.substr(0, slash).c_str(), &network) != 1)
            continue;
        int prefix = std::stoi(rule.substr(slash + 1, comma - slash - 1));
        uint32_t mask = prefix == 0 ? 0 : (0xffffffffU << (32 - prefix));
        if ((target_host & mask) == (ntohl(network.s_addr) & mask) &&
            prefix > best_prefix)
        {
            best_prefix = prefix;
            best_interface = rule.substr(comma + 1);
        }
    }
    return best_interface;
}

const std::vector<std::string>& ServerConfig::getUpstreamRoutes()
{
    return upstream_routes;
}

std::string ServerConfig::getListenInterface()
{
    return listen_interface;
}

std::string ServerConfig::getLogFile()
{
    return log_file_path;
}

size_t ServerConfig::getLogLines()
{
    return log_file_lines;
}
const std::vector<std::string>& ServerConfig::getBlacklist()
{
    return blacklist;
}
bool ServerConfig::isStripPadding()
{
    return strip_padding;
}
bool ServerConfig::isWaitKeyframe()
{
    return wait_keyframe;
}
bool ServerConfig::isWatchdogEnabled()
{
    return watchdog_enabled;
}
bool ServerConfig::isDaemonEnabled()
{
    return daemon_enabled;
}

void ServerConfig::printUsage(const std::string &program_name)
{
    std::cout << "Usage: " << program_name << " [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help                    Show this help and exit" << std::endl;
    std::cout << "  -p, --port            <port>  Set HTTP server port (default: " << port << ")" << std::endl;
    std::cout << "  -n, --enable-nat              Enable NAT (default: " << (enable_nat ? "enabled" : "disabled") << ")" << std::endl;
    std::cout << "      --nat-method      <method> Set NAT method: stun, zte (default: " << nat_method << ")" << std::endl;
    std::cout << "  -b, --buffer-pool-count <count> Set BufferPool block count (default: " << buffer_pool_count << ")" << std::endl;
    std::cout << "  -s, --buffer-pool-block-size <size>  Set BufferPool block size (default: " << buffer_pool_block_size << ")" << std::endl;
    std::cout << "  -t, --auth-token      <token> Set auth token for HTTP API and RTSP access (default: none)" << std::endl;
    std::cout << "      --upstream-route  <CIDR,iface> Route an upstream CIDR via an interface (repeatable)" << std::endl;
    std::cout << "  -l, --listen-interface <iface> Set interface to listen on" << std::endl;
    std::cout << "  -c, --config          <path>  Load settings from a TOML file" << std::endl;
    std::cout << "  -d, --daemon                  Run rtsproxy in the background" << std::endl;
    std::cout << "  -w, --watchdog                Run in watchdog mode (auto-restart on crash)" << std::endl;
    std::cout << "      --log-file        <path>  Write logs to a specific file instead of stdout" << std::endl;
    std::cout << "      --log-lines       <count> Set maximum log file lines (default: 10000)" << std::endl;
    std::cout << "      --log-level       <level> Set log level: error, warn, info, debug (default: info)" << std::endl;
    std::cout << "  -k, --kill                    Kill the running rtsproxy instance" << std::endl;
    std::cout << "      --stun-host       <host>  Set STUN server host (default: " << stun_server_host << ")" << std::endl;
    std::cout << "      --stun-port       <port>  Set STUN server port (default: " << stun_server_port << ")" << std::endl;
    std::cout << "      --strip-padding           Strip RTP padding and TS null packets" << std::endl;
    std::cout << "      --wait-keyframe           Wait for keyframe before starting relay (Anti-Greenscreen)" << std::endl;
    std::cout << "      --clear-blacklist         Remove the built-in blacklist" << std::endl;
    std::cout << "      --clear-rewrite-rules     Remove the built-in URL rewrite rules" << std::endl;
    std::cout << "      --blacklist       <entry> Add a blacklist entry (repeatable)" << std::endl;
    std::cout << "      --rewrite-remove  <match> Add a URL removal rule" << std::endl;
    std::cout << "      --rewrite-match   <match> Begin a URL replacement rule" << std::endl;
    std::cout << "      --rewrite-with    <value> Complete the preceding replacement rule" << std::endl;
    std::cout << "      --timeshift-match <match> Begin a timeshift rule" << std::endl;
    std::cout << "      --timeshift-hours <hours> Complete the preceding timeshift rule" << std::endl;
}

bool ServerConfig::loadTomlFile(const std::string &path)
{
    std::ifstream input(path);
    if (!input.is_open())
        return false;

    TomlTable settings;
    TomlTable security;
    std::vector<TomlTable> routes;
    std::vector<TomlTable> rewrites;
    TomlTable *current = nullptr;
    std::string section;
    std::string line;
    size_t line_number = 0;

    try
    {
        while (std::getline(input, line))
        {
            ++line_number;
            line = trim(strip_toml_comment(line));
            if (line.empty()) continue;

            if (line.front() == '[')
            {
                if (line.size() >= 4 && line.compare(0, 2, "[[") == 0 &&
                    line.compare(line.size() - 2, 2, "]]") == 0)
                {
                    section = trim(line.substr(2, line.size() - 4));
                    if (section == "upstream_routes")
                    {
                        routes.emplace_back();
                        current = &routes.back();
                    }
                    else if (section == "rewrite_rules")
                    {
                        rewrites.emplace_back();
                        current = &rewrites.back();
                    }
                    else
                        throw std::invalid_argument("unknown array section [[" + section + "]]");
                }
                else if (line.back() == ']')
                {
                    section = trim(line.substr(1, line.size() - 2));
                    if (section == "settings") current = &settings;
                    else if (section == "security") current = &security;
                    else throw std::invalid_argument("unknown section [" + section + "]");
                }
                else
                    throw std::invalid_argument("malformed section header");
                continue;
            }

            if (!current)
                throw std::invalid_argument("key must be inside a section");
            size_t equals = line.find('=');
            if (equals == std::string::npos)
                throw std::invalid_argument("expected key = value");
            std::string key = trim(line.substr(0, equals));
            std::string value = trim(line.substr(equals + 1));
            if (key.empty() || value.empty())
                throw std::invalid_argument("empty key or value");

            if (value.front() == '[' && value.find(']') == std::string::npos)
            {
                std::string continuation;
                while (std::getline(input, continuation))
                {
                    ++line_number;
                    continuation = trim(strip_toml_comment(continuation));
                    value += continuation;
                    if (continuation.find(']') != std::string::npos) break;
                }
                if (value.find(']') == std::string::npos)
                    throw std::invalid_argument("unterminated array");
            }
            if (!current->emplace(key, value).second)
                throw std::invalid_argument("duplicate key '" + key + "'");
        }

        require_only(settings, {
            "port", "enable_nat", "nat_method", "buffer_pool_count",
            "buffer_pool_block_size", "auth_token", "listen_interface",
            "log_file", "log_lines",
            "log_level", "strip_padding", "wait_keyframe", "watchdog",
            "daemon", "stun_host", "stun_port"
        }, "[settings]");
        require_only(security, {"blacklist"}, "[security]");
        for (const auto &route : routes)
            require_only(route, {"cidr", "interface"}, "[[upstream_routes]]");
        for (const auto &rule : rewrites)
            require_only(rule, {
                "action", "match", "replacement", "shift_hours"
            }, "[[rewrite_rules]]");

        auto has = [](const TomlTable &table, const std::string &key) {
            return table.find(key) != table.end();
        };
        auto str = [](const TomlTable &table, const std::string &key) {
            return parse_toml_string(table.at(key));
        };

        if (has(settings, "port")) setPort(static_cast<int>(parse_toml_integer(settings.at("port"))));
        if (has(settings, "enable_nat")) setNatEnabled(parse_toml_bool(settings.at("enable_nat")));
        if (has(settings, "nat_method")) setNatMethod(str(settings, "nat_method"));
        if (has(settings, "buffer_pool_count")) setBufferPoolCount(static_cast<int>(parse_toml_integer(settings.at("buffer_pool_count"))));
        if (has(settings, "buffer_pool_block_size")) setBufferPoolBlockSize(static_cast<int>(parse_toml_integer(settings.at("buffer_pool_block_size"))));
        if (has(settings, "auth_token")) setToken(str(settings, "auth_token"));
        if (has(settings, "listen_interface")) setListenInterface(str(settings, "listen_interface"));
        if (has(settings, "log_file")) setLogFile(str(settings, "log_file"));
        if (has(settings, "log_lines")) setLogLines(static_cast<size_t>(parse_toml_integer(settings.at("log_lines"))));
        if (has(settings, "strip_padding")) setStripPadding(parse_toml_bool(settings.at("strip_padding")));
        if (has(settings, "wait_keyframe")) setWaitKeyframe(parse_toml_bool(settings.at("wait_keyframe")));
        if (has(settings, "watchdog")) setWatchdogEnabled(parse_toml_bool(settings.at("watchdog")));
        if (has(settings, "daemon")) setDaemonEnabled(parse_toml_bool(settings.at("daemon")));
        if (has(settings, "stun_host")) setStunHost(str(settings, "stun_host"));
        if (has(settings, "stun_port")) setStunPort(static_cast<int>(parse_toml_integer(settings.at("stun_port"))));
        if (has(settings, "log_level"))
        {
            std::string level = str(settings, "log_level");
            if (level == "error") Logger::setLogLevel(LogLevel::ERROR);
            else if (level == "warn") Logger::setLogLevel(LogLevel::WARN);
            else if (level == "info") Logger::setLogLevel(LogLevel::INFO);
            else if (level == "debug") Logger::setLogLevel(LogLevel::DEBUG);
            else throw std::invalid_argument("invalid log_level");
        }

        if (has(security, "blacklist"))
            setBlacklist(parse_toml_string_array(security.at("blacklist")));

        if (!routes.empty())
        {
            std::vector<std::string> parsed_routes;
            for (const auto &route : routes)
            {
                if (!has(route, "cidr") || !has(route, "interface"))
                    throw std::invalid_argument("upstream route requires cidr and interface");
                parsed_routes.push_back(str(route, "cidr") + "," + str(route, "interface"));
            }
            setUpstreamRoutes(parsed_routes);
        }

        if (!rewrites.empty())
        {
            URLRewriter::clear_templates();
            for (const auto &rule : rewrites)
            {
                if (!has(rule, "action") || !has(rule, "match"))
                    throw std::invalid_argument("rewrite rule requires action and match");
                std::string action = str(rule, "action");
                std::string match = str(rule, "match");
                if (action == "remove")
                    URLRewriter::add_remove_rule(match);
                else if (action == "replace")
                {
                    if (!has(rule, "replacement"))
                        throw std::invalid_argument("replace rule requires replacement");
                    URLRewriter::add_replace_rule(match, str(rule, "replacement"));
                }
                else if (action == "timeshift")
                {
                    if (!has(rule, "shift_hours"))
                        throw std::invalid_argument("timeshift rule requires shift_hours");
                    URLRewriter::add_timeshift_rule(
                        match, static_cast<int>(parse_toml_integer(rule.at("shift_hours"))));
                }
                else
                    throw std::invalid_argument("unknown rewrite action '" + action + "'");
            }
        }
    }
    catch (const std::exception &e)
    {
        Logger::error("[CONFIG] " + path + ":" + std::to_string(line_number) +
                      ": " + e.what());
        return false;
    }
    return true;
}

void ServerConfig::printConfig()
{
    Logger::info("[CONFIG] Port:              " + std::to_string(port));
    Logger::info("[CONFIG] Listen Interface:  " + (listen_interface.empty() ? "ANY" : listen_interface));
    Logger::info("[CONFIG] NAT Enabled:       " + std::string(enable_nat ? "YES" : "NO"));
    if (enable_nat) {
        Logger::info("[CONFIG]   NAT Method:      " + nat_method);
        Logger::info("[CONFIG]   STUN Host:       " + stun_server_host);
        Logger::info("[CONFIG]   STUN Port:       " + std::to_string(stun_server_port));
    }
    Logger::info("[CONFIG] Buffer Pool Count: " + std::to_string(buffer_pool_count));
    Logger::info("[CONFIG] Buffer Pool Size:  " + std::to_string(buffer_pool_block_size));
    Logger::info("[CONFIG] Strip Padding:     " + std::string(strip_padding ? "YES" : "NO"));
    Logger::info("[CONFIG] Wait Keyframe:     " + std::string(wait_keyframe ? "YES" : "NO"));
    Logger::info("[CONFIG] Watchdog:          " + std::string(watchdog_enabled ? "YES" : "NO"));
    Logger::info("[CONFIG] Daemon:            " + std::string(daemon_enabled ? "YES" : "NO"));
    Logger::info(std::string("[CONFIG] Auth Token:        ") + (auth_token.empty() ? "NONE" : "SET (MASKED)"));
    for (const auto &rule : upstream_routes)
        Logger::info("[CONFIG] Upstream Route:    " + rule);
}

void ServerConfig::kill_previous_instance()
{
    pid_t current_pid = getpid();

    DIR *dir = opendir("/proc");
    if (!dir)
    {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type == DT_DIR && isdigit(entry->d_name[0]))
        {
            pid_t pid = atoi(entry->d_name);
            if (pid == current_pid)
                continue;

            std::string cmdline_path = "/proc/" + std::to_string(pid) + "/cmdline";
            std::ifstream cmdline_file(cmdline_path);
            if (cmdline_file)
            {
                std::string cmdline;
                std::getline(cmdline_file, cmdline);

                if (cmdline.find("rtsproxy") != std::string::npos)
                {
                    kill(pid, SIGTERM);
                    break;
                }
            }
        }
    }
    closedir(dir);
}
