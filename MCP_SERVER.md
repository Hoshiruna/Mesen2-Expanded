# MCP Debug Server

Mesen Expanded can expose debugger functionality through a Model Context Protocol (MCP) bridge for external tools and AI-assisted debugging workflows.

## Overview

The emulator-side debugger integration is designed for local use. External clients connect through an MCP bridge rather than directly modifying emulator internals.

Current implementation details visible in the codebase:

- Default HTTP endpoint: `http://127.0.0.1:51234/mcp/`
- Local-only binding by default
- Named-pipe bridge support for local tooling

## Typical Usage

There are two common ways to connect:

### 1. Stdio bridge

Use the `MCPServer.exe` bridge with tools that expect stdio transport.

Example:

```bash
codex mcp add mesen-debugger -- "path\\to\\MCPServer.exe" --stdio
```

### 2. HTTP bridge

Start the MCP bridge and connect a client to:

```text
http://127.0.0.1:51234/mcp/
```

Example:

```bash
claude mcp add --transport http mesen-debugger http://127.0.0.1:51234/mcp/
```

## Requirements

- Mesen Expanded must be running
- The debugger must be available in the current build
- The MCP bridge executable must be present when using bridge-based workflows

## Supported Platforms

Excluding Genesis, the MCP server currently has explicit structured debugger support for:

- `SNES`
- `NES`
- `Game Boy`

These platforms currently have dedicated `get_cpu_state` and `get_ppu_state` handling in the MCP server implementation.

Other systems may still work with generic debugger-oriented tools, but they do not currently have the same explicit MCP-side structured state handlers.

## Included Tools

### Generic Tools

These are debugger-oriented tools exposed by the MCP server and are not Genesis-specific:

- `debugger_status`
- `get_memory_range`
- `set_memory`
- `get_disassembly`
- `get_trace_tail`
- `get_debug_events`
- `set_breakpoints`
- `step`
- `resume`
- `pause`
- `get_rom_info`

### Structured Non-Genesis State Tools

For the currently supported non-Genesis platforms listed above, the MCP server also includes:

- `get_cpu_state`
- `get_ppu_state`

### Genesis-Specific Tools

Genesis-specific tools are available in the implementation, but they are intentionally omitted from the non-Genesis support list above:

- `get_vdp_registers`
- `get_cram`
- `get_vsram`
- `get_vram_range`
- `get_genesis_backend_state`
- `capture_genesis_snapshot`

## Security Notes

- The MCP endpoint is intended for localhost use only.
- Do not expose it to untrusted networks.
- Treat connected LLM clients as capable of reading emulator state and issuing debugger actions.

See [DISCLAIMER.md](DISCLAIMER.md) for liability, data exposure, and third-party service notes.

## Troubleshooting

If the MCP client cannot connect:

1. Confirm the emulator is running.
2. Confirm the MCP bridge is started when your workflow requires it.
3. Confirm the client is pointed at `127.0.0.1:51234`.
4. Confirm the build you are using includes MCP support.
