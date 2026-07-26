#!/usr/bin/env qjs --std
//
// A dependency-free MCP server for QuickJS.
//
// This machine has no Node.js, so the usual npm-packaged MCP servers cannot
// run here. QuickJS ships with `std` and `os` modules that provide enough --
// file I/O, popen, and raw fd reads -- to speak the protocol directly.
//
// Transport: newline-delimited JSON-RPC 2.0 on stdin/stdout.
// Run it yourself with:
//   echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | qjs --std examples/qjs-mcp-server.js
//
import * as std from "std";
import * as os from "os";

const PROTOCOL_VERSION = "2025-06-18";
const SERVER_INFO = { name: "qjs-tools", version: "0.1.0" };

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

const TOOLS = [
  {
    name: "sysinfo",
    description:
      "Report host details for this machine: kernel, hardware model, CPU count, " +
      "physical memory, and uptime.",
    inputSchema: { type: "object", properties: {} },
    run() {
      const fields = [
        ["kernel", "uname -a"],
        ["model", "sysctl -n hw.model"],
        ["cpus", "sysctl -n hw.ncpu"],
        ["memory_bytes", "sysctl -n hw.memsize"],
        ["uptime", "uptime"],
      ];
      const out = [];
      for (const [label, cmd] of fields) {
        out.push(`${label}: ${shell(cmd).trim()}`);
      }
      return out.join("\n");
    },
  },
  {
    name: "disk_usage",
    description: "Show filesystem usage for a path (defaults to /).",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Path to report on. Default /." },
      },
    },
    run(args) {
      const path = typeof args.path === "string" && args.path ? args.path : "/";
      // Reject anything that could break out of the quoted argument.
      if (/['"`$\\;|&<>\n]/.test(path)) {
        throw new Error("path contains characters that are not allowed");
      }
      return shell(`df -h '${path}'`);
    },
  },
  {
    name: "port_installed",
    description:
      "Check whether a MacPorts port is installed on this machine, and report " +
      "its version. Useful before suggesting a tool that may be missing.",
    inputSchema: {
      type: "object",
      properties: {
        name: { type: "string", description: "Port name, e.g. git or gcc15." },
      },
      required: ["name"],
    },
    run(args) {
      const name = args.name;
      if (typeof name !== "string" || !/^[A-Za-z0-9._+-]+$/.test(name)) {
        throw new Error("invalid port name");
      }
      const out = shell(`/opt/local/bin/port -q installed ${name} 2>&1`).trim();
      if (!out || /^Error|not installed/i.test(out)) {
        return `${name} is not installed`;
      }
      return out;
    },
  },
];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function shell(cmd) {
  const f = std.popen(cmd, "r");
  if (!f) throw new Error("popen failed");
  let out = "";
  let chunk;
  while ((chunk = f.getline()) !== null) out += chunk + "\n";
  f.close();
  return out;
}

// stdout must carry nothing but protocol frames, so diagnostics go to stderr.
function logErr(msg) {
  std.err.puts(`[qjs-mcp] ${msg}\n`);
  std.err.flush();
}

function send(obj) {
  std.out.puts(JSON.stringify(obj) + "\n");
  std.out.flush();
}

function reply(id, result) {
  send({ jsonrpc: "2.0", id, result });
}

function replyError(id, code, message) {
  send({ jsonrpc: "2.0", id, error: { code, message } });
}

// ---------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------

function handle(msg) {
  const { id, method, params } = msg;
  const isNotification = id === undefined || id === null;

  switch (method) {
    case "initialize":
      reply(id, {
        protocolVersion: PROTOCOL_VERSION,
        capabilities: { tools: { listChanged: false } },
        serverInfo: SERVER_INFO,
      });
      return;

    case "notifications/initialized":
      return; // nothing to acknowledge

    case "ping":
      reply(id, {});
      return;

    case "tools/list":
      reply(id, {
        tools: TOOLS.map((t) => ({
          name: t.name,
          description: t.description,
          inputSchema: t.inputSchema,
        })),
      });
      return;

    case "tools/call": {
      const name = params && params.name;
      const tool = TOOLS.find((t) => t.name === name);
      if (!tool) {
        replyError(id, -32602, `unknown tool: ${name}`);
        return;
      }
      try {
        const text = tool.run((params && params.arguments) || {});
        reply(id, { content: [{ type: "text", text: String(text) }], isError: false });
      } catch (e) {
        // A failing tool is a normal result, not a protocol error -- the model
        // should see the message and be able to react to it.
        reply(id, {
          content: [{ type: "text", text: `Error: ${e.message || e}` }],
          isError: true,
        });
      }
      return;
    }

    default:
      if (!isNotification) replyError(id, -32601, `method not found: ${method}`);
  }
}

// ---------------------------------------------------------------------------
// Main loop: read newline-delimited JSON from stdin.
//
// std.in.getline() would be simpler, but it buffers in a way that stalls when
// the peer keeps the pipe open, so read raw bytes off fd 0 and frame manually.
// ---------------------------------------------------------------------------

function main() {
  const buf = new Uint8Array(65536);
  let pending = "";

  for (;;) {
    let n;
    try {
      n = os.read(0, buf.buffer, 0, buf.length);
    } catch (e) {
      break;
    }
    if (n <= 0) break; // EOF: the client closed stdin

    let chunk = "";
    for (let i = 0; i < n; i++) chunk += String.fromCharCode(buf[i]);
    pending += chunk;

    let nl;
    while ((nl = pending.indexOf("\n")) >= 0) {
      const line = pending.slice(0, nl).trim();
      pending = pending.slice(nl + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch (e) {
        logErr(`bad JSON: ${e.message}`);
        continue;
      }
      try {
        handle(msg);
      } catch (e) {
        logErr(`handler threw: ${e.message || e}`);
        if (msg && msg.id !== undefined && msg.id !== null) {
          replyError(msg.id, -32603, String(e.message || e));
        }
      }
    }
  }
}

main();
