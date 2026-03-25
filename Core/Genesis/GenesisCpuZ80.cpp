#include "pch.h"
#include "Genesis/GenesisCpuZ80.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Genesis/APU/GenesisApu.h"
#include "Shared/MemoryOperationType.h"

// ---------------------------------------------------------------------------
// Static tables
// ---------------------------------------------------------------------------
uint8_t GenesisCpuZ80::ParityTable[256] = {};
bool    GenesisCpuZ80::_tablesReady = false;

void GenesisCpuZ80::BuildTables()
{
	for(int i = 0; i < 256; i++) {
		uint8_t v = (uint8_t)i;
		v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
		ParityTable[i] = (~v & 1) ? PVF : 0;   // 1 = even parity
	}
}

// ---------------------------------------------------------------------------
// Serialization helpers (local)
// ---------------------------------------------------------------------------
namespace {
	template<typename T>
	void AppV(vector<uint8_t>& out, const T& v) {
		size_t o = out.size(); out.resize(o + sizeof(T));
		memcpy(out.data() + o, &v, sizeof(T));
	}
	template<typename T>
	bool RdV(const vector<uint8_t>& d, size_t& o, T& v) {
		if(o + sizeof(T) > d.size()) return false;
		memcpy(&v, d.data() + o, sizeof(T)); o += sizeof(T); return true;
	}
}

// ---------------------------------------------------------------------------
// Init / Reset
// ---------------------------------------------------------------------------
void GenesisCpuZ80::Init(GenesisNativeBackend* backend, GenesisApu* apu)
{
	if(!_tablesReady) { BuildTables(); _tablesReady = true; }
	_backend = backend;
	_apu     = apu;
	Reset();
}

void GenesisCpuZ80::Reset()
{
	_a = _f = 0xFF; _b = _c = _d = _e = _h = _l = 0xFF;
	_a2 = _f2 = _b2 = _c2 = _d2 = _e2 = _h2 = _l2 = 0xFF;
	_ix = _iy = 0xFFFF; _sp = 0xFFFF; _pc = 0x0000;
	_i = _r = 0;
	_iff1 = _iff2 = false; _im = 0;
	_halted = false; _nmiPending = false; _intPending = false; _afterEI = false;
	_bankReg = 0; _bankBit = 0;
	memset(_ram, 0, sizeof(_ram));
	_cycles = 0;
}

// ---------------------------------------------------------------------------
// Memory access
// ---------------------------------------------------------------------------
uint8_t GenesisCpuZ80::MemRead(uint16_t addr)
{
	if(addr < 0x4000) {
		uint8_t value = _ram[addr & 0x1FFFu];
		if(_backend) _backend->Z80ProcessRead(addr & 0x1FFFu, value, MemoryOperationType::Read);
		return value;
	}
	if(addr < 0x6000) {
		// YM2612 status read ($4000/$4002 = port 0/1 status)
		if(_apu) return _apu->ReadYmStatus((addr >> 1) & 1);
		return 0;
	}
	if(addr < 0x8000) {
		// $7F00-$7FFF mirrors the 68K VDP/PSG window at $C00000-$C000FF.
		if((addr & 0xFF00u) == 0x7F00u && _backend) {
			_cycles += _backend->GetZ80To68kBusPenaltyCycles();
			return _backend->ReadBusForZ80(0x00C00000u | (addr & 0x00FFu));
		}
		return 0xFF;
	}
	// 68K ROM window
	if(_backend) {
		_cycles += _backend->GetZ80To68kBusPenaltyCycles();
		uint32_t phys = ((uint32_t)_bankReg << 15) | (addr & 0x7FFFu);
		return _backend->ReadBusForZ80(phys);
	}
	return 0xFF;
}

void GenesisCpuZ80::MemWrite(uint16_t addr, uint8_t val)
{
	if(addr < 0x4000) {
		uint16_t ramAddr = addr & 0x1FFFu;
		if(!_backend || _backend->Z80ProcessWrite(ramAddr, val, MemoryOperationType::Write)) {
			_ram[ramAddr] = val;
		}
		return;
	}
	if(addr < 0x6000) {
		// YM2612: even = address, odd = data
		if(_apu) _apu->WriteYm((addr >> 1) & 1, (addr & 1) == 0, val);
		return;
	}
	if(addr < 0x8000) {
		// $6000-$60FF: bank register shift (bit0, 9-bit shift register).
		if((addr & 0xFF00u) == 0x6000u) {
			_bankReg = (uint16_t)(((_bankReg >> 1) | ((val & 1u) << 8)) & 0x01FFu);
			return;
		}

		// $7F00-$7FFF: VDP/PSG window.
			if((addr & 0xFF00u) == 0x7F00u) {
				// Preserve common PSG shortcut used by many drivers.
				if(addr == 0x7F11) {
					if(_apu) _apu->WritePsg(val);
					return;
				}
				if(_backend) {
					_cycles += _backend->GetZ80To68kBusPenaltyCycles();
					_backend->WriteBusForZ80(0x00C00000u | (addr & 0x00FFu), val);
				}
			}
			return;
		}
	// $8000-$FFFF: writes to 68K bus via Z80 window (rare but valid)
	if(_backend && addr >= 0x8000) {
		_cycles += _backend->GetZ80To68kBusPenaltyCycles();
		uint32_t phys = ((uint32_t)_bankReg << 15) | (addr & 0x7FFFu);
		_backend->WriteBusForZ80(phys, val);
	}
}

uint16_t GenesisCpuZ80::MemRead16(uint16_t addr)
{
	return MemRead(addr) | ((uint16_t)MemRead((uint16_t)(addr + 1)) << 8);
}

void GenesisCpuZ80::MemWrite16(uint16_t addr, uint16_t val)
{
	MemWrite(addr,               (uint8_t)val);
	MemWrite((uint16_t)(addr+1), (uint8_t)(val >> 8));
}

uint8_t GenesisCpuZ80::IoRead(uint16_t portAddr)
{
	// Genesis does not expose a rich separate I/O space to the Z80.
	// Route I/O reads through the Z80-visible bus map for compatibility.
	return MemRead(portAddr);
}

void GenesisCpuZ80::IoWrite(uint16_t portAddr, uint8_t val)
{
	// Route I/O writes through the Z80-visible bus map for compatibility.
	MemWrite(portAddr, val);
}

// ---------------------------------------------------------------------------
// 68K bus access to Z80 address space
// ---------------------------------------------------------------------------
uint8_t GenesisCpuZ80::BusRead(uint16_t z80Addr)
{
	return MemRead(z80Addr);
}

void GenesisCpuZ80::BusWrite(uint16_t z80Addr, uint8_t val)
{
	MemWrite(z80Addr, val);
}

// ---------------------------------------------------------------------------
// Flag helpers
// ---------------------------------------------------------------------------
uint8_t GenesisCpuZ80::SzpFlags(uint8_t v) const
{
	return (v & SF) | (v == 0 ? ZF : 0) | ParityTable[v];
}

bool GenesisCpuZ80::TestCond(uint8_t cc) const
{
	switch(cc & 7) {
		case 0: return !FZ();   // NZ
		case 1: return  FZ();   // Z
		case 2: return !FC();   // NC
		case 3: return  FC();   // C
		case 4: return !FPV();  // PO
		case 5: return  FPV();  // PE
		case 6: return !FS();   // P
		case 7: return  FS();   // M
	}
	return false;
}

// ---------------------------------------------------------------------------
// 8-bit register r-field access (B=0,C=1,D=2,E=3,H=4,L=5,(HL)=6,A=7)
// ---------------------------------------------------------------------------
uint8_t GenesisCpuZ80::GetR(uint8_t r)
{
	switch(r & 7) {
		case 0: return _b; case 1: return _c; case 2: return _d; case 3: return _e;
		case 4: return _h; case 5: return _l; case 6: return MemRead(HL()); case 7: return _a;
	}
	return 0;
}

void GenesisCpuZ80::SetR(uint8_t r, uint8_t v)
{
	switch(r & 7) {
		case 0: _b = v; break; case 1: _c = v; break; case 2: _d = v; break; case 3: _e = v; break;
		case 4: _h = v; break; case 5: _l = v; break; case 6: MemWrite(HL(), v); break; case 7: _a = v; break;
	}
}

// For DD/FD-prefix: r=4→idxH, r=5→idxL, r=6→(idx+d)
uint8_t GenesisCpuZ80::GetRIdx(uint8_t r, uint16_t idx, int8_t disp)
{
	if(r == 4) return (uint8_t)(idx >> 8);
	if(r == 5) return (uint8_t)idx;
	if(r == 6) { _cycles += 3; return MemRead((uint16_t)(idx + disp)); }
	return GetR(r);
}

void GenesisCpuZ80::SetRIdx(uint8_t r, uint8_t v, uint16_t idx, int8_t disp)
{
	if(r == 4) { if(&idx == &_ix) _ix = (_ix & 0x00FF) | ((uint16_t)v << 8);
	             else              _iy = (_iy & 0x00FF) | ((uint16_t)v << 8); return; }
	if(r == 5) { if(&idx == &_ix) _ix = (_ix & 0xFF00) | v;
	             else              _iy = (_iy & 0xFF00) | v; return; }
	if(r == 6) { _cycles += 3; MemWrite((uint16_t)(idx + disp), v); return; }
	SetR(r, v);
}

// ---------------------------------------------------------------------------
// ALU operations
// ---------------------------------------------------------------------------
uint8_t GenesisCpuZ80::Add8(uint8_t a, uint8_t b, bool cy)
{
	uint8_t c  = cy ? 1 : 0;
	uint16_t r = (uint16_t)a + b + c;
	uint8_t  v = (uint8_t)r;
	_f = (uint8_t)((v & SF) | (v == 0 ? ZF : 0) |
	     ((a ^ b ^ v) & HF) |
	     (((a ^ b ^ 0x80) & (a ^ v) & 0x80) ? PVF : 0) |
	     ((r >> 8) & CF));
	return v;
}

uint8_t GenesisCpuZ80::Sub8(uint8_t a, uint8_t b, bool cy)
{
	uint8_t c  = cy ? 1 : 0;
	uint16_t r = (uint16_t)a - b - c;
	uint8_t  v = (uint8_t)r;
	_f = (uint8_t)((v & SF) | (v == 0 ? ZF : 0) |
	     ((a ^ b ^ v) & HF) |
	     (((a ^ b) & (a ^ v) & 0x80) ? PVF : 0) |
	     ((r >> 8) & CF) | NF);
	return v;
}

uint8_t GenesisCpuZ80::And8(uint8_t a, uint8_t b)
{
	uint8_t v = a & b;
	_f = SzpFlags(v) | HF;
	return v;
}

uint8_t GenesisCpuZ80::Or8(uint8_t a, uint8_t b)
{
	uint8_t v = a | b;
	_f = SzpFlags(v);
	return v;
}

uint8_t GenesisCpuZ80::Xor8(uint8_t a, uint8_t b)
{
	uint8_t v = a ^ b;
	_f = SzpFlags(v);
	return v;
}

void GenesisCpuZ80::Cp8(uint8_t a, uint8_t b)
{
	Sub8(a, b, false);   // result discarded, flags kept
}

uint8_t GenesisCpuZ80::Inc8(uint8_t v)
{
	uint8_t r = v + 1;
	_f = (uint8_t)((_f & CF) |
	     (r & SF) | (r == 0 ? ZF : 0) |
	     ((v & 0x0F) == 0x0F ? HF : 0) |
	     (v == 0x7F ? PVF : 0));
	return r;
}

uint8_t GenesisCpuZ80::Dec8(uint8_t v)
{
	uint8_t r = v - 1;
	_f = (uint8_t)((_f & CF) |
	     (r & SF) | (r == 0 ? ZF : 0) |
	     ((v & 0x0F) == 0x00 ? HF : 0) |
	     (v == 0x80 ? PVF : 0) | NF);
	return r;
}

// Rotate group (RLCA/RRCA/RLA/RRA — only affect C, N, H)
uint8_t GenesisCpuZ80::Rlca(uint8_t v)
{
	uint8_t c = v >> 7;
	v = (v << 1) | c;
	_f = (uint8_t)((_f & (SF | ZF | PVF)) | c);
	return v;
}

uint8_t GenesisCpuZ80::Rrca(uint8_t v)
{
	uint8_t c = v & 1;
	v = (v >> 1) | (c << 7);
	_f = (uint8_t)((_f & (SF | ZF | PVF)) | c);
	return v;
}

uint8_t GenesisCpuZ80::Rla(uint8_t v)
{
	uint8_t cin = FC() ? 1u : 0u;
	uint8_t c   = v >> 7;
	v = (v << 1) | cin;
	_f = (uint8_t)((_f & (SF | ZF | PVF)) | c);
	return v;
}

uint8_t GenesisCpuZ80::Rra(uint8_t v)
{
	uint8_t cin = FC() ? 0x80u : 0u;
	uint8_t c   = v & 1;
	v = (v >> 1) | cin;
	_f = (uint8_t)((_f & (SF | ZF | PVF)) | c);
	return v;
}

// CB-prefix rotates — affect all flags including S/Z/P
uint8_t GenesisCpuZ80::Rlc(uint8_t v)  { uint8_t c=v>>7; v=(v<<1)|c; _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Rrc(uint8_t v)  { uint8_t c=v&1;  v=(v>>1)|(c<<7); _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Rl (uint8_t v)  { uint8_t cin=FC()?1:0; uint8_t c=v>>7; v=(v<<1)|cin; _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Rr (uint8_t v)  { uint8_t cin=FC()?0x80:0; uint8_t c=v&1; v=(v>>1)|cin; _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Sla(uint8_t v)  { uint8_t c=v>>7; v<<=1; _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Sra(uint8_t v)  { uint8_t c=v&1; v=(v>>1)|(v&0x80); _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Srl(uint8_t v)  { uint8_t c=v&1; v>>=1; _f=SzpFlags(v)|c; return v; }
uint8_t GenesisCpuZ80::Sll(uint8_t v)  { uint8_t c=v>>7; v=(v<<1)|1; _f=SzpFlags(v)|c; return v; }

uint16_t GenesisCpuZ80::AddHL(uint16_t hl, uint16_t v)
{
	uint32_t r = (uint32_t)hl + v;
	_f = (uint8_t)((_f & (SF | ZF | PVF)) |
	     (((hl ^ v ^ (uint16_t)r) >> 8) & HF) |
	     ((r >> 16) & CF));
	return (uint16_t)r;
}

uint16_t GenesisCpuZ80::Adc16(uint16_t hl, uint16_t v)
{
	uint32_t c = FC() ? 1 : 0;
	uint32_t r = (uint32_t)hl + v + c;
	uint16_t w = (uint16_t)r;
	_f = (uint8_t)(((w >> 8) & SF) | (w == 0 ? ZF : 0) |
	     (((hl ^ v ^ w) >> 8) & HF) |
	     ((((hl ^ v ^ 0x8000) & (hl ^ w)) >> 8) & 0x80 ? PVF : 0) |
	     ((r >> 16) & CF));
	return w;
}

uint16_t GenesisCpuZ80::Sbc16(uint16_t hl, uint16_t v)
{
	uint32_t c = FC() ? 1 : 0;
	uint32_t r = (uint32_t)hl - v - c;
	uint16_t w = (uint16_t)r;
	_f = (uint8_t)(((w >> 8) & SF) | (w == 0 ? ZF : 0) |
	     (((hl ^ v ^ w) >> 8) & HF) |
	     ((((hl ^ v) & (hl ^ w)) >> 8) & 0x80 ? PVF : 0) |
	     ((r >> 16) & CF) | NF);
	return w;
}

// ---------------------------------------------------------------------------
// CB prefix
// ---------------------------------------------------------------------------
void GenesisCpuZ80::ExecCB()
{
	uint8_t op = Fetch8(); _cycles += 8;
	uint8_t r = op & 7, grp = op >> 6, bit = (op >> 3) & 7;
	uint8_t v = GetR(r);
	if(r == 6) _cycles += 7;   // (HL) costs extra

	uint8_t res = v;
	switch(grp) {
		case 0:   // rotate/shift
			switch(bit) {
				case 0: res=Rlc(v); break; case 1: res=Rrc(v); break;
				case 2: res=Rl(v);  break; case 3: res=Rr(v);  break;
				case 4: res=Sla(v); break; case 5: res=Sra(v); break;
				case 6: res=Sll(v); break; case 7: res=Srl(v); break;
			}
			SetR(r, res);
			break;
		case 1:   // BIT
			_f = (uint8_t)((_f & CF) | HF | (v & (1 << bit) ? 0 : ZF) |
			     (bit == 7 && (v & 0x80) ? SF : 0) |
			     (v & (1 << bit) ? 0 : PVF));
			break;
		case 2:   // RES
			SetR(r, v & ~(uint8_t)(1 << bit));
			break;
		case 3:   // SET
			SetR(r, v | (uint8_t)(1 << bit));
			break;
	}
}

// ---------------------------------------------------------------------------
// DDCB / FDCB prefix
// ---------------------------------------------------------------------------
void GenesisCpuZ80::ExecDDCB(uint16_t& idx)
{
	int8_t   disp = FetchDisp();
	uint8_t  op   = Fetch8();
	_cycles += 15;
	uint16_t addr = (uint16_t)(idx + disp);
	uint8_t  v    = MemRead(addr);
	uint8_t  grp  = op >> 6, bit = (op >> 3) & 7, r = op & 7;
	uint8_t  res  = v;

	switch(grp) {
		case 0:
			switch(bit) {
				case 0: res=Rlc(v); break; case 1: res=Rrc(v); break;
				case 2: res=Rl(v);  break; case 3: res=Rr(v);  break;
				case 4: res=Sla(v); break; case 5: res=Sra(v); break;
				case 6: res=Sll(v); break; case 7: res=Srl(v); break;
			}
			MemWrite(addr, res);
			if(r != 6) SetR(r, res);
			break;
		case 1:
			_f = (uint8_t)((_f & CF) | HF | (v & (1<<bit) ? 0 : ZF|PVF) |
			     (bit==7&&(v&0x80)?SF:0));
			break;
		case 2:
			res = v & ~(uint8_t)(1<<bit);
			MemWrite(addr, res);
			if(r != 6) SetR(r, res);
			break;
		case 3:
			res = v | (uint8_t)(1<<bit);
			MemWrite(addr, res);
			if(r != 6) SetR(r, res);
			break;
	}
}

// ---------------------------------------------------------------------------
// ED prefix
// ---------------------------------------------------------------------------
void GenesisCpuZ80::ExecED()
{
	uint8_t op = Fetch8(); _cycles += 8;
	switch(op) {
		// SBC/ADC HL,rr
		case 0x42: SetHL(Sbc16(HL(), BC())); _cycles += 7; break;
		case 0x52: SetHL(Sbc16(HL(), DE())); _cycles += 7; break;
		case 0x62: SetHL(Sbc16(HL(), HL())); _cycles += 7; break;
		case 0x72: SetHL(Sbc16(HL(), _sp));  _cycles += 7; break;
		case 0x4A: SetHL(Adc16(HL(), BC())); _cycles += 7; break;
		case 0x5A: SetHL(Adc16(HL(), DE())); _cycles += 7; break;
		case 0x6A: SetHL(Adc16(HL(), HL())); _cycles += 7; break;
		case 0x7A: SetHL(Adc16(HL(), _sp));  _cycles += 7; break;
		// LD (nn),rr
		case 0x43: { uint16_t a=Fetch16(); MemWrite16(a,BC()); _cycles+=12; break; }
		case 0x53: { uint16_t a=Fetch16(); MemWrite16(a,DE()); _cycles+=12; break; }
		case 0x63: { uint16_t a=Fetch16(); MemWrite16(a,HL()); _cycles+=12; break; }
		case 0x73: { uint16_t a=Fetch16(); MemWrite16(a,_sp); _cycles+=12; break; }
		// LD rr,(nn)
		case 0x4B: { uint16_t a=Fetch16(); SetBC(MemRead16(a)); _cycles+=12; break; }
		case 0x5B: { uint16_t a=Fetch16(); SetDE(MemRead16(a)); _cycles+=12; break; }
		case 0x6B: { uint16_t a=Fetch16(); SetHL(MemRead16(a)); _cycles+=12; break; }
		case 0x7B: { uint16_t a=Fetch16(); _sp=MemRead16(a);    _cycles+=12; break; }
		// LD I/R,A / LD A,I/R
		case 0x47: _i = _a; _cycles += 1; break;
		case 0x4F: _r = _a; _cycles += 1; break;
		case 0x57: _a=_i; _f=(uint8_t)((_f&CF)|((_a)&SF)|(_a==0?ZF:0)|(_iff2?PVF:0)); _cycles+=1; break;
		case 0x5F: _a=_r; _f=(uint8_t)((_f&CF)|((_a)&SF)|(_a==0?ZF:0)|(_iff2?PVF:0)); _cycles+=1; break;
		// RLD/RRD
		case 0x6F: { uint8_t m=MemRead(HL()); MemWrite(HL(),(uint8_t)((m<<4)|(_a&0x0F))); _a=(_a&0xF0)|(m>>4); _f=(_f&CF)|SzpFlags(_a); _cycles+=10; break; }
		case 0x67: { uint8_t m=MemRead(HL()); MemWrite(HL(),(uint8_t)((m>>4)|(_a<<4))); _a=(_a&0xF0)|(m&0x0F); _f=(_f&CF)|SzpFlags(_a); _cycles+=10; break; }
		// NEG
		case 0x44: case 0x4C: case 0x54: case 0x5C: case 0x64: case 0x6C: case 0x74: case 0x7C:
			_a = Sub8(0, _a); break;
		// RETN / RETI
		case 0x45: case 0x55: case 0x65: case 0x75: case 0x5D: case 0x6D: case 0x7D:
		case 0x4D: { _iff1=_iff2; _pc=Pop(); _cycles+=6; break; }
		// IM 0/1/2
		case 0x46: case 0x66: _im=0; break;
		case 0x56: case 0x76: _im=1; break;
		case 0x5E: case 0x7E: _im=2; break;
		// IN r,(C)
		case 0x40: case 0x48: case 0x50: case 0x58:
		case 0x60: case 0x68: case 0x70: case 0x78: {
			uint8_t v = IoRead(BC());
			switch(op & 0x38) {
				case 0x00: _b = v; break;
				case 0x08: _c = v; break;
				case 0x10: _d = v; break;
				case 0x18: _e = v; break;
				case 0x20: _h = v; break;
				case 0x28: _l = v; break;
				case 0x38: _a = v; break;
				default: break; // 0x70: IN (C) affects flags only
			}
			_f = (uint8_t)((_f & CF) | SzpFlags(v));
			_cycles += 4;
			break;
		}
		// OUT (C),r
		case 0x41: case 0x49: case 0x51: case 0x59:
		case 0x61: case 0x69: case 0x71: case 0x79: {
			uint8_t v = 0;
			switch(op & 0x38) {
				case 0x00: v = _b; break;
				case 0x08: v = _c; break;
				case 0x10: v = _d; break;
				case 0x18: v = _e; break;
				case 0x20: v = _h; break;
				case 0x28: v = _l; break;
				case 0x38: v = _a; break;
				default: break; // 0x71: OUT (C),0
			}
			IoWrite(BC(), v);
			_cycles += 4;
			break;
		}
		// Block instructions
		case 0xA0: { MemWrite(DE(),(uint8_t)(MemRead(HL())+_i)); SetHL(HL()+1); SetDE(DE()+1); SetBC(BC()-1); _f=(_f&(SF|ZF|CF))|(BC()-1?PVF:0); _cycles+=8; break; }
		case 0xA1: { uint8_t v=MemRead(HL()); SetHL(HL()+1); SetBC(BC()-1); _f=(_f&CF)|HF|(v-_a==0?ZF:0)|((v-_a)&SF)|(BC()?PVF:0)|NF; _cycles+=8; break; }
		case 0xA2: { // INI
			uint8_t v = IoRead(BC());
			MemWrite(HL(), v);
			SetHL(HL() + 1);
			_b--;
			_f = (uint8_t)((_f & CF) | (_b ? PVF : 0) | (_b == 0 ? ZF : 0) | (_b & 0x80 ? SF : 0) | (v & 0x80 ? NF : 0));
			_cycles += 8;
			break;
		}
		case 0xA3: { // OUTI
			uint8_t v = MemRead(HL());
			IoWrite(BC(), v);
			SetHL(HL() + 1);
			_b--;
			_f = (uint8_t)((_f & CF) | (_b ? PVF : 0) | (_b == 0 ? ZF : 0) | (_b & 0x80 ? SF : 0) | (v & 0x80 ? NF : 0));
			_cycles += 8;
			break;
		}
		case 0xA8: { MemWrite(DE(),(uint8_t)(MemRead(HL())+_i)); SetHL(HL()-1); SetDE(DE()-1); SetBC(BC()-1); _f=(_f&(SF|ZF|CF))|(BC()?PVF:0); _cycles+=8; break; }
		case 0xA9: { uint8_t v=MemRead(HL()); SetHL(HL()-1); SetBC(BC()-1); _f=(_f&CF)|HF|(v-_a==0?ZF:0)|((v-_a)&SF)|(BC()?PVF:0)|NF; _cycles+=8; break; }
		case 0xB0: // LDIR
			do {
				MemWrite(DE(), MemRead(HL()));
				SetHL(HL()+1); SetDE(DE()+1); SetBC(BC()-1);
				_cycles += 21;
			} while(BC() != 0);
			_cycles -= 5;  // last iteration is 16
			_f &= ~PVF;
			break;
		case 0xB1: // CPIR
			{ uint8_t cpirV = 0;
			  do { cpirV=MemRead(HL()); SetHL(HL()+1); SetBC(BC()-1); _cycles+=21; }
			  while(BC()!=0 && cpirV!=_a);
			  _cycles-=5;
			  _f=(_f&CF)|HF|(cpirV==_a?ZF:0)|((cpirV-_a)&SF)|(BC()?PVF:0)|NF;
			}
			break;
		case 0xB8: // LDDR
			do {
				MemWrite(DE(), MemRead(HL()));
				SetHL(HL()-1); SetDE(DE()-1); SetBC(BC()-1);
				_cycles += 21;
			} while(BC() != 0);
			_cycles -= 5;
			_f &= ~PVF;
			break;
		case 0xB9: // CPDR
			{ uint8_t v=0;
			  do { v=MemRead(HL()); SetHL(HL()-1); SetBC(BC()-1); _cycles+=21; }
			  while(BC()!=0 && v!=_a);
			  _cycles-=5;
			  _f=(_f&CF)|HF|(v==_a?ZF:0)|((v-_a)&SF)|(BC()?PVF:0)|NF;
			}
			break;
		case 0xBA: // INDR
			do {
				uint8_t v = IoRead(BC());
				MemWrite(HL(), v);
				SetHL(HL() - 1);
				_b--;
				_f = (uint8_t)((_f & CF) | (_b ? PVF : 0) | (_b == 0 ? ZF : 0) | (_b & 0x80 ? SF : 0) | (v & 0x80 ? NF : 0));
				_cycles += 21;
			} while(_b != 0);
			_cycles -= 5;
			break;
		case 0xBB: // OTDR
			do {
				uint8_t v = MemRead(HL());
				IoWrite(BC(), v);
				SetHL(HL() - 1);
				_b--;
				_f = (uint8_t)((_f & CF) | (_b ? PVF : 0) | (_b == 0 ? ZF : 0) | (_b & 0x80 ? SF : 0) | (v & 0x80 ? NF : 0));
				_cycles += 21;
			} while(_b != 0);
			_cycles -= 5;
			break;
		case 0xB2: // INIR
			do {
				uint8_t v = IoRead(BC());
				MemWrite(HL(), v);
				SetHL(HL() + 1);
				_b--;
				_f = (uint8_t)((_f & CF) | (_b ? PVF : 0) | (_b == 0 ? ZF : 0) | (_b & 0x80 ? SF : 0) | (v & 0x80 ? NF : 0));
				_cycles += 21;
			} while(_b != 0);
			_cycles -= 5;
			break;
		case 0xB3: // OTIR
			do {
				uint8_t v = MemRead(HL());
				IoWrite(BC(), v);
				SetHL(HL() + 1);
				_b--;
				_f = (uint8_t)((_f & CF) | (_b ? PVF : 0) | (_b == 0 ? ZF : 0) | (_b & 0x80 ? SF : 0) | (v & 0x80 ? NF : 0));
				_cycles += 21;
			} while(_b != 0);
			_cycles -= 5;
			break;
		default: break;  // undefined ED opcode — NOP
	}
}

// ---------------------------------------------------------------------------
// DD prefix (IX operations)
// ---------------------------------------------------------------------------
void GenesisCpuZ80::ExecDD()
{
	uint8_t op = Fetch8(); _cycles += 4;
	switch(op) {
		case 0x09: _ix = AddHL(_ix, BC()); _cycles+=11; break;
		case 0x19: _ix = AddHL(_ix, DE()); _cycles+=11; break;
		case 0x29: _ix = AddHL(_ix, _ix); _cycles+=11; break;
		case 0x39: _ix = AddHL(_ix, _sp); _cycles+=11; break;
		case 0x21: _ix = Fetch16(); _cycles+=10; break;
		case 0x22: { uint16_t a=Fetch16(); MemWrite16(a,_ix); _cycles+=16; break; }
		case 0x2A: { uint16_t a=Fetch16(); _ix=MemRead16(a); _cycles+=16; break; }
		case 0x23: _ix++; _cycles+=6; break;
		case 0x2B: _ix--; _cycles+=6; break;
		case 0x24: _ix=(_ix&0x00FF)|(((uint16_t)Inc8((uint8_t)(_ix>>8)))<<8); _cycles+=4; break;
		case 0x25: _ix=(_ix&0x00FF)|(((uint16_t)Dec8((uint8_t)(_ix>>8)))<<8); _cycles+=4; break;
		case 0x2C: _ix=(_ix&0xFF00)|Inc8((uint8_t)_ix); _cycles+=4; break;
		case 0x2D: _ix=(_ix&0xFF00)|Dec8((uint8_t)_ix); _cycles+=4; break;
		case 0x26: _ix=(_ix&0x00FF)|((uint16_t)Fetch8()<<8); _cycles+=7; break;
		case 0x2E: _ix=(_ix&0xFF00)|Fetch8(); _cycles+=7; break;
		case 0xE1: _ix=Pop(); _cycles+=10; break;
		case 0xE5: Push(_ix); _cycles+=11; break;
		case 0xE3: { uint16_t t=MemRead16(_sp); MemWrite16(_sp,_ix); _ix=t; _cycles+=19; break; }
		case 0xE9: _pc=_ix; _cycles+=4; break;
		case 0xF9: _sp=_ix; _cycles+=6; break;
		case 0xCB: ExecDDCB(_ix); break;
		case 0x34: { int8_t d=FetchDisp(); uint16_t a=(uint16_t)(_ix+d); MemWrite(a,Inc8(MemRead(a))); _cycles+=19; break; }
		case 0x35: { int8_t d=FetchDisp(); uint16_t a=(uint16_t)(_ix+d); MemWrite(a,Dec8(MemRead(a))); _cycles+=19; break; }
		case 0x36: { int8_t d=FetchDisp(); uint8_t v=Fetch8(); MemWrite((uint16_t)(_ix+d),v); _cycles+=15; break; }
		default: {
			// LD r,(IX+d) / LD (IX+d),r / arithmetic with (IX+d)
			uint8_t hi = op >> 6, lo = op & 7, mid = (op>>3)&7;
			if(hi == 1) {  // LD
				int8_t d = (lo==6||mid==6) ? FetchDisp() : 0;
				uint8_t v; bool needDisp = (lo==6||mid==6);
				if(!needDisp) d=0;
				if(mid==6) { v=GetRIdx(lo,_ix,0); MemWrite((uint16_t)(_ix+d),v); _cycles+=15; }
				else if(lo==6) { v=MemRead((uint16_t)(_ix+d)); SetR(mid,v); _cycles+=15; }
				else { /* IX high/low ops */ v=GetRIdx(lo,_ix,d); SetRIdx(mid,v,_ix,d); _cycles+=4; }
			} else if(hi==2) {  // ALU r,(IX+d)
				int8_t d=FetchDisp(); uint8_t v=MemRead((uint16_t)(_ix+d)); _cycles+=15;
				switch(mid) {
					case 0:_a=Add8(_a,v);break;case 1:_a=Add8(_a,v,FC());break;
					case 2:_a=Sub8(_a,v);break;case 3:_a=Sub8(_a,v,FC());break;
					case 4:_a=And8(_a,v);break;case 5:_a=Xor8(_a,v);break;
					case 6:_a=Or8(_a,v); break;case 7:Cp8(_a,v);break;
				}
			}
			break;
		}
	}
}

void GenesisCpuZ80::ExecFD()
{
	// Identical structure to DD but uses _iy
	uint8_t op = Fetch8(); _cycles += 4;
	switch(op) {
		case 0x09: _iy=AddHL(_iy,BC()); _cycles+=11; break;
		case 0x19: _iy=AddHL(_iy,DE()); _cycles+=11; break;
		case 0x29: _iy=AddHL(_iy,_iy); _cycles+=11; break;
		case 0x39: _iy=AddHL(_iy,_sp); _cycles+=11; break;
		case 0x21: _iy=Fetch16(); _cycles+=10; break;
		case 0x22: { uint16_t a=Fetch16(); MemWrite16(a,_iy); _cycles+=16; break; }
		case 0x2A: { uint16_t a=Fetch16(); _iy=MemRead16(a); _cycles+=16; break; }
		case 0x23: _iy++; _cycles+=6; break;
		case 0x2B: _iy--; _cycles+=6; break;
		case 0xE1: _iy=Pop(); _cycles+=10; break;
		case 0xE5: Push(_iy); _cycles+=11; break;
		case 0xE9: _pc=_iy; _cycles+=4; break;
		case 0xF9: _sp=_iy; _cycles+=6; break;
		case 0xCB: ExecDDCB(_iy); break;
		case 0x34: { int8_t d=FetchDisp(); uint16_t a=(uint16_t)(_iy+d); MemWrite(a,Inc8(MemRead(a))); _cycles+=19; break; }
		case 0x35: { int8_t d=FetchDisp(); uint16_t a=(uint16_t)(_iy+d); MemWrite(a,Dec8(MemRead(a))); _cycles+=19; break; }
		case 0x36: { int8_t d=FetchDisp(); uint8_t v=Fetch8(); MemWrite((uint16_t)(_iy+d),v); _cycles+=15; break; }
		default: {
			uint8_t hi=op>>6, lo=op&7, mid=(op>>3)&7;
			if(hi==1) {
				int8_t d=(lo==6||mid==6)?FetchDisp():0;
				if(mid==6) { MemWrite((uint16_t)(_iy+d),GetR(lo)); _cycles+=15; }
				else if(lo==6) { SetR(mid,MemRead((uint16_t)(_iy+d))); _cycles+=15; }
			} else if(hi==2) {
				int8_t d=FetchDisp(); uint8_t v=MemRead((uint16_t)(_iy+d)); _cycles+=15;
				switch(mid) {
					case 0:_a=Add8(_a,v);break;case 1:_a=Add8(_a,v,FC());break;
					case 2:_a=Sub8(_a,v);break;case 3:_a=Sub8(_a,v,FC());break;
					case 4:_a=And8(_a,v);break;case 5:_a=Xor8(_a,v);break;
					case 6:_a=Or8(_a,v); break;case 7:Cp8(_a,v);break;
				}
			}
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Main opcode execution
// ---------------------------------------------------------------------------
void GenesisCpuZ80::ExecOne()
{
	if(_backend) _backend->Z80ProcessInstruction();

	uint8_t op = Fetch8();
	_r = (uint8_t)((_r & 0x80) | ((_r + 1) & 0x7F));  // increment R

	switch(op) {
		// NOP
		case 0x00: _cycles += 4; break;

		// LD r,r'
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x47:
		case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4F:
		case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x57:
		case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5F:
		case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x67:
		case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6F:
		case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7F:
			SetR((op>>3)&7, GetR(op&7));
			_cycles += ((op&7)==6||(((op>>3)&7)==6)) ? 7 : 4;
			break;

		// HALT
		case 0x76: _halted = true; _cycles += 4; _pc--; break;

		// LD r,(HL) and LD (HL),r handled above (r=6 case)

		// LD r,n
		case 0x06: _b=Fetch8(); _cycles+=7; break;
		case 0x0E: _c=Fetch8(); _cycles+=7; break;
		case 0x16: _d=Fetch8(); _cycles+=7; break;
		case 0x1E: _e=Fetch8(); _cycles+=7; break;
		case 0x26: _h=Fetch8(); _cycles+=7; break;
		case 0x2E: _l=Fetch8(); _cycles+=7; break;
		case 0x36: MemWrite(HL(),Fetch8()); _cycles+=10; break;
		case 0x3E: _a=Fetch8(); _cycles+=7; break;

		// LD rr,nn
		case 0x01: SetBC(Fetch16()); _cycles+=10; break;
		case 0x11: SetDE(Fetch16()); _cycles+=10; break;
		case 0x21: SetHL(Fetch16()); _cycles+=10; break;
		case 0x31: _sp=Fetch16();    _cycles+=10; break;

		// LD (nn),A / LD A,(nn)
		case 0x32: MemWrite(Fetch16(),_a); _cycles+=13; break;
		case 0x3A: _a=MemRead(Fetch16()); _cycles+=13; break;

		// LD (BC/DE),A / LD A,(BC/DE)
		case 0x02: MemWrite(BC(),_a); _cycles+=7; break;
		case 0x12: MemWrite(DE(),_a); _cycles+=7; break;
		case 0x0A: _a=MemRead(BC()); _cycles+=7; break;
		case 0x1A: _a=MemRead(DE()); _cycles+=7; break;

		// LD (nn),HL / LD HL,(nn)
		case 0x22: MemWrite16(Fetch16(),HL()); _cycles+=16; break;
		case 0x2A: SetHL(MemRead16(Fetch16())); _cycles+=16; break;

		// LD SP,HL
		case 0xF9: _sp=HL(); _cycles+=6; break;

		// INC/DEC rr
		case 0x03: SetBC(BC()+1); _cycles+=6; break;
		case 0x13: SetDE(DE()+1); _cycles+=6; break;
		case 0x23: SetHL(HL()+1); _cycles+=6; break;
		case 0x33: _sp++;         _cycles+=6; break;
		case 0x0B: SetBC(BC()-1); _cycles+=6; break;
		case 0x1B: SetDE(DE()-1); _cycles+=6; break;
		case 0x2B: SetHL(HL()-1); _cycles+=6; break;
		case 0x3B: _sp--;         _cycles+=6; break;

		// INC r
		case 0x04: _b=Inc8(_b); _cycles+=4; break;
		case 0x0C: _c=Inc8(_c); _cycles+=4; break;
		case 0x14: _d=Inc8(_d); _cycles+=4; break;
		case 0x1C: _e=Inc8(_e); _cycles+=4; break;
		case 0x24: _h=Inc8(_h); _cycles+=4; break;
		case 0x2C: _l=Inc8(_l); _cycles+=4; break;
		case 0x34: MemWrite(HL(),Inc8(MemRead(HL()))); _cycles+=11; break;
		case 0x3C: _a=Inc8(_a); _cycles+=4; break;

		// DEC r
		case 0x05: _b=Dec8(_b); _cycles+=4; break;
		case 0x0D: _c=Dec8(_c); _cycles+=4; break;
		case 0x15: _d=Dec8(_d); _cycles+=4; break;
		case 0x1D: _e=Dec8(_e); _cycles+=4; break;
		case 0x25: _h=Dec8(_h); _cycles+=4; break;
		case 0x2D: _l=Dec8(_l); _cycles+=4; break;
		case 0x35: MemWrite(HL(),Dec8(MemRead(HL()))); _cycles+=11; break;
		case 0x3D: _a=Dec8(_a); _cycles+=4; break;

		// ADD HL,rr
		case 0x09: SetHL(AddHL(HL(),BC())); _cycles+=11; break;
		case 0x19: SetHL(AddHL(HL(),DE())); _cycles+=11; break;
		case 0x29: SetHL(AddHL(HL(),HL())); _cycles+=11; break;
		case 0x39: SetHL(AddHL(HL(),_sp));  _cycles+=11; break;

		// Accumulator rotates
		case 0x07: _a=Rlca(_a); _cycles+=4; break;
		case 0x0F: _a=Rrca(_a); _cycles+=4; break;
		case 0x17: _a=Rla(_a);  _cycles+=4; break;
		case 0x1F: _a=Rra(_a);  _cycles+=4; break;

		// DJNZ
		case 0x10: { int8_t d=FetchDisp(); _b--; if(_b) { _pc=(uint16_t)(_pc+d); _cycles+=13; } else _cycles+=8; break; }

		// JR unconditional
		case 0x18: { int8_t d=FetchDisp(); _pc=(uint16_t)(_pc+d); _cycles+=12; break; }

		// JR cc
		case 0x20: { int8_t d=FetchDisp(); if(!FZ()) { _pc=(uint16_t)(_pc+d); _cycles+=12; } else _cycles+=7; break; }
		case 0x28: { int8_t d=FetchDisp(); if( FZ()) { _pc=(uint16_t)(_pc+d); _cycles+=12; } else _cycles+=7; break; }
		case 0x30: { int8_t d=FetchDisp(); if(!FC()) { _pc=(uint16_t)(_pc+d); _cycles+=12; } else _cycles+=7; break; }
		case 0x38: { int8_t d=FetchDisp(); if( FC()) { _pc=(uint16_t)(_pc+d); _cycles+=12; } else _cycles+=7; break; }

		// JP nn / JP cc,nn
		case 0xC3: _pc=Fetch16(); _cycles+=10; break;
		case 0xC2: { uint16_t a=Fetch16(); if(!FZ()) _pc=a; _cycles+=10; break; }
		case 0xCA: { uint16_t a=Fetch16(); if( FZ()) _pc=a; _cycles+=10; break; }
		case 0xD2: { uint16_t a=Fetch16(); if(!FC()) _pc=a; _cycles+=10; break; }
		case 0xDA: { uint16_t a=Fetch16(); if( FC()) _pc=a; _cycles+=10; break; }
		case 0xE2: { uint16_t a=Fetch16(); if(!FPV()) _pc=a; _cycles+=10; break; }
		case 0xEA: { uint16_t a=Fetch16(); if( FPV()) _pc=a; _cycles+=10; break; }
		case 0xF2: { uint16_t a=Fetch16(); if(!FS()) _pc=a; _cycles+=10; break; }
		case 0xFA: { uint16_t a=Fetch16(); if( FS()) _pc=a; _cycles+=10; break; }
		case 0xE9: _pc=HL(); _cycles+=4; break;

		// CALL / CALL cc
		case 0xCD: { uint16_t a=Fetch16(); Push(_pc); _pc=a; _cycles+=17; break; }
		case 0xC4: { uint16_t a=Fetch16(); if(!FZ()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xCC: { uint16_t a=Fetch16(); if( FZ()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xD4: { uint16_t a=Fetch16(); if(!FC()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xDC: { uint16_t a=Fetch16(); if( FC()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xE4: { uint16_t a=Fetch16(); if(!FPV()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xEC: { uint16_t a=Fetch16(); if( FPV()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xF4: { uint16_t a=Fetch16(); if(!FS()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }
		case 0xFC: { uint16_t a=Fetch16(); if( FS()) { Push(_pc); _pc=a; _cycles+=17; } else _cycles+=10; break; }

		// RET / RET cc
		case 0xC9: _pc=Pop(); _cycles+=10; break;
		case 0xC0: if(!FZ()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xC8: if( FZ()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xD0: if(!FC()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xD8: if( FC()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xE0: if(!FPV()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xE8: if( FPV()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xF0: if(!FS()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;
		case 0xF8: if( FS()) { _pc=Pop(); _cycles+=11; } else _cycles+=5; break;

		// RST p
		case 0xC7: Push(_pc); _pc=0x00; _cycles+=11; break;
		case 0xCF: Push(_pc); _pc=0x08; _cycles+=11; break;
		case 0xD7: Push(_pc); _pc=0x10; _cycles+=11; break;
		case 0xDF: Push(_pc); _pc=0x18; _cycles+=11; break;
		case 0xE7: Push(_pc); _pc=0x20; _cycles+=11; break;
		case 0xEF: Push(_pc); _pc=0x28; _cycles+=11; break;
		case 0xF7: Push(_pc); _pc=0x30; _cycles+=11; break;
		case 0xFF: Push(_pc); _pc=0x38; _cycles+=11; break;

		// PUSH / POP
		case 0xC1: SetBC(Pop()); _cycles+=10; break;
		case 0xD1: SetDE(Pop()); _cycles+=10; break;
		case 0xE1: SetHL(Pop()); _cycles+=10; break;
		case 0xF1: SetAF(Pop()); _cycles+=10; break;
		case 0xC5: Push(BC()); _cycles+=11; break;
		case 0xD5: Push(DE()); _cycles+=11; break;
		case 0xE5: Push(HL()); _cycles+=11; break;
		case 0xF5: Push(AF()); _cycles+=11; break;

		// ALU A,r (0x80-0xBF)
		case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
			_a=Add8(_a,GetR(op&7)); _cycles+=((op&7)==6)?7:4; break;
		case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
			_a=Add8(_a,GetR(op&7),FC()); _cycles+=((op&7)==6)?7:4; break;
		case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
			_a=Sub8(_a,GetR(op&7)); _cycles+=((op&7)==6)?7:4; break;
		case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
			_a=Sub8(_a,GetR(op&7),FC()); _cycles+=((op&7)==6)?7:4; break;
		case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
			_a=And8(_a,GetR(op&7)); _cycles+=((op&7)==6)?7:4; break;
		case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
			_a=Xor8(_a,GetR(op&7)); _cycles+=((op&7)==6)?7:4; break;
		case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
			_a=Or8(_a,GetR(op&7)); _cycles+=((op&7)==6)?7:4; break;
		case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
			Cp8(_a,GetR(op&7)); _cycles+=((op&7)==6)?7:4; break;

		// ALU A,n
		case 0xC6: _a=Add8(_a,Fetch8()); _cycles+=7; break;
		case 0xCE: _a=Add8(_a,Fetch8(),FC()); _cycles+=7; break;
		case 0xD6: _a=Sub8(_a,Fetch8()); _cycles+=7; break;
		case 0xDE: _a=Sub8(_a,Fetch8(),FC()); _cycles+=7; break;
		case 0xE6: _a=And8(_a,Fetch8()); _cycles+=7; break;
		case 0xEE: _a=Xor8(_a,Fetch8()); _cycles+=7; break;
		case 0xF6: _a=Or8 (_a,Fetch8()); _cycles+=7; break;
		case 0xFE: Cp8(_a,Fetch8()); _cycles+=7; break;

		// Misc
		case 0x08: { uint8_t t=_a; _a=_a2; _a2=t; t=_f; _f=_f2; _f2=t; _cycles+=4; break; }  // EX AF,AF'
		case 0xD9: { // EXX
			uint8_t t;
			t=_b;_b=_b2;_b2=t; t=_c;_c=_c2;_c2=t;
			t=_d;_d=_d2;_d2=t; t=_e;_e=_e2;_e2=t;
			t=_h;_h=_h2;_h2=t; t=_l;_l=_l2;_l2=t;
			_cycles+=4; break;
		}
		case 0xE3: { uint16_t t=MemRead16(_sp); MemWrite16(_sp,HL()); SetHL(t); _cycles+=19; break; } // EX (SP),HL
		case 0xEB: { uint8_t t=_d;_d=_h;_h=t; t=_e;_e=_l;_l=t; _cycles+=4; break; }  // EX DE,HL
		case 0xF3: _iff1=_iff2=false; _cycles+=4; break;  // DI
		case 0xFB: _iff1=_iff2=true; _afterEI=true; _cycles+=4; break;  // EI

		// SCF / CCF / CPL / DAA / NEG
		case 0x37: _f=(_f&(SF|ZF|PVF))|CF; _cycles+=4; break;  // SCF
		case 0x3F: _f=(uint8_t)((_f&(SF|ZF|PVF))|(FC()?HF:CF)); _cycles+=4; break;  // CCF
		case 0x2F: _a=~_a; _f=(_f&(SF|ZF|PVF|CF))|HF|NF; _cycles+=4; break;  // CPL
		case 0x27: { // DAA
			uint8_t corr=0; bool cn=FN(), ch=FH(), cc=FC();
			if(!cn) { if(ch||((_a&0x0F)>9)) corr|=0x06; if(cc||_a>0x99) { corr|=0x60; } }
			else    { if(ch) corr|=0x06; if(cc) corr|=0x60; }
			_a=(uint8_t)(!cn?_a+corr:_a-corr);
			_f=(uint8_t)((_a&SF)|(_a==0?ZF:0)|ParityTable[_a]|(_f&NF)|(((_a^(_a-corr))>>4)&1?HF:0)|(cc|(_a>0x99)?CF:0));
			_cycles+=4; break;
		}

		// IN/OUT (Z80 I/O ports)
		case 0xDB: { uint8_t n = Fetch8(); _a = IoRead((uint16_t)(((uint16_t)_a << 8) | n)); _cycles+=11; break; }  // IN A,(n)
		case 0xD3: { uint8_t n = Fetch8(); IoWrite((uint16_t)(((uint16_t)_a << 8) | n), _a); _cycles+=11; break; }  // OUT (n),A

		// Prefixes
		case 0xCB: ExecCB(); break;
		case 0xED: ExecED(); break;
		case 0xDD: ExecDD(); break;
		case 0xFD: ExecFD(); break;

		default: _cycles += 4; break;  // Unknown opcode: NOP
	}
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
int32_t GenesisCpuZ80::Run(int32_t cycles)
{
	_cycles = 0;
	while(_cycles < cycles) {
		// NMI — highest priority
		if(_nmiPending) {
			_nmiPending = false;
			_halted = false;
			_iff1 = false;
			Push(_pc);
			_pc = 0x0066;
			_cycles += 11;
			continue;
		}
		// INT
		if(_intPending && _iff1 && !_afterEI) {
			_halted = false;
			_iff1 = _iff2 = false;
			switch(_im) {
				case 0:  // IM0 — execute RST 38h
				case 1:  Push(_pc); _pc = 0x0038; _cycles += 13; break;
				case 2:  { uint16_t vec = (uint16_t)(_i << 8) | 0xFF; Push(_pc); _pc = MemRead16(vec); _cycles += 19; break; }
			}
			_intPending = false;
			continue;
		}
		_afterEI = false;
		if(_halted) { _cycles += 4; continue; }
		ExecOne();
	}
	return _cycles - cycles;
}

// ---------------------------------------------------------------------------
// Interrupt lines
// ---------------------------------------------------------------------------
void GenesisCpuZ80::SetNMI(bool level) { if(level) _nmiPending = true; }
void GenesisCpuZ80::SetINT(bool level) { _intPending = level; }

// ---------------------------------------------------------------------------
// State capture / restore
// ---------------------------------------------------------------------------
GenesisCpuZ80::Z80State GenesisCpuZ80::CaptureState() const
{
	Z80State s;
	s.A=_a; s.F=_f; s.B=_b; s.C=_c; s.D=_d; s.E=_e; s.H=_h; s.L=_l;
	s.A2=_a2; s.F2=_f2; s.B2=_b2; s.C2=_c2; s.D2=_d2; s.E2=_e2; s.H2=_h2; s.L2=_l2;
	s.IX=_ix; s.IY=_iy; s.SP=_sp; s.PC=_pc;
	s.I=_i; s.R=_r; s.IFF1=_iff1; s.IFF2=_iff2; s.IM=_im; s.Halted=_halted;
	s.BankReg=_bankReg; s.BankBit=_bankBit; s.CycleCount=0;
	return s;
}

void GenesisCpuZ80::RestoreState(const Z80State& s)
{
	_a=s.A; _f=s.F; _b=s.B; _c=s.C; _d=s.D; _e=s.E; _h=s.H; _l=s.L;
	_a2=s.A2; _f2=s.F2; _b2=s.B2; _c2=s.C2; _d2=s.D2; _e2=s.E2; _h2=s.H2; _l2=s.L2;
	_ix=s.IX; _iy=s.IY; _sp=s.SP; _pc=s.PC;
	_i=s.I; _r=s.R; _iff1=s.IFF1; _iff2=s.IFF2; _im=s.IM; _halted=s.Halted;
	_bankReg=s.BankReg; _bankBit=s.BankBit;
}

// ---------------------------------------------------------------------------
// Save / Load state
// ---------------------------------------------------------------------------
void GenesisCpuZ80::SaveState(vector<uint8_t>& out) const
{
	Z80State s = CaptureState();
	AppV(out, s);
	AppV(out, _ram);
}

bool GenesisCpuZ80::LoadState(const vector<uint8_t>& data, size_t& offset)
{
	Z80State s;
	if(!RdV(data, offset, s)) return false;
	RestoreState(s);
	uint8_t tmp[0x2000];
	if(!RdV(data, offset, tmp)) return false;
	memcpy(_ram, tmp, 0x2000);
	return true;
}
