# Mesen Expanded

Mesen Expanded is a fork of [Mesen](https://github.com/SourMesen/Mesen2/) with broader platform support and an MCP debugger bridge for AI-assisted workflows.

## Highlights

- Multi-system emulator for Windows, Linux, and macOS
- Native Genesis support and debugger integration
- MCP bridge support for external LLM/debugger clients
- Cross-platform release and CI build pipeline

## Supported Systems

Mesen Expanded currently includes support for:

- NES
- SNES
- Game Boy
- Game Boy Advance
- PC Engine / TurboGrafx-16
- Master System / Game Gear
- Genesis / Mega Drive (WIP)
- WonderSwan
- Other supported systems exposed by the current build, such as ColecoVision

## Releases

The latest stable version is available from the [GitHub Releases page](https://github.com/Hoshiruna/Mesen2-Expanded/releases).

## Development Builds

Development artifacts are produced by CI and can be downloaded from the [GitHub Actions page](https://github.com/Hoshiruna/Mesen2-Expanded/actions).

Notes:

- Native builds are recommended when available because they do not require a separate .NET runtime.
- .NET builds require the matching .NET runtime for that package.
- Linux and macOS builds may require `SDL2`.
- Experimental features may change without notice and may not match the latest stable release.

## Compiling

Build instructions are available in [COMPILING.md](COMPILING.md).

## MCP Debug Server

Mesen Expanded exposes debugger functionality through an MCP bridge for external tooling and AI-assisted debugging workflows.

- Setup and usage: [MCP_Server.md](MCP_Server.md)
- Safety and liability notes: [DISCLAIMER.md](DISCLAIMER.md)

## Disclaimer

This project includes MCP and AI-assisted debugging workflows. Review [DISCLAIMER](DISCLAIMER.md) before enabling those features or connecting third-party LLM services.

## License

Mesen is available under the GPL V3 license. Full text here: http://www.gnu.org/licenses/gpl-3.0.en.html

Copyright (C) 2014-2025 Sour

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see http://www.gnu.org/licenses/.
