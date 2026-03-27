// 68Kdecoder.cpp -- extracted Motorola M68000 decode/dispatch handlers

#include "pch.h"
#include "Genesis/GenesisCpu68k.h"
#include "Shared/MemoryOperationType.h"

void GenesisCpu68k::BuildTable()
{
	for(int i = 0; i < 256; i++) {
		uint8_t hi = (uint8_t)i;
		if(hi <= 0x0F)      _opTable[i] = &GenesisCpu68k::I_Group0;
		else if(hi <= 0x3F) _opTable[i] = &GenesisCpu68k::I_Move;
		else if(hi <= 0x4F) _opTable[i] = &GenesisCpu68k::I_Group4;
		else if(hi <= 0x5F) _opTable[i] = &GenesisCpu68k::I_Group5;
		else if(hi <= 0x6F) _opTable[i] = &GenesisCpu68k::I_Group6;
		else if(hi <= 0x7F) _opTable[i] = &GenesisCpu68k::I_Moveq;
		else if(hi <= 0x8F) _opTable[i] = &GenesisCpu68k::I_Group8;
		else if(hi <= 0x9F) _opTable[i] = &GenesisCpu68k::I_Group9;
		else if(hi <= 0xAF) _opTable[i] = &GenesisCpu68k::I_ALine;
		else if(hi <= 0xBF) _opTable[i] = &GenesisCpu68k::I_GroupB;
		else if(hi <= 0xCF) _opTable[i] = &GenesisCpu68k::I_GroupC;
		else if(hi <= 0xDF) _opTable[i] = &GenesisCpu68k::I_GroupD;
		else if(hi <= 0xEF) _opTable[i] = &GenesisCpu68k::I_GroupE;
		else                _opTable[i] = &GenesisCpu68k::I_FLine;
	}
}

void GenesisCpu68k::StaticInit()
{
	BuildTable();
}

void GenesisCpu68k::DoMove(uint8_t size, uint8_t dstMode, uint8_t dstReg,
                            uint8_t srcMode, uint8_t srcReg)
{
	// Set up fault PC override for source EA address errors.
	// -(An) source for MOVE.l faults before the destination extension words are
	// fetched, so the stacked PC still points to the first word after the opcode.
	// Smaller MOVE sizes keep the later prefetch timing.
	// (xxx).w and (xxx).l source: handled by SetFaultPCForEA.
	if(srcMode == 4 && !_faultPcOverrideValid) {
		_faultPcOverride = ((size == 4 ? _state.PC : (_state.PC + 2u))) & 0x00FFFFFFu;
		_faultPcOverrideValid = true;
	} else {
		SetFaultPCForEA(srcMode, srcReg, size);
	}

	// On 68000, MOVE updates CCR before any destination write.  If the destination
	// write triggers an address error, the CCR update is still committed.
	// _faultPreserveSr prevents CheckAddressError from rolling back SR.
	//
	// Destination fault PC rules (68000 bus-cycle timing):
	//   - If destination CalcEA consumed extension words: saved PC = _state.PC
	//     (already past the extension; no additional prefetch advance).
	//   - If no destination extension words: saved PC = _state.PC + 2
	//     (prefetch has already loaded one word past the current PC).
	// Detect this by comparing _state.PC before and after CalcEA.
	//
	// Destination (An)+ postincrement: on the 68000, the register is NOT
	// incremented when the write bus cycle faults.  Restore it if needed.

	auto doWritePhase = [&](auto v, uint8_t sz) {
		if(dstMode == 4 && sz != 4) {
			_faultStatusWordPreDecrement = true;
			_faultPreDecPCOffset = 0u;
		}
		uint32_t pcBefore = _state.PC;
		uint32_t savedDstA = ((dstMode == 3) || (dstMode == 4)) ? _state.A[dstReg] : 0u;
		uint32_t ea = CalcEA(dstMode, dstReg, sz);
		{
			bool extConsumed = (_state.PC != pcBefore);
				if(sz == 4 &&
				   (ea & 1u) != 0u &&
				   (((dstMode == 2 || dstMode == 3) && !extConsumed) ||
					(dstMode == 7 && dstReg == 1 && srcMode == 3))) {
					// For odd long destination address errors, the sampled 68000 corpus
					// reflects N from the low halfword write for the simple register
					// indirect cases and for the postincrement-to-abs.l path.
					SetN((((uint32_t)v) & 0x00008000u) != 0u);
				}
				bool preserveSrOnDestFault = true;
				bool restoreSrXOnlyOnDestFault = false;
				uint16_t restoreSrMaskOnDestFault = 0;
				if(sz == 4) {
					// MOVE.l does not commit NZVC on every destination fault. The
					// corpus matches committing them only once the pipeline had an
					// extra cycle between the source read and the faulting write.
					preserveSrOnDestFault =
						(dstMode == 4) ||
						extConsumed ||
						(srcMode >= 2 && !(srcMode == 7 && srcReg == 4));
					if(!preserveSrOnDestFault &&
					   srcMode >= 2 &&
					   srcMode != 3 &&
					   srcMode != 6 &&
					   !(srcMode == 7 && srcReg == 4)) {
						restoreSrXOnlyOnDestFault = true;
					}
						// Extension-consuming register/immediate sources still keep
						// pre-fault V/C in most cases, but absolute destinations are the
						// notable exception: they want the computed MOVE CCR state instead.
						if(extConsumed &&
						   (srcMode == 0 || srcMode == 1 || (srcMode == 7 && srcReg == 4)) &&
						   !(dstMode == 7 && (dstReg == 0 || dstReg == 1))) {
							restoreSrMaskOnDestFault |= (SR_V | SR_C);
						}
					// A successful longword source postincrement stays committed even
					// if the later destination write faults.
						_faultPreserveAddressRegs = (srcMode == 3);
						if(dstMode == 4) {
							_faultAddressOverride = ea + 2u;
							_faultAddressOverrideValid = true;
						}
				}
			_faultPreserveSr = preserveSrOnDestFault;
			_faultRestoreSrXOnly = restoreSrXOnlyOnDestFault;
			_faultRestoreSrMask = restoreSrMaskOnDestFault;
			if(dstMode == 7 && dstReg == 1) {
				// abs.l dest: timing depends on whether the source consumed a
				// memory bus cycle.  Register sources (Dn/An, mode 0/1) let the
				// prefetch advance past both dest ext words before the write, so
				// the saved PC = _state.PC (past both ext words).  Memory sources
				// introduce a bus cycle that delays the second ext-word prefetch,
				// so the saved PC reflects only the first ext word consumed.
				if(srcMode == 0 || srcMode == 1) {
					_faultPcOverride = _state.PC & 0x00FFFFFFu;
				} else {
					_faultPcOverride = (pcBefore + 2u) & 0x00FFFFFFu;
				}
			} else {
				_faultPcOverride = (extConsumed ? _state.PC : (_state.PC + 2u)) & 0x00FFFFFFu;
			}
			_faultPcOverrideValid = true;
		}
		if constexpr (sizeof(v) == 1)      WriteResolvedEA8 (ea, (uint8_t) v);
		else if constexpr (sizeof(v) == 2) WriteResolvedEA16(ea, (uint16_t)v);
		else                               WriteResolvedEA32(ea, (uint32_t)v);
		if(_exceptionTaken && (dstMode == 3 || (dstMode == 4 && sz == 4))) {
			_state.A[dstReg] = savedDstA;
			if(dstReg == 7) _state.SP = _state.A[7];
		}
	};

	if(size == 1) {
		uint8_t v = ReadEA8(srcMode, srcReg);
		if(_exceptionTaken) return;
		UpdateFlagsNZ8(v); SetC(false); SetV(false);
		doWritePhase(v, 1);
	} else if(size == 2) {
		uint16_t v = ReadEA16(srcMode, srcReg);
		if(_exceptionTaken) return;
		UpdateFlagsNZ16(v); SetC(false); SetV(false);
		doWritePhase(v, 2);
	} else {
		uint32_t v = ReadEA32(srcMode, srcReg);
		if(_exceptionTaken) return;
		UpdateFlagsNZ32(v); SetC(false); SetV(false);
		doWritePhase(v, 4);
	}
	_cycles += 4;
}

void GenesisCpu68k::DoBitOp(uint8_t op, uint8_t mode, uint8_t reg, uint32_t bitNum)
{
	bool isDn = (mode == 0);
	if(isDn) {
		bitNum &= 31;
		uint32_t mask = 1u << bitNum;
		SetZ((_state.D[reg] & mask) == 0);
		switch(op) {
			case 0: break;                               // BTST
			case 1: _state.D[reg] ^= mask; break;       // BCHG
			case 2: _state.D[reg] &= ~mask; break;      // BCLR
			case 3: _state.D[reg] |= mask; break;       // BSET
			}
			_cycles += (op == 0) ? 6 : 8;
	} else {
		if(mode == 7 && reg == 4 && op == 0) {
			// Illegal BTST Dn,# form consumes the extension word and tests its low byte.
			bitNum &= 7;
			uint8_t v = (uint8_t)(FetchExtWord() & 0x00FFu);
			uint8_t mask = (uint8_t)(1u << bitNum);
			SetZ((v & mask) == 0);
			return;
		}
		bitNum &= 7;
		uint32_t ea = CalcEA(mode, reg, 1);
		uint8_t  v  = BusRead8(ea);
		uint8_t  mask = (uint8_t)(1u << bitNum);
		SetZ((v & mask) == 0);
		switch(op) {
			case 0: break;                       // BTST
			case 1: BusWrite8(ea, v ^ mask); break; // BCHG
			case 2: BusWrite8(ea, v & ~mask); break; // BCLR
			case 3: BusWrite8(ea, v | mask); break;  // BSET
		}
		_cycles += (op == 0) ? 4 : 8;
	}
}

void GenesisCpu68k::DoShiftRotate(bool left, bool isArith, bool isRotate, bool isRox,
                                   uint8_t size, uint8_t mode, uint8_t reg,
                                   uint8_t count, bool countIsReg)
{
	uint8_t n = countIsReg ? (uint8_t)(_state.D[count] & 63) : count;
	if(!countIsReg && n == 0) n = 8;  // immediate count=0 encodes 8

	bool   carry    = false;
	bool   overflow = false;
	bool   x        = GetX();
	uint32_t memEa  = 0;

	_cycles += 8 + 2 * n;

	// Register-count shifts/rotates with Dn count=0 do not shift.
	if(mode == 0 && countIsReg && n == 0) {
		if(size == 0)      UpdateFlagsNZ8((uint8_t)_state.D[reg]);
		else if(size == 1) UpdateFlagsNZ16((uint16_t)_state.D[reg]);
		else               UpdateFlagsNZ32(_state.D[reg]);

		// For ROXL/ROXR with zero count: C mirrors X; otherwise C is cleared.
		SetC(isRox ? GetX() : false);
		SetV(false);
		return;
	}

	if(mode != 0) {
		// Memory shift (size forced to word, count forced to 1)
		SetFaultPCForEA(mode, reg, 2u);
		n    = 1;
		size = 1;
		memEa = CalcEA(mode, reg, 2);
		if(_exceptionTaken) {
			return;
		}
		_cycles -= 2 * (n - 1);
	}

	if(size == 0) {  // byte
		uint8_t ea = (mode == 0) ? (uint8_t)_state.D[reg] : ReadResolvedEA8(memEa);
		uint8_t r;
		if(isRox)        r = left ? Roxl8(ea, n, carry, x)  : Roxr8(ea, n, carry, x);
		else if(isRotate)r = left ? Rol8(ea, n, carry)      : Ror8(ea, n, carry);
		else if(isArith) r = left ? Asl8(ea, n, carry, overflow) : Asr8(ea, n, carry, overflow);
		else             r = left ? Lsl8(ea, n, carry)      : Lsr8(ea, n, carry);
		if(mode == 0) _state.D[reg] = (_state.D[reg] & 0xFFFFFF00u) | r;
		else          WriteResolvedEA8(memEa, r);
		UpdateFlagsNZ8(r); SetC(carry); if(!isRotate) SetX(carry); SetV(overflow);
	} else if(size == 1) {  // word
		uint16_t ea = (mode == 0) ? (uint16_t)_state.D[reg] : ReadResolvedEA16(memEa);
		if(_exceptionTaken) {
			return;
		}
		uint16_t r;
		if(isRox)        r = left ? Roxl16(ea, n, carry, x)  : Roxr16(ea, n, carry, x);
		else if(isRotate)r = left ? Rol16(ea, n, carry)      : Ror16(ea, n, carry);
		else if(isArith) r = left ? Asl16(ea, n, carry, overflow) : Asr16(ea, n, carry, overflow);
		else             r = left ? Lsl16(ea, n, carry)      : Lsr16(ea, n, carry);
		if(mode == 0) _state.D[reg] = (_state.D[reg] & 0xFFFF0000u) | r;
		else          WriteResolvedEA16(memEa, r);
		UpdateFlagsNZ16(r); SetC(carry); if(!isRotate) SetX(carry); SetV(overflow);
	} else {  // long
		uint32_t ea = _state.D[reg];
		uint32_t r;
		if(isRox)        r = left ? Roxl32(ea, n, carry, x)  : Roxr32(ea, n, carry, x);
		else if(isRotate)r = left ? Rol32(ea, n, carry)      : Ror32(ea, n, carry);
		else if(isArith) r = left ? Asl32(ea, n, carry, overflow) : Asr32(ea, n, carry, overflow);
		else             r = left ? Lsl32(ea, n, carry)      : Lsr32(ea, n, carry);
		_state.D[reg] = r;
		UpdateFlagsNZ32(r); SetC(carry); if(!isRotate) SetX(carry); SetV(overflow);
	}
}

// ===========================================================================
// I_Group0  $00-$0F  — immediate ops, bit ops, MOVEP
// ===========================================================================

void GenesisCpu68k::I_Group0()
{
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;
	bool    bit8 = (_opword >> 8) & 1;

	if(bit8) {
		// Register bit ops or MOVEP
		uint8_t dn    = (_opword >> 9) & 7;
		uint8_t bitop = (_opword >> 6) & 3;

		if(mode == 1) {
			// MOVEP
			bool    toLong = (bitop >> 1) & 1;
			bool    toReg  = !(bitop & 1);  // 0=mem->Dn (from lower bit = direction)
			// Encoding: 0000 ddd1 op 001 an
			// bit 7 of opword (within lower byte): direction; bit 6 = long/word
			// Re-decode from opword bits:
			// 0000 ddd1 op mmm rrr; when mmm=001 it's MOVEP
			// bits 8-7: bit8=1, bit7 = opword>>7&1
			toReg  = (((_opword >> 7) & 1) == 0);
			toLong = ((_opword >> 6) & 1) != 0;

			uint32_t addr = (_state.A[reg] + (int16_t)FetchExtWord()) & 0x00FFFFFFu;
			if(toLong) {
				if(toReg) {
					uint32_t v = ((uint32_t)BusRead8(addr)   << 24) |
					             ((uint32_t)BusRead8(addr+2) << 16) |
					             ((uint32_t)BusRead8(addr+4) <<  8) |
					              (uint32_t)BusRead8(addr+6);
					_state.D[dn] = v;
				} else {
					BusWrite8(addr,   (_state.D[dn] >> 24) & 0xFF);
					BusWrite8(addr+2, (_state.D[dn] >> 16) & 0xFF);
					BusWrite8(addr+4, (_state.D[dn] >>  8) & 0xFF);
					BusWrite8(addr+6,  _state.D[dn]        & 0xFF);
				}
				_cycles += 24;
			} else {
				if(toReg) {
					uint16_t v = ((uint16_t)BusRead8(addr) << 8) | BusRead8(addr+2);
					_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | v;
				} else {
					BusWrite8(addr,   (_state.D[dn] >> 8) & 0xFF);
					BusWrite8(addr+2,  _state.D[dn]       & 0xFF);
				}
				_cycles += 16;
			}
			return;
		}

		// BTST/BCHG/BCLR/BSET Dn,<ea>
		uint32_t bitNum = _state.D[dn];
		DoBitOp(bitop, mode, reg, bitNum);
		return;
	}

	// Immediate operations
	uint8_t op = (_opword >> 9) & 7;

	if(op == 4) {
		// BTST/BCHG/BCLR/BSET #imm,<ea>
		uint32_t bitNum = FetchExtWord() & 0x1F;
		DoBitOp(sz, mode, reg, bitNum);
		return;
	}

	// Read immediate data
	uint32_t imm;
	if(sz == 0)      imm = FetchExtWord() & 0xFF;
	else if(sz == 1) imm = FetchExtWord();
	else             imm = FetchExtLong();

	// Special cases: ORI/ANDI/EORI to CCR or SR
	auto handleCcrSr = [&]() -> bool {
		bool toCCR = (mode == 7 && reg == 4 && sz == 0);
		bool toSR  = (mode == 7 && reg == 4 && sz == 1);
		if(toCCR) {
			if(op == 0) SetCCR((_state.SR & 0x1F) |  (uint8_t)imm);    // ORI
			if(op == 1) SetCCR((_state.SR & 0x1F) &  (uint8_t)imm);    // ANDI
			if(op == 5) SetCCR((_state.SR & 0x1F) ^  (uint8_t)imm);    // EORI
			_cycles += 20; return true;
		}
		if(toSR) {
			if(!GetS()) {
				TakeException(8); // privilege violation
				return true;
			}
			if(op == 0) SetSR(_state.SR | (uint16_t)imm);              // ORI
			if(op == 1) SetSR(_state.SR & (uint16_t)imm);              // ANDI
			if(op == 5) SetSR(_state.SR ^ (uint16_t)imm);              // EORI
			_cycles += 20; return true;
		}
		return false;
	};

	bool carry = false, overflow = false;
	switch(op) {
		case 0: {  // ORI
			if(handleCcrSr()) return;
			if(sz == 0) {
				SetFaultPCForImmediateDestEA(mode, reg, 1u);
				uint32_t ea = CalcEA(mode, reg, 1);
				uint8_t r = (uint8_t)(ReadResolvedEA8(ea) | (uint8_t)imm);
				WriteResolvedEA8(ea, r);
				UpdateFlagsNZ8(r);
			} else if(sz == 1) {
				SetFaultPCForImmediateDestEA(mode, reg, 2u);
				uint32_t ea = CalcEA(mode, reg, 2);
				uint16_t r = (uint16_t)(ReadResolvedEA16(ea) | (uint16_t)imm);
				WriteResolvedEA16(ea, r);
				UpdateFlagsNZ16(r);
			} else {
				SetFaultPCForImmediateDestEA(mode, reg, 4u);
				uint32_t ea = CalcEA(mode, reg, 4);
				uint32_t r = ReadResolvedEA32(ea) | imm;
				WriteResolvedEA32(ea, r);
				UpdateFlagsNZ32(r);
			}
			SetC(false); SetV(false); _cycles += 8; break;
		}
		case 1: {  // ANDI
			if(handleCcrSr()) return;
			if(sz == 0) {
				SetFaultPCForImmediateDestEA(mode, reg, 1u);
				uint32_t ea = CalcEA(mode, reg, 1);
				uint8_t r = (uint8_t)(ReadResolvedEA8(ea) & (uint8_t)imm);
				WriteResolvedEA8(ea, r);
				UpdateFlagsNZ8(r);
			} else if(sz == 1) {
				SetFaultPCForImmediateDestEA(mode, reg, 2u);
				uint32_t ea = CalcEA(mode, reg, 2);
				uint16_t r = (uint16_t)(ReadResolvedEA16(ea) & (uint16_t)imm);
				WriteResolvedEA16(ea, r);
				UpdateFlagsNZ16(r);
			} else {
				SetFaultPCForImmediateDestEA(mode, reg, 4u);
				uint32_t ea = CalcEA(mode, reg, 4);
				uint32_t r = ReadResolvedEA32(ea) & imm;
				WriteResolvedEA32(ea, r);
				UpdateFlagsNZ32(r);
			}
			SetC(false); SetV(false); _cycles += 8; break;
		}
		case 2: {  // SUBI
			if(sz == 0) {
				SetFaultPCForImmediateDestEA(mode, reg, 1u);
				uint32_t ea = CalcEA(mode, reg, 1);
				uint8_t a = ReadResolvedEA8(ea), b = (uint8_t)imm;
				uint8_t r = Sub8(a, b, false, carry, overflow);
				WriteResolvedEA8(ea, r);
				UpdateFlagsNZ8(r);
			} else if(sz == 1) {
				SetFaultPCForImmediateDestEA(mode, reg, 2u);
				uint32_t ea = CalcEA(mode, reg, 2);
				uint16_t a = ReadResolvedEA16(ea), b = (uint16_t)imm;
				uint16_t r = Sub16(a, b, false, carry, overflow);
				WriteResolvedEA16(ea, r);
				UpdateFlagsNZ16(r);
			} else {
				SetFaultPCForImmediateDestEA(mode, reg, 4u);
				uint32_t ea = CalcEA(mode, reg, 4);
				uint32_t a = ReadResolvedEA32(ea), b = imm;
				uint32_t r = Sub32(a, b, false, carry, overflow);
				WriteResolvedEA32(ea, r);
				UpdateFlagsNZ32(r);
			}
			SetC(carry); SetV(overflow); SetX(carry); _cycles += 8; break;
		}
		case 3: {  // ADDI
			if(sz == 0) {
				SetFaultPCForImmediateDestEA(mode, reg, 1u);
				uint32_t ea = CalcEA(mode, reg, 1);
				uint8_t a = ReadResolvedEA8(ea), b = (uint8_t)imm;
				uint8_t r = Add8(a, b, false, carry, overflow);
				WriteResolvedEA8(ea, r);
				UpdateFlagsNZ8(r);
			} else if(sz == 1) {
				SetFaultPCForImmediateDestEA(mode, reg, 2u);
				uint32_t ea = CalcEA(mode, reg, 2);
				uint16_t a = ReadResolvedEA16(ea), b = (uint16_t)imm;
				uint16_t r = Add16(a, b, false, carry, overflow);
				WriteResolvedEA16(ea, r);
				UpdateFlagsNZ16(r);
			} else {
				SetFaultPCForImmediateDestEA(mode, reg, 4u);
				uint32_t ea = CalcEA(mode, reg, 4);
				uint32_t a = ReadResolvedEA32(ea), b = imm;
				uint32_t r = Add32(a, b, false, carry, overflow);
				WriteResolvedEA32(ea, r);
				UpdateFlagsNZ32(r);
			}
			SetC(carry); SetV(overflow); SetX(carry);
			// Under Mesen's interpreter the opcode/immediate fetches are charged
			// separately, so long immediate-to-Dn ADDI should only add the ALU
			// body cost here to stay aligned with BlastEm's total timing.
			_cycles += (sz == 2 && mode == 0) ? 4 : 8;
			break;
		}
		case 5: {  // EORI
			if(handleCcrSr()) return;
			if(sz == 0) {
				SetFaultPCForImmediateDestEA(mode, reg, 1u);
				uint32_t ea = CalcEA(mode, reg, 1);
				uint8_t r = (uint8_t)(ReadResolvedEA8(ea) ^ (uint8_t)imm);
				WriteResolvedEA8(ea, r);
				UpdateFlagsNZ8(r);
			} else if(sz == 1) {
				SetFaultPCForImmediateDestEA(mode, reg, 2u);
				uint32_t ea = CalcEA(mode, reg, 2);
				uint16_t r = (uint16_t)(ReadResolvedEA16(ea) ^ (uint16_t)imm);
				WriteResolvedEA16(ea, r);
				UpdateFlagsNZ16(r);
			} else {
				SetFaultPCForImmediateDestEA(mode, reg, 4u);
				uint32_t ea = CalcEA(mode, reg, 4);
				uint32_t r = ReadResolvedEA32(ea) ^ imm;
				WriteResolvedEA32(ea, r);
				UpdateFlagsNZ32(r);
			}
			SetC(false); SetV(false); _cycles += 8; break;
		}
		case 6: {  // CMPI
			if(sz == 0) {
				SetFaultPCForImmediateDestEA(mode, reg, 1u);
				uint8_t a = ReadEA8(mode, reg), b = (uint8_t)imm;
				Sub8(a, b, false, carry, overflow);
				UpdateFlagsNZ8((uint8_t)(a - b));
			} else if(sz == 1) {
				SetFaultPCForImmediateDestEA(mode, reg, 2u);
				uint16_t a = ReadEA16(mode, reg), b = (uint16_t)imm;
				Sub16(a, b, false, carry, overflow);
				UpdateFlagsNZ16((uint16_t)(a - b));
			} else {
				SetFaultPCForImmediateDestEA(mode, reg, 4u);
				uint32_t a = ReadEA32(mode, reg), b = imm;
				Sub32(a, b, false, carry, overflow);
				UpdateFlagsNZ32(a - b);
			}
			SetC(carry); SetV(overflow); _cycles += 8; break;
		}
		default: TakeException(4); break;
	}
}

// ===========================================================================
// I_Move  $10-$3F  — MOVE.B / MOVE.W / MOVE.L
// ===========================================================================

void GenesisCpu68k::I_Move()
{
	// Opword format: 00ss dddDDD mmmRRR
	// ss = size (01=byte, 11=word, 10=long)
	// ddd DDD = destination mode/reg
	// mmm RRR = source mode/reg
	uint8_t ss      = (_opword >> 12) & 3;
	uint8_t dstMode = (_opword >> 6)  & 7;
	uint8_t dstReg  = (_opword >> 9)  & 7;
	uint8_t srcMode = (_opword >> 3)  & 7;
	uint8_t srcReg  = _opword & 7;

	uint8_t size = (ss == 1) ? 1 : (ss == 3) ? 2 : 4;  // byte/word/long

	// MOVEA uses An as destination (dstMode==1), does not update flags
	if(dstMode == 1) {
		SetFaultPCForEA(srcMode, srcReg, (size == 2) ? 2u : 4u);
		if(size == 2) {
			int32_t v = (int32_t)(int16_t)ReadEA16(srcMode, srcReg);
			if(_exceptionTaken) return;
			_state.A[dstReg] = (uint32_t)v;
			if(dstReg == 7) _state.SP = _state.A[7];
		} else {
			uint32_t v = ReadEA32(srcMode, srcReg);
			if(_exceptionTaken) return;
			_state.A[dstReg] = v;
			if(dstReg == 7) _state.SP = v;
		}
		_cycles += 4;
		return;
	}

	DoMove(size, dstMode, dstReg, srcMode, srcReg);
}


// ---------------------------------------------------------------------------
// SetFaultPCForEA
// ---------------------------------------------------------------------------
void GenesisCpu68k::SetFaultPCForEA(uint8_t mode, uint8_t reg, uint8_t size)
{
	if(_faultPcOverrideValid) return;
	if(mode == 7 && reg == 1) {
		// (xxx).l: two extension words -> saved PC = _preExecPC+6
		_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
		_faultPcOverrideValid = true;
	} else if((mode == 4 && size < 4) || (mode == 7 && reg == 0)) {
		// -(An) byte/word, (xxx).w -> saved PC = _preExecPC+4
		_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
		_faultPcOverrideValid = true;
	}
}

void GenesisCpu68k::SetFaultPCForImmediateDestEA(uint8_t mode, uint8_t reg, uint8_t sizeBytes)
{
	if(_faultPcOverrideValid) return;
	if(mode >= 2 && (mode < 7 || reg <= 1)) {
		uint32_t savedPc = _state.PC & 0x00FFFFFFu;
		if(mode == 7 && reg == 0) {
			savedPc = (savedPc + 2) & 0x00FFFFFFu;
		} else if(mode == 7 && reg == 1) {
			savedPc = (savedPc + 4) & 0x00FFFFFFu;
		} else if(mode == 4 && sizeBytes < 4) {
			savedPc = (savedPc + 2) & 0x00FFFFFFu;
		}
		_faultPcOverride = savedPc;
		_faultPcOverrideValid = true;
	}
}

// ===========================================================================
// I_Group4  $40-$4F  — miscellaneous
// ===========================================================================

void GenesisCpu68k::I_Group4()
{
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;

	// --- Fully-fixed opcodes first ---
	if(_opword == 0x4E71) { _cycles += 4; return; }                    // NOP
	if(_opword == 0x4E75) { // RTS
		uint32_t retPc = Pull32();
		if((retPc & 1u) != 0u) {
			_state.PC = retPc & 0x00FFFFFFu;
			_faultPcOverride = (_preExecPC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			_faultPreserveAddressRegs = true;
			CheckAddressError(retPc, false, 2, MemoryOperationType::ExecOpCode);
			_cycles += 16;
			return;
		}
		_state.PC = retPc & 0x00FFFFFFu;
		_cycles += 16;
		return;
	}
	if(_opword == 0x4E73) {                                             // RTE
		if(!GetS()) {
			TakeException(8); // privilege violation
			return;
		}
		// Pop the full exception frame from the current (supervisor) stack first.
		// Applying SR before pulling PC can swap A7 to USP and corrupt the return.
		uint16_t newSr = Pull16();
		uint32_t newPc = Pull32();
		SetSR(newSr);
		if((newPc & 1u) != 0u) {
			_state.PC = newPc & 0x00FFFFFFu;
			_faultPcOverride = (_preExecPC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			_faultPreserveSr = true;
			_faultPreserveAddressRegs = true;
			CheckAddressError(newPc, false, 2, MemoryOperationType::ExecOpCode);
			_cycles += 20;
			return;
		}
		_state.PC = newPc & 0x00FFFFFFu;
		_cycles += 20;
		return;
	}
	if(_opword == 0x4E77) {                                             // RTR
		SetCCR((uint8_t)(Pull16() & 0x1F));
		uint32_t newPc = Pull32();
		if((newPc & 1u) != 0u) {
			_state.PC = newPc & 0x00FFFFFFu;
			_faultPcOverride = (_preExecPC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			_faultPreserveSr = true;
			_faultPreserveAddressRegs = true;
			CheckAddressError(newPc, false, 2, MemoryOperationType::ExecOpCode);
			_cycles += 20;
			return;
		}
		_state.PC = newPc & 0x00FFFFFFu;
		_cycles += 20;
		return;
	}
	if(_opword == 0x4E70) {                                              // RESET
		if(!GetS()) {
			TakeException(8); // privilege violation
			return;
		}
		_cycles += 132;
		return;
	}
	if(_opword == 0x4AFC) { TakeException(4); return; }                 // ILLEGAL
	if(_opword == 0x4E76) { // TRAPV
		if(GetV()) {
			_exceptionPcOverride = _state.PC & 0x00FFFFFFu;
			_exceptionPcOverrideValid = true;
			TakeException(7);
		}
		_cycles += 4;
		return;
	}
	if(_opword == 0x4E72) {                                             // STOP #imm
		if(!GetS()) {
			TakeException(8); // privilege violation
			return;
		}
		SetSR((uint16_t)FetchExtWord());
		if(_exceptionTaken) {
			return;
		}
		_state.PC = _preExecPC;
		_state.Stopped = true;
		_cycles += 4;
		return;
	}

	// TRAP #n  $4E40-$4E4F
	if((_opword & 0xFFF0) == 0x4E40) { TakeException((uint8_t)(32 + (_opword & 0xF))); _cycles += 34; return; }

	// LINK An,#disp  $4E50-$4E57
	if((_opword & 0xFFF8) == 0x4E50) {
		int16_t disp = (int16_t)FetchExtWord();
		Push32(_state.A[reg]);
		_state.A[reg] = _state.A[7]; if(reg == 7) _state.SP = _state.A[7];
		_state.A[7] += disp; _state.SP = _state.A[7];
		_cycles += 18; return;
	}

	// UNLK An  $4E58-$4E5F
	if((_opword & 0xFFF8) == 0x4E58) {
		uint32_t frameAddr = _state.A[reg];
		_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
		_faultPcOverrideValid = true;
		if(CheckAddressError(frameAddr, false, 4, MemoryOperationType::Read)) {
			return;
		}
		_state.A[7] = frameAddr;
		_state.SP = _state.A[7];
		_state.A[reg] = BusRead32(frameAddr);
		if(reg != 7) {
			_state.A[7] += 4;
			_state.SP = _state.A[7];
		}
		_cycles += 12; return;
	}

	// MOVE An,USP  $4E60-$4E67
	if((_opword & 0xFFF8) == 0x4E60) {
		if(!GetS()) {
			TakeException(8); // privilege violation
			return;
		}
		_usp = _state.A[reg];
		_cycles += 4;
		return;
	}

	// MOVE USP,An  $4E68-$4E6F
	if((_opword & 0xFFF8) == 0x4E68) {
		if(!GetS()) {
			TakeException(8); // privilege violation
			return;
		}
		_state.A[reg] = _usp;
		if(reg == 7) {
			_state.SP = _state.A[7];
		}
		_cycles += 4;
		return;
	}

	// JSR <ea>  $4E80-$4EBF
	if((_opword & 0xFFC0) == 0x4E80) {
		uint32_t target = CalcEA(mode, reg, 4);
		if((target & 1u) != 0u) {
			_faultPcOverride = _state.PC & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			_state.PC = target & 0x00FFFFFFu;
			CheckAddressError(target, false, 2, MemoryOperationType::ExecOpCode);
			_cycles += 18;
			return;
		}
		Push32(_state.PC); _state.PC = target & 0x00FFFFFFu;
		_cycles += 18; return;
	}

	// JMP <ea>  $4EC0-$4EFF
	if((_opword & 0xFFC0) == 0x4EC0) {
		uint32_t target = CalcEA(mode, reg, 4);
		if((target & 1u) != 0u) {
			_faultPcOverride = (_preExecPC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			_state.PC = target & 0x00FFFFFFu;
			CheckAddressError(target, false, 2, MemoryOperationType::ExecOpCode);
			_cycles += 8;
			return;
		}
		_state.PC = target & 0x00FFFFFFu;
		_cycles += 8; return;
	}

	// LEA <ea>,An  — 0100 aaa1 11 mmm rrr
	if((_opword & 0x01C0) == 0x01C0 && (_opword & 0x0100)) {
		uint8_t an = (_opword >> 9) & 7;
		uint32_t ea = CalcEA(mode, reg, 4);
		_state.A[an] = ea; if(an == 7) _state.SP = ea;
		_cycles += 4; return;
	}

	// CHK <ea>,Dn  — 0100 ddd1 10 mmm rrr (word)
	// Decode by the addressing mode bits (8..6 == 110) to avoid aliasing
	// unrelated Group 4 opcodes into CHK exceptions.
	if((_opword & 0x01C0) == 0x0180) {
		uint8_t dn = (_opword >> 9) & 7;
		SetFaultPCForEA(mode, reg, 2u);
		int16_t upper = (int16_t)ReadEA16(mode, reg);
		if(_exceptionTaken) {
			return;
		}
		int16_t d = (int16_t)(uint16_t)_state.D[dn];
		_state.SR &= (uint16_t)~(SR_N | SR_Z | SR_V | SR_C);
		if(d < 0 || d > upper) {
			if(d < 0) {
				_state.SR |= SR_N;
			}
				_exceptionPcOverride = _state.PC & 0x00FFFFFFu;
			_exceptionPcOverrideValid = true;
			TakeException(6);
		}
		_cycles += 10;
		return;
	}

	// SWAP Dn  $4840-$4847
	// These opcodes overlap the PEA pattern if the EA legality check is omitted.
	if((_opword & 0xFFF8) == 0x4840) {
		uint32_t v = _state.D[reg];
		v = (v >> 16) | (v << 16);
		_state.D[reg] = v;
		UpdateFlagsNZ32(v);
		SetC(false);
		SetV(false);
		_cycles += 4;
		return;
	}

	// PEA <ea>  $4840-$487F
	if((_opword & 0xFFC0) == 0x4840) {
		Push32(CalcEA(mode, reg, 4)); _cycles += 12; return;
	}

	// EXT.W / EXT.L  $4880-$4887 / $48C0-$48C7 (Dn only)
	// Use exact opcode matching and decode before MOVEM.
	// Sonic uses EXT.L in startup/runtime paths (e.g. $006A52), and aliasing
	// this into MOVEM corrupts execution/stack state and can trigger address
	// errors (e.g. write long to odd address $FF0001).
	bool isExtOpcode = ((_opword & 0xFFF8) == 0x4880) || ((_opword & 0xFFF8) == 0x48C0);
	if(isExtOpcode) {
		if(!(_opword & 0x40)) {  // EXT.W Dn
			int16_t r = (int8_t)(_state.D[reg] & 0xFF);
			_state.D[reg] = (_state.D[reg] & 0xFFFF0000u) | (uint16_t)r;
			UpdateFlagsNZ16((uint16_t)r);
		} else {              // EXT.L Dn
			int32_t r = (int16_t)(_state.D[reg] & 0xFFFF);
			_state.D[reg] = (uint32_t)r;
			UpdateFlagsNZ32((uint32_t)r);
		}
		SetC(false); SetV(false); _cycles += 4; return;
	}

	auto isMovemRegToMemEa = [&]() -> bool {
		return mode == 2 || mode == 4 || mode == 5 || mode == 6 ||
			(mode == 7 && reg <= 1);
	};
	auto isMovemMemToRegEa = [&]() -> bool {
		return mode == 2 || mode == 3 || mode == 5 || mode == 6 ||
			(mode == 7 && reg <= 3);
	};

	// MOVEM register -> memory  $4880-$48FF  (bit 10 distinguishes word/long)
	// Exclude EXT opcodes explicitly to prevent decode aliasing.
	if((_opword & 0xFF80) == 0x4880 && !isExtOpcode) {
		if(!isMovemRegToMemEa()) {
			TakeException(4);
			return;
		}

		bool     isLong = (_opword >> 6) & 1;
		uint16_t mask   = FetchExtWord();
		uint8_t  sz2    = isLong ? 4 : 2;
		_cycles += 8;
		if(mode == 4) {
			// Predecrement: mask bit 0=A7, bit 7=A0, bit 8=D7, bit 15=D0
			uint32_t dSnapshot[8];
			uint32_t aSnapshot[8];
			for(int i = 0; i < 8; i++) {
				dSnapshot[i] = _state.D[i];
				aSnapshot[i] = _state.A[i];
			}
			uint32_t addr = _state.A[reg];
			for(int i = 0; i < 16; i++) {
				if(!(mask & (1u << i))) continue;
				uint32_t val = (i < 8) ? aSnapshot[7 - i] : dSnapshot[15 - i];
				uint32_t nextAddr = addr - sz2;
				_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
				_faultPcOverrideValid = true;
				if(isLong && (nextAddr & 1u) != 0u) {
					CheckAddressError(nextAddr + 2, true, 4, MemoryOperationType::Write);
				} else if(isLong) {
					BusWrite32(nextAddr, val);
				} else {
					BusWrite16(nextAddr, (uint16_t)val);
				}
				if(_exceptionTaken) {
					return;
				}
				addr = nextAddr;
				_state.A[reg] = addr; if(reg == 7) _state.SP = _state.A[7];
				_cycles += sz2 * 2;
			}
		} else {
			uint32_t addr = CalcEA(mode, reg, sz2);
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			for(int i = 0; i < 16; i++) {
				if(!(mask & (1u << i))) continue;
				uint32_t val = (i < 8) ? _state.D[i] : _state.A[i - 8];
				if(isLong) BusWrite32(addr, val);
				else       BusWrite16(addr, (uint16_t)val);
				if(_exceptionTaken) {
					return;
				}
				addr += sz2; _cycles += sz2 * 2;
			}
		}
		return;
	}

	// MOVEM memory -> register  $4C80-$4CFF
	if((_opword & 0xFF80) == 0x4C80) {
		if(!isMovemMemToRegEa()) {
			TakeException(4);
			return;
		}

		bool     isLong = (_opword >> 6) & 1;
		uint16_t mask   = FetchExtWord();
		uint8_t  sz2    = isLong ? 4 : 2;
		_cycles += 8;
		if(mode == 3) {
			// Postincrement: mask bit 0=D0, bit 7=D7, bit 8=A0, bit 15=A7
			uint32_t addr = _state.A[reg];
			for(int i = 0; i < 16; i++) {
				if(!(mask & (1u << i))) continue;
				_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
				_faultPcOverrideValid = true;
				int32_t val = isLong ? (int32_t)BusRead32(addr)
				                     : (int32_t)(int16_t)BusRead16(addr);
				if(_exceptionTaken) {
					_state.A[reg] = addr; if(reg == 7) _state.SP = _state.A[7];
					return;
				}
				if(i < 8) _state.D[i] = (uint32_t)val;
				else if((uint8_t)(i - 8) != reg) { _state.A[i-8] = (uint32_t)val; if(i == 15) _state.SP = _state.A[7]; }
				addr += sz2;
				_state.A[reg] = addr; if(reg == 7) _state.SP = _state.A[7];
				_cycles += sz2 * 2;
			}
		} else {
			uint32_t addr = CalcEA(mode, reg, sz2);
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			for(int i = 0; i < 16; i++) {
				if(!(mask & (1u << i))) continue;
				int32_t val = isLong ? (int32_t)BusRead32(addr)
				                     : (int32_t)(int16_t)BusRead16(addr);
				if(_exceptionTaken) {
					return;
				}
				if(i < 8) _state.D[i] = (uint32_t)val;
				else { _state.A[i-8] = (uint32_t)val; if(i == 15) _state.SP = _state.A[7]; }
				addr += sz2; _cycles += sz2 * 2;
			}
		}
		return;
	}

	// Decode by upper bits of low byte (sz at bits 7-6)
	uint8_t sz = (_opword >> 6) & 3;
	uint8_t subOp = ((_opword >> 8) & 0x0F);

	bool carry = false, overflow = false;

	switch(subOp) {
		case 0:  // NEGX / MOVE from SR
			if(sz == 3) {  // MOVE from SR <ea>
				_faultStatusExtra = 0x0010u;
				uint32_t ea = CalcEA(mode, reg, 2);
				uint32_t savedPc = _state.PC & 0x00FFFFFFu;
				if(mode == 4) {
					savedPc = (savedPc + 2) & 0x00FFFFFFu;
				} else if(mode == 5 || mode == 6) {
					savedPc = (savedPc - 2) & 0x00FFFFFFu;
				}
				_faultPcOverride = savedPc;
				_faultPcOverrideValid = true;
				WriteResolvedEA16(ea, _state.SR); _cycles += 8;
			} else {
				if(sz == 0) {
					bool oldZ = GetZ();
					SetFaultPCForEA(mode, reg, 1u);
					uint32_t ea = CalcEA(mode, reg, 1);
					uint8_t v = ReadResolvedEA8(ea);
					uint8_t r = Sub8(0, v, GetX(), carry, overflow);
					WriteResolvedEA8(ea, r);
					UpdateFlagsNZ8(r);
					SetZ(oldZ && (r == 0));
					SetC(carry); SetV(overflow); SetX(carry);
				} else if(sz == 1) {
					bool oldZ = GetZ();
					SetFaultPCForEA(mode, reg, 2u);
					uint32_t ea = CalcEA(mode, reg, 2);
					uint16_t v = ReadResolvedEA16(ea);
					uint16_t r = Sub16(0, v, GetX(), carry, overflow);
					WriteResolvedEA16(ea, r);
					UpdateFlagsNZ16(r);
					SetZ(oldZ && (r == 0));
					SetC(carry); SetV(overflow); SetX(carry);
				} else {
					bool oldZ = GetZ();
					SetFaultPCForEA(mode, reg, 4u);
					uint32_t ea = CalcEA(mode, reg, 4);
					uint32_t v = ReadResolvedEA32(ea);
					uint32_t r = Sub32(0, v, GetX(), carry, overflow);
					WriteResolvedEA32(ea, r);
					UpdateFlagsNZ32(r);
					SetZ(oldZ && (r == 0));
					SetC(carry); SetV(overflow); SetX(carry);
				}
				_cycles += 8;
			}
			break;

		case 2:  // CLR / MOVE from CCR
			if(sz == 3) {  // MOVE from CCR <ea>
				WriteEA16(mode, reg, (uint16_t)(_state.SR & 0x001Fu)); _cycles += 8;
			} else {
				if(sz == 0) {
					SetFaultPCForEA(mode, reg, 1u);
					uint32_t ea = CalcEA(mode, reg, 1);
					(void)ReadResolvedEA8(ea); // CLR performs a read cycle before write
					WriteResolvedEA8(ea, 0);
				} else if(sz == 1) {
					SetFaultPCForEA(mode, reg, 2u);
					uint32_t ea = CalcEA(mode, reg, 2);
					(void)ReadResolvedEA16(ea);
					WriteResolvedEA16(ea, 0);
				} else {
					SetFaultPCForEA(mode, reg, 4u);
					uint32_t ea = CalcEA(mode, reg, 4);
					(void)ReadResolvedEA32(ea);
					WriteResolvedEA32(ea, 0);
				}
				SetZ(true); SetN(false); SetC(false); SetV(false); _cycles += 8;
			}
			break;

			case 4:  // NEG / MOVE to CCR
				if(sz == 3) {  // MOVE to CCR
					SetFaultPCForEA(mode, reg, 2u);
					uint16_t value = ReadEA16(mode, reg);
					if(_exceptionTaken) return;
					SetCCR((uint8_t)(value & 0x1F)); _cycles += 12;
				} else {
					if(sz == 0) {
						SetFaultPCForEA(mode, reg, 1u);
						uint32_t ea = CalcEA(mode, reg, 1);
					uint8_t v = ReadResolvedEA8(ea);
					uint8_t r = Sub8(0, v, false, carry, overflow);
					WriteResolvedEA8(ea, r);
					UpdateFlagsNZ8(r);
					SetC(carry); SetV(overflow); SetX(carry);
				} else if(sz == 1) {
					SetFaultPCForEA(mode, reg, 2u);
					uint32_t ea = CalcEA(mode, reg, 2);
					uint16_t v = ReadResolvedEA16(ea);
					uint16_t r = Sub16(0, v, false, carry, overflow);
					WriteResolvedEA16(ea, r);
					UpdateFlagsNZ16(r);
					SetC(carry); SetV(overflow); SetX(carry);
				} else {
					SetFaultPCForEA(mode, reg, 4u);
					uint32_t ea = CalcEA(mode, reg, 4);
					uint32_t v = ReadResolvedEA32(ea);
					uint32_t r = Sub32(0, v, false, carry, overflow);
					WriteResolvedEA32(ea, r);
					UpdateFlagsNZ32(r);
					SetC(carry); SetV(overflow); SetX(carry);
				}
				_cycles += 8;
			}
			break;

			case 6:  // NOT / MOVE to SR
				if(sz == 3) {  // MOVE to SR
					if(!GetS()) {
						TakeException(8); // privilege violation
						return;
					}
					SetFaultPCForEA(mode, reg, 2u);
					uint16_t value = ReadEA16(mode, reg);
					if(_exceptionTaken) return;
					SetSR(value);
					_cycles += 12;
				} else {
				if(sz == 0) {
					SetFaultPCForEA(mode, reg, 1u);
					uint32_t ea = CalcEA(mode, reg, 1);
					uint8_t r = (uint8_t)~ReadResolvedEA8(ea);
					WriteResolvedEA8(ea, r);
					UpdateFlagsNZ8(r);
					SetC(false); SetV(false);
				} else if(sz == 1) {
					SetFaultPCForEA(mode, reg, 2u);
					uint32_t ea = CalcEA(mode, reg, 2);
					uint16_t r = (uint16_t)~ReadResolvedEA16(ea);
					WriteResolvedEA16(ea, r);
					UpdateFlagsNZ16(r);
					SetC(false); SetV(false);
				} else {
					SetFaultPCForEA(mode, reg, 4u);
					uint32_t ea = CalcEA(mode, reg, 4);
					uint32_t r = ~ReadResolvedEA32(ea);
					WriteResolvedEA32(ea, r);
					UpdateFlagsNZ32(r);
					SetC(false); SetV(false);
				}
				_cycles += 8;
			}
			break;

		case 8:  // NBCD / PEA / MOVEM(handled above) / EXT(handled above) / SWAP
			if(sz == 0) {  // NBCD or SWAP
				if((_opword & 0xFFF8) == 0x4840) {  // SWAP Dn
					uint32_t v = _state.D[reg];
					v = (v >> 16) | (v << 16);
					_state.D[reg] = v;
					UpdateFlagsNZ32(v); SetC(false); SetV(false); _cycles += 4;
				} else {  // NBCD
					bool c = false, rawCarry = false, rawOverflow = false;
					bool z = (_state.SR & SR_Z) != 0;
					SetFaultPCForEA(mode, reg, 1u);
					uint32_t ea = CalcEA(mode, reg, 1);
					uint8_t v = ReadResolvedEA8(ea);
					(void)Sub8(0, v, GetX(), rawCarry, rawOverflow);
					uint8_t r = Sbcd(0, v, c, z, GetX());
					uint8_t binary = static_cast<uint8_t>(0u - v - (GetX() ? 1u : 0u));
					WriteResolvedEA8(ea, r);
					SetC(c);
					SetX(c);
					SetV((binary & static_cast<uint8_t>(~r) & 0x80) != 0);
					SetZ(z);
					SetN((r >> 7) & 1);
					_cycles += 8;
				}
			}
			break;

		case 10:  // TST / ILLEGAL ($4AFC)
			if(sz == 3) {  // TAS
				if(mode == 0) {
					uint8_t v = (uint8_t)_state.D[reg];
					UpdateFlagsNZ8(v); SetC(false); SetV(false);
					_state.D[reg] = (_state.D[reg] & 0xFFFFFF00u) | (uint32_t)((uint8_t)(v | 0x80));
					_cycles += 4;
				} else {
					SetFaultPCForEA(mode, reg, 1u);
					uint32_t ea = CalcEA(mode, reg, 1);
					uint8_t v = ReadResolvedEA8(ea);
					UpdateFlagsNZ8(v); SetC(false); SetV(false);
					// Genesis/68000 "broken TAS" behavior: memory destinations do not
					// perform the final write-back cycle. BlastEm accounts for this
					// as BUS*2 + 2 (10 cycles with BUS=4), which matches the trace
					// delta for the benchmark hot loop more closely than the older
					// flat 14-cycle charge.
					_cycles += 10;
				}
			} else {
				SetFaultPCForEA(mode, reg, (uint8_t)(1u << sz));
				if(sz == 0) { UpdateFlagsNZ8 (ReadEA8 (mode,reg)); }
				else if(sz==1){ UpdateFlagsNZ16(ReadEA16(mode,reg)); }
				else          { UpdateFlagsNZ32(ReadEA32(mode,reg)); }
				SetC(false); SetV(false); _cycles += 4;
			}
			break;

		default:
			TakeException(4);
			break;
	}
}

// ===========================================================================
// I_Group5  $50-$5F  — ADDQ / SUBQ / Scc / DBcc
// ===========================================================================

void GenesisCpu68k::I_Group5()
{
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;
	uint8_t data = (_opword >> 9) & 7;
	if(data == 0) data = 8;

	bool carry = false, overflow = false;

	if(sz == 3) {
		// Scc / DBcc
		uint8_t cc = (_opword >> 8) & 0xF;
			if(mode == 1) {
				// DBcc Dn,#d16
				int16_t disp = (int16_t)FetchExtWord();
				if(!TestCC(cc)) {
					int16_t cnt = (int16_t)(uint16_t)_state.D[reg];
					int16_t next = (int16_t)(cnt - 1);
					if(next != -1) {
						uint32_t target = (_state.PC - 2 + disp) & 0x00FFFFFFu;
						if((target & 1u) != 0u) {
							uint32_t savedPc = _state.PC & 0x00FFFFFFu;
							_state.PC = target;
							_faultPcOverride = savedPc;
							_faultPcOverrideValid = true;
							CheckAddressError(target, false, 2, MemoryOperationType::ExecOpCode);
						} else {
							_state.D[reg] = (_state.D[reg] & 0xFFFF0000u) | (uint16_t)next;
							_state.PC = target;
						}
						_cycles += 6;
					} else {
						_state.D[reg] = (_state.D[reg] & 0xFFFF0000u) | (uint16_t)next;
						_cycles += 10;
					}
				} else {
					_cycles += 8;
				}
		} else {
			// Scc <ea>
			uint8_t result = TestCC(cc) ? 0xFF : 0x00;
			WriteEA8(mode, reg, result);
			_cycles += 8;
		}
		return;
	}

	bool isAdd = ((_opword >> 8) & 1) == 0;  // bit 8 = 0: ADDQ, 1: SUBQ
	uint32_t imm = (uint32_t)data;

	if(mode == 1) {
		// ADDQ/SUBQ to An: always 32-bit, no flags
		if(isAdd) _state.A[reg] += imm;
		else      _state.A[reg] -= imm;
		if(reg == 7) _state.SP = _state.A[7];
		_cycles += 8;
		return;
	}

	if(isAdd) {
		if(sz == 0) {
			SetFaultPCForEA(mode, reg, 1u);
			uint32_t ea = CalcEA(mode, reg, 1);
			uint8_t a = ReadResolvedEA8(ea), b = (uint8_t)imm;
			uint8_t r = Add8(a, b, false, carry, overflow);
			WriteResolvedEA8(ea, r);
			UpdateFlagsNZ8(r);
		} else if(sz == 1) {
			SetFaultPCForEA(mode, reg, 2u);
			uint32_t ea = CalcEA(mode, reg, 2);
			uint16_t a = ReadResolvedEA16(ea), b = (uint16_t)imm;
			uint16_t r = Add16(a, b, false, carry, overflow);
			WriteResolvedEA16(ea, r);
			UpdateFlagsNZ16(r);
		} else {
			SetFaultPCForEA(mode, reg, 4u);
			uint32_t ea = CalcEA(mode, reg, 4);
			uint32_t a = ReadResolvedEA32(ea), b = imm;
			uint32_t r = Add32(a, b, false, carry, overflow);
			WriteResolvedEA32(ea, r);
			UpdateFlagsNZ32(r);
		}
	} else {
		if(sz == 0) {
			SetFaultPCForEA(mode, reg, 1u);
			uint32_t ea = CalcEA(mode, reg, 1);
			uint8_t a = ReadResolvedEA8(ea), b = (uint8_t)imm;
			uint8_t r = Sub8(a, b, false, carry, overflow);
			WriteResolvedEA8(ea, r);
			UpdateFlagsNZ8(r);
		} else if(sz == 1) {
			SetFaultPCForEA(mode, reg, 2u);
			uint32_t ea = CalcEA(mode, reg, 2);
			uint16_t a = ReadResolvedEA16(ea), b = (uint16_t)imm;
			uint16_t r = Sub16(a, b, false, carry, overflow);
			WriteResolvedEA16(ea, r);
			UpdateFlagsNZ16(r);
		} else {
			SetFaultPCForEA(mode, reg, 4u);
			uint32_t ea = CalcEA(mode, reg, 4);
			uint32_t a = ReadResolvedEA32(ea), b = imm;
			uint32_t r = Sub32(a, b, false, carry, overflow);
			WriteResolvedEA32(ea, r);
			UpdateFlagsNZ32(r);
		}
	}
	SetC(carry); SetV(overflow); SetX(carry); _cycles += 8;
}

// ===========================================================================
// I_Group6  $60-$6F  — BRA / BSR / Bcc
// ===========================================================================

void GenesisCpu68k::I_Group6()
{
	uint8_t cc   = (_opword >> 8) & 0xF;
	int8_t  disp8 = (int8_t)(_opword & 0xFF);
	int32_t disp;

	if(disp8 == 0) {
		disp = (int32_t)(int16_t)FetchExtWord();
		_cycles += 12;
	} else {
		disp = (int32_t)disp8;
		_cycles += 10;
	}

	uint32_t target = (_state.PC + disp - (disp8 == 0 ? 0 : 0)) & 0x00FFFFFFu;
	// For byte displacement: PC was already advanced by 2 (opcode), target = (PC after opcode) + disp8
	// For word displacement: PC advanced 4, target = (PC after ext word) + disp
	// Since FetchExtWord already advanced PC, target is just _state.PC + disp for short
	// For byte: _state.PC is already past the opcode word, so target = _state.PC + disp8
	// (The branch disp is relative to the word AFTER the opcode word)
	// So: target = (addr_of_opcode + 2) + disp
	// _state.PC is already = addr_of_opcode + 2 (after FetchOpcode)
	// For word: _state.PC = addr_of_opcode + 4 (after FetchExtWord), target = (addr_of_opcode + 2) + disp
	//         = _state.PC - 2 + disp
	if(disp8 == 0) {
		target = (_state.PC - 2 + disp) & 0x00FFFFFFu;
	} else {
		target = (_state.PC + disp) & 0x00FFFFFFu;
	}

		if(cc == 0) {
			// BRA
			if((target & 1u) != 0u) {
				_faultPcOverride = (_preExecPC + 2) & 0x00FFFFFFu;
				_faultPcOverrideValid = true;
				_state.PC = target;
				CheckAddressError(target, false, 2, MemoryOperationType::ExecOpCode);
			return;
		}
		_state.PC = target;
	} else if(cc == 1) {
		// BSR
		if((target & 1u) != 0u) {
			Push32(_state.PC);
			_faultPcOverride = target & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			_state.PC = target;
			CheckAddressError(target, false, 2, MemoryOperationType::ExecOpCode);
			_cycles += 8;
			return;
		}
		Push32(_state.PC);
		_state.PC = target;
		_cycles += 8;
		} else {
			// Bcc
			if(TestCC(cc)) {
				if((target & 1u) != 0u) {
					_faultPcOverride = (_preExecPC + 2) & 0x00FFFFFFu;
					_faultPcOverrideValid = true;
					_state.PC = target;
					CheckAddressError(target, false, 2, MemoryOperationType::ExecOpCode);
				return;
			}
			_state.PC = target;
		} else {
			// Not taken: undo ext-word cycle cost
			if(disp8 == 0) _cycles -= 2;
		}
	}
}

// ===========================================================================
// I_Moveq  $70-$7F  — MOVEQ
// ===========================================================================

void GenesisCpu68k::I_Moveq()
{
	uint8_t dn  = (_opword >> 9) & 7;
	int32_t imm = (int32_t)(int8_t)(_opword & 0xFF);
	_state.D[dn] = (uint32_t)imm;
	UpdateFlagsNZ32((uint32_t)imm); SetC(false); SetV(false);
	_cycles += 4;
}

// ===========================================================================
// Data-dependent cycle helpers for multiply / divide
// ===========================================================================

static uint32_t Popcount16(uint16_t v)
{
	v = v - ((v >> 1) & 0x5555u);
	v = (v & 0x3333u) + ((v >> 2) & 0x3333u);
	return ((v + (v >> 4)) & 0x0F0Fu) * 0x0101u >> 8;
}

// MULU.W: 38 + 2 per set bit in 16-bit source operand.  Range 38-70.
static uint32_t CalcMuluCycles(uint16_t source)
{
	return 38 + 2 * Popcount16(source);
}

// MULS.W: 38 + 2 per 0<->1 transition in {source, 0} (17-bit).  Range 38-70.
static uint32_t CalcMulsCycles(uint16_t source)
{
	// XOR adjacent bits; bit 0 compares source[0] with the appended zero.
	uint32_t transitions = (uint32_t)source ^ ((uint32_t)source << 1);
	return 38 + 2 * Popcount16((uint16_t)(transitions & 0xFFFFu));
}

// DIVU.W: restoring-division loop on the actual dividend/divisor.
// Overflow early-out: 10 cycles.  Normal range: 76-136.
// Algorithm from the Musashi 68000 core (Jorge Cwik).
static uint32_t CalcDivuCycles(uint32_t dividend, uint16_t divisor)
{
	if((dividend >> 16) >= divisor) return 10; // overflow

	uint32_t mcycles = 38; // half-cycles
	uint32_t hdivisor = (uint32_t)divisor << 16;

	for(int i = 0; i < 15; i++) {
		if((int32_t)dividend < 0) {
			dividend = (dividend << 1) - hdivisor;
		} else {
			dividend <<= 1;
			if(dividend >= hdivisor) {
				dividend -= hdivisor;
				mcycles++;
			} else {
				mcycles += 2;
			}
		}
	}
	return mcycles * 2;
}

// DIVS.W: signed division timing matched to BlastEm's 68000 path.
// Absolute-overflow early-out occurs before the full internal divide loop.
// Signed-result overflow still runs the normal timing path.
static uint32_t CalcDivsCycles(int32_t dividend, int16_t divisor)
{
	uint32_t dividendMag = (uint32_t)dividend;
	uint32_t divisorShift = ((uint32_t)(uint16_t)divisor) << 16;
	uint32_t origDividend = dividendMag;
	uint32_t origDivisor = divisorShift;

	if(divisorShift & 0x80000000u) {
		divisorShift = 0u - divisorShift;
	}

	uint32_t cycles = 8u;
	if(dividendMag & 0x80000000u) {
		dividendMag = 0u - dividendMag;
		cycles += 2u;
	}

	if(divisorShift <= dividendMag) {
		return cycles + 4u;
	}

	for(int i = 0; i < 15; i++) {
		dividendMag <<= 1;
		if(dividendMag >= divisorShift) {
			dividendMag -= divisorShift;
			cycles += 6u;
		} else {
			cycles += 8u;
		}
	}

	cycles += 4u;
	if(origDivisor & 0x80000000u) {
		cycles += 16u;
	} else if(origDividend & 0x80000000u) {
		cycles += 18u;
	} else {
		cycles += 14u;
	}

	return cycles;
}

// ===========================================================================
// I_Group8  $80-$8F  — OR / DIVU / SBCD / DIVS
// ===========================================================================

void GenesisCpu68k::I_Group8()
{
	uint8_t dn   = (_opword >> 9) & 7;
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;

	// SBCD / ABCD: 1000 ddd1 00000 rrr (mode=4 if Dn, mode=3 if -(An) but encoded differently)
	// SBCD Dn,Dn:  $8100-$8107
	// SBCD -(An),-(An): $8108-$810F
	if((_opword & 0xF1F0) == 0x8100) {
		bool useAddr = ((_opword >> 3) & 1) != 0;
		bool z = (_state.SR & SR_Z) != 0;
		uint8_t src, dst, r;
		bool c = false;
		if(useAddr) {
			_state.A[reg] -= (reg == 7) ? 2u : 1u;
			if(reg == 7) _state.SP = _state.A[7];
			uint32_t srcAddr = _state.A[reg];
			_state.A[dn] -= (dn == 7) ? 2u : 1u;
			if(dn == 7) _state.SP = _state.A[7];
			uint32_t dstAddr = _state.A[dn];
			src = BusRead8(srcAddr);
			dst = BusRead8(dstAddr);
			uint16_t res = (dst & 0x0Fu) - (src & 0x0Fu) - (GetX() ? 1u : 0u);
			uint16_t corf = (res > 0x0Fu) ? 6u : 0u;
			res += (dst & 0xF0u) - (src & 0xF0u);
			uint8_t binary = static_cast<uint8_t>(res);
			if(res > 0xFFu) {
				res += 0xA0u;
				c = true;
			} else if(res < corf) {
				c = true;
			}
			r = static_cast<uint8_t>((res - corf) & 0xFFu);
			if(r != 0) {
				z = false;
			}
			SetV((binary & static_cast<uint8_t>(~r) & 0x80) != 0);
			BusWrite8(dstAddr, r);
		} else {
			src = (uint8_t)_state.D[reg];
			dst = (uint8_t)_state.D[dn];
			uint16_t res = (dst & 0x0Fu) - (src & 0x0Fu) - (GetX() ? 1u : 0u);
			uint16_t corf = (res > 0x0Fu) ? 6u : 0u;
			res += (dst & 0xF0u) - (src & 0xF0u);
			uint8_t binary = static_cast<uint8_t>(res);
			if(res > 0xFFu) {
				res += 0xA0u;
				c = true;
			} else if(res < corf) {
				c = true;
			}
			r = static_cast<uint8_t>((res - corf) & 0xFFu);
			if(r != 0) {
				z = false;
			}
			SetV((binary & static_cast<uint8_t>(~r) & 0x80) != 0);
			_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
		}
		SetC(c); SetX(c); SetZ(z); SetN((r >> 7) & 1);
		_cycles += useAddr ? 18 : 6; return;
	}

	// DIVU <ea>,Dn  $80C0-$80FF
	if(sz == 3 && !((_opword >> 8) & 1)) {
		if(mode == 7 && reg == 1 && !_faultPcOverrideValid) {
			// (xxx).l EA: 2 extension words, so saved PC is _preExecPC+6
			_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 7 && reg == 0 && !_faultPcOverrideValid) {
			// (xxx).w EA: 1 extension word, so saved PC is _preExecPC+4
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			} else if(mode == 4 && !_faultPcOverrideValid) {
				_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
				_faultPcOverrideValid = true;
			}
			uint16_t divisor = ReadEA16(mode, reg);
			if(_exceptionTaken) {
				return;
			}
			if(divisor == 0) { TakeException(5); _cycles += 4; return; }
			uint32_t dividend = _state.D[dn];
		uint32_t quot = dividend / (uint32_t)divisor;
		uint32_t rem  = dividend % (uint32_t)divisor;
		uint32_t divCycles = CalcDivuCycles(dividend, divisor);
		if(quot > 0xFFFF) { SetV(true); SetN(true); SetZ(false); SetC(false); _cycles += divCycles; return; }
		_state.D[dn] = ((rem & 0xFFFF) << 16) | (quot & 0xFFFF);
		UpdateFlagsNZ16((uint16_t)quot); SetC(false); SetV(false);
		_cycles += divCycles; return;
	}

	// DIVS <ea>,Dn  $81C0-$81FF
	if(sz == 3 && ((_opword >> 8) & 1)) {
		if(mode == 7 && reg == 1 && !_faultPcOverrideValid) {
			// (xxx).l EA: 2 extension words, so saved PC is _preExecPC+6
			_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 7 && reg == 0 && !_faultPcOverrideValid) {
			// (xxx).w EA: 1 extension word, so saved PC is _preExecPC+4
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 4 && !_faultPcOverrideValid) {
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		}
		int16_t divisor = (int16_t)ReadEA16(mode, reg);
		if(_exceptionTaken) {
			return;
		}
		if(divisor == 0) { TakeException(5); _cycles += 4; return; }
		int32_t dividend = (int32_t)_state.D[dn];
		int32_t quot = dividend / (int32_t)divisor;
		int32_t rem  = dividend % (int32_t)divisor;
		uint32_t divCycles = CalcDivsCycles(dividend, divisor);
		if(quot > 32767 || quot < -32768) { SetV(true); SetN(true); SetZ(false); SetC(false); _cycles += divCycles; return; }
		_state.D[dn] = ((uint32_t)(uint16_t)(int16_t)rem << 16) | (uint16_t)(int16_t)quot;
		UpdateFlagsNZ16((uint16_t)(int16_t)quot); SetC(false); SetV(false);
		_cycles += divCycles; return;
	}

	// OR: 1000 ddd s mm mmm rrr (destination: Dn if sz<3, memory if sz<3 and bit 8 set)
	bool toEA = ((_opword >> 8) & 1) != 0;
	if(sz == 0) {
		if(toEA) {
			SetFaultPCForEA(mode, reg, 1);
			uint32_t eaAddr = CalcEA(mode, reg, 1);
			uint8_t ea = ReadResolvedEA8(eaAddr);
			if(_exceptionTaken) return;
			uint8_t r = (uint8_t)_state.D[dn] | ea;
			WriteResolvedEA8(eaAddr, r);
			UpdateFlagsNZ8(r);
		} else {
			SetFaultPCForEA(mode, reg, 1);
			uint8_t ea = ReadEA8(mode, reg);
			if(_exceptionTaken) return;
			uint8_t r = (uint8_t)_state.D[dn] | ea;
			_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
			UpdateFlagsNZ8(r);
		}
	} else if(sz == 1) {
		if(toEA) {
			SetFaultPCForEA(mode, reg, 2);
			uint32_t eaAddr = CalcEA(mode, reg, 2);
			uint16_t ea = ReadResolvedEA16(eaAddr);
			if(_exceptionTaken) return;
			uint16_t r = (uint16_t)_state.D[dn] | ea;
			WriteResolvedEA16(eaAddr, r);
			UpdateFlagsNZ16(r);
		} else {
			SetFaultPCForEA(mode, reg, 2);
			uint16_t ea = ReadEA16(mode, reg);
			if(_exceptionTaken) return;
			uint16_t r = (uint16_t)_state.D[dn] | ea;
			_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | r;
			UpdateFlagsNZ16(r);
		}
	} else {
		if(toEA) {
			SetFaultPCForEA(mode, reg, 4);
			uint32_t eaAddr = CalcEA(mode, reg, 4);
			uint32_t ea = ReadResolvedEA32(eaAddr);
			if(_exceptionTaken) return;
			uint32_t r = _state.D[dn] | ea;
			WriteResolvedEA32(eaAddr, r);
			UpdateFlagsNZ32(r);
		} else {
			SetFaultPCForEA(mode, reg, 4);
			uint32_t ea = ReadEA32(mode, reg);
			if(_exceptionTaken) return;
			uint32_t r = _state.D[dn] | ea;
			_state.D[dn] = r;
			UpdateFlagsNZ32(r);
		}
	}
	SetC(false); SetV(false); _cycles += 8;
}

// ===========================================================================
// I_Group9  $90-$9F  — SUB / SUBX / SUBA
// ===========================================================================

void GenesisCpu68k::I_Group9()
{
	uint8_t dn   = (_opword >> 9) & 7;
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;
	bool    toEA = ((_opword >> 8) & 1) != 0;

	// SUBA: sz=3 (word) or sz=7 (long) — but sz field is 2 bits...
	// SUBA.W: 1001 aaa0 11 mmm rrr  (sz=3 field, bit 8=0)
	// SUBA.L: 1001 aaa1 11 mmm rrr  (sz=3, bit 8=1)
	if(sz == 3) {
		SetFaultPCForEA(mode, reg, toEA ? 4u : 2u);
		int32_t src = toEA ? (int32_t)ReadEA32(mode,reg) : (int32_t)(int16_t)ReadEA16(mode,reg);
		_state.A[dn] -= (uint32_t)src; if(dn==7) _state.SP = _state.A[7];
		_cycles += 8; return;
	}

	// SUBX: 1001 ddd1 ss 00x rrr  (mode=0 for Dn, mode=1 for -(An))
	if(toEA && (mode == 0 || mode == 1)) {
		bool c = false, v = false;
		bool oldZ = GetZ();
		if(mode == 0) {
			if(sz == 0) {
				uint8_t a = (uint8_t)_state.D[dn];
				uint8_t b = (uint8_t)_state.D[reg];
				uint8_t r = Sub8(a, b, GetX(), c, v);
				_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
				UpdateFlagsNZ8(r);
				SetZ(oldZ && (r == 0));
			} else if(sz == 1) {
				uint16_t a = (uint16_t)_state.D[dn];
				uint16_t b = (uint16_t)_state.D[reg];
				uint16_t r = Sub16(a, b, GetX(), c, v);
				_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | r;
				UpdateFlagsNZ16(r);
				SetZ(oldZ && (r == 0));
			} else {
				uint32_t a = _state.D[dn];
				uint32_t b = _state.D[reg];
				uint32_t r = Sub32(a, b, GetX(), c, v);
				_state.D[dn] = r;
				UpdateFlagsNZ32(r);
				SetZ(oldZ && (r == 0));
			}
		} else {
			uint8_t srcDec = (sz == 0) ? 1 : (sz == 1) ? 2 : 4;
			uint8_t dstDec = srcDec;
			if(reg == 7 && sz == 0) srcDec = 2;
			if(dn  == 7 && sz == 0) dstDec = 2;
			uint32_t oldSrcA = _state.A[reg];
			uint32_t oldDstA = _state.A[dn];

			_state.A[reg] -= srcDec; if(reg == 7) _state.SP = _state.A[7];
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;

			if(sz == 0) {
				uint8_t src = BusRead8(_state.A[reg]);
				if(_exceptionTaken) {
					return;
				}
				_state.A[dn] -= dstDec; if(dn == 7) _state.SP = _state.A[7];
				uint8_t dst = BusRead8(_state.A[dn]);
				if(_exceptionTaken) {
					if(sz == 2) {
						_state.A[dn] = oldDstA;
						if(dn == 7) _state.SP = _state.A[7];
					}
					return;
				}
				uint8_t r = Sub8(dst, src, GetX(), c, v);
				BusWrite8(_state.A[dn], r);
				UpdateFlagsNZ8(r);
				SetZ(oldZ && (r == 0));
			} else if(sz == 1) {
				uint16_t src = BusRead16(_state.A[reg]);
				if(_exceptionTaken) {
					return;
				}
				_state.A[dn] -= dstDec; if(dn == 7) _state.SP = _state.A[7];
				uint16_t dst = BusRead16(_state.A[dn]);
				if(_exceptionTaken) {
					if(sz == 2) {
						_state.A[dn] = oldDstA;
						if(dn == 7) _state.SP = _state.A[7];
					}
					return;
				}
				uint16_t r = Sub16(dst, src, GetX(), c, v);
				BusWrite16(_state.A[dn], r);
				UpdateFlagsNZ16(r);
				SetZ(oldZ && (r == 0));
			} else {
				uint32_t src = 0;
				if((_state.A[reg] & 1u) != 0u) {
					(void)BusRead16(_state.A[reg] + 2);
				} else {
					src = BusRead32(_state.A[reg]);
				}
				if(_exceptionTaken) {
					_state.A[reg] = oldSrcA;
					if(reg == 7) _state.SP = _state.A[7];
					return;
				}
				_state.A[dn] -= dstDec; if(dn == 7) _state.SP = _state.A[7];
				uint32_t dst = 0;
				if((_state.A[dn] & 1u) != 0u) {
					CheckAddressError(_state.A[dn] + 2, false, 4, MemoryOperationType::Read);
				} else {
					dst = BusRead32(_state.A[dn]);
				}
				if(_exceptionTaken) {
					if(sz == 2) {
						_state.A[dn] = oldDstA;
						if(dn == 7) _state.SP = _state.A[7];
					}
					return;
				}
				uint32_t r = Sub32(dst, src, GetX(), c, v);
				BusWrite32(_state.A[dn], r);
				UpdateFlagsNZ32(r);
				SetZ(oldZ && (r == 0));
			}
		}
		SetC(c); SetV(v); SetX(c); _cycles += 12; return;
	}

	// SUB <ea>,Dn (toEA=false): Dn - <ea> → Dn
	// SUB Dn,<ea> (toEA=true):  <ea> - Dn → <ea>
	SetFaultPCForEA(mode, reg, (uint8_t)(1u << sz));
	bool carry = false, overflow = false;
	if(sz == 0) {
		if(toEA) {
			uint32_t eaAddr = CalcEA(mode, reg, 1);
			uint8_t ea = ReadResolvedEA8(eaAddr);
			uint8_t r = Sub8(ea, (uint8_t)_state.D[dn], false, carry, overflow);
			WriteResolvedEA8(eaAddr, r);
			UpdateFlagsNZ8(r);
		} else {
			uint8_t ea = ReadEA8(mode, reg);
			uint8_t r = Sub8((uint8_t)_state.D[dn], ea, false, carry, overflow);
			_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
			UpdateFlagsNZ8(r);
		}
	} else if(sz == 1) {
		if(toEA) {
			uint32_t eaAddr = CalcEA(mode, reg, 2);
			uint16_t ea = ReadResolvedEA16(eaAddr);
			uint16_t r = Sub16(ea, (uint16_t)_state.D[dn], false, carry, overflow);
			WriteResolvedEA16(eaAddr, r);
			UpdateFlagsNZ16(r);
		} else {
			uint16_t ea = ReadEA16(mode, reg);
			uint16_t r = Sub16((uint16_t)_state.D[dn], ea, false, carry, overflow);
			_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | r;
			UpdateFlagsNZ16(r);
		}
	} else {
		if(toEA) {
			uint32_t eaAddr = CalcEA(mode, reg, 4);
			uint32_t ea = ReadResolvedEA32(eaAddr);
			uint32_t r = Sub32(ea, _state.D[dn], false, carry, overflow);
			WriteResolvedEA32(eaAddr, r);
			UpdateFlagsNZ32(r);
		} else {
			uint32_t ea = ReadEA32(mode, reg);
			uint32_t r = Sub32(_state.D[dn], ea, false, carry, overflow);
			_state.D[dn] = r;
			UpdateFlagsNZ32(r);
		}
	}
	SetC(carry); SetV(overflow); SetX(carry); _cycles += 8;
}

// ===========================================================================
// I_ALine  $A0-$AF  — A-line trap
// ===========================================================================

void GenesisCpu68k::I_ALine()
{
	TakeException(10);
}

// ===========================================================================
// I_GroupB  $B0-$BF  — CMP / CMPA / CMPM / EOR
// ===========================================================================

void GenesisCpu68k::I_GroupB()
{
	uint8_t dn   = (_opword >> 9) & 7;
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;
	bool    toEA = ((_opword >> 8) & 1) != 0;

	// CMPA: sz=3 (word sign-extended) or sz=7 (long)
	if(sz == 3) {
		if(mode == 7 && reg == 1 && !_faultPcOverrideValid) {
			// (xxx).l EA: 2 extension words, so saved PC is _preExecPC+6
			_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 7 && reg == 0 && !_faultPcOverrideValid) {
			// (xxx).w EA: 1 extension word, so saved PC is _preExecPC+4
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 4 && !_faultPcOverrideValid) {
			// CMPA.l -(An),An faults before the extra prefetch advance.
			_faultPcOverride = (_state.PC + (toEA ? 0u : 2u)) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		}
		int32_t src = toEA ? (int32_t)ReadEA32(mode,reg) : (int32_t)(int16_t)ReadEA16(mode,reg);
		bool c = false, v = false;
		uint32_t r = Sub32(_state.A[dn], (uint32_t)src, false, c, v);
		UpdateFlagsNZ32(r); SetC(c); SetV(v);
		_cycles += 6; return;
	}

	if(toEA) {
		// EOR / CMPM
			if(mode == 1) {
				// CMPM (An)+,(An)+
				uint8_t an = reg, am = dn;
				uint8_t srcInc = (sz == 0) ? 1 : (sz == 1) ? 2 : 4;
				uint8_t dstInc = srcInc;
				uint32_t oldSrcA = _state.A[an];
				if(!_faultPcOverrideValid) {
					_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
					_faultPcOverrideValid = true;
				}
			bool c = false, v = false;
				if(sz == 0) {
					if(an == 7) srcInc = 2;
					if(am == 7) dstInc = 2;
					uint8_t src = BusRead8(_state.A[an]);
					_state.A[an] += srcInc; if(an==7) _state.SP=_state.A[7];
					if(_exceptionTaken) {
						return;
					}
					_faultPreserveAddressRegs = true;
					uint8_t dst = BusRead8(_state.A[am]);
					if(_exceptionTaken) {
						return;
					}
				_state.A[am] += dstInc; if(am==7) _state.SP=_state.A[7];
				Sub8(dst, src, false, c, v); UpdateFlagsNZ8((uint8_t)(dst-src));
				} else if(sz == 1) {
					uint16_t src = BusRead16(_state.A[an]);
					_state.A[an] += srcInc;
					if(_exceptionTaken) {
						return;
					}
					_faultPreserveAddressRegs = true;
					uint16_t dst = BusRead16(_state.A[am]);
					if(_exceptionTaken) {
						return;
					}
				_state.A[am] += dstInc;
				Sub16(dst, src, false, c, v); UpdateFlagsNZ16((uint16_t)(dst-src));
				} else {
					uint32_t src = BusRead32(_state.A[an]);
					if(_exceptionTaken) {
						// Odd longword source faults keep the first halfword postincrement.
						if((_state.A[an] & 1u) != 0u) {
							_state.A[an] = oldSrcA + 2u;
							if(an == 7) _state.SP = _state.A[7];
						}
						return;
					}
					_state.A[an] += srcInc;
					if(_exceptionTaken) {
						return;
					}
					_faultPreserveAddressRegs = true;
					uint32_t dst = BusRead32(_state.A[am]);
					if(_exceptionTaken) {
						return;
					}
					_state.A[am] += dstInc;
					Sub32(dst, src, false, c, v); UpdateFlagsNZ32(dst-src);
				}
			SetC(c); SetV(v); _cycles += 12; return;
		} else {
			// EOR Dn,<ea>
			if(sz == 0) {
				SetFaultPCForEA(mode, reg, 1);
				uint32_t ea = CalcEA(mode, reg, 1);
				uint8_t v = ReadResolvedEA8(ea);
				if(_exceptionTaken) return;
				uint8_t r = (uint8_t)(v ^ (uint8_t)_state.D[dn]);
				WriteResolvedEA8(ea, r);
				UpdateFlagsNZ8(r);
			} else if(sz == 1) {
				SetFaultPCForEA(mode, reg, 2);
				uint32_t ea = CalcEA(mode, reg, 2);
				uint16_t v = ReadResolvedEA16(ea);
				if(_exceptionTaken) return;
				uint16_t r = (uint16_t)(v ^ (uint16_t)_state.D[dn]);
				WriteResolvedEA16(ea, r);
				UpdateFlagsNZ16(r);
			} else {
				SetFaultPCForEA(mode, reg, 4);
				uint32_t ea = CalcEA(mode, reg, 4);
				uint32_t v = ReadResolvedEA32(ea);
				if(_exceptionTaken) return;
				uint32_t r = v ^ _state.D[dn];
				WriteResolvedEA32(ea, r);
				UpdateFlagsNZ32(r);
			}
			SetC(false); SetV(false); _cycles += 8; return;
		}
	}

	// CMP <ea>,Dn  (Dn - <ea> → flags only)
	SetFaultPCForEA(mode, reg, (uint8_t)(1u << sz));
	bool c = false, v = false;
	if(sz == 0) { uint8_t  ea=ReadEA8 (mode,reg); uint8_t  r=Sub8 ((uint8_t) _state.D[dn],ea,false,c,v); UpdateFlagsNZ8 (r); }
	else if(sz==1){uint16_t ea=ReadEA16(mode,reg); uint16_t r=Sub16((uint16_t)_state.D[dn],ea,false,c,v); UpdateFlagsNZ16(r); }
	else          {uint32_t ea=ReadEA32(mode,reg); uint32_t r=Sub32(           _state.D[dn],ea,false,c,v); UpdateFlagsNZ32(r); }
	SetC(c); SetV(v); _cycles += 6;
}

// ===========================================================================
// I_GroupC  $C0-$CF  — AND / MULU / ABCD / EXG / MULS
// ===========================================================================

void GenesisCpu68k::I_GroupC()
{
	uint8_t dn   = (_opword >> 9) & 7;
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;

	// ABCD: $C100-$C10F
	if((_opword & 0xF1F0) == 0xC100) {
		bool useAddr = ((_opword >> 3) & 1) != 0;
		bool z = (_state.SR & SR_Z) != 0;
		uint8_t src, dst, r;
		bool c = false;
		if(useAddr) {
			_state.A[reg] -= (reg == 7) ? 2u : 1u;
			if(reg == 7) _state.SP = _state.A[7];
			uint32_t srcAddr = _state.A[reg];
			_state.A[dn] -= (dn == 7) ? 2u : 1u;
			if(dn == 7) _state.SP = _state.A[7];
			uint32_t dstAddr = _state.A[dn];
			src = BusRead8(srcAddr);
			dst = BusRead8(dstAddr);
			uint16_t res = (src & 0x0Fu) + (dst & 0x0Fu) + (GetX() ? 1u : 0u);
			uint16_t corf = (res > 9u) ? 6u : 0u;
			res += (src & 0xF0u) + (dst & 0xF0u);
			uint8_t binary = static_cast<uint8_t>(res);
			res += corf;
			c = res > 0x9Fu;
			if(c) {
				res -= 0xA0u;
			}
			r = static_cast<uint8_t>(res);
			if(r != 0) {
				z = false;
			}
			SetV((static_cast<uint8_t>(~binary) & r & 0x80) != 0);
			BusWrite8(dstAddr, r);
		} else {
			src = (uint8_t)_state.D[reg];
			dst = (uint8_t)_state.D[dn];
			uint16_t res = (src & 0x0Fu) + (dst & 0x0Fu) + (GetX() ? 1u : 0u);
			uint16_t corf = (res > 9u) ? 6u : 0u;
			res += (src & 0xF0u) + (dst & 0xF0u);
			uint8_t binary = static_cast<uint8_t>(res);
			res += corf;
			c = res > 0x9Fu;
			if(c) {
				res -= 0xA0u;
			}
			r = static_cast<uint8_t>(res);
			if(r != 0) {
				z = false;
			}
			SetV((static_cast<uint8_t>(~binary) & r & 0x80) != 0);
			_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
		}
		SetC(c); SetX(c); SetZ(z); SetN((r >> 7) & 1);
		_cycles += useAddr ? 18 : 6; return;
	}

	// EXG: $C140 / $C148 / $C188
	if((_opword & 0xF130) == 0xC100 && sz >= 1) {
		uint8_t mode2 = (_opword >> 3) & 0x1F;
		if(mode2 == 0x08) {
			// EXG Dn,Dn: 1100 xxx1 0100 0 yyy
			uint32_t tmp = _state.D[dn]; _state.D[dn] = _state.D[reg]; _state.D[reg] = tmp;
			_cycles += 6; return;
		} else if(mode2 == 0x09) {
			// EXG An,An
			uint32_t tmp = _state.A[dn]; _state.A[dn] = _state.A[reg]; _state.A[reg] = tmp;
			if(dn==7||reg==7) _state.SP = _state.A[7];
			_cycles += 6; return;
		} else if(mode2 == 0x11) {
			// EXG Dn,An
			uint32_t tmp = _state.D[dn]; _state.D[dn] = _state.A[reg]; _state.A[reg] = tmp;
			if(reg==7) _state.SP = _state.A[7];
			_cycles += 6; return;
		}
	}

	// MULU <ea>,Dn
	if(sz == 3 && !((_opword >> 8) & 1)) {
		if(mode == 7 && reg == 1 && !_faultPcOverrideValid) {
			// (xxx).l EA: 2 extension words, so saved PC is _preExecPC+6
			_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 7 && reg == 0 && !_faultPcOverrideValid) {
			// (xxx).w EA: 1 extension word, so saved PC is _preExecPC+4
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 4 && !_faultPcOverrideValid) {
			// -(An): the 68000 address-error frame keeps the next prefetch PC.
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
			}
			uint16_t a = (uint16_t)_state.D[dn];
			uint16_t b = ReadEA16(mode, reg);
			if(_exceptionTaken) {
				return;
			}
			uint32_t r = (uint32_t)a * (uint32_t)b;
			_state.D[dn] = r;
		UpdateFlagsNZ32(r); SetC(false); SetV(false);
		_cycles += CalcMuluCycles(b); return;
	}

	// MULS <ea>,Dn
	if(sz == 3 && ((_opword >> 8) & 1)) {
		if(mode == 7 && reg == 1 && !_faultPcOverrideValid) {
			// (xxx).l EA: 2 extension words, so saved PC is _preExecPC+6
			_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 7 && reg == 0 && !_faultPcOverrideValid) {
			// (xxx).w EA: 1 extension word, so saved PC is _preExecPC+4
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 4 && !_faultPcOverrideValid) {
			// -(An): the 68000 address-error frame keeps the next prefetch PC.
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		}
		int16_t a = (int16_t)_state.D[dn];
		int16_t b = (int16_t)ReadEA16(mode, reg);
		if(_exceptionTaken) {
			return;
		}
		int32_t r = (int32_t)a * (int32_t)b;
		_state.D[dn] = (uint32_t)r;
		UpdateFlagsNZ32((uint32_t)r); SetC(false); SetV(false);
		_cycles += CalcMulsCycles((uint16_t)b); return;
	}

	// AND: 1100 ddd s mm mmm rrr
	bool toEA = ((_opword >> 8) & 1) != 0;
	if(sz == 0) {
		if(toEA) {
			SetFaultPCForEA(mode, reg, 1);
			uint32_t eaAddr = CalcEA(mode, reg, 1);
			uint8_t ea = ReadResolvedEA8(eaAddr);
			if(_exceptionTaken) return;
			uint8_t r = (uint8_t)_state.D[dn] & ea;
			WriteResolvedEA8(eaAddr, r);
			UpdateFlagsNZ8(r);
		} else {
			SetFaultPCForEA(mode, reg, 1);
			uint8_t ea = ReadEA8(mode, reg);
			if(_exceptionTaken) return;
			uint8_t r = (uint8_t)_state.D[dn] & ea;
			_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
			UpdateFlagsNZ8(r);
		}
	} else if(sz == 1) {
		if(toEA) {
			SetFaultPCForEA(mode, reg, 2);
			uint32_t eaAddr = CalcEA(mode, reg, 2);
			uint16_t ea = ReadResolvedEA16(eaAddr);
			if(_exceptionTaken) return;
			uint16_t r = (uint16_t)_state.D[dn] & ea;
			WriteResolvedEA16(eaAddr, r);
			UpdateFlagsNZ16(r);
		} else {
			SetFaultPCForEA(mode, reg, 2);
			uint16_t ea = ReadEA16(mode, reg);
			if(_exceptionTaken) return;
			uint16_t r = (uint16_t)_state.D[dn] & ea;
			_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | r;
			UpdateFlagsNZ16(r);
		}
	} else {
		if(toEA) {
			SetFaultPCForEA(mode, reg, 4);
			uint32_t eaAddr = CalcEA(mode, reg, 4);
			uint32_t ea = ReadResolvedEA32(eaAddr);
			if(_exceptionTaken) return;
			uint32_t r = _state.D[dn] & ea;
			WriteResolvedEA32(eaAddr, r);
			UpdateFlagsNZ32(r);
		} else {
			SetFaultPCForEA(mode, reg, 4);
			uint32_t ea = ReadEA32(mode, reg);
			if(_exceptionTaken) return;
			uint32_t r = _state.D[dn] & ea;
			_state.D[dn] = r;
			UpdateFlagsNZ32(r);
		}
	}
	SetC(false); SetV(false); _cycles += 8;
}

// ===========================================================================
// I_GroupD  $D0-$DF  — ADD / ADDX / ADDA
// ===========================================================================

void GenesisCpu68k::I_GroupD()
{
	uint8_t dn   = (_opword >> 9) & 7;
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;
	uint8_t sz   = (_opword >> 6) & 3;
	bool    toEA = ((_opword >> 8) & 1) != 0;

	// ADDA: sz=3 (word sign-extended) or sz=3+bit8 (long)
	if(sz == 3) {
		if(mode == 7 && reg == 1 && !_faultPcOverrideValid) {
			// (xxx).l EA: 2 extension words, so saved PC is _preExecPC+6
			_faultPcOverride = (_state.PC + 4) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 7 && reg == 0 && !_faultPcOverrideValid) {
			// (xxx).w EA: 1 extension word, so saved PC is _preExecPC+4
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		} else if(mode == 4 && !toEA && !_faultPcOverrideValid) {
			// -(An): the 68000 address-error frame keeps the next prefetch PC.
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;
		}
		int32_t src = toEA ? (int32_t)ReadEA32(mode,reg) : (int32_t)(int16_t)ReadEA16(mode,reg);
		_state.A[dn] += (uint32_t)src; if(dn==7) _state.SP = _state.A[7];
		_cycles += 8; return;
	}

	// ADDX: 1101 ddd1 ss 00x rrr  (mode=0 for Dn, mode=1 for -(An))
	if(toEA && (mode == 0 || mode == 1)) {
		bool c = false, v = false;
		bool oldZ = GetZ();
		if(mode == 0) {
			if(sz == 0) {
				uint8_t a = (uint8_t)_state.D[dn];
				uint8_t b = (uint8_t)_state.D[reg];
				uint8_t r = Add8(a, b, GetX(), c, v);
				_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
				UpdateFlagsNZ8(r);
				SetZ(oldZ && (r == 0));
			} else if(sz == 1) {
				uint16_t a = (uint16_t)_state.D[dn];
				uint16_t b = (uint16_t)_state.D[reg];
				uint16_t r = Add16(a, b, GetX(), c, v);
				_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | r;
				UpdateFlagsNZ16(r);
				SetZ(oldZ && (r == 0));
			} else {
				uint32_t a = _state.D[dn];
				uint32_t b = _state.D[reg];
				uint32_t r = Add32(a, b, GetX(), c, v);
				_state.D[dn] = r;
				UpdateFlagsNZ32(r);
				SetZ(oldZ && (r == 0));
			}
		} else {
			uint8_t srcDec = (sz == 0) ? 1 : (sz == 1) ? 2 : 4;
			uint8_t dstDec = srcDec;
			if(reg == 7 && sz == 0) srcDec = 2;
			if(dn  == 7 && sz == 0) dstDec = 2;
			uint32_t oldSrcA = _state.A[reg];
			uint32_t oldDstA = _state.A[dn];

			_state.A[reg] -= srcDec; if(reg == 7) _state.SP = _state.A[7];
			_faultPcOverride = (_state.PC + 2) & 0x00FFFFFFu;
			_faultPcOverrideValid = true;

			if(sz == 0) {
				uint8_t src = BusRead8(_state.A[reg]);
				if(_exceptionTaken) {
					return;
				}
				_state.A[dn] -= dstDec; if(dn == 7) _state.SP = _state.A[7];
				uint8_t dst = BusRead8(_state.A[dn]);
				if(_exceptionTaken) {
					if(sz == 2) {
						_state.A[dn] = oldDstA;
						if(dn == 7) _state.SP = _state.A[7];
					}
					return;
				}
				uint8_t r = Add8(dst, src, GetX(), c, v);
				BusWrite8(_state.A[dn], r);
				UpdateFlagsNZ8(r);
				SetZ(oldZ && (r == 0));
			} else if(sz == 1) {
				uint16_t src = BusRead16(_state.A[reg]);
				if(_exceptionTaken) {
					return;
				}
				_state.A[dn] -= dstDec; if(dn == 7) _state.SP = _state.A[7];
				uint16_t dst = BusRead16(_state.A[dn]);
				if(_exceptionTaken) {
					if(sz == 2) {
						_state.A[dn] = oldDstA;
						if(dn == 7) _state.SP = _state.A[7];
					}
					return;
				}
				uint16_t r = Add16(dst, src, GetX(), c, v);
				BusWrite16(_state.A[dn], r);
				UpdateFlagsNZ16(r);
				SetZ(oldZ && (r == 0));
			} else {
				uint32_t src = 0;
				if((_state.A[reg] & 1u) != 0u) {
					(void)BusRead16(_state.A[reg] + 2);
				} else {
					src = BusRead32(_state.A[reg]);
				}
				if(_exceptionTaken) {
					_state.A[reg] = oldSrcA;
					if(reg == 7) _state.SP = _state.A[7];
					return;
				}
				_state.A[dn] -= dstDec; if(dn == 7) _state.SP = _state.A[7];
				uint32_t dst = 0;
				if((_state.A[dn] & 1u) != 0u) {
					CheckAddressError(_state.A[dn] + 2, false, 4, MemoryOperationType::Read);
				} else {
					dst = BusRead32(_state.A[dn]);
				}
				if(_exceptionTaken) {
					if(sz == 2) {
						_state.A[dn] = oldDstA;
						if(dn == 7) _state.SP = _state.A[7];
					}
					return;
				}
				uint32_t r = Add32(dst, src, GetX(), c, v);
				BusWrite32(_state.A[dn], r);
				UpdateFlagsNZ32(r);
				SetZ(oldZ && (r == 0));
			}
		}
		SetC(c); SetV(v); SetX(c); _cycles += 12; return;
	}

	// ADD
	SetFaultPCForEA(mode, reg, (uint8_t)(1u << sz));
	bool carry = false, overflow = false;
	if(sz == 0) {
		if(toEA) {
			uint32_t eaAddr = CalcEA(mode, reg, 1);
			uint8_t ea = ReadResolvedEA8(eaAddr);
			uint8_t r = Add8((uint8_t)_state.D[dn], ea, false, carry, overflow);
			WriteResolvedEA8(eaAddr, r);
			UpdateFlagsNZ8(r);
		} else {
			uint8_t ea = ReadEA8(mode, reg);
			uint8_t r = Add8(ea, (uint8_t)_state.D[dn], false, carry, overflow);
			_state.D[dn] = (_state.D[dn] & 0xFFFFFF00u) | r;
			UpdateFlagsNZ8(r);
		}
	} else if(sz == 1) {
		if(toEA) {
			uint32_t eaAddr = CalcEA(mode, reg, 2);
			uint16_t ea = ReadResolvedEA16(eaAddr);
			uint16_t r = Add16((uint16_t)_state.D[dn], ea, false, carry, overflow);
			WriteResolvedEA16(eaAddr, r);
			UpdateFlagsNZ16(r);
		} else {
			uint16_t ea = ReadEA16(mode, reg);
			uint16_t r = Add16(ea, (uint16_t)_state.D[dn], false, carry, overflow);
			_state.D[dn] = (_state.D[dn] & 0xFFFF0000u) | r;
			UpdateFlagsNZ16(r);
		}
	} else {
		if(toEA) {
			uint32_t eaAddr = CalcEA(mode, reg, 4);
			uint32_t ea = ReadResolvedEA32(eaAddr);
			uint32_t r = Add32(_state.D[dn], ea, false, carry, overflow);
			WriteResolvedEA32(eaAddr, r);
			UpdateFlagsNZ32(r);
		} else {
			uint32_t ea = ReadEA32(mode, reg);
			uint32_t r = Add32(ea, _state.D[dn], false, carry, overflow);
			_state.D[dn] = r;
			UpdateFlagsNZ32(r);
		}
	}
	SetC(carry); SetV(overflow); SetX(carry); _cycles += 8;
}

// ===========================================================================
// I_GroupE  $E0-$EF  — shift / rotate
// ===========================================================================

void GenesisCpu68k::I_GroupE()
{
	uint8_t sz   = (_opword >> 6) & 3;
	uint8_t mode = (_opword >> 3) & 7;
	uint8_t reg  = _opword & 7;

	if(sz == 3) {
		// Memory shift — 1-bit operation on word EA
		// 1110 tt d1 11 mmm rrr: tt selects arith/logical/rox/rotate,
		// and bit 8 is the direction.
		uint8_t kind = (_opword >> 9) & 3;
		bool left   = (kind & 1) != 0;
		bool isArith = false, isRotate = false, isRox = false;
		switch(kind) {
			case 0: isArith = true; break;
			case 1: break;  // LSL/LSR
			case 2: isRox = true; break;
			case 3: isRotate = true; break;
		}
		// For memory, direction is at bit 8
		left = (_opword >> 8) & 1;
		DoShiftRotate(left, isArith, isRotate, isRox, 1, mode, reg, 1, false);
		return;
	}

	// Register shift: 1110 cccc d ss t 00k rrr
	// where cccc = count or register, d = direction, ss = size, t = type (arith/logical/rot/rox), k = count/register select
	uint8_t count    = (_opword >> 9) & 7;
	bool    left     = (_opword >> 8) & 1;
	bool    countReg = (_opword >> 5) & 1;
	uint8_t kind     = (_opword >> 3) & 3;

	bool isArith = (kind == 0), isRox = (kind == 2), isRotate = (kind == 3);
	// kind 1 = logical shift

	if(countReg) count = count;  // it's a register number
	else if(count == 0) count = 8;

	// For register-direct shifts, the destination is always a data register (mode=0).
	// Bits 5-3 of the opword encode {countReg, kind} — not an EA mode — so passing
	// `mode` here would incorrectly route to the memory-shift path inside DoShiftRotate.
	DoShiftRotate(left, isArith, isRotate, isRox, sz, 0, reg, count, countReg != 0);
}

// ===========================================================================
// I_FLine  $F0-$FF  — F-line trap
// ===========================================================================

void GenesisCpu68k::I_FLine()
{
	TakeException(11);
}
