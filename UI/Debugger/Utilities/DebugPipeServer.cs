using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;
using System.Runtime.InteropServices;
using System.Threading;
using Mesen.Interop;
using Mesen.Debugger.Disassembly;

namespace Mesen.Debugger.Utilities
{
	// Named-pipe MCP server. MCPServer.exe connects here; the emulator never
	// opens an HTTP port on its own.  Protocol: line-delimited JSON (one request
	// line → one response line, both compact / newline-free).
	public class DebugPipeServer
	{
		private readonly object _lock = new();
		private Thread? _thread;
		private NamedPipeServerStream? _currentPipe;
		private volatile bool _running;
		private const string PipeName = "MesenDebug";
		private const string ServerName = "Mesen2-MCP";
		private const string ProtocolVersion = "2025-03-26";

		public void Start()
		{
			if(_running) return;
			_running = true;
			_thread = new Thread(Run) { IsBackground = true, Name = "DebugPipeServer" };
			_thread.Start();
		}

		public void Stop()
		{
			_running = false;
			lock(_lock) {
				_currentPipe?.Dispose();
				_currentPipe = null;
			}
			try {
				_thread?.Join(1000);
			} catch {
			}
		}

		// Accept one client at a time; loop to accept the next after disconnect.
		private void Run()
		{
			while(_running) {
				NamedPipeServerStream? pipe = null;
				try {
					pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 1,
						PipeTransmissionMode.Byte, PipeOptions.None, 65536, 65536);
					lock(_lock) {
						_currentPipe = pipe;
					}
					pipe.WaitForConnection();

					// Use UTF-8 without BOM so every line is plain JSON text.
					// A BOM prefix on the first response can break strict MCP clients.
					using var reader = new StreamReader(pipe, new UTF8Encoding(false), false, 65536, true);
					using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 65536, true) { AutoFlush = true };

					string? line;
					while(_running && (line = reader.ReadLine()) != null) {
						string response = HandleJsonRpc(line);
						writer.WriteLine(response);
					}
				} catch { }
				finally {
					lock(_lock) {
						if(ReferenceEquals(_currentPipe, pipe)) {
							_currentPipe = null;
						}
					}
					pipe?.Dispose();
				}
			}
		}

		// =================================================================
		// JSON-RPC 2.0 dispatch
		// =================================================================

		private string HandleJsonRpc(string body)
		{
			JsonNode? node;
			try { node = JsonNode.Parse(body); }
			catch { return MakeJsonRpcError(JsonValue.Create(0), -32700, "Parse error"); }

			if(node == null)
				return MakeJsonRpcError(JsonValue.Create(0), -32700, "Empty request");

			string? method = node["method"]?.GetValue<string>();
			JsonNode? id = node["id"];
			JsonObject? parms = node["params"] as JsonObject;

			// Notification (no id) – no reply
			if(id == null)
				return "{}";

			JsonNode idClone = JsonNode.Parse(id.ToJsonString())!;

			return method switch {
				"initialize"  => HandleInitialize(idClone),
				"ping"        => MakeJsonRpcResult(idClone, new JsonObject()),
				"tools/list"  => HandleToolsList(idClone),
				"tools/call"  => HandleToolsCall(idClone, parms),
				_ => MakeJsonRpcError(idClone, -32601, $"Unknown method: {method}")
			};
		}

		// =================================================================
		// JSON-RPC helpers
		// =================================================================

		private static string MakeJsonRpcResult(JsonNode id, JsonObject result)
		{
			return new JsonObject {
				["jsonrpc"] = "2.0",
				["id"] = id,
				["result"] = result
			}.ToJsonString();
		}

		private static string MakeJsonRpcError(JsonNode id, int code, string message)
		{
			return new JsonObject {
				["jsonrpc"] = "2.0",
				["id"] = id,
				["error"] = new JsonObject { ["code"] = code, ["message"] = message }
			}.ToJsonString();
		}

		// =================================================================
		// initialize
		// =================================================================

		private string HandleInitialize(JsonNode id)
		{
			var result = new JsonObject {
				["protocolVersion"] = ProtocolVersion,
				["capabilities"] = new JsonObject {
					["tools"] = new JsonObject { ["listChanged"] = false }
				},
				["serverInfo"] = new JsonObject {
					["name"] = ServerName,
					["version"] = "1.0.0"
				}
			};
			return MakeJsonRpcResult(id, result);
		}

		// =================================================================
		// tools/list
		// =================================================================

		private string HandleToolsList(JsonNode id)
		{
			var tools = new JsonArray();

			tools.Add((JsonNode)MakeToolDef("debugger_status",
				"Check if the Mesen2 debugger is running and get emulator state.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("get_cpu_state",
				"Get CPU register state. Supports GenesisMain(13) with full 68K register output.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_ppu_state",
				"Get PPU/graphics chip state. Supports GenesisMain(13) with VDP timing output.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_memory_range",
				"Read memory bytes (max 4096). Memory types: SnesMemory=0, NesMemory=8, GameboyMemory=7, etc.",
				new JsonObject {
					["type"] = "object",
					["required"] = MakeStringArray("memory_type", "start_address", "length"),
					["properties"] = new JsonObject {
						["memory_type"]   = new JsonObject { ["type"] = "integer" },
						["start_address"] = new JsonObject { ["type"] = "integer" },
						["length"]        = new JsonObject { ["type"] = "integer" }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("set_memory",
				"Write bytes to memory.",
				new JsonObject {
					["type"] = "object",
					["required"] = MakeStringArray("memory_type", "address", "data"),
					["properties"] = new JsonObject {
						["memory_type"] = new JsonObject { ["type"] = "integer" },
						["address"]     = new JsonObject { ["type"] = "integer" },
						["data"]        = new JsonObject { ["type"] = "array", ["items"] = new JsonObject { ["type"] = "integer" } }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_disassembly",
				"Get disassembled code. address=-1 uses current PC.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"]   = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["address"]    = new JsonObject { ["type"] = "integer", ["default"] = -1 },
						["line_count"] = new JsonObject { ["type"] = "integer", ["default"] = 20 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_trace_tail",
				"Get recent execution trace.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["count"]  = new JsonObject { ["type"] = "integer", ["default"] = 100 },
						["offset"] = new JsonObject { ["type"] = "integer", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_debug_events",
				"Get recent debug events (breakpoints, IRQs, DMAs).",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"]  = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["max_count"] = new JsonObject { ["type"] = "integer", ["default"] = 100 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("set_breakpoints",
				"Set execution/read/write breakpoints (replaces all existing).",
				new JsonObject {
					["type"] = "object",
					["required"] = MakeStringArray("breakpoints"),
					["properties"] = new JsonObject {
						["breakpoints"] = new JsonObject {
							["type"] = "array",
							["items"] = new JsonObject {
								["type"] = "object",
								["properties"] = new JsonObject {
									["type"]        = new JsonObject { ["type"] = "integer", ["description"] = "Execute=1, Read=2, Write=4" },
									["address"]     = new JsonObject { ["type"] = "integer" },
									["cpu_type"]    = new JsonObject { ["type"] = "integer", ["default"] = 0 },
									["memory_type"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
									["enabled"]     = new JsonObject { ["type"] = "boolean", ["default"] = true },
									["end_address"] = new JsonObject { ["type"] = "integer" },
									["condition"]   = new JsonObject { ["type"] = "string" }
								}
							}
						}
					}
				}));

			tools.Add((JsonNode)MakeToolDef("step",
				"Step execution. step_type: 0=Step, 1=StepOut, 2=StepOver, 4=PpuStep, 5=PpuScanline, 6=PpuFrame.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"]  = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["count"]     = new JsonObject { ["type"] = "integer", ["default"] = 1 },
						["step_type"] = new JsonObject { ["type"] = "integer", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("resume", "Resume execution.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("pause", "Pause emulation.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("get_rom_info", "Get loaded ROM information.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("get_vdp_registers",
				"Get the current Genesis VDP register file (R0-R23).",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("get_cram",
				"Read Genesis CRAM bytes. start_address defaults to 0, length defaults to 128 bytes.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["start_address"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["length"] = new JsonObject { ["type"] = "integer", ["default"] = 128 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_vsram",
				"Read Genesis VSRAM bytes. start_address defaults to 0, length defaults to 80 bytes.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["start_address"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["length"] = new JsonObject { ["type"] = "integer", ["default"] = 80 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_vram_range",
				"Read Genesis VRAM bytes. start_address defaults to 0, length defaults to 256 bytes.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["start_address"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["length"] = new JsonObject { ["type"] = "integer", ["default"] = 256 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_genesis_backend_state",
				"Get Genesis native-core scheduler, DMA, IRQ, line timing, and Z80 bus state.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("capture_genesis_snapshot",
				"Capture a deterministic Genesis debug snapshot with CPU state, VDP state, registers, backend state, selected VRAM/CRAM/VSRAM ranges, and recent trace. Optional trigger arguments: trigger_pc, trigger_scanline, or trigger_vdp_register.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["trace_count"] = new JsonObject { ["type"] = "integer", ["default"] = 32 },
						["vram_start"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["vram_length"] = new JsonObject { ["type"] = "integer", ["default"] = 256 },
						["cram_start"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["cram_length"] = new JsonObject { ["type"] = "integer", ["default"] = 128 },
						["vsram_start"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
						["vsram_length"] = new JsonObject { ["type"] = "integer", ["default"] = 80 },
						["trigger_pc"] = new JsonObject { ["type"] = "integer" },
						["trigger_scanline"] = new JsonObject { ["type"] = "integer" },
						["trigger_vdp_register"] = new JsonObject { ["type"] = "integer" },
						["trigger_vdp_value"] = new JsonObject { ["type"] = "integer" },
						["max_steps"] = new JsonObject { ["type"] = "integer", ["default"] = 200000 }
					}
				}));

			return MakeJsonRpcResult(id, new JsonObject { ["tools"] = tools });
		}

		private static JsonObject MakeToolDef(string name, string description, JsonObject inputSchema)
		{
			return new JsonObject {
				["name"] = name,
				["description"] = description,
				["inputSchema"] = inputSchema
			};
		}

		private static JsonArray MakeStringArray(params string[] values)
		{
			var arr = new JsonArray();
			foreach(string v in values)
				arr.Add((JsonNode?)JsonValue.Create(v));
			return arr;
		}

		// =================================================================
		// tools/call dispatch
		// =================================================================

		private string HandleToolsCall(JsonNode id, JsonObject? parms)
		{
			string? toolName = parms?["name"]?.GetValue<string>();
			JsonObject? args = parms?["arguments"] as JsonObject;

			if(string.IsNullOrEmpty(toolName))
				return MakeToolError(id, "Missing tool name");

			try {
				return toolName switch {
					"debugger_status"  => HandleDebuggerStatus(id),
					"get_cpu_state"    => HandleGetCpuState(id, args),
					"get_ppu_state"    => HandleGetPpuState(id, args),
					"get_memory_range" => HandleGetMemoryRange(id, args),
					"set_memory"       => HandleSetMemory(id, args),
					"get_disassembly"  => HandleGetDisassembly(id, args),
					"get_trace_tail"   => HandleGetTraceTail(id, args),
					"get_debug_events" => HandleGetDebugEvents(id, args),
					"set_breakpoints"  => HandleSetBreakpoints(id, args),
					"step"             => HandleStep(id, args),
					"resume"           => HandleResume(id),
					"pause"            => HandlePause(id),
					"get_rom_info"     => HandleGetRomInfo(id),
					"get_vdp_registers" => HandleGetVdpRegisters(id),
					"get_cram" => HandleGetCram(id, args),
					"get_vsram" => HandleGetVsram(id, args),
					"get_vram_range" => HandleGetVramRange(id, args),
					"get_genesis_backend_state" => HandleGetGenesisBackendState(id),
					"capture_genesis_snapshot" => HandleCaptureGenesisSnapshot(id, args),
					_ => MakeToolError(id, $"Unknown tool: {toolName}")
				};
			} catch(Exception ex) {
				return MakeToolError(id, $"Tool error: {ex.Message}");
			}
		}

		private static string MakeToolResult(JsonNode id, string text, bool isError)
		{
			var content = new JsonArray();
			content.Add((JsonNode)new JsonObject { ["type"] = "text", ["text"] = text });
			var result = new JsonObject { ["content"] = content };
			if(isError) result["isError"] = true;
			return MakeJsonRpcResult(id, result);
		}

		private static string MakeToolSuccess(JsonNode id, JsonObject data) =>
			MakeToolResult(id, data.ToJsonString(), false);

		private static string MakeToolError(JsonNode id, string message) =>
			MakeToolResult(id, message, true);

		// =================================================================
		// Individual tool handlers (identical logic to McpDebugServer)
		// =================================================================

		private string HandleDebuggerStatus(JsonNode id)
		{
			bool isRunning = EmuApi.IsRunning();
			bool isPaused  = EmuApi.IsPaused();
			bool debuggerRunning = DebugApi.IsDebuggerRunning();
			bool executionStopped = debuggerRunning && DebugApi.IsExecutionStopped();
			return MakeToolSuccess(id, new JsonObject {
				["debugger_running"]   = debuggerRunning,
				["emulation_running"]  = isRunning,
				["execution_stopped"]  = executionStopped,
				["emulation_paused"]   = isPaused,
				["mesen_version"]      = "2.0"
			});
		}

		private string HandleGetCpuState(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			JsonObject data;

			switch(cpuType) {
				case CpuType.GenesisMain: {
					data = GenesisMcpDebugHelper.BuildCpuState();
					break;
				}
				case CpuType.Snes: {
					var s = DebugApi.GetCpuState<SnesCpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType, ["cpu_type_name"] = cpuType.ToString(),
						["PC"] = s.PC, ["K"] = s.K, ["A"] = s.A, ["X"] = s.X, ["Y"] = s.Y,
						["SP"] = s.SP, ["D"] = s.D, ["DBR"] = s.DBR,
						["PS"] = (int)s.PS, ["emulation_mode"] = s.EmulationMode,
						["cycle_count"] = s.CycleCount
					};
					break;
				}
				case CpuType.Gameboy: {
					var s = DebugApi.GetCpuState<GbCpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType, ["cpu_type_name"] = cpuType.ToString(),
						["PC"] = s.PC, ["SP"] = s.SP, ["A"] = s.A, ["flags"] = s.Flags,
						["B"] = s.B, ["C"] = s.C, ["D"] = s.D, ["E"] = s.E,
						["H"] = s.H, ["L"] = s.L,
						["halt_counter"] = s.HaltCounter, ["cycle_count"] = s.CycleCount
					};
					break;
				}
				case CpuType.Nes: {
					var s = DebugApi.GetCpuState<NesCpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType, ["cpu_type_name"] = cpuType.ToString(),
						["PC"] = s.PC, ["SP"] = s.SP, ["A"] = s.A, ["X"] = s.X, ["Y"] = s.Y,
						["PS"] = s.PS, ["cycle_count"] = s.CycleCount
					};
					break;
				}
				default: {
					data = new JsonObject {
						["cpu_type"] = (int)cpuType, ["cpu_type_name"] = cpuType.ToString(),
						["program_counter"] = DebugApi.GetProgramCounter(cpuType, false)
					};
					break;
				}
			}

			return MakeToolSuccess(id, data);
		}

		private string HandleGetPpuState(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			JsonObject data;

			switch(cpuType) {
				case CpuType.GenesisMain: {
					data = GenesisMcpDebugHelper.BuildPpuState();
					break;
				}
				case CpuType.Snes: {
					var s = DebugApi.GetPpuState<SnesPpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["scanline"] = s.Scanline, ["cycle"] = s.Cycle, ["frame_count"] = s.FrameCount
					};
					break;
				}
				case CpuType.Gameboy: {
					var s = DebugApi.GetPpuState<GbPpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["scanline"] = s.Scanline, ["cycle"] = s.Cycle, ["frame_count"] = s.FrameCount
					};
					break;
				}
				case CpuType.Nes: {
					var s = DebugApi.GetPpuState<NesPpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["scanline"] = s.Scanline, ["cycle"] = s.Cycle, ["frame_count"] = s.FrameCount
					};
					break;
				}
				default: {
					data = new JsonObject { ["cpu_type"] = (int)cpuType, ["error"] = "Unsupported PPU type" };
					break;
				}
			}

			return MakeToolSuccess(id, data);
		}

		private string HandleGetMemoryRange(JsonNode id, JsonObject? args)
		{
			if(args == null) return MakeToolError(id, "Missing arguments");
			int memType   = args["memory_type"]?.GetValue<int>() ?? 0;
			int startAddr = args["start_address"]?.GetValue<int>() ?? 0;
			int length    = Math.Min(args["length"]?.GetValue<int>() ?? 256, 4096);

			MemoryType mt = (MemoryType)memType;
			byte[] bytes = DebugApi.GetMemoryValues(mt, (uint)startAddr, (uint)(startAddr + length - 1));

			var hex = new StringBuilder();
			for(int i = 0; i < bytes.Length; i++) {
				if(i > 0) hex.Append(' ');
				hex.Append(bytes[i].ToString("X2"));
			}

			return MakeToolSuccess(id, new JsonObject {
				["memory_type"]   = memType,
				["start_address"] = startAddr,
				["length"]        = bytes.Length,
				["hex"]           = hex.ToString()
			});
		}

		private string HandleSetMemory(JsonNode id, JsonObject? args)
		{
			if(args == null) return MakeToolError(id, "Missing arguments");
			int memType  = args["memory_type"]?.GetValue<int>() ?? 0;
			int address  = args["address"]?.GetValue<int>() ?? 0;
			JsonArray? dataArr = args["data"] as JsonArray;
			if(dataArr == null || dataArr.Count == 0) return MakeToolError(id, "Missing data array");

			byte[] bytes = new byte[dataArr.Count];
			for(int i = 0; i < dataArr.Count; i++)
				bytes[i] = (byte)(dataArr[i]?.GetValue<int>() ?? 0);

			DebugApi.SetMemoryValues((MemoryType)memType, (uint)address, bytes, bytes.Length);

			return MakeToolSuccess(id, new JsonObject {
				["success"] = true, ["bytes_written"] = bytes.Length
			});
		}

		private string HandleGetDisassembly(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			int address   = args?["address"]?.GetValue<int>() ?? -1;
			int lineCount = Math.Clamp(args?["line_count"]?.GetValue<int>() ?? 20, 1, 100);

			uint addr = address < 0 ? DebugApi.GetProgramCounter(cpuType, false) : (uint)address;

			int startRow = DebugApi.GetDisassemblyRowAddress(cpuType, addr, -(lineCount / 2));
			CodeLineData[] lines = DebugApi.GetDisassemblyOutput(cpuType, (uint)startRow, (uint)lineCount);

			var linesArray = new JsonArray();
			foreach(var line in lines) {
				if(line.Address < 0) continue;
				linesArray.Add((JsonNode)new JsonObject {
					["address"] = line.Address,
					["text"]    = line.Text.Trim(),
					["bytes"]   = line.ByteCodeStr.Trim(),
					["size"]    = line.OpSize
				});
			}

			return MakeToolSuccess(id, new JsonObject {
				["cpu_type"]   = (int)cpuType,
				["current_pc"] = DebugApi.GetProgramCounter(cpuType, false),
				["lines"]      = linesArray
			});
		}

		private string HandleGetTraceTail(JsonNode id, JsonObject? args)
		{
			int count  = Math.Min(args?["count"]?.GetValue<int>() ?? 100, 1000);
			int offset = args?["offset"]?.GetValue<int>() ?? 0;

			TraceRow[] rows = DebugApi.GetExecutionTrace((uint)offset, (uint)count);
			var linesArray = new JsonArray();
			foreach(var row in rows) {
				var hex = new StringBuilder();
				byte[] byteCode = row.GetByteCode();
				for(int i = 0; i < byteCode.Length; i++) {
					if(i > 0) hex.Append(' ');
					hex.Append(byteCode[i].ToString("X2"));
				}
				linesArray.Add((JsonNode)new JsonObject {
					["pc"]    = row.ProgramCounter,
					["text"]  = row.GetOutput(),
					["bytes"] = hex.ToString()
				});
			}

			return MakeToolSuccess(id, new JsonObject {
				["count"] = linesArray.Count, ["lines"] = linesArray
			});
		}

		private string HandleGetDebugEvents(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			int maxCount = Math.Min(args?["max_count"]?.GetValue<int>() ?? 100, 1000);

			DebugEventInfo[] events = DebugApi.GetDebugEvents(cpuType);
			var eventsArray = new JsonArray();
			int limit = Math.Min(events.Length, maxCount);
			for(int i = 0; i < limit; i++) {
				var evt = events[i];
				eventsArray.Add((JsonNode)new JsonObject {
					["type"]          = evt.Type.ToString(),
					["pc"]            = evt.ProgramCounter,
					["scanline"]      = evt.Scanline,
					["cycle"]         = evt.Cycle,
					["breakpoint_id"] = evt.BreakpointId
				});
			}

			return MakeToolSuccess(id, new JsonObject {
				["cpu_type"] = (int)cpuType, ["count"] = eventsArray.Count, ["events"] = eventsArray
			});
		}

		private string HandleSetBreakpoints(JsonNode id, JsonObject? args)
		{
			JsonArray? bpArray = args?["breakpoints"] as JsonArray;
			if(bpArray == null) return MakeToolError(id, "Missing breakpoints array");

			var breakpoints = new InteropBreakpoint[bpArray.Count];
			for(int i = 0; i < bpArray.Count; i++) {
				var bp = bpArray[i] as JsonObject;
				if(bp == null) continue;

				breakpoints[i] = new InteropBreakpoint {
					Id           = i,
					Type         = (BreakpointTypeFlags)(bp["type"]?.GetValue<int>() ?? 1),
					StartAddress = bp["address"]?.GetValue<int>() ?? 0,
					EndAddress   = bp["end_address"]?.GetValue<int>() ?? -1,
					CpuType      = (CpuType)(bp["cpu_type"]?.GetValue<int>() ?? 0),
					MemoryType   = (MemoryType)(bp["memory_type"]?.GetValue<int>() ?? 0),
					Enabled      = bp["enabled"]?.GetValue<bool>() ?? true,
					MarkEvent    = false,
					IgnoreDummyOperations = false,
					Condition    = new byte[1000]
				};

				string? condition = bp["condition"]?.GetValue<string>();
				if(!string.IsNullOrEmpty(condition)) {
					byte[] condBytes = Encoding.UTF8.GetBytes(condition);
					Array.Copy(condBytes, breakpoints[i].Condition, Math.Min(condBytes.Length, 999));
				}

				if(breakpoints[i].EndAddress < 0)
					breakpoints[i].EndAddress = breakpoints[i].StartAddress;
			}

			DebugApi.SetBreakpoints(breakpoints, (uint)breakpoints.Length);

			return MakeToolSuccess(id, new JsonObject {
				["success"] = true, ["breakpoints_set"] = breakpoints.Length
			});
		}

		private string HandleStep(JsonNode id, JsonObject? args)
		{
			CpuType cpuType  = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			int count        = args?["count"]?.GetValue<int>() ?? 1;
			StepType stepType = (StepType)(args?["step_type"]?.GetValue<int>() ?? 0);

			DebugApi.Step(cpuType, count, stepType);

			return MakeToolSuccess(id, new JsonObject {
				["success"] = true, ["cpu_type"] = (int)cpuType,
				["step_type"] = (int)stepType, ["count"] = count
			});
		}

		private string HandleResume(JsonNode id)
		{
			DebugApi.ResumeExecution();
			return MakeToolSuccess(id, new JsonObject { ["success"] = true });
		}

		private string HandlePause(JsonNode id)
		{
			CpuType cpuType = EmuApi.GetRomInfo().ConsoleType.GetMainCpuType();
			DebugApi.Step(cpuType, 1, StepType.Step);
			return MakeToolSuccess(id, new JsonObject { ["success"] = true });
		}

		private string HandleGetRomInfo(JsonNode id)
		{
			RomInfo romInfo = EmuApi.GetRomInfo();
			var cpuTypesArray = new JsonArray();
			foreach(CpuType ct in romInfo.CpuTypes)
				cpuTypesArray.Add((JsonNode)new JsonObject { ["id"] = (int)ct, ["name"] = ct.ToString() });

			return MakeToolSuccess(id, new JsonObject {
				["console_type"]      = (int)romInfo.ConsoleType,
				["console_type_name"] = romInfo.ConsoleType.ToString(),
				["rom_path"]          = romInfo.RomPath,
				["format"]            = romInfo.Format.ToString(),
				["cpu_types"]         = cpuTypesArray
			});
		}

		private string HandleGetVdpRegisters(JsonNode id)
		{
			return MakeToolSuccess(id, GenesisMcpDebugHelper.BuildVdpRegisters());
		}

		private string HandleGetCram(JsonNode id, JsonObject? args)
		{
			return MakeToolSuccess(id, GenesisMcpDebugHelper.BuildCram(args));
		}

		private string HandleGetVsram(JsonNode id, JsonObject? args)
		{
			return MakeToolSuccess(id, GenesisMcpDebugHelper.BuildVsram(args));
		}

		private string HandleGetVramRange(JsonNode id, JsonObject? args)
		{
			return MakeToolSuccess(id, GenesisMcpDebugHelper.BuildVramRange(args));
		}

		private string HandleGetGenesisBackendState(JsonNode id)
		{
			return MakeToolSuccess(id, GenesisMcpDebugHelper.BuildBackendState());
		}

		private string HandleCaptureGenesisSnapshot(JsonNode id, JsonObject? args)
		{
			return MakeToolSuccess(id, GenesisMcpDebugHelper.CaptureSnapshot(args));
		}
	}
}
