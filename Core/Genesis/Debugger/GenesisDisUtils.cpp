#include "pch.h"
#include "Genesis/Debugger/GenesisDisUtils.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/LabelManager.h"
#include "Debugger/MemoryDumper.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisAresCore.h"
#include "Shared/EmuSettings.h"
#include "Utilities/HexUtilities.h"

// ---------------------------------------------------------------------------
// Minimal M68000 disassembler stub.
// The M68000 has variable-length instructions (2-10 bytes, always even).
// All opcodes are 2 bytes (one word). For now we just output the hex bytes.
// A full disassembler can be added later.
// ---------------------------------------------------------------------------

void GenesisDisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	if(GenesisConsole* console = GenesisConsole::GetActiveConsole()) {
		if(GenesisAresImpl* impl = console->GetAresImpl()) {
			const char* disassembly = GenesisAresDisassembleInstruction(impl, memoryAddr);
			if(disassembly && disassembly[0]) {
				out = disassembly;
				return;
			}
		}
	}

	uint8_t* byteCode = info.GetByteCode();
	uint8_t opSize    = info.GetOpSize();

	// Build a hex dump of the instruction words
	out = "dc.w  $";
	out += HexUtilities::ToHex(byteCode[0]);
	out += HexUtilities::ToHex(byteCode[1]);
	if(opSize >= 4) {
		out += ",$";
		out += HexUtilities::ToHex(byteCode[2]);
		out += HexUtilities::ToHex(byteCode[3]);
	}
}

EffectiveAddressInfo GenesisDisUtils::GetEffectiveAddress(DisassemblyInfo& info, GenesisConsole* console, GenesisCpuState& state)
{
	return {};
}

uint8_t GenesisDisUtils::GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper)
{
	(void)memType;
	(void)memoryDumper;

	if(GenesisConsole* console = GenesisConsole::GetActiveConsole()) {
		if(GenesisAresImpl* impl = console->GetAresImpl()) {
			uint32_t size = GenesisAresGetInstructionSize(impl, cpuAddress);
			if(size >= 2 && size <= 10 && (size & 1) == 0) {
				return (uint8_t)size;
			}
		}
	}

	return 2;
}

bool GenesisDisUtils::IsJumpToSub(uint32_t opCode)
{
	uint16_t word = (uint16_t)opCode;
	uint8_t hi = (uint8_t)(word >> 8);
	uint8_t lo = (uint8_t)(word & 0xFF);

	// BSR (branch to subroutine): 0x61xx
	// JSR (jump to subroutine):   0x4E80..0x4EBF
	if(hi == 0x61) return true;  // BSR short/long
	if(hi == 0x4E && (lo & 0xC0) == 0x80) return true;  // JSR
	return false;
}

bool GenesisDisUtils::IsReturnInstruction(uint32_t opCode)
{
	uint16_t word = (uint16_t)opCode;
	uint8_t hi = (uint8_t)(word >> 8);
	uint8_t lo = (uint8_t)(word & 0xFF);

	// RTS = 0x4E75, RTR = 0x4E77, RTE = 0x4E73
	if(hi == 0x4E && (lo == 0x75 || lo == 0x77 || lo == 0x73)) return true;
	return false;
}

bool GenesisDisUtils::IsUnconditionalJump(uint32_t opCode)
{
	uint16_t word = (uint16_t)opCode;
	uint8_t hi = (uint8_t)(word >> 8);
	uint8_t lo = (uint8_t)(word & 0xFF);

	// JMP = 0x4EC0..0x4EFF, BRA = 0x60xx
	if(hi == 0x60) return true;  // BRA
	if(hi == 0x4E && (lo & 0xC0) == 0xC0) return true;  // JMP
	return false;
}

bool GenesisDisUtils::IsConditionalJump(uint32_t opCode)
{
	uint16_t word = (uint16_t)opCode;
	uint8_t hi = (uint8_t)(word >> 8);
	uint8_t lo = (uint8_t)(word & 0xFF);

	// Bcc = 0x62xx-0x6Fxx (all branch on condition codes except BRA=0x60 and BSR=0x61)
	if(hi >= 0x62 && hi <= 0x6F) return true;
	// DBcc = 0x50C8..0x5FC8
	if(hi >= 0x50 && hi <= 0x5F && lo == 0xC8) return true;
	return false;
}
