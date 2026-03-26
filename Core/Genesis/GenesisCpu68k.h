#pragma once

#include "pch.h"
#include "Genesis/GenesisTypes.h"
#include "Shared/Emulator.h"
#include "Debugger/DebugTypes.h"

class GenesisNativeBackend;

// ---------------------------------------------------------------------------
// GenesisCpu68k
//
// Native Motorola M68000 interpreter for the Mega Drive.
//
// Design notes:
//   - 256-entry primary dispatch table indexed by opcode bits [15:8].
//   - Each group handler decodes the remaining bits of _opword internally.
//   - Bus access calls _emu->ProcessMemoryRead/Write (no-op when no debugger).
//   - ProcessInstruction is gated by the debuggerEnabled template parameter.
//   - Cycle counts are approximate M68000 values with coarse backend wait states.
//   - VDP IRQ delivery is driven by the backend frame/scanline runner.
// ---------------------------------------------------------------------------
class GenesisCpu68k
{
public:
	// SR bit masks
	static constexpr uint16_t SR_C      = 0x0001;   // carry
	static constexpr uint16_t SR_V      = 0x0002;   // overflow
	static constexpr uint16_t SR_Z      = 0x0004;   // zero
	static constexpr uint16_t SR_N      = 0x0008;   // negative
	static constexpr uint16_t SR_X      = 0x0010;   // extend
	static constexpr uint16_t SR_I_MASK = 0x0700;   // interrupt priority mask
	static constexpr uint16_t SR_S      = 0x2000;   // supervisor mode
	static constexpr uint16_t SR_T      = 0x8000;   // trace
	static constexpr uint16_t SR_CCR    = 0x001F;   // condition code bits (X+NZVC)

private:
	// -----------------------------------------------------------------------
	// CPU state
	// -----------------------------------------------------------------------
	GenesisCpuState  _state      = {};
	uint32_t         _usp        = 0;     // inactive stack pointer
	uint8_t          _pendingIrq = 0;     // pending IRQ level (0 = none)

	Emulator*             _emu     = nullptr;
	GenesisNativeBackend* _backend = nullptr;

	uint16_t _opword     = 0;   // instruction word being executed
	uint32_t _preExecPC  = 0;   // PC at start of current instruction (pre-fetch)
	uint16_t _preExecSR  = 0;   // SR at start of current instruction
	uint32_t _preExecA[8] = {}; // address registers at start of current instruction
	int32_t  _cycles     = 0;   // cycles consumed this Run() call
	bool     _exceptionTaken = false; // latched when current instruction takes an exception
	bool     _suppressWrites = false; // decode helper: run instruction without bus writes
	bool     _wordAccessActive = false; // when true, BusRead8/BusWrite8 skip wait states (already applied by word caller)
	uint8_t  _fetchBytes = 0;         // decode helper: opcode/operand bytes fetched
	bool     _faultFramePending = false;
	uint32_t _faultAddress = 0;
	uint32_t _faultSavedPC = 0;
	uint16_t _faultStatusWord = 0;
	uint16_t _faultInstructionWord = 0;
	uint16_t _faultStatusExtra = 0;
	uint32_t _faultPcOverride = 0;
	bool     _faultPcOverrideValid = false;
	uint32_t _exceptionPcOverride = 0;
	bool     _exceptionPcOverrideValid = false;
	uint32_t _faultAddressOverride = 0;
	bool     _faultAddressOverrideValid = false;
	bool     _faultPreserveSr = false;
	bool     _faultRestoreSrXOnly = false;
	uint16_t _faultRestoreSrMask = 0;
	bool     _faultPreserveAddressRegs = false;
	bool     _faultIsPcRelativeEA = false;  // set when faulting EA is PC-relative (program space fc)
	bool     _faultPC4ForSourceRead = false; // set during ReadEA source reads so fault uses _preExecPC+4
	bool     _faultStatusWordPreDecrement = false; // -(An) dest: pre-decrement internal cycle shifts pipeline; IR/SSW = raw IRC
	uint32_t _faultPreDecPCOffset = 0;            // extra offset for IRC read: register-direct source fills free bus slot with extra prefetch
#ifdef _DEBUG
	uint32_t _debugIllegalCount = 0;
#endif

	// -----------------------------------------------------------------------
	// Dispatch table — 256 entries, indexed by _opword >> 8
	// -----------------------------------------------------------------------
	using OpHandler = void (GenesisCpu68k::*)();
	static OpHandler _opTable[256];
	static void BuildTable();

	// -----------------------------------------------------------------------
	// Bus access — always fires _emu->ProcessMemoryRead/Write hooks
	// -----------------------------------------------------------------------
	uint8_t  BusRead8 (uint32_t addr, MemoryOperationType opType = MemoryOperationType::Read);
	uint16_t BusRead16(uint32_t addr, MemoryOperationType opType = MemoryOperationType::Read);
	uint32_t BusRead32(uint32_t addr, MemoryOperationType opType = MemoryOperationType::Read);

	void BusWrite8 (uint32_t addr, uint8_t  value);
	void BusWrite16(uint32_t addr, uint16_t value);
	void BusWrite32(uint32_t addr, uint32_t value);
	bool CheckAddressError(uint32_t addr, bool isWrite, uint8_t sizeBytes, MemoryOperationType opType);

	// PC-relative fetches (advance PC and use ExecOpCode / ExecOperand types)
	uint16_t FetchOpcode();   // opcode word — ExecOpCode
	uint16_t FetchExtWord();  // extension word — ExecOperand
	uint32_t FetchExtLong();  // two extension words, big-endian — ExecOperand

	// -----------------------------------------------------------------------
	// Effective-address helpers
	// -----------------------------------------------------------------------
	// Computes the 24-bit bus address for a memory EA, consuming extension
	// words from the instruction stream.  For register-direct modes (mode=0/1)
	// returns a special sentinel — callers must handle those separately.
	static constexpr uint32_t EA_DN = 0xFFFF0000u;  // Dn direct token base (+ n)
	static constexpr uint32_t EA_AN = 0xFFFF0008u;  // An direct token base (+ n)

	uint32_t CalcEA(uint8_t mode, uint8_t reg, uint8_t size);

	// Read/write EA, handling both register-direct and memory cases.
	uint8_t  ReadEA8 (uint8_t mode, uint8_t reg);
	uint16_t ReadEA16(uint8_t mode, uint8_t reg);
	uint32_t ReadEA32(uint8_t mode, uint8_t reg);

	// Call BEFORE CalcEA/ReadEA. Primes fault saved-PC for modes with non-default offset.
	// size = element size in bytes (1/2/4).  Longword predecrement (mode4,size4) uses default saved-PC.
	void SetFaultPCForEA(uint8_t mode, uint8_t reg, uint8_t size = 2);
	void SetFaultPCForImmediateDestEA(uint8_t mode, uint8_t reg, uint8_t sizeBytes);

	void WriteEA8 (uint8_t mode, uint8_t reg, uint8_t  v);
	void WriteEA16(uint8_t mode, uint8_t reg, uint16_t v);
	void WriteEA32(uint8_t mode, uint8_t reg, uint32_t v);

	// Read/write using a pre-resolved EA token from CalcEA().
	// This is required for read-modify-write ops so extension words are consumed once.
	uint8_t  ReadResolvedEA8(uint32_t ea);
	uint16_t ReadResolvedEA16(uint32_t ea);
	uint32_t ReadResolvedEA32(uint32_t ea);

	void WriteResolvedEA8(uint32_t ea, uint8_t  v);
	void WriteResolvedEA16(uint32_t ea, uint16_t v);
	void WriteResolvedEA32(uint32_t ea, uint32_t v);

	// -----------------------------------------------------------------------
	// CCR / SR inline accessors
	// -----------------------------------------------------------------------
	bool    GetC()  const { return (_state.SR & SR_C) != 0; }
	bool    GetV()  const { return (_state.SR & SR_V) != 0; }
	bool    GetZ()  const { return (_state.SR & SR_Z) != 0; }
	bool    GetN()  const { return (_state.SR & SR_N) != 0; }
	bool    GetX()  const { return (_state.SR & SR_X) != 0; }
	bool    GetS()  const { return (_state.SR & SR_S) != 0; }
	uint8_t GetIPL()const { return static_cast<uint8_t>((_state.SR >> 8) & 7); }

	void SetC(bool v) { if(_exceptionTaken) return; v ? (_state.SR |= SR_C) : (_state.SR &= ~SR_C); }
	void SetV(bool v) { if(_exceptionTaken) return; v ? (_state.SR |= SR_V) : (_state.SR &= ~SR_V); }
	void SetZ(bool v) { if(_exceptionTaken) return; v ? (_state.SR |= SR_Z) : (_state.SR &= ~SR_Z); }
	void SetN(bool v) { if(_exceptionTaken) return; v ? (_state.SR |= SR_N) : (_state.SR &= ~SR_N); }
	void SetX(bool v) { if(_exceptionTaken) return; v ? (_state.SR |= SR_X) : (_state.SR &= ~SR_X); }

	void SetSR(uint16_t value);   // handles S-mode switch (swap A7/USP)
	void SetCCR(uint8_t value) { _state.SR = (_state.SR & 0xFF00u) | (value & 0x1Fu); }

	void UpdateFlagsNZ8 (uint8_t  r) { SetN(r  >> 7  & 1); SetZ(r  == 0); }
	void UpdateFlagsNZ16(uint16_t r) { SetN(r  >> 15 & 1); SetZ(r  == 0); }
	void UpdateFlagsNZ32(uint32_t r) { SetN(r  >> 31 & 1); SetZ(r  == 0); }

	// -----------------------------------------------------------------------
	// Stack helpers
	// -----------------------------------------------------------------------
	void Push16(uint16_t v);
	void Push32(uint32_t v);
	uint16_t Pull16();
	uint32_t Pull32();

	// -----------------------------------------------------------------------
	// Exception / interrupt
	// -----------------------------------------------------------------------
	void TakeException(uint8_t vector, uint8_t newIPL = 0);
	void CheckInterrupts();

	// -----------------------------------------------------------------------
	// Branch condition test (cc = 4-bit condition code field)
	// -----------------------------------------------------------------------
	bool TestCC(uint8_t cc) const;

	// -----------------------------------------------------------------------
	// Register convenience
	// -----------------------------------------------------------------------
	uint32_t Dn(uint8_t n) const { return _state.D[n & 7]; }
	uint32_t An(uint8_t n) const { return _state.A[n & 7]; }

	void SetDn(uint8_t n, uint32_t v) { _state.D[n & 7] = v; }
	void SetAn(uint8_t n, uint32_t v)
	{
		_state.A[n & 7] = v;
		if((n & 7) == 7) _state.SP = v;
	}

	// -----------------------------------------------------------------------
	// Arithmetic helpers
	// -----------------------------------------------------------------------
	uint8_t  Add8 (uint8_t  a, uint8_t  b, bool xin, bool& carry, bool& overflow);
	uint16_t Add16(uint16_t a, uint16_t b, bool xin, bool& carry, bool& overflow);
	uint32_t Add32(uint32_t a, uint32_t b, bool xin, bool& carry, bool& overflow);

	uint8_t  Sub8 (uint8_t  a, uint8_t  b, bool xin, bool& carry, bool& overflow);
	uint16_t Sub16(uint16_t a, uint16_t b, bool xin, bool& carry, bool& overflow);
	uint32_t Sub32(uint32_t a, uint32_t b, bool xin, bool& carry, bool& overflow);

	// Shift helpers — return the shifted value; carry gets the shifted-out bit.
	uint8_t  Asl8 (uint8_t  v, uint8_t n, bool& carry, bool& overflow);
	uint16_t Asl16(uint16_t v, uint8_t n, bool& carry, bool& overflow);
	uint32_t Asl32(uint32_t v, uint8_t n, bool& carry, bool& overflow);
	uint8_t  Asr8 (uint8_t  v, uint8_t n, bool& carry, bool& overflow);
	uint16_t Asr16(uint16_t v, uint8_t n, bool& carry, bool& overflow);
	uint32_t Asr32(uint32_t v, uint8_t n, bool& carry, bool& overflow);

	uint8_t  Lsl8 (uint8_t  v, uint8_t n, bool& carry);
	uint16_t Lsl16(uint16_t v, uint8_t n, bool& carry);
	uint32_t Lsl32(uint32_t v, uint8_t n, bool& carry);
	uint8_t  Lsr8 (uint8_t  v, uint8_t n, bool& carry);
	uint16_t Lsr16(uint16_t v, uint8_t n, bool& carry);
	uint32_t Lsr32(uint32_t v, uint8_t n, bool& carry);

	uint8_t  Rol8 (uint8_t  v, uint8_t n, bool& carry);
	uint16_t Rol16(uint16_t v, uint8_t n, bool& carry);
	uint32_t Rol32(uint32_t v, uint8_t n, bool& carry);
	uint8_t  Ror8 (uint8_t  v, uint8_t n, bool& carry);
	uint16_t Ror16(uint16_t v, uint8_t n, bool& carry);
	uint32_t Ror32(uint32_t v, uint8_t n, bool& carry);

	uint8_t  Roxl8 (uint8_t  v, uint8_t n, bool& carry, bool x);
	uint16_t Roxl16(uint16_t v, uint8_t n, bool& carry, bool x);
	uint32_t Roxl32(uint32_t v, uint8_t n, bool& carry, bool x);
	uint8_t  Roxr8 (uint8_t  v, uint8_t n, bool& carry, bool x);
	uint16_t Roxr16(uint16_t v, uint8_t n, bool& carry, bool x);
	uint32_t Roxr32(uint32_t v, uint8_t n, bool& carry, bool x);

	// BCD helpers
	uint8_t Abcd(uint8_t a, uint8_t b, bool& carry, bool& zero, bool x);
	uint8_t Sbcd(uint8_t a, uint8_t b, bool& carry, bool& zero, bool x);

	// -----------------------------------------------------------------------
	// Instruction handlers (indexed by _opword >> 8)
	// -----------------------------------------------------------------------
	void I_Group0();   // $00-$0F  bit ops, immediate ops, MOVEP
	void I_Move();     // $10-$3F  MOVE.B / MOVE.L / MOVE.W
	void I_Group4();   // $40-$4F  miscellaneous
	void I_Group5();   // $50-$5F  ADDQ / SUBQ / Scc / DBcc
	void I_Group6();   // $60-$6F  BRA / BSR / Bcc
	void I_Moveq();    // $70-$7F  MOVEQ
	void I_Group8();   // $80-$8F  OR / DIVU / SBCD / DIVS
	void I_Group9();   // $90-$9F  SUB / SUBX / SUBA
	void I_ALine();    // $A0-$AF  A-line trap
	void I_GroupB();   // $B0-$BF  CMP / CMPA / CMPM / EOR
	void I_GroupC();   // $C0-$CF  AND / MULU / ABCD / EXG / MULS
	void I_GroupD();   // $D0-$DF  ADD / ADDX / ADDA
	void I_GroupE();   // $E0-$EF  shift / rotate
	void I_FLine();    // $F0-$FF  F-line trap

	// Sub-helpers called from the group handlers
	void DoMove(uint8_t size, uint8_t dstMode, uint8_t dstReg,
	            uint8_t srcMode, uint8_t srcReg);
	void DoBitOp(uint8_t op, uint8_t mode, uint8_t reg, uint32_t bitNum);
	void DoShiftRotate(bool left, bool isArith, bool isRotate, bool isRox,
	                   uint8_t size, uint8_t mode, uint8_t reg,
	                   uint8_t count, bool countIsReg);

public:
	static void StaticInit();

	void Init(Emulator* emu, GenesisNativeBackend* backend);
	void Reset(uint32_t initialSSP, uint32_t initialPC);

	// Run for approximately targetCycles 68K cycles.
	// Returns cycles actually consumed (always >= targetCycles unless stopped).
	int32_t Run(int32_t targetCycles);

	// Single-step one instruction with or without debugger hooks.
	template<bool debuggerEnabled>
	void Exec();

	void SetPendingIrq(uint8_t level) { _pendingIrq = level; }
	void ClearPendingIrq()            { _pendingIrq = 0; }
	uint8_t GetPendingIrq() const     { return _pendingIrq; }
	int32_t GetRunCycles() const      { return _cycles; }

	const GenesisCpuState& GetState() const { return _state; }
	GenesisCpuState&       GetState()       { return _state; }

	// Returns USP regardless of current mode
	uint32_t GetUSP() const { return GetS() ? _usp : _state.A[7]; }
	uint32_t GetSSP() const { return GetS() ? _state.A[7] : _usp; }
	void SetUSP(uint32_t usp) { _usp = usp; }

	// Decode the instruction size at address using the native decoder path.
	// This suppresses all bus writes and debugger callbacks.
	uint8_t DecodeInstructionSize(uint32_t address);
};
