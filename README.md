# ppcode

A Claude Code-style terminal coding assistant, written in C++23 and running
natively on a PowerPC Power Mac G5 under Mac OS X Server 10.5.8 (Leopard).

It talks to any model on [OpenRouter](https://openrouter.ai), streams responses
token by token, calls tools to read and write files and run commands, and
speaks the Model Context Protocol.

```
  ppcode -- PowerPC Leopard build. /help for commands, Ctrl+D to quit.
  model: anthropic/claude-sonnet-5    cwd: /Users/brie/src/ppcode    tools: 10

> How many .cpp files are in src? Use your tools.

* glob  {"pattern": "src/**/*.cpp"}

    src/agent.cpp
    src/common.cpp
    ...

There are 11 .cpp files in src.

ready                                        anthropic/claude-sonnet-5  4322tok  $0.0092
>
```

## Building

The G5 is the only machine with the toolchain, so builds happen there.

```sh
gmake            # NOT /usr/bin/make -- gcc15 and GNU make both live in /opt/local
gmake install    # -> ~/bin/ppcode
```

From another machine, `./deploy.sh` rsyncs the tree over and builds remotely:

```sh
./deploy.sh              # sync + build
./deploy.sh -c           # clean build
./deploy.sh -r -p "hi"   # sync, build, then run with those arguments
```

Requirements, all present via MacPorts: `gcc15` (15.2.0, for C++23), `curl`,
`ncurses`, `gmake`. `nlohmann/json` is vendored in `third_party/`.

## Setup

```sh
export OPENROUTER_AI_API_KEY=sk-or-...   # OPENROUTER_API_KEY also accepted
ppcode --write-config                    # ~/.config/ppcode/config.json
```

The key is read from the environment and is never written to disk or logged.

## Usage

Interactive:

```sh
ppcode
```

| Key | Action |
| --- | --- |
| `Enter` | send |
| `Ctrl+J` | newline within a message |
| `Ctrl+C` | cancel the in-flight request |
| `Ctrl+D` | quit |
| `PgUp` / `PgDn` | scroll the transcript |
| `Up` / `Down` | recall previous inputs |

Slash commands: `/help` `/model` `/models` `/tools` `/mcp` `/cwd` `/yolo`
`/clear` `/save` `/load` `/cost` `/quit`.

### Scripting

`ppcode` is designed to be driven from scripts, not just typed at.

```sh
# one-shot
ppcode -p "what does src/http.cpp do?"

# from a pipe
git diff | ppcode -p "review this diff"

# machine-readable
ppcode -p "count the source files" --output json | \
  python3 -c 'import json,sys; print(json.load(sys.stdin)["text"])'

# event stream (JSONL: text, tool_start, tool_result, done)
ppcode -p "fix the warning in tools.cpp" --output stream-json --yolo

# resumable sessions
ppcode -p "read the Makefile"  --save /tmp/s.json
ppcode -p "now explain the lint target" --resume /tmp/s.json
```

Exit codes: `0` success, `1` the model or a tool failed, `2` a usage error.

### Tool permissions

Read-only tools (`read_file`, `list_dir`, `glob`, `grep`) always run. Tools
that change something (`write_file`, `edit_file`, `bash`, and every MCP tool)
need permission:

- **Interactively** you get a prompt: `y` allow once, `n` deny, `a` allow
  everything for the rest of the session.
- **Headless** they are *refused by default*, so a script that forgets to grant
  permission fails safe rather than rewriting your disk. Opt in with
  `--allow-tool NAME` (repeatable) or `--yolo`. `--deny-tool` overrides both.

## MCP

Configure servers in `~/.config/ppcode/config.json`. Their tools are namespaced
`servername__toolname` and offered to the model alongside the builtins.

```json
{
  "model": "anthropic/claude-sonnet-5",
  "mcp_servers": [
    {
      "name": "qjs",
      "transport": "stdio",
      "command": "/opt/local/bin/qjs",
      "args": ["--std", "/Users/brie/src/ppcode/examples/qjs-mcp-server.js"]
    },
    {
      "name": "filesystem",
      "transport": "http",
      "url": "http://10.0.0.50:3000/mcp",
      "headers": { "Authorization": "Bearer ..." }
    }
  ]
}
```

Both transports are implemented and tested: **stdio** (fork/exec, newline
delimited JSON-RPC) and **Streamable HTTP** (JSON or SSE responses).

### There is no Node.js on this machine

This is the central constraint for MCP here, and it is worth being blunt about:
**the great majority of MCP servers ship as npm packages and cannot run on this
box.** There is no `nodejs` port available for PowerPC Leopard, and modern MCP
server packages need a modern Node regardless. Two things that do work:

1. **Run npm-based servers on another machine** and point ppcode at them over
   `transport: "http"`. This is the practical route to the wider ecosystem, and
   the HTTP transport exists for exactly this reason.

2. **Write servers for QuickJS**, which *is* installed (`qjs`, 2025-09-13).
   `examples/qjs-mcp-server.js` is a complete, dependency-free MCP server in
   ~200 lines exposing `sysinfo`, `disk_usage`, and `port_installed`. QuickJS
   provides `std.popen`, `os.exec`, and `os.read`, which is all the protocol
   needs.

   The catch: QuickJS has **no Node compatibility layer** — no `require`, no
   `fs`/`child_process`/`stream` modules, no `@modelcontextprotocol/sdk`. So
   existing servers will not run unmodified; you write against `std`/`os`
   directly. For the small, host-specific tools that are actually useful on a
   machine like this, that is not much of a burden.

`scripts/http_mcp_test_server.py` is a small Python fixture for exercising the
HTTP path locally.

## Testing

```sh
./build/ppcode --selftest         # 85 offline checks
./build/ppcode --selftest --net   # adds live OpenRouter calls
```

Covers SSE framing at every chunk boundary, streaming tool-call assembly,
message serialisation, the shell timeout, every builtin tool, and the approval
gate. `scripts/tui_drive.py` runs the TUI inside a pty and renders the screen,
so the interface can be regression-tested without a human at the keyboard:

```sh
python3 scripts/tui_drive.py ./build/ppcode -- 1.0 '/help\r' 1.5 '\x04'
```

## Notes on this platform

Two things cost real time to discover and are worth recording.

### Two libstdc++ runtimes, and why there are no iostreams here

Linking libcurl pulls in CoreFoundation → CoreServices → Carbon frameworks →
Leopard's `/usr/lib/libstdc++.6.dylib`, which then coexists with gcc15's own
`/opt/local/lib/libgcc/libstdc++.6.dylib`. `otool -L` on the binary shows only
one; the other arrives transitively. Confirm with:

```sh
DYLD_PRINT_LIBRARIES=1 ./build/ppcode 2>&1 | grep stdc
```

Darwin coalesces weak symbols process-wide regardless of two-level namespace,
so a `std::ios_base::Init` in your code binds to the *system* libstdc++'s
`basic_ostream` constructor, whose locale setup then calls `free()` on a pointer
in a dylib's `__DATA` segment. Every locale construction prints:

```
malloc: *** error for object 0x...: Non-aligned pointer being freed
```

The allocator rejects the free, so nothing crashes — but stderr is unusable,
which breaks scripted output. `-static-libstdc++` does **not** fix it.

The fix is to avoid C++ iostreams entirely: this codebase uses stdio plus
`read_file_text`/`write_file_text`, and `gmake lint` fails the build if anyone
reintroduces `<iostream>`, `<fstream>`, `<sstream>`, or `<iomanip>`. That took
the error count from 15 per run to zero.

### ncurses `nl()` hides the Enter key

By default ncurses translates CR to LF on input, making `Enter` and `Ctrl+J`
indistinguishable. `nonl()` keeps them apart, which is what lets `Enter` send a
message while `Ctrl+J` inserts a newline.

## Layout

```
src/
  common.*      strings, JSON helpers, stdio file I/O, logging
  config.*      config file + environment
  http.*        libcurl wrapper and the incremental SSE parser
  openrouter.*  chat API, streaming, tool-call assembly
  tools.*       tool registry, builtins, shell execution
  mcp.*         MCP client: stdio and HTTP transports
  agent.*       the model/tool loop, shared by both front ends
  ui.*          ncurses interface
  headless.*    non-interactive runner
  selftest.*    internal checks
examples/qjs-mcp-server.js       MCP server for QuickJS
scripts/tui_drive.py             pty harness for testing the TUI
scripts/http_mcp_test_server.py  HTTP MCP fixture
```
