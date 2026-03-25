#include "pch.h"
#include "Genesis/Debugger/GenesisDisUtils.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/LabelManager.h"
#include "Debugger/MemoryDumper.h"
#include "Genesis/GenesisConsole.h"
#include "Shared/EmuSettings.h"
#include "Utilities/HexUtilities.h"

// ---------------------------------------------------------------------------
// Disassembly delegates to the active Genesis backend.
// Native backend uses the native 68K decoder.
// Fallback keeps a small "dc.w" representation if no backend is active.
// ---------------------------------------------------------------------------

void GenesisDisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	(void)labelManager;
	(void)settings;

	if(GenesisConsole* console = GenesisConsole::GetActiveConsole()) {
		const char* disassembly = console->DisassembleInstruction(memoryAddr);
		if(disassembly && disassembly[0]) {
			out = disassembly;
			return;
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
	(void)console;

	uint8_t* b = info.GetByteCode();
	if(info.GetOpSize() < 2) return {};

	uint16_t op     = (uint16_t)((b[0] << 8) | b[1]);
	uint8_t  grp    = (uint8_t)((op >> 12) & 0xF);
	uint32_t opAddr = state.PC & 0x00FFFFFFu;

	// Returns bytes of extension words consumed by an EA field (not counting immediate data)
	auto eaExtBytes = [](uint8_t mode, uint8_t reg) -> int {
		if(mode == 5) return 2;           // (d16,An)
		if(mode == 6) return 2;           // (d8,An,Xn) - brief extension word
		if(mode == 7) {
			if(reg == 0) return 2;        // (xxx).W
			if(reg == 1) return 4;        // (xxx).L
			if(reg == 2) return 2;        // (d16,PC)
			if(reg == 3) return 2;        // (d8,PC,Xn)
		}
		return 0;
	};

	// Compute effective address for a 6-bit EA field.
	// ext     = pointer into byteCode at first extension word for this EA.
	// pcBase  = address of that first extension word (used for PC-relative modes).
	// Returns -1 for register-direct (Dn/An) or immediate (no memory address).
	auto computeEA = [&](uint8_t mode, uint8_t reg, const uint8_t* ext, uint32_t pcBase) -> int64_t {
		switch(mode) {
			case 2: // (An)
				return (int64_t)(state.A[reg] & 0x00FFFFFFu);
			case 3: // (An)+
				return (int64_t)(state.A[reg] & 0x00FFFFFFu);
			case 5: { // (d16,An)
				int16_t d = (int16_t)((ext[0] << 8) | ext[1]);
				return (int64_t)((state.A[reg] + d) & 0x00FFFFFFu);
			}
			case 6: { // (d8,An,Xn) - brief extension word
				uint16_t bew  = (uint16_t)((ext[0] << 8) | ext[1]);
				int8_t   disp = (int8_t)(bew & 0xFF);
				uint8_t  xrn  = (uint8_t)((bew >> 12) & 0x7);
				bool     xrA  = (bew >> 15) & 1;
				bool     xrL  = (bew >> 11) & 1;
				int32_t  xrV  = xrA ? (int32_t)state.A[xrn] : (int32_t)state.D[xrn];
				if(!xrL) xrV  = (int32_t)(int16_t)(xrV & 0xFFFF);
				return (int64_t)((state.A[reg] + xrV + disp) & 0x00FFFFFFu);
			}
			case 7:
				switch(reg) {
					case 0: { // (xxx).W
						int16_t a = (int16_t)((ext[0] << 8) | ext[1]);
						return (int64_t)((uint32_t)(int32_t)a & 0x00FFFFFFu);
					}
					case 1: { // (xxx).L
						uint32_t a = ((uint32_t)ext[0] << 24) | ((uint32_t)ext[1] << 16) |
						             ((uint32_t)ext[2] << 8)  |  (uint32_t)ext[3];
						return (int64_t)(a & 0x00FFFFFFu);
					}
					case 2: { // (d16,PC)
						int16_t d = (int16_t)((ext[0] << 8) | ext[1]);
						return (int64_t)((pcBase + d) & 0x00FFFFFFu);
					}
					case 3: { // (d8,PC,Xn) - brief extension word
						uint16_t bew  = (uint16_t)((ext[0] << 8) | ext[1]);
						int8_t   disp = (int8_t)(bew & 0xFF);
						uint8_t  xrn  = (uint8_t)((bew >> 12) & 0x7);
						bool     xrA  = (bew >> 15) & 1;
						bool     xrL  = (bew >> 11) & 1;
						int32_t  xrV  = xrA ? (int32_t)state.A[xrn] : (int32_t)state.D[xrn];
						if(!xrL) xrV  = (int32_t)(int16_t)(xrV & 0xFFFF);
						return (int64_t)((pcBase + xrV + disp) & 0x00FFFFFFu);
					}
					default: return -1; // immediate (reg==4) or reserved
				}
			default: return -1; // Dn (mode 0), An (mode 1), -(An) (mode 4 - skip; size unknown here)
		}
	};

	// -----------------------------------------------------------------------
	// MOVE instructions (groups 1=.B, 2=.L, 3=.W):
	//   bits [5:0]  = source EA
	//   bits [11:6] = destination EA (mode at [8:6], reg at [11:9])
	//   extension words: source first, then destination
	// -----------------------------------------------------------------------
	if(grp >= 1 && grp <= 3) {
		uint8_t srcMode = (uint8_t)((op >> 3) & 0x7);
		uint8_t srcReg  = (uint8_t)(op & 0x7);
		uint8_t dstReg  = (uint8_t)((op >> 9) & 0x7);
		uint8_t dstMode = (uint8_t)((op >> 6) & 0x7);
		uint8_t sz = (grp == 1) ? 1 : (grp == 2) ? 4 : 2;

		// Source EA extension starts at b+2; PC base = opAddr+2
		int64_t srcAddr = computeEA(srcMode, srcReg, b + 2, opAddr + 2);
		if(srcAddr >= 0) return { srcAddr, sz, true, MemoryType::GenesisMemory };

		// Destination EA extension follows source extension
		int srcExt = eaExtBytes(srcMode, srcReg);
		// Immediate source: ext size = sz (but for byte, MOVE stores word-aligned: 2 bytes)
		if(srcMode == 7 && srcReg == 4) srcExt = (sz == 4) ? 4 : 2;

		int64_t dstAddr = computeEA(dstMode, dstReg, b + 2 + srcExt, opAddr + 2 + srcExt);
		if(dstAddr >= 0) return { dstAddr, sz, true, MemoryType::GenesisMemory };
		return {};
	}

	// -----------------------------------------------------------------------
	// Instructions with no interesting EA
	// -----------------------------------------------------------------------
	// BRA/BSR/Bcc (group 6), MOVEQ (group 7), A-line (group A), F-line (group F)
	if(grp == 6 || grp == 7 || grp == 0xA || grp == 0xF) return {};

	// Singleton opcodes that don't touch memory via an EA field
	if(op >= 0x4E70 && op <= 0x4E77) return {}; // RESET/NOP/STOP/RTE/RTD/RTS/TRAPV/RTR
	if((op & 0xFFF8) == 0x4840) return {};       // SWAP Dn
	if((op & 0xFEB8) == 0x4880) return {};       // EXT.W/EXT.L Dn
	if((op & 0xF1F8) == 0xC140) return {};       // EXG

	// MOVEM (0x48xx store, 0x4Cxx load): has a register-list word between opcode and EA ext.
	// The EA ext words therefore start at b+4, not b+2.
	if((op & 0xFB80) == 0x4880) {
		uint8_t eaMode = (uint8_t)((op >> 3) & 0x7);
		uint8_t eaReg  = (uint8_t)(op & 0x7);
		int64_t addr = computeEA(eaMode, eaReg, b + 4, opAddr + 4);
		if(addr >= 0) return { addr, 4, true, MemoryType::GenesisMemory };
		return {};
	}

	// -----------------------------------------------------------------------
	// General case: EA at bits [5:0], extension at b+2, PC base at opAddr+2
	// -----------------------------------------------------------------------
	uint8_t eaMode = (uint8_t)((op >> 3) & 0x7);
	uint8_t eaReg  = (uint8_t)(op & 0x7);

	// Determine value size for common instruction groups
	uint8_t sz = 2;
	if(grp == 0) {
		// Immediate ops (BTST/BCHG/BCLR/BSET have no size field at [7:6])
		// ORI/ANDI/SUBI/ADDI/EORI/CMPI: size at bits [7:6]
		uint8_t sub6 = (uint8_t)((op >> 6) & 0x3);
		sz = (sub6 == 0) ? 1 : (sub6 == 1) ? 2 : 4;
	} else if(grp == 4) {
		// Misc: CLR/NEG/NEGX/NOT/TST size at [7:6]
		uint8_t sub6 = (uint8_t)((op >> 6) & 0x3);
		sz = (sub6 == 0) ? 1 : (sub6 == 1) ? 2 : 4;
	} else if(grp == 5) {
		// ADDQ/SUBQ: size at [7:6]; Scc: always byte; DBcc: no EA mem
		uint8_t sub6 = (uint8_t)((op >> 6) & 0x3);
		if(sub6 == 3) return {}; // Scc/DBcc - no memory EA in the usual sense
		sz = (sub6 == 0) ? 1 : (sub6 == 1) ? 2 : 4;
	} else {
		// Groups 8-E (OR,SUB,CMP,AND,ADD,shifts): size at [7:6]
		uint8_t sub6 = (uint8_t)((op >> 6) & 0x3);
		sz = (sub6 == 0) ? 1 : (sub6 == 1) ? 2 : 4;
	}

	int64_t addr = computeEA(eaMode, eaReg, b + 2, opAddr + 2);
	if(addr >= 0) return { addr, sz, true, MemoryType::GenesisMemory };
	return {};
}

uint8_t GenesisDisUtils::GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper)
{
	(void)memType;
	(void)memoryDumper;

	if(GenesisConsole* console = GenesisConsole::GetActiveConsole()) {
		uint32_t size = console->GetInstructionSize(cpuAddress);
		if(size >= 2 && size <= 10 && (size & 1) == 0) {
			return (uint8_t)size;
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
