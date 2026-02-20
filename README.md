# Mesen Expanded

Mesen Expanded is a fork developed based on [Mesen](https://github.com/SourMesen/Mesen2/).
 It features an MCP server for connecting to large language models (LLMs) and offers broader platform support, making it a multi-system emulator compatible with Windows, Linux, and macOS.

## Releases

The latest stable version is available from the [releases on GitHub](https://github.com/Hoshiruna/Mesen2-Expanded/releases).  

## Development Builds

#### <ins>Native builds</ins> (recommended) 

These builds don't require .NET to be installed.  

* [Windows 10 / 11]()  
* [Linux x64]()  (requires **SDL2**)
* [macOS - Intel]()  (requires **SDL2**)
* [macOS - Apple Silicon]()  (requires **SDL2**)

#### <ins>.NET builds</ins> 

These builds require **.NET 8** to be installed (except the Windows 7 build which requires .NET 6).  
For Linux and macOS, **SDL2** must also be installed.

* [Windows 7 / 8 (.NET 6)]()  
* [Linux x64 - AppImage]()  
* [Linux ARM64]()  
* [Linux ARM64 - AppImage]()


#### <ins>Notes</ins> 
**Any experimental features may not be stable. Please refer to the released version for final functionality.**

Other builds are also available in the [Actions](https://github.com/Hoshiruna/Mesen2-Expanded/actions) tab.

**SteamOS**: See [SteamOS.md](SteamOS.md)

## Compiling

See [COMPILING.md](COMPILING.md)

## MCP Debug Server (Mesen Expanded)

Mesen Expanded includes a built-in MCP server for debugger automation.

- Transport: HTTP (JSON-RPC 2.0)
- URL: `http://localhost:51234/mcp`
- Bind address: `127.0.0.1` (local machine only)
- Started automatically when the main window opens
- Protocol version: `2025-03-26`
- Server name: `Mesen2-MCP`

You can also copy the MCP URL from the app via `Debug -> MCP Server Info`.

### Claude Code

```bash
claude mcp add --transport http mesen-debugger http://localhost:51234/mcp
```

### Claude Desktop

Add this to your Claude Desktop config:

```json
{
  "mcpServers": {
    "mesen-debugger": {
      "url": "http://localhost:51234/mcp"
    }
  }
}
```

### Available MCP Tools

- `debugger_status`
- `get_cpu_state`
- `get_ppu_state`
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

## License

Mesen is available under the GPL V3 license.  Full text here: <http://www.gnu.org/licenses/gpl-3.0.en.html>

Copyright (C) 2014-2025 Sour

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
