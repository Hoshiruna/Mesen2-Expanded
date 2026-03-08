#pragma once
#include "pch.h"

class GenesisNativeBackend;
class GenesisApu;

// ---------------------------------------------------------------------------
// GenesisCpuZ80 — Zilog Z80 interpreter for Genesis sound subsystem 
//
// Memory map (Z80 side, 64 KB address space):
//   $0000-$1FFF  Z80 RAM (8 KB)
//   $2000-$3FFF  Z80 RAM mirror
//   $4000-$5FFF  YM2612 ($4000=A0 addr, $4001=A0 data, $4002=A1 addr, $4003=A1 data)
//   $6000        Bank-switch register (one bit per write, LSB first, 9 bits)
//   $7F11        SN76489 PSG write port
//   $8000-$FFFF  68K ROM window (32 KB, address = bankReg << 15)
// ---------------------------------------------------------------------------
class GenesisCpuZ80
{
public:
	struct Z80State
	{
		uint8_t  A, F;
		uint8_t  B, C, D, E, H, L;
		uint8_t  A2, F2;
		uint8_t  B2, C2, D2, E2, H2, L2;
		uint16_t IX, IY, SP, PC;
		uint8_t  I, R;
		bool     IFF1, IFF2;
		uint8_t  IM;
		bool     Halted;
		uint16_t BankReg;
		uint8_t  BankBit;
		int64_t  CycleCount;
	};

private:
	// -----------------------------------------------------------------------
	// Working registers (fast access)
	// -----------------------------------------------------------------------
	uint8_t  _a = 0xFF, _f = 0xFF;
	uint8_t  _b = 0xFF, _c = 0xFF;
	uint8_t  _d = 0xFF, _e = 0xFF;
	uint8_t  _h = 0xFF, _l = 0xFF;
	uint8_t  _a2 = 0xFF, _f2 = 0xFF;
	uint8_t  _b2 = 0xFF, _c2 = 0xFF;
	uint8_t  _d2 = 0xFF, _e2 = 0xFF;
	uint8_t  _h2 = 0xFF, _l2 = 0xFF;
	uint16_t _ix = 0xFFFF, _iy = 0xFFFF;
	uint16_t _sp = 0xFFFF, _pc = 0x0000;
	uint8_t  _i = 0, _r = 0;
	bool     _iff1 = false, _iff2 = false;
	uint8_t  _im = 0;
	bool     _halted = false;
	bool     _nmiPending = false;
	bool     _intPending = false;
	bool     _afterEI  = false;   // suppress IRQ one cycle after EI

	// Bank register for Z80→68K ROM window
	uint16_t _bankReg = 0;   // 9-bit value (bits 8:0)
	uint8_t  _bankBit = 0;   // current shift position (0-8)

	// 8 KB Z80 RAM
	uint8_t _ram[0x2000] = {};

	// Cycle counter (tracks cycles consumed during Run())
	int32_t  _cycles = 0;

	// Owners
	GenesisNativeBackend* _backend = nullptr;
	GenesisApu*           _apu     = nullptr;

	// -----------------------------------------------------------------------
	// Register pair accessors
	// -----------------------------------------------------------------------
	uint16_t BC()  const { return (uint16_t)(_b << 8) | _c; }
	uint16_t DE()  const { return (uint16_t)(_d << 8) | _e; }
	uint16_t HL()  const { return (uint16_t)(_h << 8) | _l; }
	uint16_t AF()  const { return (uint16_t)(_a << 8) | _f; }
	void SetBC(uint16_t v) { _b = v >> 8; _c = (uint8_t)v; }
	void SetDE(uint16_t v) { _d = v >> 8; _e = (uint8_t)v; }
	void SetHL(uint16_t v) { _h = v >> 8; _l = (uint8_t)v; }
	void SetAF(uint16_t v) { _a = v >> 8; _f = (uint8_t)v; }

	// -----------------------------------------------------------------------
	// Flag bits
	// -----------------------------------------------------------------------
	static constexpr uint8_t CF  = 0x01;   // Carry
	static constexpr uint8_t NF  = 0x02;   // Add/Subtract
	static constexpr uint8_t PVF = 0x04;   // Parity/Overflow
	static constexpr uint8_t HF  = 0x10;   // Half-carry
	static constexpr uint8_t ZF  = 0x40;   // Zero
	static constexpr uint8_t SF  = 0x80;   // Sign

	bool FC()  const { return (_f & CF)  != 0; }
	bool FN()  const { return (_f & NF)  != 0; }
	bool FPV() const { return (_f & PVF) != 0; }
	bool FH()  const { return (_f & HF)  != 0; }
	bool FZ()  const { return (_f & ZF)  != 0; }
	bool FS()  const { return (_f & SF)  != 0; }

	// -----------------------------------------------------------------------
	// Memory access
	// -----------------------------------------------------------------------
	uint8_t  MemRead(uint16_t addr);
	void     MemWrite(uint16_t addr, uint8_t val);
	uint16_t MemRead16(uint16_t addr);
	void     MemWrite16(uint16_t addr, uint16_t val);
	uint8_t  IoRead(uint16_t portAddr);
	void     IoWrite(uint16_t portAddr, uint8_t val);

	// Stack helpers
	void     Push(uint16_t v) { _sp -= 2; MemWrite16(_sp, v); }
	uint16_t Pop()            { uint16_t v = MemRead16(_sp); _sp += 2; return v; }

	// Fetch from PC and advance
	uint8_t  Fetch8()         { return MemRead(_pc++); }
	uint16_t Fetch16()        { uint16_t lo = Fetch8(); return lo | ((uint16_t)Fetch8() << 8); }
	int8_t   FetchDisp()      { return (int8_t)Fetch8(); }

	// -----------------------------------------------------------------------
	// Flag computation helpers
	// -----------------------------------------------------------------------
	static uint8_t ParityTable[256];   // pre-computed parity
	static bool    _tablesReady;
	static void    BuildTables();

	uint8_t  SzpFlags(uint8_t v) const;   // returns SF|ZF|PVF from 8-bit value

	// ALU operations — return result and update _f
	uint8_t  Add8 (uint8_t a, uint8_t b, bool cy = false);
	uint8_t  Sub8 (uint8_t a, uint8_t b, bool cy = false);
	uint8_t  And8 (uint8_t a, uint8_t b);
	uint8_t  Or8  (uint8_t a, uint8_t b);
	uint8_t  Xor8 (uint8_t a, uint8_t b);
	void     Cp8  (uint8_t a, uint8_t b);
	uint8_t  Inc8 (uint8_t v);
	uint8_t  Dec8 (uint8_t v);
	uint8_t  Rlca (uint8_t v);   // RLCA
	uint8_t  Rrca (uint8_t v);   // RRCA
	uint8_t  Rla  (uint8_t v);   // RLA
	uint8_t  Rra  (uint8_t v);   // RRA
	uint8_t  Rlc  (uint8_t v);   // RLC  (CB)
	uint8_t  Rrc  (uint8_t v);   // RRC  (CB)
	uint8_t  Rl   (uint8_t v);   // RL   (CB)
	uint8_t  Rr   (uint8_t v);   // RR   (CB)
	uint8_t  Sla  (uint8_t v);   // SLA  (CB)
	uint8_t  Sra  (uint8_t v);   // SRA  (CB)
	uint8_t  Srl  (uint8_t v);   // SRL  (CB)
	uint8_t  Sll  (uint8_t v);   // SLL  (CB, undocumented)
	uint16_t AddHL(uint16_t hl, uint16_t v);
	uint16_t Adc16(uint16_t hl, uint16_t v);
	uint16_t Sbc16(uint16_t hl, uint16_t v);

	// Condition test (cc = bits [5:3] of opcode)
	bool TestCond(uint8_t cc) const;

	// 8-bit register read/write by r-field (0=B,1=C,2=D,3=E,4=H,5=L,6=(HL),7=A)
	uint8_t  GetR  (uint8_t r);
	void     SetR  (uint8_t r, uint8_t v);

	// IX/IY variant (replaces H/L access with IXH/IXL or IYH/IYL for r!=6)
	uint8_t  GetRIdx (uint8_t r, uint16_t idx, int8_t disp);
	void     SetRIdx (uint8_t r, uint8_t v, uint16_t idx, int8_t disp);

	// -----------------------------------------------------------------------
	// Opcode execution
	// -----------------------------------------------------------------------
	void ExecOne();
	void ExecCB();
	void ExecED();
	void ExecDD();
	void ExecFD();
	void ExecDDCB(uint16_t& idx);

public:
	void Init(GenesisNativeBackend* backend, GenesisApu* apu);
	void Reset();

	// Run at least |cycles| Z80 cycles; returns overshoot
	int32_t Run(int32_t cycles);

	// Interrupt lines
	void SetNMI(bool level);
	void SetINT(bool level);

	// 68K access to Z80 address space ($A00000-$A0FFFF mapped to Z80 $0000-$FFFF)
	uint8_t BusRead (uint16_t z80Addr);
	void    BusWrite(uint16_t z80Addr, uint8_t val);

	// State capture / restore
	Z80State CaptureState() const;
	void     RestoreState(const Z80State& s);
	int32_t  GetRunCycles() const { return _cycles; }

	uint8_t* Ram() { return _ram; }

	// Save / load
	void SaveState(vector<uint8_t>& out) const;
	bool LoadState(const vector<uint8_t>& data, size_t& offset);
};
