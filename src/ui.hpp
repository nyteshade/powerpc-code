// ui.hpp -- the interactive ncurses front end.
#pragma once

#include "agent.hpp"
#include "config.hpp"
#include "envinfo.hpp"
#include "mcp.hpp"
#include "openrouter.hpp"
#include "tools.hpp"

namespace ppcode {

// Runs the TUI until the user quits. Returns a process exit code.
// `mcp_manager` may be null when no MCP servers are configured.
int run_tui(Agent& agent, Client& client, ToolRegistry& tools, const Config& cfg,
            mcp::Manager* mcp_manager);

} // namespace ppcode
