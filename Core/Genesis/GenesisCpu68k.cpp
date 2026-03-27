// GenesisCpu68k.cpp — Native Motorola M68000 interpreter
//
// Coverage: complete M68000 instruction set with Mesen debugger hooks.
// Cycle counts are approximate, with coarse bus wait-state penalties from backend.
// ===========================================================================

#include "pch.h"
#include "Genesis/GenesisCpu68k.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Shared/MemoryOperationType.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/HexUtilities.h"

// ===========================================================================
// Static data
// ===========================================================================

GenesisCpu68k::OpHandler GenesisCpu68k::_opTable[256] = {};

// ===========================================================================
// Init / Reset
// ===========================================================================

void GenesisCpu68k::Init(Emulator* emu, GenesisNativeBackend* backend)
{
	_emu     = emu;
	_backend = backend;
}

void GenesisCpu68k::Reset(uint32_t initialSSP, uint32_t initialPC)
{
	_state          = {};
	_state.A[7]     = initialSSP;
	_state.SP       = initialSSP;
	_state.PC       = initialPC & 0x00FFFFFFu;
	_state.SR       = SR_S | (7u << 8);   // supervisor mode, IPL=7
	_usp            = 0;
	_pendingIrq     = 0;
	_faultFramePending = false;
	_faultAddress      = 0;
	_faultSavedPC      = 0;
	_faultStatusWord   = 0;
	_faultInstructionWord = 0;
	_faultStatusExtra = 0;
	_faultPcOverride = 0;
	_faultPcOverrideValid = false;
	_exceptionPcOverride = 0;
	_exceptionPcOverrideValid = false;
	_faultAddressOverride = 0;
	_faultAddressOverrideValid = false;
	_faultPreserveSr = false;
	_faultRestoreSrXOnly = false;
	_faultRestoreSrMask = 0;
	_faultPreserveAddressRegs = false;
	_faultStatusWordPreDecrement = false;
	_faultPreDecPCOffset = 0;
	_cycles         = 0;
	_state.Stopped  = false;
#ifdef _DEBUG
	_debugIllegalCount = 0;
#endif
}

// ===========================================================================
// Run / Exec
// ===========================================================================

int32_t GenesisCpu68k::Run(int32_t targetCycles)
{
	_cycles = 0;
	bool dbg = _emu && _emu->IsDebugging();

	// When halted by STOP, keep checking for interrupts each scanline quantum.
	// CheckInterrupts() calls TakeException() which clears _state.Stopped, so
	// once a pending IRQ arrives the normal execution loop continues below.
	if(_state.Stopped) {
		CheckInterrupts();
		if(_state.Stopped) {
			// Still stopped — consume the time slice and return.
			_state.CycleCount += (uint64_t)targetCycles;
			return targetCycles;
		}
	}

	while(_cycles < targetCycles && !_state.Stopped) {
		CheckInterrupts();
		if(_state.Stopped) break;
		if(dbg) Exec<true>();
		else    Exec<false>();
	}
	_state.CycleCount += (uint64_t)_cycles;
	return _cycles;
}

template<bool debuggerEnabled>
void GenesisCpu68k::Exec()
{
	_exceptionTaken = false;
	uint32_t pc = _state.PC & 0x00FFFFFFu;
	_preExecPC  = pc;
	_preExecSR  = _state.SR;
	memcpy(_preExecA, _state.A, sizeof(_preExecA));
	_faultPcOverrideValid = false;
	_exceptionPcOverrideValid = false;
	_faultAddressOverrideValid = false;
	_faultStatusExtra = 0;
	_faultPreserveSr = false;
	_faultRestoreSrXOnly = false;
	_faultRestoreSrMask = 0;
	_faultPreserveAddressRegs = false;
	_faultIsPcRelativeEA = false;
	_faultPC4ForSourceRead = false;
	_faultStatusWordPreDecrement = false;
	_faultPreDecPCOffset = 0;
#ifdef _DEBUG
	if(!_suppressWrites) {
		static bool sHit020C = false;
		static bool sHit0210 = false;
		static bool sHit0292 = false;
		static bool sHit0400 = false;
		static bool sHit0448 = false;
		static bool sHit051A = false;
		switch(pc) {
			case 0x00020Cu: if(!sHit020C) { LogDebug("[MD Native][68K] Hit PC=$00020C"); sHit020C = true; } break;
			case 0x000210u: if(!sHit0210) { LogDebug("[MD Native][68K] Hit PC=$000210"); sHit0210 = true; } break;
			case 0x000292u: if(!sHit0292) { LogDebug("[MD Native][68K] Hit PC=$000292"); sHit0292 = true; } break;
			case 0x000400u: if(!sHit0400) { LogDebug("[MD Native][68K] Hit PC=$000400"); sHit0400 = true; } break;
			case 0x000448u: if(!sHit0448) { LogDebug("[MD Native][68K] Hit PC=$000448"); sHit0448 = true; } break;
			case 0x00051Au: if(!sHit051A) { LogDebug("[MD Native][68K] Hit PC=$00051A"); sHit051A = true; } break;
			default: break;
		}
	}
#endif
	_opword = FetchOpcode();
	if(_exceptionTaken) {
		return;
	}
#ifdef _DEBUG
	if(!_suppressWrites) {
		static uint32_t sExecLogCount = 0;
		if(sExecLogCount < 256) {
			LogDebug("[MD Native][68K] PC=$" + HexUtilities::ToHex24((int32_t)pc) +
				" OP=$" + HexUtilities::ToHex(_opword));
			sExecLogCount++;
		}
	}
#endif
	if constexpr(debuggerEnabled) {
		_emu->ProcessInstruction<CpuType::GenesisMain>();
	}
	(this->*_opTable[_opword >> 8])();
	if(_exceptionTaken) {
		return;
	}
}

template void GenesisCpu68k::Exec<true>();
template void GenesisCpu68k::Exec<false>();

uint8_t GenesisCpu68k::DecodeInstructionSize(uint32_t address)
{
	GenesisCpuState savedState = _state;
	uint32_t savedUsp = _usp;
	uint8_t savedPendingIrq = _pendingIrq;
	uint16_t savedOpWord = _opword;
	int32_t savedCycles = _cycles;
	bool savedSuppressWrites = _suppressWrites;
	uint8_t savedFetchBytes = _fetchBytes;
	bool savedExceptionTaken = _exceptionTaken;
	uint32_t savedPreExecPc = _preExecPC;

	_state.PC = address & 0x00FFFFFFu;
	_state.Stopped = false;
	_cycles = 0;
	_fetchBytes = 0;
	_suppressWrites = true;
	Exec<false>();

	uint8_t size = _fetchBytes;
	if(size < 2 || size > 10 || (size & 1u) != 0) {
		size = 2;
	}

	_state = savedState;
	_usp = savedUsp;
	_pendingIrq = savedPendingIrq;
	_opword = savedOpWord;
	_cycles = savedCycles;
	_suppressWrites = savedSuppressWrites;
	_fetchBytes = savedFetchBytes;
	_exceptionTaken = savedExceptionTaken;
	_preExecPC = savedPreExecPc;

	return size;
}

// ===========================================================================
// Bus access
// ===========================================================================

bool GenesisCpu68k::CheckAddressError(uint32_t addr, bool isWrite, uint8_t sizeBytes, MemoryOperationType opType)
{
	if(_suppressWrites || _exceptionTaken || sizeBytes < 2 || (addr & 1u) == 0u) {
		return false;
	}

#ifdef _DEBUG
	static uint32_t sAddressErrorLogCount = 0;
	if(sAddressErrorLogCount < 64) {
		LogDebug(string("[MD Native][68K] Address error")
			+ " op=" + (isWrite ? "W" : "R")
			+ std::to_string(sizeBytes * 8)
			+ " addr=$" + HexUtilities::ToHex24((int32_t)(addr & 0x00FFFFFFu))
			+ " pc=$" + HexUtilities::ToHex24((int32_t)(_state.PC & 0x00FFFFFFu))
			+ " prepc=$" + HexUtilities::ToHex24((int32_t)_preExecPC)
			+ " opword=$" + HexUtilities::ToHex(_opword)
			+ " type=" + std::to_string((int)opType));
		sAddressErrorLogCount++;
	}
#endif

	_faultFramePending   = true;
	_faultAddress        = _faultAddressOverrideValid ? _faultAddressOverride : addr;
	_faultSavedPC        = _faultPcOverrideValid ? (_faultPcOverride & 0x00FFFFFFu) : ((_preExecPC + (_faultPC4ForSourceRead ? 4u : 2u)) & 0x00FFFFFFu);
	uint16_t fc = 0;
	if(opType == MemoryOperationType::ExecOpCode || opType == MemoryOperationType::ExecOperand || _faultIsPcRelativeEA) {
		fc = GetS() ? 6u : 2u;
	} else {
		fc = GetS() ? 5u : 1u;
	}
	// For -(An) destination write faults the pre-decrement internal cycle shifts
	// the pipeline one extra step.  Both the IR and the SSW reflect the prefetched
	// IRC word at _state.PC, with only the low FC bits overridden in the SSW.
	// For all other fault types the pipeline has not advanced:
	//   IR  = the executing opcode
	//   SSW = opcode bits[15:5] preserved, FC in bits[2:0], R/W in bit4.
	if(_faultStatusWordPreDecrement) {
		// For -(An) dest, the pre-decrement internal cycle shifts the 68000 pipeline.
		// IR captures the prefetched IRC word.  The SSW keeps the IRC opcode-like
		// bits [15:5] and replaces the low FC bits with the actual cycle space.
		uint32_t ircAddr = (_state.PC + _faultPreDecPCOffset) & 0x00FFFFFFu;
		_faultInstructionWord = BusRead16(ircAddr, MemoryOperationType::ExecOperand);
		_faultStatusWord = (uint16_t)((_faultInstructionWord & 0xFFE0u) | (fc & 7u));
	} else {
		_faultInstructionWord = _opword;
		_faultStatusWord = (uint16_t)(_opword & 0xFFE0u);
		if(!isWrite) { _faultStatusWord |= 0x0010u; }
		_faultStatusWord |= fc;
	}
	_faultStatusWord |= _faultStatusExtra;

	// Address errors do not commit the in-flight instruction's CCR updates.
	// The sampled corpus expects longword (+4) postincrements to roll back on a
	// fault, while word (+2) postincrements remain committed.
	if(_faultRestoreSrXOnly) {
		_state.SR = (uint16_t)((_state.SR & 0xFFE0u) | (_preExecSR & SR_X));
	} else if(_faultRestoreSrMask) {
		_state.SR = (uint16_t)((_state.SR & ~_faultRestoreSrMask) | (_preExecSR & _faultRestoreSrMask));
	} else if(!_faultPreserveSr) {
		_state.SR = _preExecSR;
	}
	if(!_faultPreserveAddressRegs) {
		for(uint8_t i = 0; i < 8; i++) {
			uint32_t delta = _state.A[i] - _preExecA[i];
			if(_state.A[i] > _preExecA[i] && delta == 4) {
				_state.A[i] = _preExecA[i];
				if(i == 7) {
					_state.SP = _state.A[7];
				}
			}
		}
	}

	TakeException(3);
	return true;
}

uint8_t GenesisCpu68k::BusRead8(uint32_t addr, MemoryOperationType opType)
{
	// DecodeInstructionSize runs Exec() with _suppressWrites=true. In that mode,
	// avoid non-fetch bus reads entirely because they can trigger real device side
	// effects (VDP/input status reads) and perturb emulation state.
	if(_suppressWrites &&
	   opType != MemoryOperationType::ExecOpCode &&
	   opType != MemoryOperationType::ExecOperand)
	{
		return 0;
	}

	addr &= 0x00FFFFFFu;
	if(!_wordAccessActive) {
		_cycles += _backend->CpuBusWaitStates(addr, false);
	}
	uint8_t value = _backend->CpuBusRead8(addr);
	if(_suppressWrites) {
		if(opType == MemoryOperationType::ExecOpCode || opType == MemoryOperationType::ExecOperand) {
			_fetchBytes++;
		}
	} else if(_emu) {
		_emu->ProcessMemoryRead<CpuType::GenesisMain>(addr, value, opType);
	}
	return value;
}

uint16_t GenesisCpu68k::BusRead16(uint32_t addr, MemoryOperationType opType)
{
	if(CheckAddressError(addr, false, 2, opType)) {
		return 0;
	}
	addr &= 0x00FFFFFFu;
	// Apply wait states once for the word-wide bus cycle (not per byte).
	_cycles += _backend->CpuBusWaitStates(addr, false);
	_wordAccessActive = true;
	uint16_t hi = BusRead8(addr,     opType);
	uint16_t lo = BusRead8(addr + 1, opType);
	_wordAccessActive = false;
	return (hi << 8) | lo;
}

uint32_t GenesisCpu68k::BusRead32(uint32_t addr, MemoryOperationType opType)
{
	if(CheckAddressError(addr, false, 4, opType)) {
		return 0;
	}
	addr &= 0x00FFFFFFu;
	uint32_t hi = BusRead16(addr,     opType);
	uint32_t lo = BusRead16(addr + 2, opType);
	return (hi << 16) | lo;
}

void GenesisCpu68k::BusWrite8(uint32_t addr, uint8_t value)
{
	if(_suppressWrites) {
		return;
	}

	addr &= 0x00FFFFFFu;
	if(!_wordAccessActive) {
		_cycles += _backend->CpuBusWaitStates(addr, true);
	}
	if(_emu) {
		if(!_emu->ProcessMemoryWrite<CpuType::GenesisMain>(addr, value, MemoryOperationType::Write))
			return;
	}
	_backend->CpuBusWrite8(addr, value);
}

void GenesisCpu68k::BusWrite16(uint32_t addr, uint16_t value)
{
	if(CheckAddressError(addr, true, 2, MemoryOperationType::Write)) {
		return;
	}
	addr &= 0x00FFFFFFu;
	// Apply wait states once for the word-wide bus cycle (not per byte).
	_cycles += _backend->CpuBusWaitStates(addr, true);
	_wordAccessActive = true;
	BusWrite8(addr,     (uint8_t)(value >> 8));
	BusWrite8(addr + 1, (uint8_t)(value));
	_wordAccessActive = false;
}

void GenesisCpu68k::BusWrite32(uint32_t addr, uint32_t value)
{
	if(CheckAddressError(addr, true, 4, MemoryOperationType::Write)) {
		return;
	}
	addr &= 0x00FFFFFFu;
	BusWrite16(addr,     (uint16_t)(value >> 16));
	BusWrite16(addr + 2, (uint16_t)(value));
}

// ===========================================================================
// PC-relative fetches
// ===========================================================================

uint16_t GenesisCpu68k::FetchOpcode()
{
	uint16_t v = BusRead16(_state.PC, MemoryOperationType::ExecOpCode);
	if(_exceptionTaken) {
		return 0;
	}
	_state.PC = (_state.PC + 2) & 0x00FFFFFFu;
	_cycles += 4;
	return v;
}

uint16_t GenesisCpu68k::FetchExtWord()
{
	uint16_t v = BusRead16(_state.PC, MemoryOperationType::ExecOperand);
	if(_exceptionTaken) {
		return 0;
	}
	_state.PC = (_state.PC + 2) & 0x00FFFFFFu;
	_cycles += 4;
	return v;
}

uint32_t GenesisCpu68k::FetchExtLong()
{
	uint32_t hi = FetchExtWord();
	if(_exceptionTaken) {
		return 0;
	}
	uint32_t lo = FetchExtWord();
	if(_exceptionTaken) {
		return 0;
	}
	return (hi << 16) | lo;
}

// ===========================================================================
// Effective address calculation
// ===========================================================================

uint32_t GenesisCpu68k::CalcEA(uint8_t mode, uint8_t reg, uint8_t size)
{
	_faultIsPcRelativeEA = false;
	switch(mode) {
		case 0: return EA_DN + reg;
		case 1: return EA_AN + reg;

		case 2:
			_cycles += 4;
			return _state.A[reg];

		case 3: {
			uint32_t addr = _state.A[reg];
			uint32_t inc  = (reg == 7 && size == 1) ? 2u : (uint32_t)size;
			_state.A[reg] += inc;
			if(reg == 7) _state.SP = _state.A[7];
			_cycles += 4;
			return addr;
		}

		case 4: {
			uint32_t dec = (reg == 7 && size == 1) ? 2u : (uint32_t)size;
			_state.A[reg] -= dec;
			if(reg == 7) _state.SP = _state.A[7];
			_cycles += 6;
			return _state.A[reg];
		}

		case 5: {
			int16_t disp = (int16_t)FetchExtWord();
			_cycles += 4;
			return _state.A[reg] + (int32_t)disp;
		}

		case 6: {
			uint16_t ext  = FetchExtWord();
			int8_t   d8   = (int8_t)(ext & 0xFF);
			uint8_t  xn   = (ext >> 12) & 7;
			bool     xl   = (ext >> 11) & 1;
			bool     isAn = (ext >> 15) & 1;
			uint32_t xval = isAn ? _state.A[xn] : _state.D[xn];
			int32_t  xi   = xl ? (int32_t)xval : (int32_t)(int16_t)(uint16_t)xval;
			_cycles += 6;
			return _state.A[reg] + xi + d8;
		}

		case 7:
			switch(reg) {
				case 0: {
					int16_t a = (int16_t)FetchExtWord();
					_cycles += 4;
					return (uint32_t)(int32_t)a;
				}
				case 1: {
					uint32_t a = FetchExtLong();
					// Extension fetch already covers the absolute-long operand timing.
					return a;
				}
				case 2: {
					// d16(PC): base is the PC at the extension word address.
					_faultIsPcRelativeEA = true;
					uint32_t base = _state.PC;
					int16_t  disp = (int16_t)FetchExtWord();
					_cycles += 4;
					return base + (int32_t)disp;
				}
				case 3: {
					// For d8(PC,Xn), base is the PC at the extension word address.
					// MODE_PC_INDEX_DISP8 path (inst->address + 2).
					_faultIsPcRelativeEA = true;
					uint32_t base = _state.PC;
					uint16_t ext  = FetchExtWord();
					int8_t   d8   = (int8_t)(ext & 0xFF);
					uint8_t  xn   = (ext >> 12) & 7;
					bool     xl   = (ext >> 11) & 1;
					bool     isAn = (ext >> 15) & 1;
					uint32_t xval = isAn ? _state.A[xn] : _state.D[xn];
					int32_t  xi   = xl ? (int32_t)xval : (int32_t)(int16_t)(uint16_t)xval;
					_cycles += 6;
					return base + xi + d8;
				}
				case 4:
					return 0xFFFFFFFFu;  // immediate sentinel — handled by ReadEA*
				default: break;
			}
			break;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// ReadEA / WriteEA
// ---------------------------------------------------------------------------

uint8_t GenesisCpu68k::ReadEA8(uint8_t mode, uint8_t reg)
{
	if(mode == 7 && reg == 4) return (uint8_t)(FetchExtWord() & 0xFF);
	return ReadResolvedEA8(CalcEA(mode, reg, 1));
}

uint16_t GenesisCpu68k::ReadEA16(uint8_t mode, uint8_t reg)
{
	if(mode == 7 && reg == 4) return FetchExtWord();
	uint32_t ea = CalcEA(mode, reg, 2);
	return ReadResolvedEA16(ea);
}

uint32_t GenesisCpu68k::ReadEA32(uint8_t mode, uint8_t reg)
{
	if(mode == 7 && reg == 4) return FetchExtLong();
	uint32_t ea = CalcEA(mode, reg, 4);
	return ReadResolvedEA32(ea);
}

void GenesisCpu68k::WriteEA8(uint8_t mode, uint8_t reg, uint8_t v)
{
	WriteResolvedEA8(CalcEA(mode, reg, 1), v);
}

void GenesisCpu68k::WriteEA16(uint8_t mode, uint8_t reg, uint16_t v)
{
	WriteResolvedEA16(CalcEA(mode, reg, 2), v);
}

void GenesisCpu68k::WriteEA32(uint8_t mode, uint8_t reg, uint32_t v)
{
	WriteResolvedEA32(CalcEA(mode, reg, 4), v);
}

uint8_t GenesisCpu68k::ReadResolvedEA8(uint32_t ea)
{
	if(ea >= EA_DN && ea < (EA_AN + 8u)) {
		if(ea < (EA_DN + 8u)) return (uint8_t)_state.D[ea - EA_DN];
		if(ea < (EA_AN + 8u)) return (uint8_t)_state.A[ea - EA_AN];
		return 0;
	}
	return BusRead8(ea);
}

uint16_t GenesisCpu68k::ReadResolvedEA16(uint32_t ea)
{
	if(ea >= EA_DN && ea < (EA_AN + 8u)) {
		if(ea < (EA_DN + 8u)) return (uint16_t)_state.D[ea - EA_DN];
		if(ea < (EA_AN + 8u)) return (uint16_t)_state.A[ea - EA_AN];
		return 0;
	}
	return BusRead16(ea);
}

uint32_t GenesisCpu68k::ReadResolvedEA32(uint32_t ea)
{
	if(ea >= EA_DN && ea < (EA_AN + 8u)) {
		if(ea < (EA_DN + 8u)) return _state.D[ea - EA_DN];
		if(ea < (EA_AN + 8u)) return _state.A[ea - EA_AN];
		return 0;
	}
	return BusRead32(ea);
}

void GenesisCpu68k::WriteResolvedEA8(uint32_t ea, uint8_t v)
{
	if(ea >= EA_DN && ea < (EA_AN + 8u)) {
		if(ea < (EA_DN + 8u)) {
			_state.D[ea - EA_DN] = (_state.D[ea - EA_DN] & 0xFFFFFF00u) | v;
			return;
		}
		if(ea < (EA_AN + 8u)) {
			uint8_t an = (uint8_t)(ea - EA_AN);
			_state.A[an] = (uint32_t)(int8_t)v;  // address reg: sign-extend
			if(an == 7) _state.SP = _state.A[7];
		}
		return;
	}
	BusWrite8(ea, v);
}

void GenesisCpu68k::WriteResolvedEA16(uint32_t ea, uint16_t v)
{
	if(ea >= EA_DN && ea < (EA_AN + 8u)) {
		if(ea < (EA_DN + 8u)) {
			_state.D[ea - EA_DN] = (_state.D[ea - EA_DN] & 0xFFFF0000u) | v;
			return;
		}
		if(ea < (EA_AN + 8u)) {
			uint8_t an = (uint8_t)(ea - EA_AN);
			_state.A[an] = (uint32_t)(int16_t)v;  // sign-extend to 32-bit
			if(an == 7) _state.SP = _state.A[7];
		}
		return;
	}
	BusWrite16(ea, v);
}

void GenesisCpu68k::WriteResolvedEA32(uint32_t ea, uint32_t v)
{
	if(ea >= EA_DN && ea < (EA_AN + 8u)) {
		if(ea < (EA_DN + 8u)) {
			_state.D[ea - EA_DN] = v;
			return;
		}
		if(ea < (EA_AN + 8u)) {
			uint8_t an = (uint8_t)(ea - EA_AN);
			_state.A[an] = v;
			if(an == 7) _state.SP = v;
		}
		return;
	}
	BusWrite32(ea, v);
}

// ===========================================================================
// SR / CCR
// ===========================================================================

void GenesisCpu68k::SetSR(uint16_t value)
{
	bool wasS = GetS();
	bool nowS = (value & SR_S) != 0;
	if(wasS != nowS) {
		// Swap active A7 with the inactive stack pointer bank.
		// - In supervisor mode: A7=SSP, _usp=USP
		// - In user mode:       A7=USP, _usp=SSP
		uint32_t oldA7 = _state.A[7];
		_state.A[7] = _usp;
		_usp = oldA7;
		_state.SP = _state.A[7];
	}
	_state.SR = value & 0xA71Fu;  // mask reserved bits
}

bool GenesisCpu68k::TestCC(uint8_t cc) const
{
	bool c = GetC(), v = GetV(), z = GetZ(), n = GetN();
	switch(cc & 0xF) {
		case 0:  return true;           // T  (always true)
		case 1:  return false;          // F  (always false)
		case 2:  return !c && !z;       // HI
		case 3:  return c || z;         // LS
		case 4:  return !c;             // CC (carry clear)
		case 5:  return c;              // CS (carry set)
		case 6:  return !z;             // NE
		case 7:  return z;              // EQ
		case 8:  return !v;             // VC (overflow clear)
		case 9:  return v;              // VS (overflow set)
		case 10: return !n;             // PL
		case 11: return n;              // MI
		case 12: return n == v;         // GE
		case 13: return n != v;         // LT
		case 14: return (n == v) && !z; // GT
		case 15: return (n != v) || z;  // LE
	}
	return false;
}

// ===========================================================================
// Stack
// ===========================================================================

void GenesisCpu68k::Push16(uint16_t v)
{
	_state.A[7] -= 2;
	_state.SP    = _state.A[7];
	BusWrite16(_state.A[7], v);
}

void GenesisCpu68k::Push32(uint32_t v)
{
	_state.A[7] -= 4;
	_state.SP    = _state.A[7];
	BusWrite32(_state.A[7], v);
}

uint16_t GenesisCpu68k::Pull16()
{
	uint16_t v = BusRead16(_state.A[7]);
	_state.A[7] += 2;
	_state.SP    = _state.A[7];
	return v;
}

uint32_t GenesisCpu68k::Pull32()
{
	uint32_t v = BusRead32(_state.A[7]);
	_state.A[7] += 4;
	_state.SP    = _state.A[7];
	return v;
}

// ===========================================================================
// Exception / interrupt
// ===========================================================================

void GenesisCpu68k::TakeException(uint8_t vector, uint8_t newIPL)
{
	_exceptionTaken = true;

	// Save SR and PC *before* any modification so the correct values are
	// pushed onto the supervisor stack (M68K architecture requirement).
	// Most instruction exceptions use the opcode address (_preExecPC).
	// Address/bus errors on 68000 save the prefetched second word address:
	// opcode PC + 2, regardless of how many extension words the interpreter
	// may already have consumed while resolving the EA.
	// Interrupts and TRAP instructions use the next PC in _state.PC.
	uint16_t oldSR = _state.SR;
	uint32_t oldPC = _state.PC;
	if(newIPL == 0) {
		if(vector == 2 || vector == 3) {
			oldPC = _faultFramePending ? _faultSavedPC : ((_preExecPC + 2) & 0x00FFFFFFu);
		} else if(_exceptionPcOverrideValid) {
			oldPC = _exceptionPcOverride & 0x00FFFFFFu;
		} else if(vector < 32) {
			oldPC = _preExecPC;
		}
	}

	// If this is an interrupt (newIPL > 0), raise the IPL mask in SR now.
	if(newIPL > 0) {
		_state.SR = (_state.SR & ~SR_I_MASK) | ((uint16_t)newIPL << 8);
	}

	// Enter supervisor, clear trace
	if(!GetS()) {
		// Switch to supervisor stack
		uint32_t usp = _state.A[7];
		_state.A[7] = _usp;
		_usp = usp;
		_state.SP = _state.A[7];
	}
	_state.SR |= SR_S;
	_state.SR &= ~SR_T;

	// Push exception frame (using the *pre-exception* values).
	// Bus/address errors (vectors 2/3) require a special 14-byte frame on 68000:
	//   [SSW][fault address][instruction word][SR][PC]
	// Sonic's error handler depends on this exact layout.
	if(newIPL == 0 && (vector == 2 || vector == 3)) {
		uint16_t statusWord = _faultFramePending ? _faultStatusWord : 0;
		uint32_t faultAddr  = _faultFramePending ? _faultAddress : 0;
		uint16_t irWord     = _faultFramePending ? _faultInstructionWord : _opword;

		Push32(oldPC);
		Push16(oldSR);
		Push16(irWord);
		Push32(faultAddr);
		Push16(statusWord);
	} else {
		Push32(oldPC);
		Push16(oldSR);
	}
	_faultFramePending = false;

	// Fetch vector
	uint32_t vecAddr = (uint32_t)vector * 4u;
	_state.PC = BusRead32(vecAddr) & 0x00FFFFFFu;

	_state.Stopped = false;
	_cycles += 44;

#ifdef _DEBUG
	if((vector == 2 || vector == 3 || vector == 10 || vector == 11) && !_suppressWrites) {
		static uint32_t sTrapLogCount = 0;
		if(sTrapLogCount < 128) {
			LogDebug("[MD Native][68K] Exception vec=" + std::to_string(vector) +
				" op=$" + HexUtilities::ToHex(_opword) +
				" oldPC=$" + HexUtilities::ToHex24((int32_t)oldPC) +
				" newPC=$" + HexUtilities::ToHex24((int32_t)_state.PC) +
				" preExecPC=$" + HexUtilities::ToHex24((int32_t)_preExecPC) +
				" SR=$" + HexUtilities::ToHex(oldSR) +
				" SP=$" + HexUtilities::ToHex32(_state.SP) +
				" A7=$" + HexUtilities::ToHex32(_state.A[7]) +
				" USP=$" + HexUtilities::ToHex32(_usp));
			sTrapLogCount++;
		}
	}

	if(vector == 4 && !_suppressWrites) {
		_debugIllegalCount++;
		// Keep logs bounded; first few are enough to diagnose boot loops.
		if(_debugIllegalCount <= 64) {
			LogDebug("[MD Native][68K] Illegal op #" + std::to_string(_debugIllegalCount) +
				" op=$" + HexUtilities::ToHex(_opword) +
				" pc=$" + HexUtilities::ToHex24((int32_t)oldPC) +
				" -> vec4=$" + HexUtilities::ToHex24((int32_t)_state.PC));
		}
	}
#endif

	if(!_suppressWrites && _emu) {
		_emu->ProcessInterrupt<CpuType::GenesisMain>(oldPC, _state.PC, vector == 7 /*NMI*/);
	}
}

void GenesisCpu68k::CheckInterrupts()
{
	if(_pendingIrq == 0) return;
	if(_pendingIrq == 7 || _pendingIrq > GetIPL()) {
		// Acknowledge interrupt. Pass newIPL to TakeException so it raises
		// the IPL *after* saving the original SR to the stack frame.
		uint8_t level = _pendingIrq;
		if(_backend && (level == 4 || level == 6)) {
			_backend->VdpInterruptAcknowledge();
		}
		_pendingIrq   = 0;
		// Autovector: level 1-7 → vector 25-31
		TakeException((uint8_t)(24 + level), level);
	}
}

// ===========================================================================
// Arithmetic helpers
// ===========================================================================

uint8_t GenesisCpu68k::Add8(uint8_t a, uint8_t b, bool xin, bool& carry, bool& overflow)
{
	uint16_t r = (uint16_t)a + (uint16_t)b + (uint16_t)(xin ? 1 : 0);
	carry    = (r > 0xFF);
	overflow = (~(a ^ b) & ((uint8_t)r ^ a)) >> 7;
	return (uint8_t)r;
}

uint16_t GenesisCpu68k::Add16(uint16_t a, uint16_t b, bool xin, bool& carry, bool& overflow)
{
	uint32_t r = (uint32_t)a + (uint32_t)b + (uint32_t)(xin ? 1 : 0);
	carry    = (r > 0xFFFF);
	overflow = (~(a ^ b) & ((uint16_t)r ^ a)) >> 15;
	return (uint16_t)r;
}

uint32_t GenesisCpu68k::Add32(uint32_t a, uint32_t b, bool xin, bool& carry, bool& overflow)
{
	uint64_t r = (uint64_t)a + (uint64_t)b + (uint64_t)(xin ? 1 : 0);
	carry    = (r > 0xFFFFFFFF);
	overflow = (~(a ^ b) & ((uint32_t)r ^ a)) >> 31;
	return (uint32_t)r;
}

uint8_t GenesisCpu68k::Sub8(uint8_t a, uint8_t b, bool xin, bool& carry, bool& overflow)
{
	uint16_t r = (uint16_t)a - (uint16_t)b - (uint16_t)(xin ? 1 : 0);
	carry    = (r > 0xFF);
	overflow = ((a ^ b) & (a ^ (uint8_t)r)) >> 7;
	return (uint8_t)r;
}

uint16_t GenesisCpu68k::Sub16(uint16_t a, uint16_t b, bool xin, bool& carry, bool& overflow)
{
	uint32_t r = (uint32_t)a - (uint32_t)b - (uint32_t)(xin ? 1 : 0);
	carry    = (r > 0xFFFF);
	overflow = ((a ^ b) & (a ^ (uint16_t)r)) >> 15;
	return (uint16_t)r;
}

uint32_t GenesisCpu68k::Sub32(uint32_t a, uint32_t b, bool xin, bool& carry, bool& overflow)
{
	uint64_t r = (uint64_t)a - (uint64_t)b - (uint64_t)(xin ? 1 : 0);
	carry    = (r > 0xFFFFFFFFULL);
	overflow = ((a ^ b) & (a ^ (uint32_t)r)) >> 31;
	return (uint32_t)r;
}

// ===========================================================================
// Shift helpers
// ===========================================================================

uint8_t GenesisCpu68k::Asl8(uint8_t v, uint8_t n, bool& carry, bool& overflow)
{
	if(n == 0) { carry = false; overflow = false; return v; }
	overflow = false;
	for(uint8_t i = 0; i < n; i++) {
		uint8_t prev = v;
		carry = (v & 0x80) != 0;
		v <<= 1;
		overflow |= ((prev ^ v) & 0x80) != 0;
	}
	return v;
}

uint16_t GenesisCpu68k::Asl16(uint16_t v, uint8_t n, bool& carry, bool& overflow)
{
	if(n == 0) { carry = false; overflow = false; return v; }
	overflow = false;
	for(uint8_t i = 0; i < n; i++) {
		uint16_t prev = v;
		carry = (v & 0x8000) != 0;
		v <<= 1;
		overflow |= ((prev ^ v) & 0x8000) != 0;
	}
	return v;
}

uint32_t GenesisCpu68k::Asl32(uint32_t v, uint8_t n, bool& carry, bool& overflow)
{
	if(n == 0) { carry = false; overflow = false; return v; }
	overflow = false;
	for(uint8_t i = 0; i < n; i++) {
		uint32_t prev = v;
		carry = (v & 0x80000000u) != 0;
		v <<= 1;
		overflow |= ((prev ^ v) & 0x80000000u) != 0;
	}
	return v;
}

uint8_t GenesisCpu68k::Asr8(uint8_t v, uint8_t n, bool& carry, bool& overflow)
{
	if(n == 0) { carry = false; overflow = false; return v; }
	carry    = (n <= 8) ? ((v >> (n - 1)) & 1) != 0 : (v >> 7) != 0;
	overflow = false;
	if(n >= 8) return (uint8_t)((int8_t)v >> 7);
	return (uint8_t)((int8_t)v >> n);
}

uint16_t GenesisCpu68k::Asr16(uint16_t v, uint8_t n, bool& carry, bool& overflow)
{
	if(n == 0) { carry = false; overflow = false; return v; }
	carry    = (n <= 16) ? ((v >> (n - 1)) & 1) != 0 : (v >> 15) != 0;
	overflow = false;
	if(n >= 16) return (uint16_t)((int16_t)v >> 15);
	return (uint16_t)((int16_t)v >> n);
}

uint32_t GenesisCpu68k::Asr32(uint32_t v, uint8_t n, bool& carry, bool& overflow)
{
	if(n == 0) { carry = false; overflow = false; return v; }
	carry    = (n <= 32) ? ((v >> (n - 1)) & 1) != 0 : (v >> 31) != 0;
	overflow = false;
	if(n >= 32) return (uint32_t)((int32_t)v >> 31);
	return (uint32_t)((int32_t)v >> n);
}

uint8_t GenesisCpu68k::Lsl8(uint8_t v, uint8_t n, bool& carry)
{
	if(n == 0) { carry = false; return v; }
	carry = (n <= 8) ? ((v >> (8 - n)) & 1) != 0 : false;
	return (n >= 8) ? 0 : (v << n);
}

uint16_t GenesisCpu68k::Lsl16(uint16_t v, uint8_t n, bool& carry)
{
	if(n == 0) { carry = false; return v; }
	carry = (n <= 16) ? ((v >> (16 - n)) & 1) != 0 : false;
	return (n >= 16) ? 0 : (v << n);
}

uint32_t GenesisCpu68k::Lsl32(uint32_t v, uint8_t n, bool& carry)
{
	if(n == 0) { carry = false; return v; }
	carry = (n <= 32) ? ((v >> (32 - n)) & 1) != 0 : false;
	return (n >= 32) ? 0 : (v << n);
}

uint8_t GenesisCpu68k::Lsr8(uint8_t v, uint8_t n, bool& carry)
{
	if(n == 0) { carry = false; return v; }
	carry = (n <= 8) ? ((v >> (n - 1)) & 1) != 0 : false;
	return (n >= 8) ? 0 : (v >> n);
}

uint16_t GenesisCpu68k::Lsr16(uint16_t v, uint8_t n, bool& carry)
{
	if(n == 0) { carry = false; return v; }
	carry = (n <= 16) ? ((v >> (n - 1)) & 1) != 0 : false;
	return (n >= 16) ? 0 : (v >> n);
}

uint32_t GenesisCpu68k::Lsr32(uint32_t v, uint8_t n, bool& carry)
{
	if(n == 0) { carry = false; return v; }
	carry = (n <= 32) ? ((v >> (n - 1)) & 1) != 0 : false;
	return (n >= 32) ? 0 : (v >> n);
}

uint8_t GenesisCpu68k::Rol8(uint8_t v, uint8_t n, bool& carry)
{
	n &= 7;
	if(n == 0) {
		carry = (v & 1) != 0;
		return v;
	}
	uint8_t r = (uint8_t)((v << n) | (v >> (8 - n)));
	carry = (r & 1) != 0;
	return r;
}

uint16_t GenesisCpu68k::Rol16(uint16_t v, uint8_t n, bool& carry)
{
	n &= 15;
	if(n == 0) {
		carry = (v & 1) != 0;
		return v;
	}
	uint16_t r = (uint16_t)((v << n) | (v >> (16 - n)));
	carry = (r & 1) != 0;
	return r;
}

uint32_t GenesisCpu68k::Rol32(uint32_t v, uint8_t n, bool& carry)
{
	n &= 31;
	if(n == 0) {
		carry = (v & 1) != 0;
		return v;
	}
	uint32_t r = (v << n) | (v >> (32 - n));
	carry = (r & 1) != 0;
	return r;
}

uint8_t GenesisCpu68k::Ror8(uint8_t v, uint8_t n, bool& carry)
{
	n &= 7;
	if(n == 0) {
		carry = (v & 0x80) != 0;
		return v;
	}
	uint8_t r = (uint8_t)((v >> n) | (v << (8 - n)));
	carry = (r & 0x80) != 0;
	return r;
}

uint16_t GenesisCpu68k::Ror16(uint16_t v, uint8_t n, bool& carry)
{
	n &= 15;
	if(n == 0) {
		carry = (v & 0x8000) != 0;
		return v;
	}
	uint16_t r = (uint16_t)((v >> n) | (v << (16 - n)));
	carry = (r & 0x8000) != 0;
	return r;
}

uint32_t GenesisCpu68k::Ror32(uint32_t v, uint8_t n, bool& carry)
{
	n &= 31;
	if(n == 0) {
		carry = (v & 0x80000000u) != 0;
		return v;
	}
	uint32_t r = (v >> n) | (v << (32 - n));
	carry = (r & 0x80000000u) != 0;
	return r;
}

uint8_t GenesisCpu68k::Roxl8(uint8_t v, uint8_t n, bool& carry, bool x)
{
	n %= 9; if(n == 0) { carry = x; return v; }
	uint16_t w = ((uint16_t)v << 1) | (x ? 1 : 0);
	for(uint8_t i = 1; i < n; i++) {
		bool top = (w & 0x100) != 0;
		w = (w << 1) | (top ? 1 : 0);
	}
	carry = (w & 0x100) != 0;
	return (uint8_t)w;
}

uint16_t GenesisCpu68k::Roxl16(uint16_t v, uint8_t n, bool& carry, bool x)
{
	n %= 17; if(n == 0) { carry = x; return v; }
	uint32_t w = ((uint32_t)v << 1) | (x ? 1 : 0);
	for(uint8_t i = 1; i < n; i++) {
		bool top = (w & 0x10000) != 0;
		w = (w << 1) | (top ? 1 : 0);
	}
	carry = (w & 0x10000) != 0;
	return (uint16_t)w;
}

uint32_t GenesisCpu68k::Roxl32(uint32_t v, uint8_t n, bool& carry, bool x)
{
	n %= 33; if(n == 0) { carry = x; return v; }
	uint64_t w = ((uint64_t)v << 1) | (x ? 1 : 0);
	for(uint8_t i = 1; i < n; i++) {
		bool top = (w & 0x100000000ULL) != 0;
		w = (w << 1) | (top ? 1 : 0);
	}
	carry = (w & 0x100000000ULL) != 0;
	return (uint32_t)w;
}

uint8_t GenesisCpu68k::Roxr8(uint8_t v, uint8_t n, bool& carry, bool x)
{
	n %= 9; if(n == 0) { carry = x; return v; }
	uint16_t w = ((uint16_t)v) | ((x ? 1 : 0) << 8);
	for(uint8_t i = 0; i < n; i++) {
		bool bot = (w & 1) != 0;
		w = (w >> 1) | (bot ? 0x100 : 0);
	}
	carry = (w & 0x100) != 0;
	return (uint8_t)(w);
}

uint16_t GenesisCpu68k::Roxr16(uint16_t v, uint8_t n, bool& carry, bool x)
{
	n %= 17; if(n == 0) { carry = x; return v; }
	uint32_t w = ((uint32_t)v) | ((x ? 1u : 0u) << 16);
	for(uint8_t i = 0; i < n; i++) {
		bool bot = (w & 1) != 0;
		w = (w >> 1) | (bot ? 0x10000u : 0u);
	}
	carry = (w & 0x10000u) != 0;
	return (uint16_t)(w);
}

uint32_t GenesisCpu68k::Roxr32(uint32_t v, uint8_t n, bool& carry, bool x)
{
	n %= 33; if(n == 0) { carry = x; return v; }
	uint64_t w = ((uint64_t)v) | ((x ? 1ULL : 0ULL) << 32);
	for(uint8_t i = 0; i < n; i++) {
		bool bot = (w & 1) != 0;
		w = (w >> 1) | (bot ? 0x100000000ULL : 0ULL);
	}
	carry = (w & 0x100000000ULL) != 0;
	return (uint32_t)(w);
}

// ===========================================================================
// BCD helpers
// ===========================================================================

uint8_t GenesisCpu68k::Abcd(uint8_t a, uint8_t b, bool& carry, bool& zero, bool x)
{
	int r = (int)(a & 0x0F) + (int)(b & 0x0F) + (x ? 1 : 0);
	if(r > 9) r += 6;
	r += (int)(a & 0xF0) + (int)(b & 0xF0);
	if(r > 0x99) { r -= 0xA0; carry = true; } else carry = false;
	uint8_t result = (uint8_t)r;
	if(result) zero = false;
	return result;
}

uint8_t GenesisCpu68k::Sbcd(uint8_t a, uint8_t b, bool& carry, bool& zero, bool x)
{
	int r = (int)(a & 0x0F) - (int)(b & 0x0F) - (x ? 1 : 0);
	if(r < 0) r -= 6;
	r += (int)(a & 0xF0) - (int)(b & 0xF0);
	if(r < 0) { r += 0xA0; carry = true; } else carry = false;
	uint8_t result = (uint8_t)r;
	if(result) zero = false;
	return result;
}
