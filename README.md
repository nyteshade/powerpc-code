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

| `Shift+Up` / `Shift+Down` | scroll one line |
| mouse wheel | scroll the transcript |

Slash commands: `/help` `/model` `/models` `/tools` `/mcp` `/env` `/jobs`
`/cwd` `/yolo` `/unicode` `/clear` `/save` `/load` `/cost` `/quit`.

`/model` with no argument opens a searchable picker showing each model's context
window, price per Mtok, and whether it supports tools and vision, with a pinned
set of favourites at the top so you never have to remember an exact id.

Assistant replies are rendered as markdown with syntax-highlighted code blocks.
Colour depth is detected automatically — exact RGB where the terminal allows
redefinable colours, then 256, then 16, then attributes only — and UTF-8 is
enabled when the locale supports it.

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

### Job files

A job file is a checkable-in description of a piece of work: YAML frontmatter for
the model and its routing, a markdown body for the task.

```sh
ppcode -j examples/jobs/build-sample.md
ppcode -j task.md --dry-run     # resolve everything and print it, run nothing
```

```yaml
---
name: build-sample
model: z-ai/glm-5.2
models:                     # fallbacks, tried in order
  - deepseek/deepseek-v4-pro
provider:                   # OpenRouter provider routing
  sort: throughput          # price | throughput | latency
  order: [together, fireworks]
  allow_fallbacks: true
  only: [together]
  ignore: [some-provider]
  require_parameters: true
  data_collection: deny
  quantizations: [fp16, bf16]
reasoning:
  effort: high              # minimal | low | medium | high
temperature: 0.2
max_turns: 25
seed: 42
web_search: true            # OpenRouter's web plugin, no extra credentials
tools:
  allow: [bash, run_background]
  deny:  [file_op]
environment:
  detail: full              # none | minimal | brief | standard | full
  knowledge: true
attachments:
  - path: docs/mockup.png
    detail: high
output: text                # text | json | stream-json
save: /tmp/session.json
---

Build the project and fix any warnings.
```

Explicit CLI flags override the file. Unknown frontmatter keys are reported
rather than silently ignored.

### Machine and platform context

Models waste turns guessing at an unfamiliar host — reaching for GNU flags the
BSD userland lacks, or suggesting a package manager that isn't here. So ppcode
probes the machine once, caches it, and states the answers up front: hardware,
OS, Xcode and SDKs, MacPorts prefix and installed ports, every compiler found
*with its platform caveats*, languages, build tools, notable applications, and a
live check of whether outbound HTTPS actually works.

Alongside it, a knowledge corpus in `knowledge/` covering Leopard/PPC API
availability, endianness pitfalls, the real cost of a `port install` on this
hardware, Aqua and Cocoa design (including that the correct aesthetic here is
skeuomorphic, not flat), browsers, and the `project.pbxproj` format.

**How much of this gets sent scales with the model.** The detail tier is chosen
from the context window and then stepped down if it would exceed a share of it,
so a 1M-context frontier model gets the full picture while a small cheap model
gets only the facts that prevent build failures. Knowledge documents declare a
minimum context in their own frontmatter and are dropped when the budget is
spent.

```sh
ppcode --show-context -p "hi"        # report how the system message was built
ppcode --env-detail brief -p "..."   # override the tier
ppcode --refresh-env -p "..."        # re-probe instead of using the cache
ppcode --no-knowledge -p "..."       # omit the corpus
```

Add your own documents to `~/.config/ppcode/knowledge/*.md`, or point
`PPCODE_KNOWLEDGE_DIR` elsewhere.

### Tools

| | |
| --- | --- |
| Files | `read_file` `read_many_files` `write_file` `edit_file` `multi_edit` `file_op` `list_dir` `glob` `grep` |
| Shell | `bash` (timed out and killed by process group) |
| Long work | `run_background` `job_list` `job_output` `job_stop` |
| Web | `web_fetch` `web_search` |
| Xcode | `xcode_info` `xcode_add_file` `xcode_set_setting` `xcode_add_framework` |
| Planning | `todo_write` |

**Long-running work.** A `port install` or a large compile on this hardware can
run for hours, sometimes more than a day — far past any sensible `bash` timeout.
`run_background` spawns the command in its own session with `setsid`, streams
output to a log, and records metadata on disk, so the job survives ppcode exiting
and can be checked from a later session with `job_output`.

**Web access** works only because MacPorts supplies a current curl and OpenSSL;
the stock Leopard pair cannot complete a handshake with most hosts. The probe
checks this for real and tells the model whether downloading anything is viable.
For search, DuckDuckGo's HTML endpoint now serves a bot check, so the
credential-free path is Wikipedia plus DuckDuckGo instant answers — good for
reference material, not general web results. For real search, either set
`TAVILY_API_KEY`, `BRAVE_SEARCH_API_KEY` or `SERPER_API_KEY`, point
`PPCODE_SEARXNG_URL` at a SearXNG instance, or set `web_search: true` to use
OpenRouter's own plugin, which needs no extra credentials.

**Xcode projects.** Xcode 3 stores `project.pbxproj` as an old-style
NeXT/OpenStep ASCII plist — plain text, not XML and not binary — so it can be
edited reliably. ppcode parses and rewrites that format, grouping objects by
`isa` the way Xcode does, writes a `.ppcode-bak` alongside, and refuses to save
anything it cannot re-parse. Adding a source file correctly means three linked
edits (file reference, group membership, and a build file in the right phase);
the tool does all three. Verified end to end: a file added and a framework linked
this way compile and appear in the resulting `ppc` binary, and `plutil` accepts
the regenerated project.

### Tool permissions

Read-only tools (`read_file`, `list_dir`, `glob`, `grep`, …) always run. Tools
that change something (`write_file`, `edit_file`, `multi_edit`, `file_op`,
`bash`, the background-job and web tools, the Xcode mutators, and every MCP tool)
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
./build/ppcode --selftest         # 270 offline checks
./build/ppcode --selftest --net   # adds live OpenRouter calls
```

Covers SSE framing at every chunk boundary, streaming tool-call assembly,
message serialisation, the shell timeout, every builtin tool, the approval gate,
the YAML and job-file parsers, UTF-8 width and boundary handling, markdown and
syntax highlighting, HTML-to-text, attachment loading and degradation, the
environment probe's detail scaling, and a full pbxproj parse/mutate/reload
round-trip. `scripts/tui_drive.py` runs the TUI inside a pty and renders the
screen, so the interface can be regression-tested without a human at the
keyboard:

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

### ncurses macros collide with ordinary identifiers

`hline`, `vline`, `border`, `move`, `clear` and friends are `#define`d to inject
`stdscr` as a first argument. A local variable named `hline` becomes a function
call with the wrong arity, and the resulting error points at `basic_string` rather
than at the real cause. Avoid those names in any file that includes `ncurses.h`.

### Not every PowerPC Mac has the same ports

`port install` here means **compiling from source** — there are no binary archives
for this architecture, and a large port can take many hours or more than a day.
ppcode therefore probes what is actually present rather than assuming, and the
system prompt tells the model explicitly not to propose an install casually.

Build requirements are consequently real constraints, not conveniences: ppcode
needs MacPorts `gcc15` (or another C++23-capable compiler), `curl`, `ncurses` and
`gmake`. Without a modern curl and OpenSSL it will build but have no network
reach, and the probe will say so.

## Layout

```
src/
  common.*      strings, JSON helpers, stdio file I/O, base64, logging
  config.*      config file + environment
  http.*        libcurl wrapper and the incremental SSE parser
  openrouter.*  chat API, streaming, tool-call assembly, model catalogue
  yaml.*        minimal YAML parser for frontmatter
  job.*         job files: frontmatter + markdown body
  attach.*      attachments to message content parts
  envinfo.*     machine and toolchain probe, with detail tiers
  sysprompt.*   assembles the system message within a token budget
  tools.*       tool registry, core builtins, shell execution
  tools_extra.* multi_edit, read_many_files, file_op, todo, job tools
  jobs.*        detached background commands that outlive ppcode
  webtools.*    web_fetch, web_search, HTML to text
  plist.*       old-style NeXT/OpenStep ASCII plists
  xcodeproj.*   Xcode 3 project inspection and modification
  mcp.*         MCP client: stdio and HTTP transports
  agent.*       the model/tool loop, shared by both front ends
  utf8.*        codepoint and display-width aware string handling
  render.*      markdown and syntax highlighting into styled spans
  ui.*          ncurses interface
  headless.*    non-interactive runner
  selftest.*    internal checks
knowledge/*.md                   platform knowledge loaded into context
examples/jobs/*.md               example job files
examples/qjs-mcp-server.js       MCP server for QuickJS
scripts/tui_drive.py             pty harness for testing the TUI
scripts/http_mcp_test_server.py  HTTP MCP fixture
```
