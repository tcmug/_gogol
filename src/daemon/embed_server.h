// embed_server.h — Daemon server: unix socket + optional TCP
#pragma once
#include <string>

// Run the embedding server (blocks until shutdown signal or SIGTERM)
// tcp_addr: empty = no TCP, "host:port" = also listen on TCP
// Returns 0 on clean shutdown, 1 on error.
int run_embed_server(const std::string &model_path, bool foreground,
                     const std::string &tcp_addr = "");

// Stop a running server by sending shutdown request
bool stop_embed_server();

// Check server status, print info
int server_status();
