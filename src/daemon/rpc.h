// rpc.h — Shared types for RPC protocol
#pragma once
#include "core/loc.h"
#include <cstdint>
#include <string>
#include <vector>

// Command types
enum class RpcCmd : uint8_t {
    PING = 0,
    SHUTDOWN = 1,
    QUERY = 2,
    ADD = 3,
    RM = 4,
    GET = 5,
    LIST = 6,
    INDEX = 7,
    STATUS = 8,
};

// Response status
enum class RpcStatus : uint8_t {
    OK = 0,
    ERR = 1,
};

// Request/Response structures

struct QueryRequest {
    std::string query;
    std::string index;   // comma-separated, empty = all
    int type_filter = -1; // -1 = all types; else (int)EntryType (DOC/NOTE/TERM)
    int top_k = 5;
    int show_lines = 0;
    bool scores = false;
};

struct QueryResultEntry {
    std::string index;
    std::string path;
    uint32_t line = 0;
    std::string chunk;
    uint8_t proto = 0;   // 0=doc, 1=note, 2=term
    std::string status;
    float score = 0;
    float cosine = 0;
    std::string snippet;
};

// A (type, index, path) triple identifying an entry, used by ADD/RM/GET.
struct EntryRef {
    EntryType type = EntryType::DOC;
    std::string index;
    std::string path;
};

struct AddRequest {
    EntryType type = EntryType::DOC;
    std::string index;
    std::string path;
    std::string content;
    std::string sources;
};

struct ListResultEntry {
    uint8_t proto = 0;    // 0=doc, 1=note, 2=term
    std::string index;
    std::string path;
    uint32_t line = 0;
    std::string chunk;
};

struct IndexRequest {
    std::string index;
    bool force = false;
};
