#include "core/server_config.h"
#include "utils/url_rewriter.h"
#include "3rd/json.hpp"
#include "core/logger.h"
#include <iostream>
#include <signal.h>
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#include <getopt.h>
#include <cstring>
#include <stdexcept>

int ServerConfig::port = 8554;
bool ServerConfig::enable_nat = false;
std::string ServerConfig::nat_method = "stun";
int ServerConfig::buffer_pool_count = 8192;
int ServerConfig::buffer_pool_block_size = 2048;
int ServerConfig::stun_server_port = 19302;
std::string ServerConfig::stun_server_host = "stun.l.google.com";
std::string ServerConfig::json_path = "config.json";
std::string ServerConfig::auth_token = "";
std::string ServerConfig::http_upstream_interface = "";
std::string ServerConfig::mitm_upstream_interface = "";
std::string ServerConfig::listen_interface = "";
std::string ServerConfig::log_file_path = "";
size_t ServerConfig::log_file_lines = 10000;
bool ServerConfig::strip_padding = false;
bool ServerConfig::wait_keyframe = false;
bool ServerConfig::watchdog_enabled = false;
bool ServerConfig::daemon_enabled = false;
std::vector<std::string> ServerConfig::blacklist = {};

void ServerConfig::parseCommandLine(int argc, char *argv[])
{
    struct option long_options[] = {
        {"port", required_argument, nullptr, 'p'},
        {"enable-nat", no_argument, nullptr, 'n'},
        {"nat-method", required_argument, nullptr, 0},
        {"buffer-pool-count", required_argument, nullptr, 'b'},
        {"buffer-pool-block-size", required_argument, nullptr, 's'},
        {"auth-token", required_argument, nullptr, 't'},
        {"http-interface", required_argument, nullptr, 0},
        {"mitm-interface", required_argument, nullptr, 0},
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
        {nullptr, 0, nullptr, 0}
    };

    // First pass to find the config file
    int opt;
    int longindex = -1;
    optind = 1; // Reset getopt
    while ((opt = getopt_long(argc, argv, "p:nb:s:t:c:l:kdw", long_options, &longindex)) != -1) {
        if (opt == 'c' || (opt == 0 && longindex >= 0 && strcmp(long_options[longindex].name, "config") == 0)) {
            setJsonPath(optarg);
        }
    }

    // Load from file if exists. A file that is there but unusable stays fatal:
    // starting up with the operator's auth token and blacklist silently missing
    // would be worse than not starting at all. A file that is simply absent is
    // not an error, the compiled-in defaults and the CLI options cover it.
    {
        std::ifstream probe(getJsonPath());
        bool present = probe.is_open();
        probe.close();
        if (!loadFromFile(getJsonPath()) && present)
            throw std::runtime_error("config file '" + getJsonPath() + "' was rejected, see the log for the offending setting");
    }

    // Second pass to override with CLI options
    optind = 1; // Reset getopt
    while ((opt = getopt_long(argc, argv, "p:nb:s:t:c:l:kdw", long_options, &longindex)) != -1) {
        switch (opt) {
        case 'p': setPort(std::stoi(optarg)); break;
        case 'n': setNatEnabled(true); break;
        case 'b': setBufferPoolCount(std::stoi(optarg)); break;
        case 's': setBufferPoolBlockSize(std::stoi(optarg)); break;
        case 't': setToken(optarg); break;
        case 'c': break; // Handled in first pass
        case 'l': setListenInterface(optarg); break;
        case 'k': kill_previous_instance(); exit(0);
        case 'd': setDaemonEnabled(true); break;
        case 'w': setWatchdogEnabled(true); break;
        case 0:
            if (longindex >= 0 && strcmp(long_options[longindex].name, "nat-method") == 0) setNatMethod(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "stun-host") == 0) setStunHost(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "stun-port") == 0) setStunPort(std::stoi(optarg));
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "http-interface") == 0) setHttpUpstreamInterface(optarg);
            else if (longindex >= 0 && strcmp(long_options[longindex].name, "mitm-interface") == 0) setMitmUpstreamInterface(optarg);
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

void ServerConfig::setJsonPath(std::string path)
{
    json_path = path;
}

void ServerConfig::setToken(std::string token)
{
    auth_token = token;
}

void ServerConfig::setHttpUpstreamInterface(std::string iface)
{
    http_upstream_interface = iface;
}

void ServerConfig::setMitmUpstreamInterface(std::string iface)
{
    mitm_upstream_interface = iface;
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

std::string ServerConfig::getJsonPath()
{
    return json_path;
}

std::string ServerConfig::getToken()
{
    return auth_token;
}

std::string ServerConfig::getHttpUpstreamInterface()
{
    return http_upstream_interface;
}

std::string ServerConfig::getMitmUpstreamInterface()
{
    return mitm_upstream_interface;
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
    std::cout << "  -p, --port            <port>  Set HTTP server port (default: " << port << ")" << std::endl;
    std::cout << "  -n, --enable-nat              Enable NAT (default: " << (enable_nat ? "enabled" : "disabled") << ")" << std::endl;
    std::cout << "      --nat-method      <method> Set NAT method: stun, zte (default: " << nat_method << ")" << std::endl;
    std::cout << "  -b, --buffer-pool-count <count> Set BufferPool block count (default: " << buffer_pool_count << ")" << std::endl;
    std::cout << "  -s, --buffer-pool-block-size <size>  Set BufferPool block size (default: " << buffer_pool_block_size << ")" << std::endl;
    std::cout << "  -t, --auth-token      <token> Set auth token for HTTP API and RTSP access (default: none)" << std::endl;
    std::cout << "      --http-interface  <iface> Set HTTP mode upstream interface" << std::endl;
    std::cout << "      --mitm-interface  <iface> Set MITM mode upstream interface" << std::endl;
    std::cout << "  -l, --listen-interface <iface> Set interface to listen on" << std::endl;
    std::cout << "  -c, --config          <path>  Set JSON file path (default: " << json_path << ")" << std::endl;
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
}

bool ServerConfig::loadFromFile(const std::string &path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        Logger::warn("[SERVER] Failed to open config file: " + path);
        return false;
    }

    nlohmann::json config;
    try {
        config = nlohmann::json::parse(ifs, nullptr, true, true);
    } catch (const nlohmann::json::parse_error& e) {
        Logger::error("[CONFIG] JSON parse error: " + std::string(e.what()));
        return false;
    }

    // Nothing below is committed to global state until the settings block has
    // been applied in full, so a file rejected halfway leaves the process
    // exactly as it was.
    bool has_blacklist = false;
    std::vector<std::string> bl;
    if (config.contains("blacklist") && config["blacklist"].is_array())
    {
        has_blacklist = true;
        for (const auto &item : config["blacklist"])
        {
            if (item.is_string()) bl.push_back(item.get<std::string>());
        }
    }

    if (config.contains("settings") && config["settings"].is_object())
    {
        const auto& s = config["settings"];

        // The setters double as the validators, so a bad key is only found
        // after the keys before it have already been stored. Keep the previous
        // values so they can be put back verbatim.
        const int old_port = port;
        const bool old_enable_nat = enable_nat;
        const std::string old_nat_method = nat_method;
        const int old_buffer_pool_count = buffer_pool_count;
        const int old_buffer_pool_block_size = buffer_pool_block_size;
        const int old_stun_server_port = stun_server_port;
        const std::string old_stun_server_host = stun_server_host;
        const std::string old_auth_token = auth_token;
        const std::string old_http_upstream_interface = http_upstream_interface;
        const std::string old_mitm_upstream_interface = mitm_upstream_interface;
        const std::string old_listen_interface = listen_interface;
        const std::string old_log_file_path = log_file_path;
        const size_t old_log_file_lines = log_file_lines;
        const LogLevel old_log_level = Logger::getLogLevel();
        const bool old_strip_padding = strip_padding;
        const bool old_wait_keyframe = wait_keyframe;
        const bool old_watchdog_enabled = watchdog_enabled;
        const bool old_daemon_enabled = daemon_enabled;

        // Names the key currently being applied, so a failure can point the
        // operator at the exact line of their config file.
        std::string key;
        try {
            key = "port";                   if (s.contains(key)) setPort(s[key].get<int>());
            key = "nat_method";             if (s.contains(key)) setNatMethod(s[key].get<std::string>());
            key = "enable_nat";             if (s.contains(key)) setNatEnabled(s[key].get<bool>());
            key = "buffer_pool_count";      if (s.contains(key)) setBufferPoolCount(s[key].get<int>());
            key = "buffer_pool_block_size"; if (s.contains(key)) setBufferPoolBlockSize(s[key].get<int>());
            key = "auth_token";             if (s.contains(key)) setToken(s[key].get<std::string>());
            key = "log_file";               if (s.contains(key)) setLogFile(s[key].get<std::string>());
            key = "log_lines";              if (s.contains(key)) setLogLines(s[key].get<size_t>());
            key = "log_level";
            if (s.contains(key)) {
                std::string level = s[key].get<std::string>();
                if (level == "error") Logger::setLogLevel(LogLevel::ERROR);
                else if (level == "warn") Logger::setLogLevel(LogLevel::WARN);
                else if (level == "info") Logger::setLogLevel(LogLevel::INFO);
                else if (level == "debug") Logger::setLogLevel(LogLevel::DEBUG);
            }
            key = "strip_padding";          if (s.contains(key)) setStripPadding(s[key].get<bool>());
            key = "wait_keyframe";          if (s.contains(key)) setWaitKeyframe(s[key].get<bool>());
            key = "watchdog";               if (s.contains(key)) setWatchdogEnabled(s[key].get<bool>());
            key = "daemon";                 if (s.contains(key)) setDaemonEnabled(s[key].get<bool>());
            key = "http_interface";         if (s.contains(key)) setHttpUpstreamInterface(s[key].get<std::string>());
            key = "mitm_interface";         if (s.contains(key)) setMitmUpstreamInterface(s[key].get<std::string>());
            key = "listen_interface";       if (s.contains(key)) setListenInterface(s[key].get<std::string>());
            key = "stun_host";              if (s.contains(key)) setStunHost(s[key].get<std::string>());
            key = "stun_port";              if (s.contains(key)) setStunPort(s[key].get<int>());
        } catch (const std::exception& e) {
            port = old_port;
            enable_nat = old_enable_nat;
            nat_method = old_nat_method;
            buffer_pool_count = old_buffer_pool_count;
            buffer_pool_block_size = old_buffer_pool_block_size;
            stun_server_port = old_stun_server_port;
            stun_server_host = old_stun_server_host;
            auth_token = old_auth_token;
            http_upstream_interface = old_http_upstream_interface;
            mitm_upstream_interface = old_mitm_upstream_interface;
            listen_interface = old_listen_interface;
            log_file_path = old_log_file_path;
            log_file_lines = old_log_file_lines;
            Logger::setLogLevel(old_log_level);
            strip_padding = old_strip_padding;
            wait_keyframe = old_wait_keyframe;
            watchdog_enabled = old_watchdog_enabled;
            daemon_enabled = old_daemon_enabled;
            Logger::error("[CONFIG] Bad value for setting '" + key + "': " + std::string(e.what()));
            return false;
        }
    }

    if (has_blacklist) setBlacklist(bl);

    if (config.contains("replace_templates") && config["replace_templates"].is_array())
    {
        // A rule without a string action/match can never fire, so drop it here
        // rather than letting rewrite_path complain on every single request.
        nlohmann::json templates = nlohmann::json::array();
        size_t index = 0;
        for (const auto &tpl : config["replace_templates"])
        {
            if (!tpl.is_object() ||
                !tpl.contains("action") || !tpl["action"].is_string() ||
                !tpl.contains("match") || !tpl["match"].is_string())
            {
                Logger::warn("[CONFIG] Ignoring replace_templates[" + std::to_string(index) +
                             "]: needs a string 'action' and a string 'match'");
            }
            else
            {
                templates.push_back(tpl);
            }
            ++index;
        }
        URLRewriter::set_replace_templates(templates);
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
    if (!http_upstream_interface.empty()) 
        Logger::info("[CONFIG] HTTP Upstream If:  " + http_upstream_interface);
    if (!mitm_upstream_interface.empty())
        Logger::info("[CONFIG] MITM Upstream If:  " + mitm_upstream_interface);
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
