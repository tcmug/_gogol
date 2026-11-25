// gogol MCP server — stdio JSON-RPC 2.0 front-end.
//
// `gogol mcp` runs an MCP server over stdio (newline-delimited JSON-RPC 2.0).
// It implements the protocol methods:
//   - initialize   → serverInfo + capabilities { tools: {} }
//   - tools/list   → the full tool catalog derived from the Tool Registry
//   - tools/call   → registry lookup + param validation, then dispatch to the
//                    handler
//
// gogol is daemon-less: each handler reads/writes the unified store (~/.gogol/cache.db + notes.db)
// DIRECTLY and embeds via a process-lifetime warm model singleton
// (get_shared_embedder) — the model loads once and stays resident across tool
// calls for this long-lived process. No socket, no RPC, no `gogol serve`.
#pragma once

#include <istream>
#include <ostream>
#include <string>
#include <optional>

#include "mcp/json.h"

namespace gogol {
namespace mcp {

// Pure JSON-RPC 2.0 dispatch core, shared by every transport (stdio + HTTP).
// Parses nothing about the wire; takes a parsed request Json, fills
// out_response, and returns true if a response should be sent.
bool process_request(const Json& msg, Json& out_response);

// Transport-shared request handler: parse one raw JSON-RPC request string, dispatch
// via process_request inside a try/catch (so an exception becomes a JSON-RPC error,
// never an escape), and return the response to send. nullopt = no response (a
// notification). A parse failure returns a framed parse-error. EVERY transport
// (stdio, HTTP, unix socket) routes through THIS so the parse/dispatch/error-wrap
// logic lives in exactly one place — transports only move bytes.
std::optional<std::string> handle_request_json(const std::string& raw);

// Arm the daemon's idle-timeout watcher: a detached thread that _Exit()s the process
// once no request has arrived for `timeout_secs` (0 = never). Call once at daemon start.
void start_idle_watcher(int timeout_secs);

// The daemon/shim build identity (version + compile timestamp). Shim sends it on connect
// via __daemon_version; a mismatch means the daemon is stale (shim rebuilt) -> it self-
// exits and the shim respawns. Same function both sides => same value in one binary.
std::string daemon_build_id();

// Message bus persistence (ADR-003). The daemon calls set_bus_db_path(messages.db)
// before the bus is first used; otherwise the bus is in-memory. requeue_bus_claims
// returns every unacked claim to queued and reports how many.
void set_bus_db_path(const std::string& path);
// Pin `[mcp] messaging` regardless of config: 1 on, 0 off, -1 back to config (tests).
void set_messaging_override(int on);
int requeue_bus_claims();
// Detached thread: MessageBus::sweep now, then hourly (lease requeue + 30-day prune).
void start_bus_sweeper();

// Run the stdio JSON-RPC loop, reading requests from `in` and writing one JSON
// response object per line to `out`. Returns a process exit code (0 on clean
// EOF). `gogol mcp` is a local stdio tool, so every tool is exposed once the
// server is enabled — there is no read/write gate.
int run_stdio_server(std::istream& in, std::ostream& out);

// Run an HTTP MCP server: a single POST /mcp endpoint (JSON-RPC body in/out)
// plus GET / (status dashboard). Blocks until the process is killed.
int run_http_server(const std::string& host, int port);

}  // namespace mcp
}  // namespace gogol

// CLI entry point (registered as the `mcp` subcommand in main.cpp).
int cmd_mcp();
int cmd_serve(const std::string& host, int port);
