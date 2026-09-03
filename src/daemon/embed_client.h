// embed_client.h — RPC client for communicating with gogol daemon
#pragma once
#include "daemon/rpc.h"
#include "daemon/rpc_v2.h"
#include <iostream>
#include <string>
#include <vector>

class RpcClient {
public:
    RpcClient();
    ~RpcClient();

    bool connected() const { return fd_ >= 0; }

    std::string cmd_query(const QueryRequest& req, std::vector<QueryResultEntry>& results);
    std::string cmd_add(const AddRequest& req);
    std::string cmd_rm(const EntryRef& ref);
    std::string cmd_get(const EntryRef& ref, int max_lines = 0);
    std::string cmd_list(EntryType type, bool has_type, const std::string& index,
                         std::vector<ListResultEntry>& entries);
    std::string cmd_index(const IndexRequest& req);
    std::string cmd_status(const std::string& index, bool& indexing, std::string& indexing_name,
                         int& progress, int& total, int& elapsed_sec);

private:
    int fd_ = -1;
    CryptoState crypto_;

    bool connect_to_daemon();
    bool check_daemon_version();
    void disconnect();
    bool send_msg(const MsgWriter& msg);
    std::vector<uint8_t> recv_msg();
};
