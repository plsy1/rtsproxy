#pragma once

#include <string>
#include <vector>

class ServerConfig
{
public:
    static void parseCommandLine(int argc, char *argv[]);
    static void setPort(int p);
    static void setNatEnabled(bool enable);
    static void setBufferPoolCount(int count);
    static void setBufferPoolBlockSize(int size);
    static void setRtpBufferSize(int count) { setBufferPoolCount(count); }
    static void setUdpPacketSize(int size) { setBufferPoolBlockSize(size); }
    static void setStunPort(int port);
    static void setStunHost(std::string host);
    static void setNatMethod(const std::string &method);
    static void setConfigPath(std::string path);
    static void setToken(std::string token);
    static void addUpstreamRoute(const std::string &rule);
    static void setUpstreamRoutes(const std::vector<std::string> &rules);
    static void setListenInterface(std::string iface);
    static void setLogFile(std::string path);
    static void setLogLines(size_t lines);
    static void setBlacklist(const std::vector<std::string> &list);
    static void addBlacklist(const std::string &entry);
    static void setStripPadding(bool enable);
    static void setWaitKeyframe(bool enable);
    static void setWatchdogEnabled(bool enable);
    static void setDaemonEnabled(bool enable);
    static void setAuthToken(std::string token) { setToken(token); }
    static void setLogFileLines(size_t lines) { setLogLines(lines); }

    static bool isNatEnabled();
    static std::string getNatMethod();
    static int getPort();
    static int getBufferPoolCount();
    static int getBufferPoolBlockSize();
    static int getStunPort();
    static std::string getStunHost();
    static std::string getConfigPath();
    static std::string getToken();
    static std::string getUpstreamInterface(const std::string &ip,
                                            const std::string &fallback = "");
    static const std::vector<std::string>& getUpstreamRoutes();
    static std::string getListenInterface();
    static std::string getLogFile();
    static size_t getLogLines();
    static bool isStripPadding();
    static bool isWaitKeyframe();
    static bool isWatchdogEnabled();
    static bool isDaemonEnabled();
    static const std::vector<std::string>& getBlacklist();
    static void printUsage(const std::string &program_name);
    static void printConfig();
    static bool loadTomlFile(const std::string &path);
    static void kill_previous_instance();

private:
    static int port;
    static bool enable_nat;
    static std::string nat_method;
    static int buffer_pool_count;
    static int buffer_pool_block_size;
    static int stun_server_port;
    static std::string stun_server_host;
    static std::string config_path;
    static std::string auth_token;
    static std::vector<std::string> upstream_routes;
    static std::string listen_interface;
    static std::string log_file_path;
    static size_t log_file_lines;
    static bool strip_padding;
    static bool wait_keyframe;
    static bool watchdog_enabled;
    static bool daemon_enabled;
    static std::vector<std::string> blacklist;
};
