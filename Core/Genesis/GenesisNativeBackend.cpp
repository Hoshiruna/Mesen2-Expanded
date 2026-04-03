#include "pch.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string.h>
#include <vector>

#include "Genesis/GenesisCpu68k.h"
#include "Genesis/GenesisCpuZ80.h"
#include "Genesis/GenesisI2cEeprom.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Genesis/GenesisTypes.h"
#include "Genesis/IGenesisPlatformCallbacks.h"
#include "Genesis/GenesisVdp.h"
#include "Shared/CpuType.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Shared/MemoryOperationType.h"
#include "Shared/MemoryType.h"
#include "Shared/SettingTypes.h"
#include "Utilities/StringUtilities.h"
#include "Utilities/HexUtilities.h"

#ifdef _DEBUG
#ifndef MD_NATIVE_TRACE
#define MD_NATIVE_TRACE 0
#endif
#ifndef MD_NATIVE_TRACE_BUS
#define MD_NATIVE_TRACE_BUS MD_NATIVE_TRACE
#endif
#ifndef MD_NATIVE_TRACE_VDP
#define MD_NATIVE_TRACE_VDP MD_NATIVE_TRACE
#endif
#ifndef MD_NATIVE_TRACE_IRQ
#define MD_NATIVE_TRACE_IRQ MD_NATIVE_TRACE
#endif
#ifndef MD_NATIVE_TRACE_PAD
#define MD_NATIVE_TRACE_PAD MD_NATIVE_TRACE
#endif
#ifndef MD_NATIVE_TRACE_SCHED
#define MD_NATIVE_TRACE_SCHED 0
#endif
#define MD_TRACE_CAT(ENABLED, CAT, MSG) \
	do { if((ENABLED) != 0) LogDebug(string("[MD Native][") + CAT + "] " + MSG); } while(0)
#define MD_TRACE_BUS(MSG) MD_TRACE_CAT(MD_NATIVE_TRACE_BUS, "BUS", MSG)
#define MD_TRACE_VDP(MSG) MD_TRACE_CAT(MD_NATIVE_TRACE_VDP, "VDP", MSG)
#define MD_TRACE_IRQ(MSG) MD_TRACE_CAT(MD_NATIVE_TRACE_IRQ, "IRQ", MSG)
#define MD_TRACE_PAD(MSG) MD_TRACE_CAT(MD_NATIVE_TRACE_PAD, "PAD", MSG)
#define MD_TRACE_SCHED(MSG) MD_TRACE_CAT(MD_NATIVE_TRACE_SCHED, "SCHED", MSG)
#else
#define MD_TRACE_BUS(MSG) do { } while(0)
#define MD_TRACE_VDP(MSG) do { } while(0)
#define MD_TRACE_IRQ(MSG) do { } while(0)
#define MD_TRACE_PAD(MSG) do { } while(0)
#define MD_TRACE_SCHED(MSG) do { } while(0)
#define MD_NATIVE_TRACE_SCHED 0
#endif

#ifndef MD_NATIVE_DISABLE_LINE_SLICES
// Forced scanline slices amplify 68K instruction-boundary overruns and skew
// throughput-sensitive code such as the GenTest benchmark. Leave them off by
// default and only re-enable for A/B timing experiments.
#define MD_NATIVE_DISABLE_LINE_SLICES 0
#endif

// ===========================================================================
// Serialization helpers (anonymous namespace)
// ===========================================================================
namespace
{
	template<typename T>
	void AppendValue(vector<uint8_t>& out, const T& value)
	{
		size_t old = out.size();
		out.resize(old + sizeof(T));
		memcpy(out.data() + old, &value, sizeof(T));
	}

	template<typename T>
	bool ReadValue(const vector<uint8_t>& data, size_t& offset, T& value)
	{
		if(offset + sizeof(T) > data.size()) return false;
		memcpy(&value, data.data() + offset, sizeof(T));
		offset += sizeof(T);
		return true;
	}

	void AppendBlob(vector<uint8_t>& out, const vector<uint8_t>& blob)
	{
		uint32_t sz = static_cast<uint32_t>(blob.size());
		AppendValue(out, sz);
		if(sz > 0) out.insert(out.end(), blob.begin(), blob.end());
	}

	bool ReadBlob(const vector<uint8_t>& data, size_t& offset, vector<uint8_t>& out)
	{
		uint32_t sz = 0;
		if(!ReadValue(data, offset, sz)) return false;
		if(offset + sz > data.size()) return false;
		out.resize(sz);
		if(sz > 0) memcpy(out.data(), data.data() + offset, sz);
		offset += sz;
		return true;
	}

	// Big-endian 32-bit read from a byte buffer.
	uint32_t ReadBE32(const vector<uint8_t>& data, size_t offset)
	{
		if(offset + 4 > data.size()) return 0;
		return (static_cast<uint32_t>(data[offset    ]) << 24) |
		       (static_cast<uint32_t>(data[offset + 1]) << 16) |
		       (static_cast<uint32_t>(data[offset + 2]) <<  8) |
		        static_cast<uint32_t>(data[offset + 3]);
	}

	const char* kBranchCond[16] = {
		"t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
		"vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"
	};

	template<typename ReadWordFunc>
	string FormatEa(uint8_t mode, uint8_t reg, uint8_t size, uint32_t extAddr, ReadWordFunc readWord, uint32_t& consumedBytes)
	{
		consumedBytes = 0;
		switch(mode) {
			case 0: return "D" + std::to_string(reg);
			case 1: return "A" + std::to_string(reg);
			case 2: return "(A" + std::to_string(reg) + ")";
			case 3: return "(A" + std::to_string(reg) + ")+";
			case 4: return "-(A" + std::to_string(reg) + ")";
			case 5: {
				int16_t d16 = (int16_t)readWord(extAddr);
				consumedBytes = 2;
				return "$" + HexUtilities::ToHex((uint16_t)d16) + "(A" + std::to_string(reg) + ")";
			}
			case 6: {
				uint16_t ext = readWord(extAddr);
				consumedBytes = 2;
				int8_t d8 = (int8_t)(ext & 0xFF);
				uint8_t xn = (ext >> 12) & 7;
				bool isAn = (ext & 0x8000) != 0;
				bool longSize = (ext & 0x0800) != 0;
				return "$" + HexUtilities::ToHex((uint8_t)d8) + "(A" + std::to_string(reg) + "," + (isAn ? "A" : "D") +
				       std::to_string(xn) + "." + (longSize ? "l" : "w") + ")";
			}
			case 7:
				switch(reg) {
					case 0: {
						uint16_t absw = readWord(extAddr);
						consumedBytes = 2;
						return "$" + HexUtilities::ToHex(absw) + ".w";
				}
					case 1: {
						uint32_t absl = ((uint32_t)readWord(extAddr) << 16) | readWord(extAddr + 2);
						consumedBytes = 4;
						return "$" + HexUtilities::ToHex32(absl) + ".l";
				}
					case 2: {
						int16_t d16 = (int16_t)readWord(extAddr);
						consumedBytes = 2;
						uint32_t target = (extAddr + 2 + d16) & 0x00FFFFFFu;
						return "$" + HexUtilities::ToHex24((int32_t)target) + "(pc)";
				}
					case 3: {
						uint16_t ext = readWord(extAddr);
						consumedBytes = 2;
						int8_t d8 = (int8_t)(ext & 0xFF);
						uint8_t xn = (ext >> 12) & 7;
						bool isAn = (ext & 0x8000) != 0;
						bool longSize = (ext & 0x0800) != 0;
						return "$" + HexUtilities::ToHex((uint8_t)d8) + "(pc," + (isAn ? "A" : "D") +
						       std::to_string(xn) + "." + (longSize ? "l" : "w") + ")";
				}
					case 4: {
						if(size == 4) {
							uint32_t imm = ((uint32_t)readWord(extAddr) << 16) | readWord(extAddr + 2);
							consumedBytes = 4;
							return "#$" + HexUtilities::ToHex32(imm);
					} else {
							uint16_t immw = readWord(extAddr);
							consumedBytes = 2;
							if(size == 1) {
								return "#$" + HexUtilities::ToHex((uint8_t)immw);
						}
							return "#$" + HexUtilities::ToHex(immw);
					}
				}
				}
				break;
		}
		return "ea";
	}

	template<typename ReadWordFunc>
	string DisassembleM68k(uint32_t address, uint8_t opSize, ReadWordFunc readWord)
	{
		uint16_t op = readWord(address);

		if(op == 0x4E71) return "nop";
		if(op == 0x4E75) return "rts";
		if(op == 0x4E73) return "rte";
		if(op == 0x4E77) return "rtr";
		if(op == 0x4E70) return "reset";
		if(op == 0x4E76) return "trapv";
		if(op == 0x4E72) return "stop";
		if((op & 0xFFF0u) == 0x4E40u) {
			return "trap #" + std::to_string(op & 0x0Fu);
		}

		if((op & 0xFF00u) == 0x6000u || (op & 0xFF00u) == 0x6100u || ((op & 0xF000u) == 0x6000u)) {
			uint8_t cc = (uint8_t)((op >> 8) & 0x0Fu);
			string mnem = (cc == 0) ? "bra" : (cc == 1 ? "bsr" : ("b" + string(kBranchCond[cc])));
			int32_t disp = 0;
			uint32_t base = (address + 2) & 0x00FFFFFFu;
			if((op & 0x00FFu) == 0) {
				disp = (int16_t)readWord(address + 2);
				base = (base + 2) & 0x00FFFFFFu;
			} else {
				disp = (int8_t)(op & 0x00FFu);
			}
			uint32_t target = (base + disp) & 0x00FFFFFFu;
			return mnem + " $" + HexUtilities::ToHex24((int32_t)target);
		}

		if((op & 0xF100u) == 0x7000u) {
			uint8_t reg = (uint8_t)((op >> 9) & 7);
			int8_t imm = (int8_t)(op & 0xFF);
			return "moveq #" + std::to_string((int)imm) + ",D" + std::to_string(reg);
		}

		if((op & 0xFFC0u) == 0x4E80u || (op & 0xFFC0u) == 0x4EC0u) {
			uint8_t mode = (uint8_t)((op >> 3) & 7);
			uint8_t reg = (uint8_t)(op & 7);
			uint32_t used = 0;
			string ea = FormatEa(mode, reg, 4, address + 2, readWord, used);
			return ((op & 0xFFC0u) == 0x4E80u ? "jsr " : "jmp ") + ea;
		}

		uint8_t top = (uint8_t)(op >> 12);
		if(top >= 1 && top <= 3) {
			uint8_t size = top == 1 ? 1 : (top == 2 ? 4 : 2);
			uint8_t srcMode = (uint8_t)((op >> 3) & 7);
			uint8_t srcReg  = (uint8_t)(op & 7);
			uint8_t dstMode = (uint8_t)((op >> 6) & 7);
			uint8_t dstReg  = (uint8_t)((op >> 9) & 7);
			uint32_t used1 = 0;
			uint32_t used2 = 0;
			string src = FormatEa(srcMode, srcReg, size, address + 2, readWord, used1);
			string dst = FormatEa(dstMode, dstReg, size, address + 2 + used1, readWord, used2);
			char sz = size == 1 ? 'b' : (size == 2 ? 'w' : 'l');
			return string("move.") + sz + " " + src + "," + dst;
		}

		// Fallback: raw words when mnemonic decode is not implemented yet.
		string out = "dc.w $" + HexUtilities::ToHex(op);
		for(uint8_t i = 2; i + 1 < opSize && i <= 8; i += 2) {
			out += ",$" + HexUtilities::ToHex(readWord(address + i));
		}
		return out;
	}

	static constexpr const char* kCpuRamTraceDirectory = "reference";
	static constexpr const char* kCpuRamTracePath = "reference/cpu_ram_trace.log";
	static FILE* sCpuRamTraceFile = nullptr;
	static uint32_t sCpuRamTraceLines = 0;
	static bool sCpuRamTraceConfigLoaded = false;
	static uint32_t kCpuRamTraceFrameStart = 0u;
	static uint32_t kCpuRamTraceFrameEnd = 50u;
	static uint32_t kCpuRamTraceAddrStart = 0xFFCC00u;
	static uint32_t kCpuRamTraceAddrEnd = 0xFFCFFFu;
	static uint32_t kCpuRamTraceMaxLines = 300000u;

	static bool TryParseEnvU32AutoBase(const char* name, uint32_t minVal, uint32_t maxVal, uint32_t& outVal)
	{
		const char* raw = std::getenv(name);
		if(!raw || !*raw) {
			return false;
		}

		char* end = nullptr;
		unsigned long v = std::strtoul(raw, &end, 0);
		bool ok = (end != raw && *end == '\0' && v >= minVal && v <= maxVal);
		if(!ok) return false;
		outVal = (uint32_t)v;
		return true;
	}

	static void LoadCpuRamTraceConfigFromEnv()
	{
		if(sCpuRamTraceConfigLoaded) return;
		sCpuRamTraceConfigLoaded = true;

		uint32_t v = 0;
		if(TryParseEnvU32AutoBase("MESEN_WRAM_FRAME_START", 0u, 0xFFFFFFFFu, v)) kCpuRamTraceFrameStart = v;
		if(TryParseEnvU32AutoBase("MESEN_WRAM_FRAME_END",   0u, 0xFFFFFFFFu, v)) kCpuRamTraceFrameEnd = v;
		if(TryParseEnvU32AutoBase("MESEN_WRAM_ADDR_START",  0u, 0xFFFFFFu,   v)) kCpuRamTraceAddrStart = v;
		if(TryParseEnvU32AutoBase("MESEN_WRAM_ADDR_END",    0u, 0xFFFFFFu,   v)) kCpuRamTraceAddrEnd = v;
		if(TryParseEnvU32AutoBase("MESEN_WRAM_MAX_LINES",   1u, 0xFFFFFFFFu, v)) kCpuRamTraceMaxLines = v;

		if(kCpuRamTraceFrameStart > kCpuRamTraceFrameEnd) {
			std::swap(kCpuRamTraceFrameStart, kCpuRamTraceFrameEnd);
		}
		if(kCpuRamTraceAddrStart > kCpuRamTraceAddrEnd) {
			std::swap(kCpuRamTraceAddrStart, kCpuRamTraceAddrEnd);
		}
	}

	static void EnsureCpuRamTraceOpen()
	{
		if(sCpuRamTraceFile) return;
		LoadCpuRamTraceConfigFromEnv();
		std::error_code fsErr;
		std::filesystem::create_directories(kCpuRamTraceDirectory, fsErr);
		sCpuRamTraceFile = fopen(kCpuRamTracePath, "w");
		if(sCpuRamTraceFile) {
			fprintf(sCpuRamTraceFile, "# CPU work-RAM write trace\n");
			fprintf(sCpuRamTraceFile, "# frameRange=%u-%u addrRange=%06X-%06X maxLines=%u\n",
				kCpuRamTraceFrameStart, kCpuRamTraceFrameEnd,
				(unsigned)kCpuRamTraceAddrStart, (unsigned)kCpuRamTraceAddrEnd, kCpuRamTraceMaxLines);
			fflush(sCpuRamTraceFile);
		}
	}

	static bool CpuRamTraceShouldLog(uint32_t frame, uint32_t addr)
	{
		EnsureCpuRamTraceOpen();
		if(!sCpuRamTraceFile) return false;
		if(sCpuRamTraceLines >= kCpuRamTraceMaxLines) return false;
		if(frame < kCpuRamTraceFrameStart || frame > kCpuRamTraceFrameEnd) return false;
		if(addr < kCpuRamTraceAddrStart || addr > kCpuRamTraceAddrEnd) return false;
		return true;
	}

	static void CpuRamTraceLog(uint32_t frame, uint16_t line, uint32_t addr, uint8_t data, uint32_t pc, uint64_t mclk)
	{
		if(!sCpuRamTraceFile) return;
		fprintf(sCpuRamTraceFile, "F%04u L%03u WRAM addr=%06X data=%02X pc=%06X mclk=%llu\n",
			(unsigned)frame, (unsigned)line, (unsigned)addr, (unsigned)data, (unsigned)pc, (unsigned long long)mclk);
		sCpuRamTraceLines++;
		if((sCpuRamTraceLines & 0x3FFu) == 0u) {
			fflush(sCpuRamTraceFile);
		}
	}
}

// ===========================================================================
// Construction
// ===========================================================================

GenesisNativeBackend::GenesisNativeBackend(Emulator* emu, IGenesisPlatformCallbacks* callbacks)
	: _callbacks(callbacks), _emu(emu)
{
	static bool cpuTableInitialized = false;
	if(!cpuTableInitialized) {
		GenesisCpu68k::StaticInit();
		cpuTableInitialized = true;
	}

	_cpu.Init(_emu, this);
	_vdp.Init(_emu, this, false);
	_apu.Init(_emu, this, false);
	_z80.Init(this, &_apu);
}

void GenesisNativeBackend::EnableCpuTestBus()
{
	_cpuTestBusEnabled = true;
	if(_cpuTestBus.size() != 0x1000000u) {
		_cpuTestBus.assign(0x1000000u, 0);
	}
}

void GenesisNativeBackend::DisableCpuTestBus()
{
	_cpuTestBusEnabled = false;
}

void GenesisNativeBackend::ClearCpuTestBus(uint8_t fillValue)
{
	EnableCpuTestBus();
	std::fill(_cpuTestBus.begin(), _cpuTestBus.end(), fillValue);
}

void GenesisNativeBackend::SetCpuTestBusByte(uint32_t address, uint8_t value)
{
	EnableCpuTestBus();
	_cpuTestBus[address & 0x00FFFFFFu] = value;
}

uint8_t GenesisNativeBackend::GetCpuTestBusByte(uint32_t address) const
{
	if(!_cpuTestBusEnabled || _cpuTestBus.size() != 0x1000000u) {
		return 0;
	}
	return _cpuTestBus[address & 0x00FFFFFFu];
}

void GenesisNativeBackend::SetCpuStateForTest(const GenesisCpuState& state, uint8_t pendingIrq)
{
	bool supervisor = (state.SR & GenesisCpu68k::SR_S) != 0;
	uint32_t activeSp = state.SP;
	uint32_t inactiveSp = state.USP;
	if(!supervisor) {
		activeSp = state.USP;
		inactiveSp = state.SP;
	}

	_cpu.Reset(activeSp, state.PC & 0x00FFFFFFu);
	GenesisCpuState& cpuState = _cpu.GetState();
	cpuState.CycleCount = state.CycleCount;
	cpuState.PC = state.PC & 0x00FFFFFFu;
	cpuState.SR = state.SR & 0xA71Fu;
	cpuState.Stopped = state.Stopped;
	memcpy(cpuState.D, state.D, sizeof(cpuState.D));
	memcpy(cpuState.A, state.A, sizeof(cpuState.A));
	cpuState.A[7] = activeSp;
	cpuState.SP = activeSp;
	_cpu.SetUSP(inactiveSp);
	if(pendingIrq != 0) {
		_cpu.SetPendingIrq(pendingIrq & 0x07u);
	} else {
		_cpu.ClearPendingIrq();
	}

	_cpuState = cpuState;
	_cpuUsp = _cpu.GetUSP();
	_cpuPendingIrq = _cpu.GetPendingIrq();
}

void GenesisNativeBackend::GetCpuStateForTest(GenesisCpuState& state) const
{
	state = _cpu.GetState();
	state.USP = _cpu.GetUSP();
	state.SP = _cpu.GetSSP();
}

int32_t GenesisNativeBackend::RunCpuInstructionForTest()
{
	int32_t cycles = _cpu.Run(1);
	_cpuState = _cpu.GetState();
	_cpuUsp = _cpu.GetUSP();
	_cpuPendingIrq = _cpu.GetPendingIrq();
	return cycles;
}

uint8_t GenesisNativeBackend::GetVdpRegister(uint8_t index) const
{
	return _vdp.GetRegister(index);
}

uint16_t GenesisNativeBackend::GetHVCounter() const
{
	return _vdp.GetHVCounter();
}

// ===========================================================================
// IGenesisCoreBackend — identity
// ===========================================================================

GenesisCoreType GenesisNativeBackend::GetCoreType() const
{
	return GenesisCoreType::Native;
}

// ===========================================================================
// Cart header parsing
// ===========================================================================

// MD ROM header layout (all addresses are byte offsets into the ROM image):
//   $0000-$0003   Initial stack pointer  (big-endian)
//   $0004-$0007   Initial program counter (big-endian)
//   $01A0-$01A3   ROM start address
//   $01A4-$01A7   ROM end address
//   $01B0         'R' — extra-memory identifier byte 0
//   $01B1         'A' — extra-memory identifier byte 1
//   $01B2         RAM type byte
//                   bit 7: 1 = extra memory present
//                   bit 6: 0 = SRAM, 1 = EEPROM
//                   bit 5: width (0 = 16-bit word, 1 = 8-bit byte)
//                   bit 4: byte lane (if 8-bit: 0 = lower, 1 = upper)
//   $01B3         $20 (padding)
//   $01B4-$01B7   Extra-memory start address (big-endian)
//   $01B8-$01BB   Extra-memory end address   (big-endian)

void GenesisNativeBackend::ParseCartHeader()
{
	_sramStart   = 0;
	_sramEnd     = 0;
	_sramMode    = SramMode::None;
	_hasEeprom   = false;
	_mapperType  = MapperType::Linear;
	// This frontend exposes a 6-button Genesis pad layout by default.
	// Keep six-button mode enabled unless a future per-port option overrides it.
	_preferSixButton = true;

	// --- Extra-memory field ($01B0-$01BB) ---
	if(_rom.size() >= 0x01BC &&
	   _rom[0x01B0] == 'R' && _rom[0x01B1] == 'A' && (_rom[0x01B2] & 0x80u))
	{
		uint8_t  typeFlags = _rom[0x01B2];
		uint32_t memStart  = ReadBE32(_rom, 0x01B4);
		uint32_t memEnd    = ReadBE32(_rom, 0x01B8);

		if(typeFlags & 0x40u) {
			// EEPROM — use standard pin mapping; chip type resolved from
			// save data size or defaulted to X24C01.
			_hasEeprom     = true;
			_eepromBusStart = memStart & 0x00FFFFFFu;
			_eepromBusEnd   = memEnd   & 0x00FFFFFFu;
			_rsda = 0;   // bit 0 of high byte (address byte)
			_wsda = 0;
			_wscl = 1;
		} else {
			// Battery-backed SRAM
			_sramStart = memStart & 0x00FFFFFFu;
			_sramEnd   = memEnd   & 0x00FFFFFFu;

			if(typeFlags & 0x20u) {
				// 8-bit byte SRAM
				_sramMode = (typeFlags & 0x10u) ? SramMode::UpperByte : SramMode::LowerByte;
			} else {
				_sramMode = SramMode::Word;
			}

			// Guard against corrupted header values.
			if(_sramEnd < _sramStart || (_sramEnd - _sramStart) > 0x10000u) {
				_sramEnd = _sramStart + 0xFFFFu;
			}
		}
	}

	// --- SSF2 / large-ROM banking ---
	// Engage the banked mapper for ROMs larger than 4 MB.
	if(_rom.size() > 0x400000u) {
		_mapperType = MapperType::SSF2;
	}

	// --- Input capabilities hint ---
	// MD header peripheral list ($190-$19F) commonly contains '6' for 6-button support.
	if(_rom.size() >= 0x1A0) {
		for(size_t i = 0x190; i < 0x1A0; i++) {
			if(_rom[i] == '6') {
				_preferSixButton = true;
				break;
			}
		}
	}

	InitBankTable();
}

void GenesisNativeBackend::InitBankTable()
{
	// Default: identity mapping — window N → physical page N.
	for(int i = 0; i < 8; i++) {
		uint32_t start = static_cast<uint32_t>(i) << 19;   // i * 512 KB
		_romBank[i] = (start < _rom.size()) ? static_cast<uint8_t>(i) : 0xFFu;
	}
}

GenesisNativeBackend::BusRegion GenesisNativeBackend::DecodeBusRegion(uint32_t address) const
{
	address &= 0x00FFFFFFu;

	if(address < 0x400000u) return BusRegion::Cart;
	if((address & 0xFF0000u) == 0xA00000u) return BusRegion::Z80Space;
	if((address & 0xFFFFE0u) == 0xA10000u) return BusRegion::IoRegs;
	if(address == 0xA11000u) return BusRegion::MemoryMode;
	if((address & 0xFFFFFEu) == 0xA11100u) return BusRegion::Z80BusReq;
	if((address & 0xFFFFFEu) == 0xA11200u) return BusRegion::Z80Reset;
	if((address & 0xFFFF00u) == 0xA13000u) return BusRegion::MapperRegs;
	if((address & 0xFFFFFCu) == 0xA14000u) return BusRegion::Tmss;   // $A14000–$A14003
	if(address == 0xA14101u) return BusRegion::TmssCart;
	if((address & 0xFFFFE0u) == 0xC00000u) return BusRegion::VdpPorts;
	if(address >= 0xE00000u) return BusRegion::WorkRam;
	return BusRegion::OpenBus;
}

bool GenesisNativeBackend::IsZ80BusGranted() const
{
	// 68K can own Z80 bus either by BUSREQ/BUSACK handshake or while Z80 reset is held.
	return _z80BusAck || !_z80Reset;
}

void GenesisNativeBackend::AdvanceZ80BusArbitration(uint32_t masterClocks)
{
	if(masterClocks == 0u) {
		return;
	}

	// While reset is asserted, Z80 is halted and bus can be treated as granted
	// when BUSREQ is asserted.
	if(!_z80Reset) {
		_z80BusReqDelayMclk = 0;
		_z80ResumeDelayMclk = 0;
		_z80BusAck = _z80BusRequest;
		return;
	}

	if(_z80BusRequest) {
		_z80ResumeDelayMclk = 0;
		if(!_z80BusAck && _z80BusReqDelayMclk > 0u) {
			if(masterClocks >= _z80BusReqDelayMclk) {
				_z80BusReqDelayMclk = 0;
				_z80BusAck = true;
			} else {
				_z80BusReqDelayMclk -= (uint16_t)masterClocks;
			}
		}
	} else {
		_z80BusReqDelayMclk = 0;
		if(_z80ResumeDelayMclk > 0u) {
			if(masterClocks >= _z80ResumeDelayMclk) {
				_z80ResumeDelayMclk = 0;
			} else {
				_z80ResumeDelayMclk -= (uint16_t)masterClocks;
			}
		}
	}
}

void GenesisNativeBackend::UpdateFrameGeometry()
{
	uint32_t newW = _vdp.ActiveWidth();
	uint32_t newH = _vdp.ActiveHeight();
	if(newW != _frameWidth || newH != _frameHeight || _frameBuffer.empty()) {
		_frameWidth  = newW;
		_frameHeight = newH;
		_frameBuffer.assign(static_cast<size_t>(_frameWidth) * _frameHeight, 0xFF000000u);
	}
}

void GenesisNativeBackend::DeliverPendingVdpInterrupts()
{
	if(_vdp.HasNewVInt() && RaiseVdpIrq(6)) {
		MD_TRACE_IRQ("VINT asserted -> IRQ6"
			" mclk=" + std::to_string(_masterClock) +
			" line=" + std::to_string(_vdp.GetScanline()));
		_vdp.MarkVIntDelivered();
	}
	if(_vdp.HasNewHInt() && RaiseVdpIrq(4)) {
		MD_TRACE_IRQ("HINT asserted -> IRQ4"
			" mclk=" + std::to_string(_masterClock) +
			" line=" + std::to_string(_vdp.GetScanline()));
		_vdp.MarkHIntDelivered();
	}
}

bool GenesisNativeBackend::RaiseVdpIrq(uint8_t level)
{
	if(level == 6) {
		if(_cpu.GetPendingIrq() < 6) {
			_cpu.SetPendingIrq(6);
			return true;
		}
		return false;
	} else if(level == 4 && _cpu.GetPendingIrq() < 4) {
		_cpu.SetPendingIrq(4);
		return true;
	}
	return false;
}

void GenesisNativeBackend::VdpInterruptAcknowledge()
{
	_vdp.InterruptAcknowledge();
}

uint32_t GenesisNativeBackend::GetCurrentSliceOffsetMclk() const
{
	if(_sliceMasterClocks == 0u) {
		return 0u;
	}

	uint64_t offset = 0u;
	switch(_execContext) {
		case ExecContext::Cpu68k:
			offset = (uint64_t)_slice68kStartMclk + (uint64_t)std::max(_cpu.GetRunCycles(), 0) * 7u;
			break;
		case ExecContext::Z80:
			offset = (uint64_t)std::max(_z80.GetRunCycles(), 0) * 15u;
			break;
		default:
			offset = _apuSliceSyncedMclk;
			break;
	}

	if(offset > _sliceMasterClocks) {
		offset = _sliceMasterClocks;
	}
	return (uint32_t)offset;
}

void GenesisNativeBackend::SyncApuToSliceOffset(uint32_t offsetMclk)
{
	if(_sliceMasterClocks == 0u) {
		return;
	}

	if(offsetMclk > _sliceMasterClocks) {
		offsetMclk = _sliceMasterClocks;
	}
	if(offsetMclk <= _apuSliceSyncedMclk) {
		return;
	}

	_apu.SyncToMasterClock(_sliceStartMasterClock + offsetMclk);
	_apuSliceSyncedMclk = offsetMclk;
}

void GenesisNativeBackend::SyncApuToCurrentExecution()
{
	if(_sliceMasterClocks == 0u) {
		_apu.SyncToMasterClock(_masterClock);
		return;
	}
	SyncApuToSliceOffset(GetCurrentSliceOffsetMclk());
}

void GenesisNativeBackend::RunMasterClockSlice(uint32_t masterClocks)
{
	if(masterClocks == 0u) {
		return;
	}

	uint32_t frameMclk = (_isPal ? GenesisVdp::LinesPal : GenesisVdp::LinesNtsc) * GenesisVdp::MCLKS_PER_LINE;
	uint32_t sliceStartMclk = frameMclk ? (uint32_t)(_masterClock % frameMclk) : 0u;
	_sliceStartMasterClock = _masterClock;
	_sliceMasterClocks = masterClocks;
	_slice68kStartMclk = 0u;
	_apuSliceSyncedMclk = 0u;
	_execContext = ExecContext::None;
	_masterClock += masterClocks;

	// Process VDP-side DMA effects first. Bus68k DMA locks 68K bus for the
	// consumed portion of this slice; Z80/APU continue to advance.
	_vdp.ConsumeInternalDma(masterClocks);
	uint32_t dmaLockMclk = _vdp.Consume68kBusDma(masterClocks, sliceStartMclk);
	if(dmaLockMclk > masterClocks) {
		dmaLockMclk = masterClocks;
	}
	uint32_t cpuMclk = masterClocks - dmaLockMclk;
	_slice68kStartMclk = dmaLockMclk;

	uint32_t cpuAccum = (uint32_t)_cpuClockRemainder + cpuMclk;
	uint32_t cpuCycles = cpuAccum / 7u;
	_cpuClockRemainder = (uint8_t)(cpuAccum % 7u);
	if(cpuCycles > 0u) {
		_execContext = ExecContext::Cpu68k;
		int32_t actualCpuCycles = _cpu.Run((int32_t)cpuCycles);
		_execContext = ExecContext::None;
		if(actualCpuCycles > (int32_t)cpuCycles) {
			uint32_t overrun = (uint32_t)(actualCpuCycles - (int32_t)cpuCycles);
			_diag68kSliceOverrunCycles += overrun;
			_diag68kSliceOverrunCount++;
			_diag68kMaxSliceOverrun = std::max(_diag68kMaxSliceOverrun, overrun);
			MD_TRACE_SCHED("68K slice overrun"
				" frame=" + std::to_string(_diagFrameCounter) +
				" mclk=" + std::to_string(_masterClock) +
				" sliceMclk=" + std::to_string(masterClocks) +
				" budget=" + std::to_string(cpuCycles) +
				" actual=" + std::to_string(actualCpuCycles) +
				" overrun=" + std::to_string(overrun));
		}
	}

	uint32_t z80Accum = (uint32_t)_z80ClockRemainder + masterClocks;
	uint32_t z80Cycles = z80Accum / 15u;
	_z80ClockRemainder = (uint8_t)(z80Accum % 15u);
	if(z80Cycles > 0u && _z80Reset && !_z80BusAck && _z80ResumeDelayMclk == 0u) {
		_execContext = ExecContext::Z80;
		_z80.Run((int32_t)z80Cycles);
		_execContext = ExecContext::None;
	}

	SyncApuToSliceOffset(masterClocks);
	AdvanceZ80BusArbitration(masterClocks);
	_sliceMasterClocks = 0u;
	_slice68kStartMclk = 0u;
	_apuSliceSyncedMclk = 0u;
	_execContext = ExecContext::None;
}

// ===========================================================================
// LoadRom
// ===========================================================================

bool GenesisNativeBackend::LoadRom(const vector<uint8_t>& romData, const char* region,
                                   const uint8_t* saveRamData,    uint32_t saveRamSize,
                                   const uint8_t* saveEepromData, uint32_t saveEepromSize)
{
	if(romData.empty()) return false;

	// --- Store ROM ---
	_rom = romData;

	// --- Fixed hardware memories ---
	_workRam.assign(64 * 1024, 0);

	// --- Parse cart header for SRAM / EEPROM / mapper ---
	ParseCartHeader();

	// --- SRAM ---
	_saveRam.clear();
	if(saveRamData && saveRamSize > 0) {
		_saveRam.assign(saveRamData, saveRamData + saveRamSize);
	} else if(_sramMode != SramMode::None && _sramStart <= _sramEnd) {
		// Allocate zero-filled SRAM when the header declares it but no
		// save file exists yet.
		uint32_t sz = _sramEnd - _sramStart + 1u;
		_saveRam.assign(sz, 0xFF);
	}

	// If no header SRAM window was found but we have save data, use the
	// conventional window $200000-$20FFFF and word mode.
	if(!_saveRam.empty() && _sramMode == SramMode::None) {
		_sramStart = 0x200000u;
		_sramEnd   = 0x200000u + static_cast<uint32_t>(_saveRam.size()) - 1u;
		_sramMode  = SramMode::Word;
	}

	// --- EEPROM ---
	if(_hasEeprom || (saveEepromData && saveEepromSize > 0)) {
		_hasEeprom = true;
		if(!_eepromBusStart || _eepromBusEnd < _eepromBusStart) {
			// Conservative default window for cart EEPROM mappings.
			_eepromBusStart = 0x200000u;
			_eepromBusEnd   = 0x200001u;
		}

		// Choose chip type from save data size; default to X24C01.
		GenesisI2cEeprom::ChipType chipType = GenesisI2cEeprom::ChipType::X24C01;
		if(saveEepromSize > 128)  chipType = GenesisI2cEeprom::ChipType::M24C02;
		if(saveEepromSize > 256)  chipType = GenesisI2cEeprom::ChipType::M24C04;
		if(saveEepromSize > 512)  chipType = GenesisI2cEeprom::ChipType::M24C08;
		if(saveEepromSize > 1024) chipType = GenesisI2cEeprom::ChipType::M24C16;
		if(saveEepromSize > 2048) chipType = GenesisI2cEeprom::ChipType::M24C32;
		if(saveEepromSize > 4096) chipType = GenesisI2cEeprom::ChipType::M24C64;

		_eeprom.Load(chipType, saveEepromData, saveEepromSize);
		_eeprom.Power();
	}

	// --- RAM enable initial state ---
	_ramEnable   = (_sramMode != SramMode::None) && (_sramStart >= _rom.size());
	_ramWritable = true;
	_z80BusRequest = false;
	_z80Reset = false;  // Z80 starts in reset; game releases it via $A11200 write of 0x01
	_z80BusAck = false;
	_z80BusReqDelayMclk = 0;
	_z80ResumeDelayMclk = 0;
	_ioData[0] = _ioData[1] = _ioData[2] = 0x00;
	// data/control registers start cleared, with TH/TR pins configured as inputs.
	_ioCtrl[0] = _ioCtrl[1] = _ioCtrl[2] = 0x00u;
	_ioExt[0] = 0xFFu; // Port A TxData
	_ioExt[1] = 0x00u; // Port A RxData
	_ioExt[2] = 0x00u; // Port A serial control
	_ioExt[3] = 0xFFu; // Port B TxData
	_ioExt[4] = 0x00u; // Port B RxData
	_ioExt[5] = 0x00u; // Port B serial control
	_ioExt[6] = 0xFBu; // Port C TxData
	_ioExt[7] = 0x00u; // Port C RxData
	_ioExt[8] = 0x00u; // Port C serial control
	_ioPadState[0] = _ioPadState[1] = 0x40;
	_ioPadCounter[0] = _ioPadCounter[1] = 0;
	_ioPadTimeout[0] = _ioPadTimeout[1] = 0;
	_ioPadLatency[0] = _ioPadLatency[1] = 0;
	_ioSixButton[0] = _ioSixButton[1] = _preferSixButton;
	_bootStallFrames = 0;
	_bootInjectFrames = 0;
	_bootInjectCount = 0;

	// --- Region / timing ---
	_isPal = region && (StringUtilities::ToLower(region) == "pal");

	// --- CPU initial state from reset vector ---
	_cpuState        = {};
	_cpuState.SP     = ReadBE32(_rom, 0x0000);
	_cpuState.PC     = ReadBE32(_rom, 0x0004) & 0x00FFFFFFu;
	_cpuState.SR     = GenesisCpu68k::SR_S | (7u << 8);
	_cpuState.Stopped = false;
	_cpuUsp = 0;
	_cpuPendingIrq = 0;
	_masterClock     = 0;
	_cpuClockRemainder = 0;
	_z80ClockRemainder = 0;
	_cpu.Reset(_cpuState.SP, _cpuState.PC);
	_cpu.GetState().CycleCount = 0;
	_cpu.SetUSP(_cpuUsp);
	_cpu.ClearPendingIrq();
	_cpuState = _cpu.GetState();

	// --- VDP ---
	_vdp.Reset(_isPal);

	// --- Z80 + APU ---
	_z80.Reset();
	_apu.Reset(_isPal);

	// --- Frame buffer ---
	_frameWidth  = _vdp.ActiveWidth();
	_frameHeight = _vdp.ActiveHeight();
	_frameBuffer.assign(static_cast<size_t>(_frameWidth) * _frameHeight, 0xFF000000u);

#ifdef _DEBUG
	LogDebug("[MD Native] LoadRom"
		" size=" + std::to_string(_rom.size()) +
		" region=" + string(region ? region : "null") +
		" mapper=" + string(_mapperType == MapperType::SSF2 ? "SSF2" : "Linear") +
		" resetPC=$" + HexUtilities::ToHex24((int32_t)_cpuState.PC) +
		" resetSP=$" + HexUtilities::ToHex32(_cpuState.SP) +
		" sram=" + std::to_string(_saveRam.size()) +
		" eeprom=" + std::to_string(_hasEeprom ? _eeprom.GetMemory().size() : 0));
#endif

	return true;
}

// ===========================================================================
// RunFrame (native 68K/Z80 runner with scanline IRQ timing)
// ===========================================================================

void GenesisNativeBackend::RunFrame()
{
	// 6-button pads reset to 3-button mode if TH pulses stop for ~25 scanlines.
	// (25 * ~63.5us ≈ 1.5ms on NTSC). This must be scanline-based, not frame-based.

	// Update frame dimensions from VDP (may change if VDP registers change).
	UpdateFrameGeometry();

	// Event-driven master-clock scheduler.
	// The VDP tracks its own master-clock position (_mclkPos).
	// Each iteration: run CPU/Z80 up to the next event boundary, then advance
	// VDP to that same boundary and deliver any newly-raised VDP interrupts.
	const uint32_t totalLines = _isPal ? GenesisVdp::LinesPal : GenesisVdp::LinesNtsc;
	const uint32_t frameMclk  = totalLines * GenesisVdp::MCLKS_PER_LINE;

	_vdp.BeginFrame(_frameBuffer.data(), _frameWidth, _frameHeight);

	uint32_t frameMclkDone = 0;
	_diag68kSliceOverrunCycles = 0;
	_diag68kSliceOverrunCount = 0;
	_diag68kMaxSliceOverrun = 0;
	_diagFrameCounter++;
	while(frameMclkDone < frameMclk) {
		uint32_t sliceStartMclk = frameMclkDone;

		// Query the next timing boundary from the VDP.
		uint32_t nextVInt  = _vdp.NextVIntMclk();
		uint32_t nextHInt  = _vdp.NextHIntMclk();
		uint32_t nextVBlank = _vdp.NextVBlankFlagMclk();
		uint32_t nextEvent = frameMclk;
		if(nextVInt != UINT32_MAX && nextVInt < nextEvent) nextEvent = nextVInt;
		if(nextHInt != UINT32_MAX && nextHInt < nextEvent) nextEvent = nextHInt;
		if(nextVBlank != UINT32_MAX && nextVBlank < nextEvent) nextEvent = nextVBlank;
		if(_z80Reset) {
			uint32_t nextBusArb = UINT32_MAX;
			if(_z80BusRequest && !_z80BusAck && _z80BusReqDelayMclk > 0u) {
				nextBusArb = frameMclkDone + _z80BusReqDelayMclk;
			} else if(!_z80BusRequest && _z80ResumeDelayMclk > 0u) {
				nextBusArb = frameMclkDone + _z80ResumeDelayMclk;
			}
			if(nextBusArb < nextEvent) {
				nextEvent = nextBusArb;
			}
		}
		uint32_t nextLineBoundary = ((frameMclkDone / GenesisVdp::MCLKS_PER_LINE) + 1u) * GenesisVdp::MCLKS_PER_LINE;
		if(nextLineBoundary > frameMclk) {
			nextLineBoundary = frameMclk;
		}
		if(MD_NATIVE_DISABLE_LINE_SLICES == 0 && nextLineBoundary < nextEvent) {
			nextEvent = nextLineBoundary;
		}

		// Guard: always make forward progress of at least one line.
		if(nextEvent <= frameMclkDone) {
			nextEvent = frameMclkDone + GenesisVdp::MCLKS_PER_LINE;
			if(nextEvent > frameMclk) nextEvent = frameMclk;
		}
		// Run CPU and Z80 up to the event point.
		// Interrupts generated at nextEvent must not be visible before this slice
		// is executed, otherwise polling code can observe future VDP state.
		uint32_t sliceMclk = nextEvent - frameMclkDone;
		if(sliceMclk > 0u) {
			RunMasterClockSlice(sliceMclk);
		}

		// Advance VDP to event point (fires interrupt flags, renders lines), then
		// expose those interrupts to the CPU for the next instruction boundary.
		_vdp.AdvanceToMclk(nextEvent);
		DeliverPendingVdpInterrupts();

		frameMclkDone = nextEvent;

		uint32_t crossedLineBoundaries =
			(frameMclkDone / GenesisVdp::MCLKS_PER_LINE) -
			(sliceStartMclk / GenesisVdp::MCLKS_PER_LINE);
		for(uint32_t i = 0; i < crossedLineBoundaries; i++) {
			for(uint32_t port = 0; port < 2; port++) {
				if(_ioSixButton[port] && ++_ioPadTimeout[port] > 25u) {
					_ioPadCounter[port] = 0;
					_ioPadTimeout[port] = 0;
				}
			}
		}
	}

	if(_diag68kSliceOverrunCount > 0 || MD_NATIVE_TRACE_SCHED != 0) {
		MD_TRACE_SCHED("frame summary"
			" frame=" + std::to_string(_diagFrameCounter) +
			" lineSlices=" + string(MD_NATIVE_DISABLE_LINE_SLICES ? "off" : "on") +
			" overrunSlices=" + std::to_string(_diag68kSliceOverrunCount) +
			" overrunCycles=" + std::to_string(_diag68kSliceOverrunCycles) +
			" maxOverrun=" + std::to_string(_diag68kMaxSliceOverrun));
	}

	_cpuState    = _cpu.GetState();
	_cpuUsp      = _cpu.GetUSP();
	_cpuPendingIrq = _cpu.GetPendingIrq();

	// Boot assist disabled: rely on pure hardware-modeled IO/timing behavior.
	_bootStallFrames = 0u;
	_bootInjectFrames = 0u;

	// Flush audio samples to SoundMixer
	_apu.FlushFrame();

	if(_callbacks) {
		_callbacks->OnVideoFrame(
			_frameBuffer.data(),
			_frameWidth * sizeof(uint32_t),
			_frameWidth,
			_frameHeight
		);
	}

}

void GenesisNativeBackend::SyncSaveData()
{
	// No-op for skeleton — save data is always accessible via GetMemoryPointer.
}

// ===========================================================================
// Memory region accessors (for debugger registration)
// ===========================================================================

const uint8_t* GenesisNativeBackend::GetMemoryPointer(MemoryType type, uint32_t& size)
{
	switch(type) {
		case MemoryType::GenesisPrgRom:
			size = static_cast<uint32_t>(_rom.size());
			return _rom.empty() ? nullptr : _rom.data();
		case MemoryType::GenesisWorkRam:
			size = static_cast<uint32_t>(_workRam.size());
			return _workRam.empty() ? nullptr : _workRam.data();
		case MemoryType::GenesisSaveRam:
			size = static_cast<uint32_t>(_saveRam.size());
			return _saveRam.empty() ? nullptr : _saveRam.data();
		case MemoryType::GenesisVideoRam:
			size = 0x10000u;
			return _vdp.Vram();
		case MemoryType::GenesisColorRam:
			size = 0x80u;
			return _vdp.Cram();
		case MemoryType::GenesisVScrollRam:
			size = 0x50u;
			return _vdp.Vsram();
		case MemoryType::GenesisAudioRam:
			size = 0x2000u;
			return _z80.Ram();
		default:
			break;
	}
	size = 0;
	return nullptr;
}

const uint8_t* GenesisNativeBackend::GetSaveEeprom(uint32_t& size)
{
	if(!_hasEeprom || _eeprom.GetMemory().empty()) { size = 0; return nullptr; }
	size = static_cast<uint32_t>(_eeprom.GetMemory().size());
	return _eeprom.GetMemory().data();
}

// ===========================================================================
// Timing / state queries
// ===========================================================================

bool     GenesisNativeBackend::IsPAL()           const { return _isPal; }
double   GenesisNativeBackend::GetFps()           const { return _isPal ? 49.701460 : 59.922743; }
uint32_t GenesisNativeBackend::GetFrameCount()    const { return _vdp.GetFrameCount(); }
uint64_t GenesisNativeBackend::GetMasterClock()   const
{
	if(_execContext != ExecContext::None && _sliceMasterClocks > 0u) {
		return _sliceStartMasterClock + GetCurrentSliceOffsetMclk();
	}
	return _masterClock;
}

uint32_t GenesisNativeBackend::GetMasterClockRate() const
{
	return _isPal ? 53203424u : 53693175u;
}

void GenesisNativeBackend::GetCpuState(GenesisCpuState& state)    const { state = _cpu.GetState(); state.USP = _cpu.GetUSP(); }
void GenesisNativeBackend::GetVdpState(GenesisVdpState& state)    const
{
	state = {};
	state.FrameCount = _vdp.GetFrameCount();
	state.HClock = _vdp.GetHClock();
	state.VClock = _vdp.GetVClock();
	state.Width = (uint16_t)_frameWidth;
	state.Height = (uint16_t)_frameHeight;
	state.PAL = _isPal;
}

void GenesisNativeBackend::GetVdpRegisters(uint8_t regs[24]) const
{
	_vdp.GetRegisters(regs);
}

bool GenesisNativeBackend::GetVdpDebugState(GenesisVdpDebugState& state) const
{
	_vdp.GetDebugState(state);
	return true;
}

bool GenesisNativeBackend::GetVdpTraceLines(GenesisTraceBufferKind kind, vector<string>& lines) const
{
	return _vdp.GetDebugTraceLines(kind, lines);
}

void GenesisNativeBackend::GetFrameSize(uint32_t& w, uint32_t& h) const { w = _frameWidth; h = _frameHeight; }
bool GenesisNativeBackend::GetBackendDebugState(GenesisBackendState& state) const
{
	state = {};
	state.MasterClock = GetMasterClock();
	state.SchedulerFrameCounter = _diagFrameCounter;
	state.SliceOverrunCycles = _diag68kSliceOverrunCycles;
	state.FrameWidth = _frameWidth;
	state.FrameHeight = _frameHeight;
	state.ActiveWidth = _vdp.ActiveWidth();
	state.ActiveHeight = _vdp.ActiveHeight();
	state.VdpMclkPos = _vdp.GetMclkPos();
	state.DmaSource = _vdp.GetDmaSource();
	state.DmaLength = _vdp.GetDmaLength();
	state.Scanline = _vdp.GetScanline();
	state.HClock = _vdp.GetHClock();
	state.VdpStatus = _vdp.GetStatus();
	state.DmaFillValue = _vdp.GetDmaFillValue();
	state.DmaType = _vdp.GetDmaType();
	state.CpuPendingIrq = _cpuPendingIrq;
	state.VintPending = _vdp.IsVIntPending() ? 1 : 0;
	state.HintPending = _vdp.IsHIntPending() ? 1 : 0;
	state.DmaFillPending = _vdp.IsDmaFillPending() ? 1 : 0;
	state.DisplayEnabled = _vdp.IsDisplayEnabled() ? 1 : 0;
	state.Z80BusRequest = _z80BusRequest ? 1 : 0;
	state.Z80Reset = _z80Reset ? 1 : 0;
	state.Z80BusAck = _z80BusAck ? 1 : 0;
	state.PAL = _isPal ? 1 : 0;
	state.LineSlicesEnabled = MD_NATIVE_DISABLE_LINE_SLICES ? 0 : 1;
	state.SliceOverrunCount = _diag68kSliceOverrunCount;
	state.MaxSliceOverrun = _diag68kMaxSliceOverrun;
	return true;
}

uint16_t GenesisNativeBackend::ReadCartBusWord(uint32_t address)
{
	uint16_t hi = ReadCartBus(address);
	uint16_t lo = ReadCartBus((address + 1) & 0x00FFFFFFu);
	return (hi << 8) | lo;
}

void GenesisNativeBackend::WriteCartBusWord(uint32_t address, uint16_t value)
{
	WriteCartBus(address, (uint8_t)(value >> 8));
	WriteCartBus((address + 1) & 0x00FFFFFFu, (uint8_t)value);
}

uint8_t GenesisNativeBackend::ReadGamepadPort(uint32_t port, uint32_t buttons, bool connected)
{
	if(port >= 2u || !connected) {
		return 0x7Fu;
	}

	// Keep six-button mode enabled once an extra face button is observed.
	if(buttons & (GenesisButton::X | GenesisButton::Y | GenesisButton::Z | GenesisButton::Mode)) {
		_ioSixButton[port] = true;
	}

	// D7 is not connected; D6 reflects TH input level.
	uint8_t data = (uint8_t)(_ioPadState[port] | 0x3Fu);

	uint8_t step = (uint8_t)(_ioPadCounter[port] | (data >> 6));
	if(!_ioSixButton[port]) {
		// 3-button pads never advance into extended states.
		step = (uint8_t)(data >> 6);
	}

	// TH direction switching latency (measured on real hardware).
	uint64_t cycles = _cpu.GetState().CycleCount;
	if(cycles < _ioPadLatency[port]) {
		step &= 0xFEu;
	}

	auto activeLow = [&](uint8_t bit, uint32_t mask) {
		if(buttons & mask) {
			data &= (uint8_t)~(1u << bit);
		}
	};

	switch(step) {
		case 2: // Third low:  ?0SA0000
			activeLow(5, GenesisButton::Start);
			activeLow(4, GenesisButton::A);
			data &= 0xF0u;
			break;

		case 5: // Fourth high: ?1CBMXYZ
			activeLow(5, GenesisButton::C);
			activeLow(4, GenesisButton::B);
			activeLow(3, GenesisButton::Mode);
			activeLow(2, GenesisButton::X);
			activeLow(1, GenesisButton::Y);
			activeLow(0, GenesisButton::Z);
			break;

		case 4: // Fourth low: ?0SA1111
			activeLow(5, GenesisButton::Start);
			activeLow(4, GenesisButton::A);
			break;

		default:
			if(step & 1u) {
				// TH=1: ?1CBRLDU
				activeLow(5, GenesisButton::C);
				activeLow(4, GenesisButton::B);
				activeLow(3, GenesisButton::Right);
				activeLow(2, GenesisButton::Left);
				activeLow(1, GenesisButton::Down);
				activeLow(0, GenesisButton::Up);
			} else {
				// TH=0: ?0SA00DU
				activeLow(5, GenesisButton::Start);
				activeLow(4, GenesisButton::A);
				data &= static_cast<uint8_t>(~uint8_t{0x0Cu});
				activeLow(1, GenesisButton::Down);
				activeLow(0, GenesisButton::Up);
			}
			break;
	}

	return data;
}

void GenesisNativeBackend::WriteGamepadPort(uint32_t port, uint8_t data, uint8_t mask, bool connected)
{
	if(port >= 2u) {
		return;
	}
	uint8_t prevState = (uint8_t)(_ioPadState[port] & 0x40u);

	bool thOutput = (mask & 0x40u) != 0u;
	// Compatibility fallback: several games/pads rely on TH phase writes to
	// $A10003/$A10005 during early boot before direction bits are initialized.
	// SegaCxx models this behavior by stepping on data writes directly.
	bool compatBootThDrive = !thOutput && mask == 0x00u;

	if(thOutput || compatBootThDrive) {
		uint8_t newState = (uint8_t)(data & 0x40u);
		_ioPadLatency[port] = 0;

		// 6-button counter increments on TH low->high transitions.
		if(_ioSixButton[port] && _ioPadCounter[port] < 8u && newState && !prevState) {
			_ioPadCounter[port] = (_ioPadCounter[port] <= 6u) ? (uint8_t)(_ioPadCounter[port] + 2u) : 8u;
			_ioPadTimeout[port] = 0;
			MD_TRACE_PAD("TH rise pulse"
				" port=" + std::to_string(port) +
				" ctr=" + std::to_string(_ioPadCounter[port]));
		}

		_ioPadState[port] = newState;
		if(newState != prevState) {
			MD_TRACE_PAD("TH state change"
				" port=" + std::to_string(port) +
				" th=" + std::to_string((newState >> 6) & 1u) +
				" mode=" + std::string(thOutput ? "ctrl" : "compat"));
		}
	} else {
		// TH is pulled high while configured as input.
		_ioPadState[port] = 0x40u;

		// Input-state switching to high is delayed on hardware.
		if(!prevState) {
			_ioPadLatency[port] = _cpu.GetState().CycleCount + 172u;
			MD_TRACE_PAD("TH input latency arm"
				" port=" + std::to_string(port) +
				" at=" + std::to_string((uint64_t)_ioPadLatency[port]));
		}
	}

	if(!connected) {
		// Disconnected ports should not keep stale six-button sequence state.
		_ioPadCounter[port] = 0;
		_ioPadTimeout[port] = 0;
	}
}

uint8_t GenesisNativeBackend::ReadIoRegister(uint32_t offset)
{
	offset &= 0x0Fu;
	switch(offset) {
		case 0x00:
			// Version register:
			//   bit7=overseas, bit6=PAL, bit5=hardware ID (set on MD/Genesis)
			// Typical values: NTSC export=0xA0, PAL export=0xE0.
			return _isPal ? 0xE0u : 0xA0u;

			case 0x01:
			case 0x02:
			case 0x03: {
				uint32_t port = offset - 0x01u;
				// Treat gameplay ports as virtual gamepads by default.
				// Some frontends may report keyboard-backed devices as
				// "not connected" even though button states are available.
				bool connected = (port < 2u);
				uint32_t buttons = 0u;
				if(_callbacks && port < 2u) {
					connected = _callbacks->IsControllerConnected((int)port);
					buttons = _callbacks->GetControllerButtons((int)port);
					if(!connected) {
						connected = true;
					}
				}

					// I/O data/control pins are 7-bit for controller ports.
					// D7 is not connected and should read as 0.
					uint8_t ctrl = (uint8_t)(_ioCtrl[port] & 0x7Fu);
					uint8_t inputData = ReadGamepadPort(port, buttons, connected);
					uint8_t out = (uint8_t)((_ioData[port] & ctrl) | (inputData & (uint8_t)~ctrl));
					return (uint8_t)(out & 0x7Fu);
				}

		case 0x04:
		case 0x05:
		case 0x06: {
			uint32_t port = offset - 0x04u;
			return port < 3u ? (uint8_t)(_ioCtrl[port] & 0x7Fu) : 0x00u;
		}

		case 0x07:
		case 0x08:
		case 0x09:
		case 0x0A:
		case 0x0B:
		case 0x0C:
		case 0x0D:
		case 0x0E:
		case 0x0F:
			// Serial/Tx registers are simple latches on MD.
			return _ioExt[offset - 0x07u];

		default:
			return 0x00u;
	}
}

void GenesisNativeBackend::WriteIoRegister(uint32_t offset, uint8_t value)
{
	offset &= 0x0Fu;
	switch(offset) {
			case 0x01:
			case 0x02:
			case 0x03: {
				uint32_t port = offset - 0x01u;
				if(port < 3u) {
					_ioData[port] = (uint8_t)(value & 0x7Fu);
						if(port < 2u) {
							bool connected = true;
							if(_callbacks) {
								connected = _callbacks->IsControllerConnected((int)port);
								if(!connected) {
									connected = true;
								}
							}
							WriteGamepadPort(port, _ioData[port], (uint8_t)(_ioCtrl[port] & 0x7Fu), connected);
						}
					}
					break;
			}

			case 0x04:
			case 0x05:
			case 0x06: {
				uint32_t port = offset - 0x04u;
				if(port < 3u) {
					uint8_t ctrl = (uint8_t)(value & 0x7Fu);
					if(_ioCtrl[port] != ctrl) {
						_ioCtrl[port] = ctrl;
							if(port < 2u) {
								bool connected = true;
								if(_callbacks) {
									connected = _callbacks->IsControllerConnected((int)port);
									if(!connected) {
										connected = true;
									}
								}
								WriteGamepadPort(port, _ioData[port], (uint8_t)(_ioCtrl[port] & 0x7Fu), connected);
							}
						}
					}
				break;
			}

		case 0x07:
		case 0x0A:
		case 0x0D:
			// TxData registers: raw latch.
			_ioExt[offset - 0x07u] = value;
			break;

		case 0x09:
		case 0x0C:
		case 0x0F:
			// Serial control: lower 3 bits are read-only.
			_ioExt[offset - 0x07u] = (uint8_t)(value & 0xF8u);
			break;

		default:
			break;
	}
}

uint8_t GenesisNativeBackend::CpuBusRead8(uint32_t address)
{
	if(_cpuTestBusEnabled && _cpuTestBus.size() == 0x1000000u) {
		return _cpuTestBus[address & 0x00FFFFFFu];
	}
	return ReadCartBus(address);
}

void GenesisNativeBackend::CpuBusWrite8(uint32_t address, uint8_t value)
{
	if(_cpuTestBusEnabled && _cpuTestBus.size() == 0x1000000u) {
		_cpuTestBus[address & 0x00FFFFFFu] = value;
		return;
	}
	WriteCartBus(address, value);
}

uint8_t GenesisNativeBackend::CpuBusWaitStates(uint32_t address, bool isWrite) const
{
	if(_cpuTestBusEnabled) {
		return 0u;
	}
	// 68K->VDP DMA freezes the 68K bus until DMA progress frees it again.
	// Apply a strong per-access penalty so the CPU effectively stalls while
	// RunMasterClockSlice() advances DMA at the start of each slice.
	if(_vdp.Is68kBusDmaActive()) {
		return 0xFFu;
	}
	(void)isWrite;
	switch(DecodeBusRegion(address & 0x00FFFFFFu)) {
		case BusRegion::Z80Space:
			// Access to Z80 address space incurs a wait state on the 68K bus.
			return 1u;
		case BusRegion::VdpPorts:
			// During V-blank or display-off the VDP bus is free; minimal penalty.
			// During active display the 68K must wait for an external access slot.
			return _vdp.IsBlanking() ? 1u : 4u;
		default:
			return 0u;
	}
}

uint8_t GenesisNativeBackend::ReadBusForZ80(uint32_t physAddr)
{
	return ReadCartBus(physAddr & 0x00FFFFFFu);
}

void GenesisNativeBackend::WriteBusForZ80(uint32_t physAddr, uint8_t val)
{
	WriteCartBus(physAddr & 0x00FFFFFFu, val);
}

uint8_t GenesisNativeBackend::GetZ80To68kBusPenaltyCycles() const
{
	// Coarse contention model based on BlastEm behavior:
	// - baseline arbitration cost for Z80 bank-window access
	// - additional stall while VDP 68K-bus DMA is running
	uint8_t penalty = 3u;
	if(_vdp.Is68kBusDmaActive()) {
		penalty = (uint8_t)(penalty + 8u);
	}
	return penalty;
}

// ===========================================================================
// EEPROM bus helpers
//
// The Genesis EEPROM is word-accessed but the debugger uses byte granularity.
// Pin assignments use 4-bit codes: bit3 = byte lane (1=high/even, 0=low/odd),
// bits2-0 = bit within that byte.
// ===========================================================================

uint8_t GenesisNativeBackend::EepromRead(uint32_t address) const
{
	// Determine byte lane of this address within the EEPROM window.
	bool isHighByte = ((address & 1u) == 0u);   // big-endian: even addr = high byte
	bool rsdaHigh   = ((_rsda >> 3) != 0u);

	if(isHighByte == rsdaHigh) {
		// This byte carries the RSDA bit.
		uint8_t bit = _rsda & 0x07u;
		return _eeprom.ReadSda() ? (1u << bit) : 0u;
	}
	return 0u;
}

void GenesisNativeBackend::EepromWrite(uint32_t address, uint8_t value)
{
	bool isHighByte = ((address & 1u) == 0u);

	bool scl = false;
	bool sda = false;

	bool wsclHigh = ((_wscl >> 3) != 0u);
	bool wsdaHigh = ((_wsda >> 3) != 0u);

	if(isHighByte == wsclHigh) scl = (value >> (_wscl & 0x07u)) & 1u;
	if(isHighByte == wsdaHigh) sda = (value >> (_wsda & 0x07u)) & 1u;

	// Only update and clock the state machine on the byte that carries the
	// clock line; the data line is sampled on the same write.
	if(isHighByte == wsclHigh) {
		_eeprom.Update(scl, sda);
	}
}

// ===========================================================================
// Cart bus read / write (68K memory map)
//
// Memory map summary:
//   $000000-$3FFFFF  Cart ROM (SSF2-banked or linear)
//   $A00000-$A0FFFF  Z80 address space (open-bus placeholder)
//   $A10000-$A1001F  I/O registers (controller + version)
//   $A11000          Memory mode register
//   $A11100-$A11101  Z80 bus request
//   $A11200-$A11201  Z80 reset
//   $A130F0          RAM control   (bit0=enable, bit1=write-protect)
//   $A130F1-$A130FF  SSF2 bank registers (odd addresses, written as word)
//   $A14000-$A14003  TMSS unlock ("SEGA")
//   $A14101          TMSS/cartridge register
//   $C00000-$C0001F  VDP data/control/status
//   $E00000-$FFFFFF  Work RAM (64 KB, mirrored)
// ===========================================================================

uint8_t GenesisNativeBackend::ReadCartBus(uint32_t address)
{
	address &= 0x00FFFFFFu;
	switch(DecodeBusRegion(address)) {
		case BusRegion::Cart: {
			// SRAM / EEPROM window takes priority if enabled.
			if(_hasEeprom && _ramEnable &&
			   address >= _eepromBusStart && address <= _eepromBusEnd)
			{
				return EepromRead(address);
			}

			if(_sramMode != SramMode::None && _ramEnable &&
			   !_saveRam.empty() &&
			   address >= _sramStart && address <= _sramEnd)
			{
				uint32_t off = address - _sramStart;
				switch(_sramMode) {
					case SramMode::Word:
						return (off < _saveRam.size()) ? _saveRam[off] : 0xFFu;
					case SramMode::UpperByte:
						// Upper byte at even addresses; odd addresses open-bus.
						if((address & 1u) == 0u && (off >> 1) < _saveRam.size())
							return _saveRam[off >> 1];
						return 0xFFu;
					case SramMode::LowerByte:
						// Lower byte at odd addresses; even addresses open-bus.
						if((address & 1u) == 1u && (off >> 1) < _saveRam.size())
							return _saveRam[off >> 1];
						return 0xFFu;
					default: break;
				}
			}

			// ROM — apply SSF2 bank table.
			uint32_t window   = address >> 19; // 512 KB window index (0-7)
			uint32_t bank     = _romBank[window];
			if(bank == 0xFFu) return 0xFFu;
			uint32_t physAddr = (static_cast<uint32_t>(bank) << 19) | (address & 0x7FFFFu);
			return (physAddr < _rom.size()) ? _rom[physAddr] : 0xFFu;
		}

		case BusRegion::CartOverflow:
			// $400000-$7FFFFF is reserved in the documented map.
			return 0xFFu;

		case BusRegion::Z80Space:
			if((address & 0xFFFFFCu) == 0xA04000u) {
				// YM2612 register interface uses the standard 4-byte layout:
				// $A04000=addr0, $A04001=data0, $A04002=addr1, $A04003=data1.
				uint8_t part = (uint8_t)((address >> 1) & 1u);
				if((address & 1u) == 0u) {
					return _apu.ReadYmStatus(part);
				}
				return 0x00u;
			}
			// 68K can access Z80 bus when:
			//   1. BUSREQ is asserted and Z80 has released the bus, or
			//   2. Z80 is in reset (not driving the bus).
			// The latter is the common startup pattern: write Z80 program with
			// reset held, then release reset.
			if(IsZ80BusGranted()) {
				return _z80.BusRead((uint16_t)(address & 0xFFFFu));
			}
			return 0xFFu;

		case BusRegion::IoRegs:
			// I/O device registers are on odd addresses ($A10001,$03,...,$1F).
			// Even addresses are effectively open bus for this block.
			return (address & 1u) ? ReadIoRegister((address >> 1) & 0x0Fu) : 0x00u;

		case BusRegion::MemoryMode:
			// $A11000 memory mode register (32X/MCD path). Not modeled here.
			return 0x00u;

			case BusRegion::Z80BusReq: {
				// Some software polls $A11101 with BTST while others read $A11100.
				// Mirror bit0 on both lanes for compatibility.
				//   bit0=0 => 68K owns Z80 bus (BUSACK asserted)
				//   bit0=1 => Z80 bus not granted
				uint8_t status = (uint8_t)(0xFEu | (_z80BusAck ? 0x00u : 0x01u));
				return status;
			}

		case BusRegion::Z80Reset:
			// Mirror bit0 on both lanes (same rationale as Z80BusReq above).
			return _z80Reset ? 0x01u : 0x00u;

		case BusRegion::MapperRegs: {
			uint32_t reg = address & 0xFFu;
			// $A130F1 (odd) — SRAM access status; $A130F0 (even) returns 0
			if(reg == 0xF1u) {
				return (uint8_t)((_ramEnable ? 0x01u : 0x00u) | (_ramWritable ? 0x00u : 0x02u));
			}
			if(reg == 0xF0u) return 0x00u;
			// $A130F3,$A130F5,...,$A130FF (odd) — SSF2 bank registers (windows 1–7)
			if(reg >= 0xF3u && reg <= 0xFFu && (reg & 1u) == 1u) {
				uint32_t window = (reg - 0xF3u) / 2u + 1u;
				return window < 8u ? _romBank[window] : 0x00u;
			}
			return 0x00u;
		}

		case BusRegion::Tmss:
			// $A14000–$A14003: TMSS unlock registers — return open-bus on reads.
			return 0xFFu;

		case BusRegion::TmssCart:
			// $A14101 TMSS/cartridge register — not modeled, return open bus.
			return 0xFFu;

		case BusRegion::VdpPorts: {
			uint32_t regGroup = address & 0x1Cu;
			// PSG window ($C00010-$C00017) is write-only.
			if(regGroup == 0x10u || regGroup == 0x14u) {
				return 0xFFu;
			}
			return _vdp.ReadByte(address);
		}

		case BusRegion::WorkRam:
			return _workRam[address & 0xFFFFu];

		default:
			return 0xFFu;
	}
}

void GenesisNativeBackend::WriteCartBus(uint32_t address, uint8_t value)
{
	address &= 0x00FFFFFFu;
	switch(DecodeBusRegion(address)) {
		case BusRegion::Cart:
			if(_hasEeprom && _ramEnable &&
			   address >= _eepromBusStart && address <= _eepromBusEnd) {
				EepromWrite(address, value);
				return;
			}

			if(_sramMode != SramMode::None && _ramWritable && !_saveRam.empty() &&
			   address >= _sramStart && address <= _sramEnd)
			{
				uint32_t off = address - _sramStart;
				switch(_sramMode) {
					case SramMode::Word:
						if(off < _saveRam.size()) _saveRam[off] = value;
						break;
					case SramMode::UpperByte:
						if((address & 1u) == 0u && (off >> 1) < _saveRam.size())
							_saveRam[off >> 1] = value;
						break;
					case SramMode::LowerByte:
						if((address & 1u) == 1u && (off >> 1) < _saveRam.size())
							_saveRam[off >> 1] = value;
						break;
					default: break;
				}
			}
			return;

		case BusRegion::IoRegs:
			// Genesis I/O chip: register index comes from A1-A4.
			// Byte writes affect only odd addresses; word writes land on low byte.
			if(address & 1u) {
				WriteIoRegister((address >> 1) & 0x0Fu, value);
			}
			return;

		case BusRegion::MemoryMode:
			// $A11000 memory mode register (used by add-on hardware).
			// Base Genesis path does not model this register yet.
			return;

		case BusRegion::Z80BusReq:
			// Games typically use word writes (#$0100 / #$0000); only the high byte
			// should affect the latch so the trailing low-byte write does not undo it.
			if((address & 1u) == 0u) {
				bool request = (value & 0x01u) != 0;
				if(request) {
					_z80BusRequest = true;
					_z80ResumeDelayMclk = 0;
					if(_z80Reset) {
						if(!_z80BusAck && _z80BusReqDelayMclk == 0u) {
							_z80BusReqDelayMclk = Z80BusReqAckDelayMclk;
						}
					} else {
						_z80BusAck = true;
						_z80BusReqDelayMclk = 0;
					}
				} else {
					_z80BusRequest = false;
					_z80BusAck = false;
					_z80BusReqDelayMclk = 0;
					_z80ResumeDelayMclk = _z80Reset ? Z80BusResumeDelayMclk : 0u;
				}
				MD_TRACE_BUS("A11100 write"
					" req=" + std::to_string(request ? 1 : 0) +
					" ack=" + std::to_string(_z80BusAck ? 1 : 0) +
					" zreset=" + std::to_string(_z80Reset ? 1 : 0) +
					" reqDelay=" + std::to_string(_z80BusReqDelayMclk) +
					" resumeDelay=" + std::to_string(_z80ResumeDelayMclk));
			}
			return;

		case BusRegion::Z80Reset:
			// Same byte-lane behavior as $A11100.
			if((address & 1u) == 0u) {
				bool newReset = (value & 0x01u) != 0;
				// On /RESET release, restart Z80 state (and APU core state in this
				// simplified model). On /RESET assert, keep the CPU halted.
				if(newReset && !_z80Reset) {
					_z80.Reset();
					_apu.Reset(_isPal);
				}
				_z80Reset = newReset;
				if(!_z80Reset) {
					_z80BusReqDelayMclk = 0;
					_z80ResumeDelayMclk = 0;
					_z80BusAck = _z80BusRequest;
				} else if(_z80BusRequest) {
					_z80BusAck = false;
					_z80BusReqDelayMclk = Z80BusReqAckDelayMclk;
					_z80ResumeDelayMclk = 0;
				}
				else {
					_z80BusAck = false;
					_z80BusReqDelayMclk = 0;
					_z80ResumeDelayMclk = 0;
				}
				MD_TRACE_BUS("A11200 write"
					" reset=" + std::to_string(_z80Reset ? 1 : 0) +
					" req=" + std::to_string(_z80BusRequest ? 1 : 0) +
					" ack=" + std::to_string(_z80BusAck ? 1 : 0) +
					" reqDelay=" + std::to_string(_z80BusReqDelayMclk));
			}
			return;

		case BusRegion::MapperRegs: {
			uint32_t reg = address & 0xFFu;
			// $A130F1 (odd) — SRAM access register
			if(reg == 0xF1u) {
				_ramEnable   = (value & 0x01u) != 0u;
				_ramWritable = (value & 0x02u) == 0u; // bit1=0 means writable
			} else if(reg >= 0xF3u && reg <= 0xFFu && (reg & 1u) == 1u) {
				// $A130F3,$A130F5,...,$A130FF (odd) — SSF2 bank registers (windows 1–7)
				uint32_t window = (reg - 0xF3u) / 2u + 1u;
				if(window < 8u) {
					_romBank[window] = value & 0x3Fu;
				}
			}
			return;
		}

				case BusRegion::Z80Space:
					if((address & 0xFFFFFCu) == 0xA04000u) {
						SyncApuToCurrentExecution();
						uint8_t part = (uint8_t)((address >> 1) & 1u);
						bool isAddr = (address & 1u) == 0u;
						_apu.WriteYm(part, isAddr, value);
						return;
					}
			// Mirror read rule: 68K can write Z80 bus when BUSREQ is granted
			// OR Z80 is held in reset (common startup: load sound driver then release reset).
			if(IsZ80BusGranted()) {
				_z80.BusWrite((uint16_t)(address & 0xFFFFu), value);
			}
			return;

		case BusRegion::Tmss:
			// $A14000–$A14003: games write "SEGA" here as a TMSS unlock.
			// We don't enforce TMSS; silently accept the write.
			return;

		case BusRegion::TmssCart:
			// $A14101 TMSS/cartridge register.
			// Not modeled; accept writes as no-ops.
			return;

		case BusRegion::VdpPorts: {
			uint32_t regGroup = address & 0x1Cu;
			// PSG write window ($C00010/$14 groups), low byte only.
			if(regGroup == 0x10u || regGroup == 0x14u) {
				if(address & 1u) {
					SyncApuToCurrentExecution();
					_apu.WritePsg(value);
				}
				return;
			}
			_vdp.WriteByte(address, value);
			return;
		}

		case BusRegion::WorkRam:
			_workRam[address & 0xFFFFu] = value;
			if(CpuRamTraceShouldLog(_vdp.GetFrameCount(), address)) {
				CpuRamTraceLog(_vdp.GetFrameCount(), _vdp.GetScanline(), address, value,
					(_cpu.GetState().PC & 0x00FFFFFFu), _masterClock);
			}
			return;

		default:
			return;
	}
}

// ===========================================================================
// IGenesisCoreBackend — bus read / write (for debugger)
// ===========================================================================

uint8_t GenesisNativeBackend::ReadMemory(MemoryType type, uint32_t address)
{
	switch(type) {
		case MemoryType::GenesisMemory:
			return ReadCartBus(address);
		case MemoryType::GenesisVideoRam:
			return _vdp.Vram()[address & 0xFFFFu];
		case MemoryType::GenesisColorRam:
			return _vdp.Cram()[address & 0x7Fu];
		case MemoryType::GenesisVScrollRam:
			return _vdp.Vsram()[address & 0x4Fu];
		case MemoryType::GenesisAudioRam:
			return _z80.Ram()[address & 0x1FFFu];
		default:
			return 0;
	}
}

void GenesisNativeBackend::WriteMemory(MemoryType type, uint32_t address, uint8_t value)
{
	switch(type) {
		case MemoryType::GenesisMemory:
			WriteCartBus(address, value);
			break;
		case MemoryType::GenesisVideoRam:
			_vdp.Vram()[address & 0xFFFFu] = value;
			break;
		case MemoryType::GenesisColorRam:
			_vdp.Cram()[address & 0x7Fu] = value;
			break;
		case MemoryType::GenesisVScrollRam:
			_vdp.Vsram()[address & 0x4Fu] = value;
			break;
		case MemoryType::GenesisAudioRam:
			_z80.Ram()[address & 0x1FFFu] = value;
			break;
		default:
			break;
	}
}

// ===========================================================================
// Debugger — CPU control / disassembly
// ===========================================================================

bool GenesisNativeBackend::SetProgramCounter(uint32_t address)
{
	_cpu.GetState().PC = address & 0x00FFFFFFu;
	_cpuState = _cpu.GetState();
	return true;
}

uint32_t GenesisNativeBackend::GetInstructionSize(uint32_t address)
{
	return _cpu.DecodeInstructionSize(address & 0x00FFFFFFu);
}

const char* GenesisNativeBackend::DisassembleInstruction(uint32_t address)
{
	address &= 0x00FFFFFFu;
	uint8_t opSize = (uint8_t)GetInstructionSize(address);
	if(opSize < 2 || opSize > 10 || (opSize & 1u) != 0) {
		opSize = 2;
	}

	auto readWord = [this](uint32_t a) -> uint16_t {
		return ReadCartBusWord(a & 0x00FFFFFFu);
	};

	_disasmText = DisassembleM68k(address, opSize, readWord);
	return _disasmText.c_str();
}

// ===========================================================================
// Save state
// ===========================================================================

bool GenesisNativeBackend::SaveState(vector<uint8_t>& outState)
{
	outState.clear();
	_cpuState = _cpu.GetState();
	_cpuUsp = _cpu.GetUSP();
	_cpuPendingIrq = _cpu.GetPendingIrq();

	// Header
	AppendValue(outState, NativeStateMagic);
	AppendValue(outState, NativeStateVersion);

	// Region / timing
	AppendValue(outState, _isPal);
	AppendValue(outState, _masterClock);
	AppendValue(outState, _cpuClockRemainder);
	AppendValue(outState, _z80ClockRemainder);

	// CPU state
	AppendValue(outState, _cpuState.CycleCount);
	AppendValue(outState, _cpuState.PC);
	AppendValue(outState, _cpuState.SP);
	AppendValue(outState, _cpuState.D);
	AppendValue(outState, _cpuState.A);
	AppendValue(outState, _cpuState.SR);
	AppendValue(outState, _cpuState.Stopped);
	AppendValue(outState, _cpuUsp);
	AppendValue(outState, _cpuPendingIrq);

	// Cart / mapper state
	AppendValue(outState, static_cast<uint8_t>(_mapperType));
	AppendValue(outState, _romBank);
	AppendValue(outState, _ramEnable);
	AppendValue(outState, _ramWritable);
	AppendValue(outState, _sramStart);
	AppendValue(outState, _sramEnd);
	AppendValue(outState, static_cast<uint8_t>(_sramMode));
	AppendValue(outState, _z80BusRequest);
	AppendValue(outState, _z80Reset);
	AppendValue(outState, _z80BusAck);
	AppendValue(outState, _z80BusReqDelayMclk);
	AppendValue(outState, _z80ResumeDelayMclk);
	AppendValue(outState, _ioData);
	AppendValue(outState, _ioCtrl);
	AppendValue(outState, _ioExt);
	AppendValue(outState, _ioPadState);
	AppendValue(outState, _ioPadCounter);
	AppendValue(outState, _ioPadTimeout);
	AppendValue(outState, _ioPadLatency);
	AppendValue(outState, _ioSixButton);
	AppendValue(outState, _preferSixButton);
	AppendValue(outState, _bootStallFrames);
	AppendValue(outState, _bootInjectFrames);
	AppendValue(outState, _bootInjectCount);

	// EEPROM scalar state
	AppendValue(outState, _hasEeprom);
	AppendValue(outState, _eepromBusStart);
	AppendValue(outState, _eepromBusEnd);
	AppendValue(outState, _rsda);
	AppendValue(outState, _wsda);
	AppendValue(outState, _wscl);
	if(_hasEeprom) {
		GenesisI2cEeprom::SavedState es = _eeprom.CaptureState();
		AppendValue(outState, es);
		AppendBlob(outState, _eeprom.GetMemory());
	}

	// VDP state
	_vdp.SaveState(outState);

	// Z80 + APU state
	_z80.SaveState(outState);
	_apu.SaveState(outState);

	// Memory blobs
	AppendBlob(outState, _workRam);
	AppendBlob(outState, _saveRam);

	return true;
}

bool GenesisNativeBackend::LoadState(const vector<uint8_t>& state)
{
	size_t offset = 0;

	uint32_t magic = 0, version = 0;
	if(!ReadValue(state, offset, magic) || !ReadValue(state, offset, version)) return false;
	if(magic != NativeStateMagic || version != NativeStateVersion)             return false;

	// Region / timing
	if(!ReadValue(state, offset, _isPal))        return false;
	if(!ReadValue(state, offset, _masterClock))  return false;
	if(!ReadValue(state, offset, _cpuClockRemainder)) return false;
	if(!ReadValue(state, offset, _z80ClockRemainder)) return false;

	// CPU state
	if(!ReadValue(state, offset, _cpuState.CycleCount)) return false;
	if(!ReadValue(state, offset, _cpuState.PC))         return false;
	if(!ReadValue(state, offset, _cpuState.SP))         return false;
	if(!ReadValue(state, offset, _cpuState.D))          return false;
	if(!ReadValue(state, offset, _cpuState.A))          return false;
	if(!ReadValue(state, offset, _cpuState.SR))         return false;
	if(!ReadValue(state, offset, _cpuState.Stopped))    return false;
	if(!ReadValue(state, offset, _cpuUsp))              return false;
	if(!ReadValue(state, offset, _cpuPendingIrq))       return false;

	// Cart / mapper state
	uint8_t mapperByte = 0, sramModeByte = 0;
	if(!ReadValue(state, offset, mapperByte))   return false;
	_mapperType = static_cast<MapperType>(mapperByte);
	if(!ReadValue(state, offset, _romBank))     return false;
	if(!ReadValue(state, offset, _ramEnable))   return false;
	if(!ReadValue(state, offset, _ramWritable)) return false;
	if(!ReadValue(state, offset, _sramStart))   return false;
	if(!ReadValue(state, offset, _sramEnd))     return false;
	if(!ReadValue(state, offset, sramModeByte)) return false;
	_sramMode = static_cast<SramMode>(sramModeByte);
	if(!ReadValue(state, offset, _z80BusRequest)) return false;
	if(!ReadValue(state, offset, _z80Reset))      return false;
	if(!ReadValue(state, offset, _z80BusAck))     return false;
	if(!ReadValue(state, offset, _z80BusReqDelayMclk)) return false;
	if(!ReadValue(state, offset, _z80ResumeDelayMclk)) return false;
	if(!ReadValue(state, offset, _ioData))        return false;
	if(!ReadValue(state, offset, _ioCtrl))        return false;
	if(!ReadValue(state, offset, _ioExt))         return false;
	if(!ReadValue(state, offset, _ioPadState))      return false;
	if(!ReadValue(state, offset, _ioPadCounter))    return false;
	if(!ReadValue(state, offset, _ioPadTimeout))    return false;
	if(!ReadValue(state, offset, _ioPadLatency))    return false;
	if(!ReadValue(state, offset, _ioSixButton))     return false;
	if(!ReadValue(state, offset, _preferSixButton)) return false;
	if(!ReadValue(state, offset, _bootStallFrames)) return false;
	if(!ReadValue(state, offset, _bootInjectFrames)) return false;
	if(!ReadValue(state, offset, _bootInjectCount)) return false;

	// EEPROM scalar state
	if(!ReadValue(state, offset, _hasEeprom))      return false;
	if(!ReadValue(state, offset, _eepromBusStart)) return false;
	if(!ReadValue(state, offset, _eepromBusEnd))   return false;
	if(!ReadValue(state, offset, _rsda))           return false;
	if(!ReadValue(state, offset, _wsda))           return false;
	if(!ReadValue(state, offset, _wscl))           return false;
	if(_hasEeprom) {
		GenesisI2cEeprom::SavedState es = {};
		if(!ReadValue(state, offset, es)) return false;
		_eeprom.RestoreState(es);
		vector<uint8_t> eepromMem;
		if(!ReadBlob(state, offset, eepromMem)) return false;
		_eeprom.GetMemory().swap(eepromMem);
	}

	// VDP state
	if(!_vdp.LoadState(state, offset)) return false;

	// Z80 + APU state
	if(!_z80.LoadState(state, offset)) return false;
	if(!_apu.LoadState(state, offset)) return false;

	// Memory blobs
	if(!ReadBlob(state, offset, _workRam)) return false;
	if(!ReadBlob(state, offset, _saveRam)) return false;

	// Sanity-check frame dimensions.
	_frameWidth  = _vdp.ActiveWidth();
	_frameHeight = _vdp.ActiveHeight();
	if(_frameWidth  == 0 || _frameWidth  > 512) _frameWidth  = 320;
	if(_frameHeight == 0 || _frameHeight > 512) _frameHeight = 224;
	_frameBuffer.assign(static_cast<size_t>(_frameWidth) * _frameHeight, 0xFF000000u);
	_sliceStartMasterClock = _masterClock;
	_sliceMasterClocks = 0u;
	_slice68kStartMclk = 0u;
	_apuSliceSyncedMclk = 0u;
	_execContext = ExecContext::None;

	_cpu.GetState() = _cpuState;
	_cpu.SetUSP(_cpuUsp);
	_cpu.SetPendingIrq(_cpuPendingIrq);

	return true;
}

// ===========================================================================
// Z80 debug interface
// ===========================================================================

void GenesisNativeBackend::Z80ProcessInstruction()
{
	_emu->ProcessInstruction<CpuType::GenesisZ80>();
}

void GenesisNativeBackend::Z80ProcessRead(uint16_t addr, uint8_t& value, MemoryOperationType opType)
{
	_emu->ProcessMemoryRead<CpuType::GenesisZ80>(addr, value, opType);
}

bool GenesisNativeBackend::Z80ProcessWrite(uint16_t addr, uint8_t& value, MemoryOperationType opType)
{
	return _emu->ProcessMemoryWrite<CpuType::GenesisZ80>(addr, value, opType);
}

GenesisZ80State GenesisNativeBackend::GetZ80DebugState() const
{
	GenesisCpuZ80::Z80State s = _z80.CaptureState();
	GenesisZ80State out;
	out.CycleCount = (s.CycleCount >= 0) ? (uint64_t)s.CycleCount : 0;
	out.PC = s.PC;  out.SP = s.SP;
	out.IX = s.IX;  out.IY = s.IY;
	out.A = s.A;    out.F = s.F;
	out.B = s.B;    out.C = s.C;
	out.D = s.D;    out.E = s.E;
	out.H = s.H;    out.L = s.L;
	out.A2 = s.A2;  out.F2 = s.F2;
	out.B2 = s.B2;  out.C2 = s.C2;
	out.D2 = s.D2;  out.E2 = s.E2;
	out.H2 = s.H2;  out.L2 = s.L2;
	out.I = s.I;    out.R = s.R;
	out.IFF1 = s.IFF1; out.IFF2 = s.IFF2;
	out.IM = s.IM;  out.Halted = s.Halted;
	out.BankReg = s.BankReg;
	return out;
}

void GenesisNativeBackend::SetZ80ProgramCounter(uint16_t addr)
{
	GenesisCpuZ80::Z80State s = _z80.CaptureState();
	s.PC = addr;
	_z80.RestoreState(s);
}
