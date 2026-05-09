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

    // Load from file if exists
    loadFromFile(getJsonPath());

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
    port = p;
}

void ServerConfig::setNatEnabled(bool enable)
{
    enable_nat = enable;
}

void ServerConfig::setNatMethod(const std::string &method)
{
    nat_method = method;
}

void ServerConfig::setBufferPoolCount(int count)
{
    buffer_pool_count = count;
}

void ServerConfig::setBufferPoolBlockSize(int size)
{
    buffer_pool_block_size = size;
}

void ServerConfig::setStunPort(int port)
{
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

    if (config.contains("blacklist") && config["blacklist"].is_array())
    {
        std::vector<std::string> bl;
        for (const auto &item : config["blacklist"])
        {
            if (item.is_string()) bl.push_back(item.get<std::string>());
        }
        setBlacklist(bl);
    }

    if (config.contains("settings") && config["settings"].is_object())
    {
        const auto& s = config["settings"];
        if (s.contains("port")) setPort(s["port"].get<int>());
        if (s.contains("nat_method")) setNatMethod(s["nat_method"].get<std::string>());
        if (s.contains("enable_nat")) setNatEnabled(s["enable_nat"].get<bool>());
        if (s.contains("buffer_pool_count")) setBufferPoolCount(s["buffer_pool_count"].get<int>());
        if (s.contains("buffer_pool_block_size")) setBufferPoolBlockSize(s["buffer_pool_block_size"].get<int>());
        if (s.contains("auth_token")) setToken(s["auth_token"].get<std::string>());
        if (s.contains("log_file")) setLogFile(s["log_file"].get<std::string>());
        if (s.contains("log_lines")) setLogLines(s["log_lines"].get<size_t>());
        if (s.contains("log_level")) {
            std::string level = s["log_level"].get<std::string>();
            if (level == "error") Logger::setLogLevel(LogLevel::ERROR);
            else if (level == "warn") Logger::setLogLevel(LogLevel::WARN);
            else if (level == "info") Logger::setLogLevel(LogLevel::INFO);
            else if (level == "debug") Logger::setLogLevel(LogLevel::DEBUG);
        }
        if (s.contains("strip_padding")) setStripPadding(s["strip_padding"].get<bool>());
        if (s.contains("wait_keyframe")) setWaitKeyframe(s["wait_keyframe"].get<bool>());
        if (s.contains("watchdog")) setWatchdogEnabled(s["watchdog"].get<bool>());
        if (s.contains("daemon")) setDaemonEnabled(s["daemon"].get<bool>());
        if (s.contains("http_interface")) setHttpUpstreamInterface(s["http_interface"].get<std::string>());
        if (s.contains("mitm_interface")) setMitmUpstreamInterface(s["mitm_interface"].get<std::string>());
        if (s.contains("listen_interface")) setListenInterface(s["listen_interface"].get<std::string>());
        if (s.contains("stun_host")) setStunHost(s["stun_host"].get<std::string>());
        if (s.contains("stun_port")) setStunPort(s["stun_port"].get<int>());
    }

    if (config.contains("replace_templates") && config["replace_templates"].is_array())
    {
        URLRewriter::set_replace_templates(config["replace_templates"]);
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
