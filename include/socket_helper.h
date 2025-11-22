#pragma once

int create_listen_socket(int port);
int create_nonblocking_tcp(const std::string &ip, uint16_t port, const std::string &iface = "");
int bind_udp_socket_with_retry(int &fd, uint16_t &port, int max_attempts, const std::string &iface = "");
int bind_udp_socket(int &fd, const uint16_t &port, const std::string &iface = "");