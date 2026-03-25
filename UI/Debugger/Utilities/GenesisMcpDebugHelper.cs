using Mesen.Interop;
using System;
using System.Text;
using System.Text.Json.Nodes;

namespace Mesen.Debugger.Utilities
{
	internal static class GenesisMcpDebugHelper
	{
		private const CpuType GenesisCpuType = CpuType.GenesisMain;
		private const int VdpRegisterCount = 24;
		private const int VramSize = 0x10000;
		private const int CramSize = 0x80;
		private const int VsramSize = 0x50;

		public static void EnsureGenesisLoaded()
		{
			RomInfo romInfo = EmuApi.GetRomInfo();
			if(romInfo.Format == RomFormat.Unknown) {
				throw new Exception("No ROM is currently loaded.");
			}

			if(romInfo.ConsoleType != ConsoleType.Genesis) {
				throw new Exception("The loaded ROM is not a Genesis title.");
			}
		}

		public static JsonObject BuildCpuState()
		{
			EnsureGenesisLoaded();

			GenesisCpuState cpu = DebugApi.GetCpuState<GenesisCpuState>(GenesisCpuType);
			JsonArray dRegs = new JsonArray();
			JsonArray aRegs = new JsonArray();

			foreach(UInt32 value in cpu.D ?? Array.Empty<UInt32>()) {
				dRegs.Add((JsonNode?)JsonValue.Create(value));
			}

			foreach(UInt32 value in cpu.A ?? Array.Empty<UInt32>()) {
				aRegs.Add((JsonNode?)JsonValue.Create(value));
			}

			return new JsonObject {
				["cpu_type"] = (int)GenesisCpuType,
				["cpu_type_name"] = GenesisCpuType.ToString(),
				["PC"] = cpu.PC,
				["SP"] = cpu.SP,
				["SR"] = cpu.SR,
				["USP"] = cpu.USP,
				["stopped"] = cpu.Stopped,
				["cycle_count"] = cpu.CycleCount,
				["D"] = dRegs,
				["A"] = aRegs
			};
		}

		public static JsonObject BuildPpuState()
		{
			EnsureGenesisLoaded();

			GenesisVdpState vdp = DebugApi.GetPpuState<GenesisVdpState>(GenesisCpuType);
			return new JsonObject {
				["cpu_type"] = (int)GenesisCpuType,
				["cpu_type_name"] = GenesisCpuType.ToString(),
				["frame_count"] = vdp.FrameCount,
				["scanline"] = vdp.VClock,
				["cycle"] = vdp.HClock,
				["hclock"] = vdp.HClock,
				["vclock"] = vdp.VClock,
				["width"] = vdp.Width,
				["height"] = vdp.Height,
				["pal"] = vdp.PAL
			};
		}

		public static JsonObject BuildVdpRegisters()
		{
			EnsureGenesisLoaded();

			byte[] registers = DebugApi.GetGenesisVdpRegisters();
			var items = new JsonArray();
			for(int i = 0; i < registers.Length; i++) {
				items.Add((JsonNode)new JsonObject {
					["index"] = i,
					["value"] = registers[i]
				});
			}

			return new JsonObject {
				["register_count"] = registers.Length,
				["registers"] = items
			};
		}

		public static JsonObject BuildBackendState()
		{
			EnsureGenesisLoaded();

			GenesisBackendState state = DebugApi.GetGenesisBackendState();
			return new JsonObject {
				["master_clock"] = state.MasterClock,
				["frame_width"] = state.FrameWidth,
				["frame_height"] = state.FrameHeight,
				["active_width"] = state.ActiveWidth,
				["active_height"] = state.ActiveHeight,
				["vdp_mclk_pos"] = state.VdpMclkPos,
				["scanline"] = state.Scanline,
				["hclock"] = state.HClock,
				["vdp_status"] = state.VdpStatus,
				["vdp_status_hex"] = state.VdpStatus.ToString("X4"),
				["dma_type"] = state.DmaType,
				["dma_type_name"] = GetDmaTypeName(state.DmaType),
				["dma_source"] = state.DmaSource,
				["dma_length"] = state.DmaLength,
				["dma_fill_value"] = state.DmaFillValue,
				["cpu_pending_irq"] = state.CpuPendingIrq,
				["vint_pending"] = state.VintPending != 0,
				["hint_pending"] = state.HintPending != 0,
				["dma_fill_pending"] = state.DmaFillPending != 0,
				["display_enabled"] = state.DisplayEnabled != 0,
				["z80_bus_request"] = state.Z80BusRequest != 0,
				["z80_reset"] = state.Z80Reset != 0,
				["z80_bus_ack"] = state.Z80BusAck != 0,
				["pal"] = state.PAL != 0
			};
		}

		public static JsonObject BuildVramRange(JsonObject? args)
		{
			return BuildMemoryDump(
				MemoryType.GenesisVideoRam,
				args?["start_address"]?.GetValue<int>() ?? 0,
				args?["length"]?.GetValue<int>() ?? 0x100,
				VramSize,
				"vram"
			);
		}

		public static JsonObject BuildCram(JsonObject? args)
		{
			return BuildMemoryDump(
				MemoryType.GenesisColorRam,
				args?["start_address"]?.GetValue<int>() ?? 0,
				args?["length"]?.GetValue<int>() ?? CramSize,
				CramSize,
				"cram"
			);
		}

		public static JsonObject BuildVsram(JsonObject? args)
		{
			return BuildMemoryDump(
				MemoryType.GenesisVScrollRam,
				args?["start_address"]?.GetValue<int>() ?? 0,
				args?["length"]?.GetValue<int>() ?? VsramSize,
				VsramSize,
				"vsram"
			);
		}

		public static JsonObject CaptureSnapshot(JsonObject? args)
		{
			EnsureGenesisLoaded();

			JsonObject triggerInfo = RunOptionalTrigger(args);
			int traceCount = Math.Clamp(args?["trace_count"]?.GetValue<int>() ?? 32, 0, 256);

			var snapshot = new JsonObject {
				["rom"] = BuildRomInfo(),
				["cpu"] = BuildCpuState(),
				["vdp"] = BuildPpuState(),
				["vdp_registers"] = BuildVdpRegisters()["registers"]?.DeepClone(),
				["backend"] = BuildBackendState(),
				["vram"] = BuildMemoryDump(
					MemoryType.GenesisVideoRam,
					args?["vram_start"]?.GetValue<int>() ?? 0,
					args?["vram_length"]?.GetValue<int>() ?? 0x100,
					VramSize,
					"vram"
				),
				["cram"] = BuildMemoryDump(
					MemoryType.GenesisColorRam,
					args?["cram_start"]?.GetValue<int>() ?? 0,
					args?["cram_length"]?.GetValue<int>() ?? CramSize,
					CramSize,
					"cram"
				),
				["vsram"] = BuildMemoryDump(
					MemoryType.GenesisVScrollRam,
					args?["vsram_start"]?.GetValue<int>() ?? 0,
					args?["vsram_length"]?.GetValue<int>() ?? VsramSize,
					VsramSize,
					"vsram"
				),
				["trace"] = BuildTraceTail(traceCount)
			};

			if(triggerInfo.Count > 0) {
				snapshot["trigger"] = triggerInfo;
			}

			return snapshot;
		}

		private static JsonObject BuildRomInfo()
		{
			RomInfo romInfo = EmuApi.GetRomInfo();
			return new JsonObject {
				["rom_path"] = romInfo.RomPath,
				["console_type"] = romInfo.ConsoleType.ToString(),
				["format"] = romInfo.Format.ToString()
			};
		}

		private static JsonObject BuildTraceTail(int count)
		{
			var linesArray = new JsonArray();
			if(count <= 0) {
				return new JsonObject { ["count"] = 0, ["lines"] = linesArray };
			}

			TraceRow[] rows = DebugApi.GetExecutionTrace(0, (uint)count);
			foreach(TraceRow row in rows) {
				var hex = new StringBuilder();
				byte[] byteCode = row.GetByteCode();
				for(int i = 0; i < byteCode.Length; i++) {
					if(i > 0) {
						hex.Append(' ');
					}
					hex.Append(byteCode[i].ToString("X2"));
				}

				linesArray.Add((JsonNode)new JsonObject {
					["pc"] = row.ProgramCounter,
					["text"] = row.GetOutput(),
					["bytes"] = hex.ToString()
				});
			}

			return new JsonObject {
				["count"] = linesArray.Count,
				["lines"] = linesArray
			};
		}

		private static JsonObject BuildMemoryDump(MemoryType type, int start, int length, int maxSize, string label)
		{
			EnsureGenesisLoaded();

			if(start < 0 || start >= maxSize) {
				throw new Exception($"Invalid {label} start address.");
			}

			int clampedLength = Math.Clamp(length, 0, maxSize - start);
			byte[] bytes = clampedLength > 0
				? DebugApi.GetMemoryValues(type, (uint)start, (uint)(start + clampedLength - 1))
				: Array.Empty<byte>();

			var values = new JsonArray();
			var hex = new StringBuilder();
			for(int i = 0; i < bytes.Length; i++) {
				values.Add((JsonNode?)JsonValue.Create((int)bytes[i]));
				if(i > 0) {
					hex.Append(' ');
				}
				hex.Append(bytes[i].ToString("X2"));
			}

			return new JsonObject {
				["label"] = label,
				["memory_type"] = (int)type,
				["memory_type_name"] = type.ToString(),
				["start_address"] = start,
				["length"] = bytes.Length,
				["bytes"] = values,
				["hex"] = hex.ToString()
			};
		}

		private static JsonObject RunOptionalTrigger(JsonObject? args)
		{
			int? triggerPc = args?["trigger_pc"]?.GetValue<int>();
			int? triggerScanline = args?["trigger_scanline"]?.GetValue<int>();
			int? triggerVdpRegister = args?["trigger_vdp_register"]?.GetValue<int>();
			int enabledTriggerCount =
				(triggerPc.HasValue ? 1 : 0) +
				(triggerScanline.HasValue ? 1 : 0) +
				(triggerVdpRegister.HasValue ? 1 : 0);

			if(enabledTriggerCount > 1) {
				throw new Exception("Only one Genesis snapshot trigger can be used at a time.");
			}

			int maxSteps = Math.Clamp(args?["max_steps"]?.GetValue<int>() ?? 200000, 1, 1000000);
			var trigger = new JsonObject();

			if(triggerPc.HasValue) {
				RunToPc((uint)triggerPc.Value, maxSteps);
				trigger["type"] = "pc";
				trigger["value"] = triggerPc.Value;
			} else if(triggerScanline.HasValue) {
				DebugApi.Step(GenesisCpuType, triggerScanline.Value, StepType.SpecificScanline);
				trigger["type"] = "scanline";
				trigger["value"] = triggerScanline.Value;
			} else if(triggerVdpRegister.HasValue) {
				int registerIndex = triggerVdpRegister.Value;
				if(registerIndex < 0 || registerIndex >= VdpRegisterCount) {
					throw new Exception("Genesis VDP register index must be between 0 and 23.");
				}

				int? targetValue = args?["trigger_vdp_value"]?.GetValue<int>();
				RunToVdpRegisterWrite(registerIndex, targetValue, maxSteps);
				trigger["type"] = "vdp_register_write";
				trigger["register"] = registerIndex;
				if(targetValue.HasValue) {
					trigger["value"] = targetValue.Value;
				}
			}

			return trigger;
		}

		private static void RunToPc(uint targetPc, int maxSteps)
		{
			if((DebugApi.GetProgramCounter(GenesisCpuType, false) & 0x00FFFFFFu) == (targetPc & 0x00FFFFFFu)) {
				return;
			}

			for(int i = 0; i < maxSteps; i++) {
				DebugApi.Step(GenesisCpuType, 1, StepType.Step);
				if((DebugApi.GetProgramCounter(GenesisCpuType, false) & 0x00FFFFFFu) == (targetPc & 0x00FFFFFFu)) {
					return;
				}
			}

			throw new Exception($"Genesis PC ${targetPc:X6} was not reached within {maxSteps} steps.");
		}

		private static void RunToVdpRegisterWrite(int registerIndex, int? targetValue, int maxSteps)
		{
			byte[] previous = DebugApi.GetGenesisVdpRegisters();

			for(int i = 0; i < maxSteps; i++) {
				DebugApi.Step(GenesisCpuType, 1, StepType.Step);
				byte[] current = DebugApi.GetGenesisVdpRegisters();
				if(current[registerIndex] != previous[registerIndex] &&
					(!targetValue.HasValue || current[registerIndex] == (byte)targetValue.Value)) {
					return;
				}

				previous = current;
			}

			throw new Exception($"Genesis VDP register R{registerIndex} was not written within {maxSteps} steps.");
		}

		private static string GetDmaTypeName(byte dmaType)
		{
			return dmaType switch {
				1 => "Bus68k",
				2 => "VramFill",
				3 => "VramCopy",
				_ => "None"
			};
		}
	}
}
