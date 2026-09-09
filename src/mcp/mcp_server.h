// gogol MCP server — stdio JSON-RPC 2.0 front-end.
//
// `gogol mcp` runs a thin, mostly-stateless MCP server over stdio (newline-
// delimited JSON-RPC 2.0). It implements the protocol methods:
//   - initialize   → serverInfo + capabilities { tools: {} }
//   - tools/list   → the read-only tool catalog derived from the Tool Registry
//   - tools/call   → registry lookup + param validation, then dispatch to the
//                    EXISTING daemon RpcClient (reuse — no reimplemented logic)
//
// All heavy state lives in the daemon (`gogol serve`). This process only
// translates MCP calls into existing RPC calls, mirroring the CLI client.
#pragma once

#include <istream>
#include <ostream>

namespace gogol {
namespace mcp {

// Run the stdio JSON-RPC loop, reading requests from `in` and writing one JSON
// response object per line to `out`. Returns a process exit code (0 on clean
// EOF). `include_write` controls whether write tools are exposed in tools/list
// (P2: always read-only, so callers pass false; config gating arrives in P3).
int run_stdio_server(std::istream& in, std::ostream& out, bool include_write);

}  // namespace mcp
}  // namespace gogol

// CLI entry point (registered as the `mcp` subcommand in main.cpp).
int cmd_mcp();
