#!/usr/bin/env python3
"""A minimal Streamable-HTTP MCP server, used to exercise ppcode's http transport.

This is a test fixture, not a production server. It exists because the useful
MCP servers are npm packages and this machine has no Node.js -- so to prove the
HTTP path works, we need something to talk to. In real use you would point
ppcode at a server running on another machine on the LAN.

  python3 scripts/http_mcp_test_server.py 8765
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PROTOCOL_VERSION = "2025-06-18"

# Real servers hand out a session id on initialize and reject anything that
# comes back without it. Modelled here because a client that ignored the header
# looked, from the outside, exactly like a server with no tools: the handshake
# succeeded and tools/list came back 400.
SESSION_ID = "ppcode-test-session"

TOOLS = [
    {
        "name": "echo",
        "description": "Echo back the text you send. Used to verify transport wiring.",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string", "description": "Text to echo."}},
            "required": ["text"],
        },
    },
    {
        "name": "add",
        "description": "Add two numbers and return the sum.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "a": {"type": "number", "description": "First addend."},
                "b": {"type": "number", "description": "Second addend."},
            },
            "required": ["a", "b"],
        },
    },
]


def call_tool(name, args):
    if name == "echo":
        return f"echo: {args.get('text', '')}", False
    if name == "add":
        try:
            return str(float(args["a"]) + float(args["b"])), False
        except (KeyError, TypeError, ValueError) as e:
            return f"Error: {e}", True
    return f"unknown tool {name}", True


def handle(msg):
    """Return a JSON-RPC response dict, or None for a notification."""
    mid = msg.get("id")
    method = msg.get("method")
    params = msg.get("params") or {}

    if method == "initialize":
        result = {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "http-test", "version": "0.1.0"},
        }
    elif method == "notifications/initialized":
        return None
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        text, is_err = call_tool(params.get("name"), params.get("arguments") or {})
        result = {"content": [{"type": "text", "text": text}], "isError": is_err}
    else:
        if mid is None:
            return None
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601,
                                                       "message": f"no method {method}"}}

    if mid is None:
        return None
    return {"jsonrpc": "2.0", "id": mid, "result": result}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass  # keep the terminal quiet

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        try:
            msg = json.loads(body)
        except json.JSONDecodeError:
            self.send_response(400)
            self.end_headers()
            return

        # initialize opens the session; everything after it must present the id.
        is_init = msg.get("method") == "initialize"
        if not is_init and self.headers.get("Mcp-Session-Id") != SESSION_ID:
            payload = b'{"error":"Mcp-Session-Id required"}'
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        response = handle(msg)
        if response is None:
            # Notifications get an empty 202, per the spec.
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        payload = json.dumps(response).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        if is_init:
            self.send_header("Mcp-Session-Id", SESSION_ID)
        self.end_headers()
        self.wfile.write(payload)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    # Threading matters: with HTTP/1.1 keep-alive a single-threaded server can
    # stall a second client while the first connection is still open.
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"http mcp test server on 127.0.0.1:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
