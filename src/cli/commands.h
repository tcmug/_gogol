// commands.h — CLI command handlers
#pragma once
#include "core/loc.h"
#include <string>
#include <vector>

class RpcClient;

// Returns true if the daemon is running and rpc is connected. Otherwise prints
// "No gogol daemon running. Start it with: gogol serve" to stderr and returns
// false. All read/write commands require the daemon (direct mode was removed).
bool require_daemon(RpcClient& rpc);

struct Args {
    std::string name;
    std::string model;
    std::string scope;
    std::string query_text;
    std::string topic;
    std::string content;
    std::string content_file;
    std::string uri;
    std::string sources;
    // Explicit entry-type command model (no sigils):
    //   add/get/rm <type> <index> <path>, list [<type>] <index>, query --type
    EntryType entry_type = EntryType::DOC; // parsed from type_arg
    std::string type_arg;                  // "doc"/"note"/"term" token
    std::string path_arg;                  // path/topic/term argument
    bool has_type = false;                 // list: whether a type token was given
    int top_k = 5;
    int show_lines = 0;
    int get_lines = 0;   // get -n (dedicated so explore's -n default_val(15) can't bleed in)
    int depth = 1;
    int limit = 20;
    std::string sort_by;
    bool force = false;
    bool debug = false;
    bool scores = false;
    bool verbose = false;
    bool calls_in = false;
    bool calls_out = false;
    bool use_stdin = false;
    std::string path_mode;
    std::string filter;
    std::vector<std::string> files;
};

int cmd_index(const Args& args);
int cmd_query(const Args& args);
int cmd_add(const Args& args);
int cmd_rm(const Args& args);
int cmd_get(const Args& args);
int cmd_status(const Args& args);
int cmd_sync(const Args& args);
int cmd_list(const Args& args);
int cmd_prune(const Args& args);
int cmd_calls(const Args& args);
int cmd_metrics(const Args& args);
int cmd_affected(const Args& args);
int cmd_explore(const Args& args);
