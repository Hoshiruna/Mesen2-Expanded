using System;
using System.IO;
using System.Net;
using System.Text;
using System.Threading;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Runtime.InteropServices;
using Mesen.Interop;
using Mesen.Debugger.Disassembly;

namespace Mesen.Debugger.Utilities
{
	public class McpDebugServer
	{
		private HttpListener? _listener;
		private Thread? _thread;
		private volatile bool _running;
		private const string Prefix = "http://127.0.0.1:51234/mcp/";
		private const string ServerName = "Mesen2-MCP";
		private const string ProtocolVersion = "2025-03-26";

		public void Start()
		{
			if(_running) return;
			_running = true;
			_thread = new Thread(Run) { IsBackground = true, Name = "McpDebugServer" };
			_thread.Start();
		}

		public void Stop()
		{
			_running = false;
			try { _listener?.Stop(); } catch { }
			_listener = null;
		}

		private void Run()
		{
			try {
				_listener = new HttpListener();
				_listener.Prefixes.Add(Prefix);
				_listener.Start();
			} catch(Exception ex) {
				Console.Error.WriteLine($"[MCP] Failed to start HTTP listener: {ex.Message}");
				_running = false;
				return;
			}

			Console.Error.WriteLine($"[MCP] Listening on {Prefix}");

			while(_running) {
				HttpListenerContext ctx;
				try {
					ctx = _listener.GetContext();
				} catch {
					break;
				}

				try {
					HandleRequest(ctx);
				} catch(Exception ex) {
					Console.Error.WriteLine($"[MCP] Error handling request: {ex.Message}");
					try {
						SendJsonResponse(ctx.Response, 500, MakeJsonRpcError(
							JsonValue.Create(0), -32603, "Internal error"));
					} catch { }
				}
			}
		}

		private void HandleRequest(HttpListenerContext ctx)
		{
			var req = ctx.Request;
			var resp = ctx.Response;

			// CORS headers for browser-based clients
			resp.Headers.Set("Access-Control-Allow-Origin", "*");
			resp.Headers.Set("Access-Control-Allow-Methods", "POST, OPTIONS");
			resp.Headers.Set("Access-Control-Allow-Headers", "Content-Type");

			if(req.HttpMethod == "OPTIONS") {
				resp.StatusCode = 204;
				resp.Close();
				return;
			}

			if(req.HttpMethod != "POST") {
				SendJsonResponse(resp, 405, MakeJsonRpcError(
					JsonValue.Create(0), -32600, "Only POST is supported"));
				return;
			}

			string body;
			using(var reader = new StreamReader(req.InputStream, Encoding.UTF8)) {
				body = reader.ReadToEnd();
			}

			JsonNode? node;
			try {
				node = JsonNode.Parse(body);
			} catch {
				SendJsonResponse(resp, 400, MakeJsonRpcError(
					JsonValue.Create(0), -32700, "Parse error"));
				return;
			}

			if(node == null) {
				SendJsonResponse(resp, 400, MakeJsonRpcError(
					JsonValue.Create(0), -32700, "Empty request"));
				return;
			}

			string? method = node["method"]?.GetValue<string>();
			JsonNode? id = node["id"];
			JsonObject? parms = node["params"] as JsonObject;

			// Notifications (no id) - just accept
			if(id == null) {
				resp.StatusCode = 202;
				resp.Close();
				return;
			}

			// Clone the id for safe reuse
			JsonNode idClone = JsonNode.Parse(id.ToJsonString())!;

			string result = method switch {
				"initialize" => HandleInitialize(idClone),
				"ping" => MakeJsonRpcResult(idClone, new JsonObject()),
				"tools/list" => HandleToolsList(idClone),
				"tools/call" => HandleToolsCall(idClone, parms),
				_ => MakeJsonRpcError(idClone, -32601, $"Unknown method: {method}")
			};

			SendJsonResponse(resp, 200, result);
		}

		private static void SendJsonResponse(HttpListenerResponse resp, int status, string json)
		{
			resp.StatusCode = status;
			resp.ContentType = "application/json";
			byte[] data = Encoding.UTF8.GetBytes(json);
			resp.ContentLength64 = data.Length;
			resp.OutputStream.Write(data, 0, data.Length);
			resp.Close();
		}

		// =================================================================
		// JSON-RPC 2.0 helpers
		// =================================================================

		private static string MakeJsonRpcResult(JsonNode id, JsonObject result)
		{
			var envelope = new JsonObject {
				["jsonrpc"] = "2.0",
				["id"] = id,
				["result"] = result
			};
			return envelope.ToJsonString();
		}

		private static string MakeJsonRpcError(JsonNode id, int code, string message)
		{
			var envelope = new JsonObject {
				["jsonrpc"] = "2.0",
				["id"] = id,
				["error"] = new JsonObject {
					["code"] = code,
					["message"] = message
				}
			};
			return envelope.ToJsonString();
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
				"Check if the Mesen2 debugger is running and get emulator state. Returns debugger_running, emulation_running, execution_stopped, emulation_paused, and mesen_version.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("get_cpu_state",
				"Get CPU register state including program counter, registers, and flags. cpu_type: 0=SNES, 3=SA1, 7=Gameboy, 8=NES.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["description"] = "CPU type: 0=SNES, 3=SA1, 7=Gameboy, 8=NES", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_ppu_state",
				"Get PPU/graphics chip state (scanline, cycle, frame count). cpu_type: 0=SNES, 7=Gameboy, 8=NES.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["description"] = "CPU type: 0=SNES, 7=Gameboy, 8=NES", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_memory_range",
				"Read a range of memory bytes. Max 4096 bytes per request. Memory types: SnesMemory(0), SnesWorkRam(15), SnesPrgRom(14), NesMemory(8), NesInternalRam(46), NesPrgRom(45), GameboyMemory(7), GbWorkRam(39), GbPrgRom(38), etc.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["memory_type"] = new JsonObject { ["type"] = "integer", ["description"] = "Memory type enum value" },
						["start_address"] = new JsonObject { ["type"] = "integer", ["description"] = "Starting address" },
						["length"] = new JsonObject { ["type"] = "integer", ["description"] = "Number of bytes to read (max 4096)" }
					},
					["required"] = MakeStringArray("memory_type", "start_address", "length")
				}));

			tools.Add((JsonNode)MakeToolDef("set_memory",
				"Write bytes to memory. Provide data as array of byte values.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["memory_type"] = new JsonObject { ["type"] = "integer", ["description"] = "Memory type enum value" },
						["address"] = new JsonObject { ["type"] = "integer", ["description"] = "Target address" },
						["data"] = new JsonObject { ["type"] = "array", ["items"] = new JsonObject { ["type"] = "integer" }, ["description"] = "Array of byte values (0-255)" }
					},
					["required"] = MakeStringArray("memory_type", "address", "data")
				}));

			tools.Add((JsonNode)MakeToolDef("get_disassembly",
				"Get disassembled code around an address. Returns instruction text, bytes, and addresses.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["description"] = "CPU type: 0=SNES, 7=Gameboy, 8=NES", ["default"] = 0 },
						["address"] = new JsonObject { ["type"] = "integer", ["description"] = "Address to disassemble at. -1 or omit for current PC", ["default"] = -1 },
						["line_count"] = new JsonObject { ["type"] = "integer", ["description"] = "Number of lines (max 100)", ["default"] = 20 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_trace_tail",
				"Get recent execution trace lines with PC, bytes, and disassembly text.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["count"] = new JsonObject { ["type"] = "integer", ["description"] = "Number of lines (max 1000)", ["default"] = 100 },
						["offset"] = new JsonObject { ["type"] = "integer", ["description"] = "Offset from most recent", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("get_debug_events",
				"Get recent debug events (breakpoints hit, IRQs, NMIs, DMA).",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["description"] = "CPU type: 0=SNES, 7=Gameboy, 8=NES", ["default"] = 0 },
						["max_count"] = new JsonObject { ["type"] = "integer", ["description"] = "Maximum events to return (max 1000)", ["default"] = 100 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("set_breakpoints",
				"Set execution/read/write breakpoints. Replaces all existing breakpoints.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["breakpoints"] = new JsonObject {
							["type"] = "array",
							["items"] = new JsonObject {
								["type"] = "object",
								["properties"] = new JsonObject {
									["type"] = new JsonObject { ["type"] = "integer", ["description"] = "BreakpointTypeFlags: Execute=1, Read=2, Write=4" },
									["address"] = new JsonObject { ["type"] = "integer" },
									["cpu_type"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
									["memory_type"] = new JsonObject { ["type"] = "integer", ["default"] = 0 },
									["enabled"] = new JsonObject { ["type"] = "boolean", ["default"] = true },
									["end_address"] = new JsonObject { ["type"] = "integer" },
									["condition"] = new JsonObject { ["type"] = "string" }
								}
							},
							["description"] = "Array of breakpoint objects"
						}
					},
					["required"] = MakeStringArray("breakpoints")
				}));

			tools.Add((JsonNode)MakeToolDef("step",
				"Step execution by instruction, scanline, or frame.",
				new JsonObject {
					["type"] = "object",
					["properties"] = new JsonObject {
						["cpu_type"] = new JsonObject { ["type"] = "integer", ["description"] = "CPU type: 0=SNES, 7=Gameboy, 8=NES", ["default"] = 0 },
						["count"] = new JsonObject { ["type"] = "integer", ["description"] = "Number of steps", ["default"] = 1 },
						["step_type"] = new JsonObject { ["type"] = "integer", ["description"] = "StepType: 0=Step, 1=StepOut, 2=StepOver, 3=CpuCycleStep, 4=PpuStep, 5=PpuScanline, 6=PpuFrame", ["default"] = 0 }
					}
				}));

			tools.Add((JsonNode)MakeToolDef("resume",
				"Resume execution from a breakpoint or pause.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("pause",
				"Pause emulation.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			tools.Add((JsonNode)MakeToolDef("get_rom_info",
				"Get loaded ROM information including console type (Snes=0, Gameboy=1, Nes=2), available CPU types, and ROM path.",
				new JsonObject { ["type"] = "object", ["properties"] = new JsonObject() }));

			var result = new JsonObject { ["tools"] = tools };
			return MakeJsonRpcResult(id, result);
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
			foreach(string v in values) {
				arr.Add((JsonNode?)JsonValue.Create(v));
			}
			return arr;
		}

		// =================================================================
		// tools/call - Route to Tool Handlers
		// =================================================================

		private string HandleToolsCall(JsonNode id, JsonObject? parms)
		{
			string? toolName = parms?["name"]?.GetValue<string>();
			JsonObject? args = parms?["arguments"] as JsonObject;

			if(string.IsNullOrEmpty(toolName)) {
				return MakeToolCallError(id, "Missing tool name");
			}

			try {
				return toolName switch {
					"debugger_status" => HandleDebuggerStatus(id),
					"get_cpu_state" => HandleGetCpuState(id, args),
					"get_ppu_state" => HandleGetPpuState(id, args),
					"get_memory_range" => HandleGetMemoryRange(id, args),
					"set_memory" => HandleSetMemory(id, args),
					"get_disassembly" => HandleGetDisassembly(id, args),
					"get_trace_tail" => HandleGetTraceTail(id, args),
					"get_debug_events" => HandleGetDebugEvents(id, args),
					"set_breakpoints" => HandleSetBreakpoints(id, args),
					"step" => HandleStep(id, args),
					"resume" => HandleResume(id),
					"pause" => HandlePause(id),
					"get_rom_info" => HandleGetRomInfo(id),
					_ => MakeToolCallError(id, $"Unknown tool: {toolName}")
				};
			} catch(Exception ex) {
				return MakeToolCallError(id, $"Tool error: {ex.Message}");
			}
		}

		private static string MakeToolCallResult(JsonNode id, string text, bool isError)
		{
			var content = new JsonArray();
			content.Add((JsonNode)new JsonObject {
				["type"] = "text",
				["text"] = text
			});
			var result = new JsonObject { ["content"] = content };
			if(isError) {
				result["isError"] = true;
			}
			return MakeJsonRpcResult(id, result);
		}

		private static string MakeToolCallSuccess(JsonNode id, JsonObject data)
		{
			string json = data.ToJsonString();
			return MakeToolCallResult(id, json, false);
		}

		private static string MakeToolCallError(JsonNode id, string message)
		{
			return MakeToolCallResult(id, message, true);
		}

		// =================================================================
		// Tool Handlers
		// =================================================================

		private string HandleDebuggerStatus(JsonNode id)
		{
			bool isRunning = EmuApi.IsRunning();
			bool isPaused = EmuApi.IsPaused();
			var data = new JsonObject {
				["debugger_running"] = true,
				["emulation_running"] = isRunning,
				["execution_stopped"] = isPaused,
				["emulation_paused"] = isPaused,
				["mesen_version"] = "2.0"
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetCpuState(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			JsonObject data;

			switch(cpuType) {
				case CpuType.Snes: {
					var s = DebugApi.GetCpuState<SnesCpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["cpu_type_name"] = cpuType.ToString(),
						["PC"] = s.PC, ["K"] = s.K,
						["A"] = s.A, ["X"] = s.X, ["Y"] = s.Y,
						["SP"] = s.SP, ["D"] = s.D, ["DBR"] = s.DBR,
						["PS"] = (int)s.PS, ["emulation_mode"] = s.EmulationMode,
						["cycle_count"] = s.CycleCount
					};
					break;
				}
				case CpuType.Gameboy: {
					var s = DebugApi.GetCpuState<GbCpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["cpu_type_name"] = cpuType.ToString(),
						["PC"] = s.PC, ["SP"] = s.SP,
						["A"] = s.A, ["flags"] = s.Flags,
						["B"] = s.B, ["C"] = s.C,
						["D"] = s.D, ["E"] = s.E,
						["H"] = s.H, ["L"] = s.L,
						["halt_counter"] = s.HaltCounter, ["cycle_count"] = s.CycleCount
					};
					break;
				}
				case CpuType.Nes: {
					var s = DebugApi.GetCpuState<NesCpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["cpu_type_name"] = cpuType.ToString(),
						["PC"] = s.PC, ["SP"] = s.SP,
						["A"] = s.A, ["X"] = s.X, ["Y"] = s.Y,
						["PS"] = s.PS, ["cycle_count"] = s.CycleCount
					};
					break;
				}
				default: {
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["cpu_type_name"] = cpuType.ToString(),
						["program_counter"] = DebugApi.GetProgramCounter(cpuType, false)
					};
					break;
				}
			}

			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetPpuState(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			JsonObject data;

			switch(cpuType) {
				case CpuType.Snes: {
					var s = DebugApi.GetPpuState<SnesPpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["scanline"] = s.Scanline, ["cycle"] = s.Cycle,
						["frame_count"] = s.FrameCount
					};
					break;
				}
				case CpuType.Gameboy: {
					var s = DebugApi.GetPpuState<GbPpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["scanline"] = s.Scanline, ["cycle"] = s.Cycle,
						["frame_count"] = s.FrameCount
					};
					break;
				}
				case CpuType.Nes: {
					var s = DebugApi.GetPpuState<NesPpuState>(cpuType);
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["scanline"] = s.Scanline, ["cycle"] = s.Cycle,
						["frame_count"] = s.FrameCount
					};
					break;
				}
				default: {
					data = new JsonObject {
						["cpu_type"] = (int)cpuType,
						["error"] = "Unsupported PPU type"
					};
					break;
				}
			}

			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetMemoryRange(JsonNode id, JsonObject? args)
		{
			if(args == null) return MakeToolCallError(id, "Missing arguments");

			int memType = args["memory_type"]?.GetValue<int>() ?? 0;
			int startAddr = args["start_address"]?.GetValue<int>() ?? 0;
			int length = args["length"]?.GetValue<int>() ?? 256;
			length = Math.Min(length, 4096);

			MemoryType mt = (MemoryType)memType;
			byte[] bytes = DebugApi.GetMemoryValues(mt, (uint)startAddr, (uint)(startAddr + length - 1));

			var hexBuilder = new StringBuilder();
			for(int i = 0; i < bytes.Length; i++) {
				if(i > 0) hexBuilder.Append(' ');
				hexBuilder.Append(bytes[i].ToString("X2"));
			}

			var data = new JsonObject {
				["memory_type"] = memType,
				["start_address"] = startAddr,
				["length"] = bytes.Length,
				["hex"] = hexBuilder.ToString()
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleSetMemory(JsonNode id, JsonObject? args)
		{
			if(args == null) return MakeToolCallError(id, "Missing arguments");

			int memType = args["memory_type"]?.GetValue<int>() ?? 0;
			int address = args["address"]?.GetValue<int>() ?? 0;
			JsonArray? dataArr = args["data"] as JsonArray;

			if(dataArr == null || dataArr.Count == 0) {
				return MakeToolCallError(id, "Missing data array");
			}

			MemoryType mt = (MemoryType)memType;
			byte[] bytes = new byte[dataArr.Count];
			for(int i = 0; i < dataArr.Count; i++) {
				bytes[i] = (byte)(dataArr[i]?.GetValue<int>() ?? 0);
			}

			DebugApi.SetMemoryValues(mt, (uint)address, bytes, bytes.Length);

			var data = new JsonObject {
				["success"] = true,
				["bytes_written"] = bytes.Length
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetDisassembly(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			int address = args?["address"]?.GetValue<int>() ?? -1;
			int lineCount = args?["line_count"]?.GetValue<int>() ?? 20;
			lineCount = Math.Clamp(lineCount, 1, 100);

			uint addr;
			if(address < 0) {
				addr = DebugApi.GetProgramCounter(cpuType, false);
			} else {
				addr = (uint)address;
			}

			// Center the disassembly around the target address
			int startOffset = -(lineCount / 2);
			int startRow = DebugApi.GetDisassemblyRowAddress(cpuType, addr, startOffset);
			CodeLineData[] lines = DebugApi.GetDisassemblyOutput(cpuType, (uint)startRow, (uint)lineCount);

			var linesArray = new JsonArray();
			foreach(var line in lines) {
				if(line.Address < 0) continue;
				var entry = new JsonObject {
					["address"] = line.Address,
					["text"] = line.Text.Trim(),
					["bytes"] = line.ByteCodeStr.Trim(),
					["size"] = line.OpSize
				};
				linesArray.Add((JsonNode)entry);
			}

			var data = new JsonObject {
				["cpu_type"] = (int)cpuType,
				["current_pc"] = DebugApi.GetProgramCounter(cpuType, false),
				["lines"] = linesArray
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetTraceTail(JsonNode id, JsonObject? args)
		{
			int count = args?["count"]?.GetValue<int>() ?? 100;
			int offset = args?["offset"]?.GetValue<int>() ?? 0;
			count = Math.Min(count, 1000);

			TraceRow[] rows = DebugApi.GetExecutionTrace((uint)offset, (uint)count);

			var linesArray = new JsonArray();
			foreach(var row in rows) {
				var entry = new JsonObject {
					["pc"] = row.ProgramCounter,
					["text"] = row.GetOutput()
				};

				byte[] byteCode = row.GetByteCode();
				var hexBuilder = new StringBuilder();
				for(int i = 0; i < byteCode.Length; i++) {
					if(i > 0) hexBuilder.Append(' ');
					hexBuilder.Append(byteCode[i].ToString("X2"));
				}
				entry["bytes"] = hexBuilder.ToString();

				linesArray.Add((JsonNode)entry);
			}

			var data = new JsonObject {
				["count"] = linesArray.Count,
				["lines"] = linesArray
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetDebugEvents(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			int maxCount = args?["max_count"]?.GetValue<int>() ?? 100;
			maxCount = Math.Min(maxCount, 1000);

			DebugEventInfo[] events = DebugApi.GetDebugEvents(cpuType);

			var eventsArray = new JsonArray();
			int limit = Math.Min(events.Length, maxCount);
			for(int i = 0; i < limit; i++) {
				var evt = events[i];
				var entry = new JsonObject {
					["type"] = evt.Type.ToString(),
					["pc"] = evt.ProgramCounter,
					["scanline"] = evt.Scanline,
					["cycle"] = evt.Cycle,
					["breakpoint_id"] = evt.BreakpointId
				};
				eventsArray.Add((JsonNode)entry);
			}

			var data = new JsonObject {
				["cpu_type"] = (int)cpuType,
				["count"] = eventsArray.Count,
				["events"] = eventsArray
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleSetBreakpoints(JsonNode id, JsonObject? args)
		{
			JsonArray? bpArray = args?["breakpoints"] as JsonArray;
			if(bpArray == null) {
				return MakeToolCallError(id, "Missing breakpoints array");
			}

			var breakpoints = new InteropBreakpoint[bpArray.Count];
			for(int i = 0; i < bpArray.Count; i++) {
				var bp = bpArray[i] as JsonObject;
				if(bp == null) continue;

				breakpoints[i] = new InteropBreakpoint {
					Id = i,
					Type = (BreakpointTypeFlags)(bp["type"]?.GetValue<int>() ?? 1),
					StartAddress = bp["address"]?.GetValue<int>() ?? 0,
					EndAddress = bp["end_address"]?.GetValue<int>() ?? -1,
					CpuType = (CpuType)(bp["cpu_type"]?.GetValue<int>() ?? 0),
					MemoryType = (MemoryType)(bp["memory_type"]?.GetValue<int>() ?? 0),
					Enabled = bp["enabled"]?.GetValue<bool>() ?? true,
					MarkEvent = false,
					IgnoreDummyOperations = false,
					Condition = new byte[1000]
				};

				string? condition = bp["condition"]?.GetValue<string>();
				if(!string.IsNullOrEmpty(condition)) {
					byte[] condBytes = Encoding.UTF8.GetBytes(condition);
					Array.Copy(condBytes, breakpoints[i].Condition, Math.Min(condBytes.Length, 999));
				}

				if(breakpoints[i].EndAddress < 0) {
					breakpoints[i].EndAddress = breakpoints[i].StartAddress;
				}
			}

			DebugApi.SetBreakpoints(breakpoints, (uint)breakpoints.Length);

			var data = new JsonObject {
				["success"] = true,
				["breakpoints_set"] = breakpoints.Length
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleStep(JsonNode id, JsonObject? args)
		{
			CpuType cpuType = (CpuType)(args?["cpu_type"]?.GetValue<int>() ?? 0);
			int count = args?["count"]?.GetValue<int>() ?? 1;
			StepType stepType = (StepType)(args?["step_type"]?.GetValue<int>() ?? 0);

			DebugApi.Step(cpuType, count, stepType);

			var data = new JsonObject {
				["success"] = true,
				["cpu_type"] = (int)cpuType,
				["step_type"] = (int)stepType,
				["count"] = count
			};
			return MakeToolCallSuccess(id, data);
		}

		private string HandleResume(JsonNode id)
		{
			DebugApi.ResumeExecution();
			var data = new JsonObject { ["success"] = true };
			return MakeToolCallSuccess(id, data);
		}

		private string HandlePause(JsonNode id)
		{
			DebugApi.Step(CpuType.Snes, 1, StepType.Step);
			var data = new JsonObject { ["success"] = true };
			return MakeToolCallSuccess(id, data);
		}

		private string HandleGetRomInfo(JsonNode id)
		{
			RomInfo romInfo = EmuApi.GetRomInfo();

			var cpuTypesArray = new JsonArray();
			foreach(CpuType ct in romInfo.CpuTypes) {
				cpuTypesArray.Add((JsonNode)new JsonObject {
					["id"] = (int)ct,
					["name"] = ct.ToString()
				});
			}

			var data = new JsonObject {
				["console_type"] = (int)romInfo.ConsoleType,
				["console_type_name"] = romInfo.ConsoleType.ToString(),
				["rom_path"] = romInfo.RomPath,
				["format"] = romInfo.Format.ToString(),
				["cpu_types"] = cpuTypesArray
			};
			return MakeToolCallSuccess(id, data);
		}
	}
}
