#include "pch.h"
#include <algorithm>
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"

// ---------------------------------------------------------------------------
// Serialization helpers (local)
// ---------------------------------------------------------------------------
namespace {
	template<typename T>
	void AppV(vector<uint8_t>& out, const T& v)
	{
		size_t old = out.size();
		out.resize(old + sizeof(T));
		memcpy(out.data() + old, &v, sizeof(T));
	}
	template<typename T>
	bool RdV(const vector<uint8_t>& d, size_t& o, T& v)
	{
		if(o + sizeof(T) > d.size()) return false;
		memcpy(&v, d.data() + o, sizeof(T));
		o += sizeof(T);
		return true;
	}
}

// ===========================================================================
// Init / Reset
// ===========================================================================

void GenesisVdp::Init(Emulator* emu, GenesisNativeBackend* backend, bool isPal)
{
	_emu     = emu;
	_backend = backend;
	Reset(isPal);
}

void GenesisVdp::Reset(bool isPal)
{
	_isPal = isPal;
	memset(_vram,  0, sizeof(_vram));
	memset(_cram,  0, sizeof(_cram));
	memset(_vsram, 0, sizeof(_vsram));
	memset(_reg,   0, sizeof(_reg));
	// Initialise palette from CRAM (all zeros → opaque black via CramWordToArgb)
	for(uint8_t i = 0; i < 64; i++) {
		RefreshPalette(i);
	}

	_ctrlPend    = false;
	_ctrlFirst   = 0;
	_addrReg     = 0;
	_codeReg     = 0;
	_readBuf     = 0;
	_writeHi     = 0;
	_writeHiData = false;
	_writeHiCtrl = false;

	// Default status: FIFO empty set, PAL bit set appropriately
	_status = 0x3600 | (_isPal ? 0x0001u : 0x0000u);  // bit 9 = FIFO empty, always 1 at reset
	_debugReg = 0;

	_vintPending = false;
	_vintNew     = false;
	_hintPending = false;
	_hintCounter = 0;
	_statusReadLatch = 0;
	_statusReadLatchValid = false;

	_dmaType     = DmaType::None;
	_dmaSrc      = 0;
	_dmaLen      = 0;
	_dmaFillVal  = 0;
	_dmaFillPend = false;
	_dmaBusStartDelayMclk = 0;
	_dmaBusMclkRemainder = 0;
	_dmaVdpMclkRemainder = 0;

	_interlaceField = false;

	_scanline    = 0;
	_frameCount  = 0;
	_prevLineDotOverflow = false;
	_fb          = nullptr;
	_fbW         = 320;
	_fbH         = 224;

	_mclkPos        = 0;
	_lineBegun      = false;
	_vintFiredFrame = false;
	_frameFb        = nullptr;
	_frameFbW       = 320;
	_frameFbH       = 224;
	memset(_lineBackdropMask, 0, sizeof(_lineBackdropMask));

	// Sensible power-on register defaults
	_reg[1]  = 0x04;   // display disable at reset
	_reg[12] = 0x81;   // H40 mode, no shadow/highlight
	_reg[15] = 0x02;   // auto-increment = 2
}

// ===========================================================================
// VRAM / CRAM / VSRAM word access (all big-endian pairs in byte arrays)
// ===========================================================================

uint16_t GenesisVdp::VramRead(uint16_t wordAddr) const
{
	uint32_t b = (uint32_t)(wordAddr & 0x7FFFu) * 2u;
	return ((uint16_t)_vram[b] << 8) | _vram[b + 1];
}

void GenesisVdp::VramWrite(uint16_t wordAddr, uint16_t value)
{
	uint32_t b = (uint32_t)(wordAddr & 0x7FFFu) * 2u;
	_vram[b]     = (uint8_t)(value >> 8);
	_vram[b + 1] = (uint8_t)(value);
}

uint16_t GenesisVdp::CramRead(uint8_t idx) const
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	return ((uint16_t)_cram[b] << 8) | _cram[b + 1];
}

void GenesisVdp::CramWrite(uint8_t idx, uint16_t value)
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	_cram[b]     = (uint8_t)(value >> 8);
	_cram[b + 1] = (uint8_t)(value);
	RefreshPalette(idx & 0x3Fu);
}

uint16_t GenesisVdp::VsramRead(uint8_t idx) const
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	if(b + 1 >= sizeof(_vsram)) return 0;
	return ((uint16_t)_vsram[b] << 8) | _vsram[b + 1];
}

void GenesisVdp::VsramWrite(uint8_t idx, uint16_t value)
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	if(b + 1 >= sizeof(_vsram)) return;
	_vsram[b]     = (uint8_t)(value >> 8);
	_vsram[b + 1] = (uint8_t)(value);
}

// ===========================================================================
// Palette
// ===========================================================================

// CRAM word format: 0000 BBB0 GGG0 RRR0
// B = bits 11:9, G = bits 7:5, R = bits 3:1
uint32_t GenesisVdp::CramWordToArgb(uint16_t w)
{
	uint8_t r3 = (w >> 1) & 7;
	uint8_t g3 = (w >> 5) & 7;
	uint8_t b3 = (w >> 9) & 7;
	// Match GenplusGX Mode 5 normal palette expansion:
	// 3-bit channel -> even 4-bit steps -> duplicated nibble.
	uint8_t r8 = (uint8_t)(r3 * 34u);
	uint8_t g8 = (uint8_t)(g3 * 34u);
	uint8_t b8 = (uint8_t)(b3 * 34u);
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

// Match GenplusGX shadow lookup: 3-bit channel duplicated as a nibble.
uint32_t GenesisVdp::CramWordToArgbShadow(uint16_t w)
{
	uint8_t r3 = (w >> 1) & 7;
	uint8_t g3 = (w >> 5) & 7;
	uint8_t b3 = (w >> 9) & 7;
	uint8_t r8 = (uint8_t)(r3 * 17u);
	uint8_t g8 = (uint8_t)(g3 * 17u);
	uint8_t b8 = (uint8_t)(b3 * 17u);
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

// Match GenplusGX highlight lookup: add 7 to the 3-bit channel, then duplicate.
uint32_t GenesisVdp::CramWordToArgbHighlight(uint16_t w)
{
	uint8_t r3 = (w >> 1) & 7;
	uint8_t g3 = (w >> 5) & 7;
	uint8_t b3 = (w >> 9) & 7;
	uint8_t r8 = (uint8_t)((r3 + 7u) * 17u);
	uint8_t g8 = (uint8_t)((g3 + 7u) * 17u);
	uint8_t b8 = (uint8_t)((b3 + 7u) * 17u);
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

void GenesisVdp::RefreshPalette(uint8_t idx)
{
	uint16_t w         = CramRead(idx);
	_palette[idx]          = CramWordToArgb         (w);
	_shadowPalette[idx]    = CramWordToArgbShadow   (w);
	_highlightPalette[idx] = CramWordToArgbHighlight(w);
}

// ===========================================================================
// Register write
// ===========================================================================

void GenesisVdp::WriteReg(uint8_t r, uint8_t val)
{
	if(r >= 24) return;
#ifdef _DEBUG
	uint8_t oldVal = _reg[r];
#endif
	_reg[r] = val;

	switch(r) {
		case 1:
			// Display enable / V-int enable / DMA enable — update status PAL bit
			_status = (_status & ~0x0001u) | (_isPal ? 0x0001u : 0x0000u);
#ifdef _DEBUG
			{
				bool oldDisp = (oldVal & 0x40u) != 0;
				bool newDisp = (val    & 0x40u) != 0;
				if(oldDisp != newDisp) {
					LogDebug(string("[MD Native][VDP] Display ") + (newDisp ? "enabled" : "disabled")
						+ " (R1=$" + HexUtilities::ToHex(val)
						+ ", frame=" + std::to_string(_frameCount)
						+ ", line=" + std::to_string(_scanline) + ")");
				}
			}
#endif
			break;
		case 10:
			// H-int counter reload — takes effect next frame reload
			break;
		case 12:
			// Mode register 4: RS0/RS1 (H32/H40), shadow/highlight, interlace
			break;
		case 15:
			// Auto-increment: 0 is legal (no advance)
			break;
		default:
			break;
	}
}

// ===========================================================================
// Control port state machine
// ===========================================================================

void GenesisVdp::AdvanceAddr()
{
	uint8_t inc = AutoInc();
	if(inc == 0) return;
	_addrReg = (uint16_t)(_addrReg + inc);
}

void GenesisVdp::PrimeBuf()
{
	// Fill the read-ahead buffer from the current address/code target
	uint8_t cd = _codeReg & 0x0F;  // lower 4 bits of code select source

	switch(cd) {
		case 0x00: // VRAM read
			_readBuf = ((uint16_t)_vram[_addrReg & 0xFFFFu] << 8)
			         | _vram[(_addrReg + 1u) & 0xFFFFu];
			AdvanceAddr();
			break;
		case 0x08: // CRAM read
			_readBuf = CramRead((_addrReg >> 1) & 0x3F);
			AdvanceAddr();
			break;
		case 0x04: // VSRAM read
			_readBuf = VsramRead((_addrReg >> 1) & 0x27u);  // 40 entries, max idx 0x27
			AdvanceAddr();
			break;
		default:
			break;
	}
}

void GenesisVdp::BeginOperation()
{
	// Check if this is a DMA start
	if((_codeReg & 0x20) && DmaEnabled()) {
		uint8_t dmaMode = (_reg[23] >> 6) & 3;
		if(dmaMode == 2) {
			// VRAM fill — starts on next data-port write.
			// Fill DMA length behaves as a byte count in common software usage
			// (e.g. Sonic disassembly fillVRAM macro programs end-start-1).
			// Keep 0 => 65536 compatibility.
			_dmaType     = DmaType::VramFill;
			uint32_t rawLen = (uint32_t)_reg[19] | ((uint32_t)_reg[20] << 8);
			_dmaLen      = (rawLen == 0u) ? 0x10000u : (rawLen + 1u);
			_dmaFillPend = false;
			_dmaBusStartDelayMclk = 0;
			_status |= 0x0002u; // DMA busy
		} else if(dmaMode == 3) {
			// VRAM copy
			_dmaType = DmaType::VramCopy;
			_dmaLen  = (uint32_t)_reg[19] | ((uint32_t)_reg[20] << 8);
			if(_dmaLen == 0) _dmaLen = 0x10000u;
			_dmaSrc  = (uint32_t)_reg[21]
			         | ((uint32_t)_reg[22] << 8);   // source VRAM word address
			_dmaBusStartDelayMclk = 0;
			_dmaVdpMclkRemainder = 0;
			_status |= 0x0002u;
		} else {
			// Bus 68K to VDP (DMA mode 0 or 1)
			constexpr uint8_t Dma68kStartDelayMclk = 32u;
			_dmaType = DmaType::Bus68k;
			_dmaLen  = (uint32_t)_reg[19] | ((uint32_t)_reg[20] << 8);
			if(_dmaLen == 0) _dmaLen = 0x10000u;
			_dmaSrc  = ((uint32_t)_reg[21]       )
			         | ((uint32_t)_reg[22] << 8  )
			         | ((uint32_t)(_reg[23] & 0x7Fu) << 16);
			_dmaSrc <<= 1;  // source is in words, convert to bytes
			_dmaBusStartDelayMclk = Dma68kStartDelayMclk;
			_dmaBusMclkRemainder = 0;
			_status |= 0x0002u;
			// 68K bus DMA is paced from the backend scheduler via Consume68kBusDma().
		}
	} else {
		// Read operation: prime buffer
		PrimeBuf();
	}
}

void GenesisVdp::HandleControlWrite(uint16_t word)
{
	// Register write: bit15=1, bit14=0
	if((word & 0xC000u) == 0x8000u) {
		uint8_t reg = (uint8_t)((word >> 8) & 0x1Fu);
		uint8_t val = (uint8_t)(word & 0xFFu);
		WriteReg(reg, val);
		_ctrlPend = false;  // reset pair state
		return;
	}

	if(!_ctrlPend) {
		// First word of address command
		_ctrlFirst = word;
		_ctrlPend  = true;
	} else {
		// Second word — assemble address and code
		_ctrlPend = false;
		// Flush any in-flight DMA before overwriting the destination encoded by
		// the previous control command. GenTest image slides rely on this when
		// they arm a VRAM DMA, then immediately program the tilemap target.
		if(_dmaType != DmaType::None && (_dmaLen > 0 || _dmaFillPend)) {
			ExecPendingDma();
		}
		_addrReg  = (uint16_t)((_ctrlFirst & 0x3FFFu) | ((word & 0x0003u) << 14));
		_codeReg  = (uint8_t)((_ctrlFirst >> 14) | ((word >> 2) & 0x3Cu));
		BeginOperation();
	}
}

// ===========================================================================
// Bus interface — called by GenesisNativeBackend ReadCartBus / WriteCartBus
// addr is the full 24-bit bus address in range $C00000-$C0001F
// ===========================================================================

uint8_t GenesisVdp::ReadByte(uint32_t addr)
{
	uint32_t reg = addr & 0x1Fu;
	bool isStatusOddRead = (reg >= 0x04u && reg <= 0x07u && (reg & 1u));
	if(_statusReadLatchValid && !isStatusOddRead) {
		// Latch is only valid for the immediate low-byte status read.
		_statusReadLatchValid = false;
	}

	switch(reg) {
		// Data port: $C00000-$C00003 (all map to data read)
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03: {
			// Even byte = high byte of read buffer, odd = low byte
			uint8_t shift = (addr & 1u) ? 0 : 8;
			uint8_t result = (uint8_t)(_readBuf >> shift);

			if((addr & 1u) == 1u) {
				// Low byte — advance and refill buffer
				PrimeBuf();
			}
			return result;
		}

		// Control / status port: $C00004-$C00007
			case 0x04:
			case 0x05:
			case 0x06:
			case 0x07: {
				auto latchStatusRead = [&]() -> uint16_t {
					// Reading the status port clears the pending-control latch.
					_ctrlPend = false;
					_writeHiCtrl = false;

					// Keep DMA busy bit coherent with pending DMA state.
					bool dmaBusy = (_dmaType != DmaType::None && _dmaLen > 0);
					if(dmaBusy) _status |= 0x0002u;
					else        _status &= ~0x0002u;

					// FIFO bits (bits 9:8):
					//   bit 9: FIFO empty — 1 when no data writes are queued.
					//           Since we commit writes immediately, always 1 unless
					//           a Bus68k DMA is actively feeding the VDP.
					//   bit 8: FIFO full  — 1 when all 4 FIFO slots are taken.
					//           We signal this when Bus68k DMA is in flight (VDP
					//           is fully occupied draining the transfer queue).
					bool busTransfer = (_dmaType == DmaType::Bus68k && _dmaLen > 0);
					if(busTransfer) {
						_status &= ~0x0200u;  // FIFO not empty
						_status |=  0x0100u;  // FIFO full
					} else {
						_status |=  0x0200u;  // FIFO empty
						_status &= ~0x0100u;  // FIFO not full
					}

					uint16_t s = _status;
					// Bit 7: V-interrupt pending — set at VBlank, cleared on status read.
					if(_vintPending) {
						s |= 0x0080u;
						_vintPending = false;
					}
					// Hardware clears sprite-overflow (bit 6) and sprite-collision (bit 5) on read.
					_status &= ~0x0060u;
					return s;
				};

				if((reg & 1u) == 0u) {
					// Even byte read (high byte): latch the full status so the
					// subsequent odd-byte read returns the matching low byte.
					_statusReadLatch = latchStatusRead();
					_statusReadLatchValid = true;
					return (uint8_t)(_statusReadLatch >> 8);
				}

				if(_statusReadLatchValid) {
					_statusReadLatchValid = false;
					return (uint8_t)_statusReadLatch;
				}

				uint16_t s = latchStatusRead();
				return (uint8_t)s;
			}

			// H/V counter: $C00008-$C00009, mirrored through $C0000A-$C0000F
			case 0x08:
			case 0x09:
			case 0x0A:
			case 0x0B:
			case 0x0C:
			case 0x0D:
			case 0x0E:
			case 0x0F: {
				// H/V counters derived from backend master-clock timeline.
				uint64_t baseClock = _backend ? _backend->GetMasterClock() : 0;
				uint32_t frameMclk = (uint32_t)TotalScanlines() * MCLKS_PER_LINE;
				uint32_t framePos  = frameMclk ? (uint32_t)(baseClock % frameMclk) : 0u;
				uint32_t line      = framePos / MCLKS_PER_LINE;
				uint32_t hSub      = framePos % MCLKS_PER_LINE;

				if((reg & 1u) == 0u) {
					// Even = V counter (high byte of word read).
					uint8_t vc = (uint8_t)(line & 0xFFu);
					// For NTSC: lines 234-255 are remapped (V-counter wraps).
					if(!_isPal && line >= 234u) {
						vc = (uint8_t)(line - 6u);
					}
					return vc;
				}

				// Odd = H counter (low byte of word read).
				return (uint8_t)((hSub * 256u) / MCLKS_PER_LINE);
			}

			// Debug register: $C0001C-$C0001F (mirrors)
			case 0x1C:
			case 0x1E:
				return (uint8_t)(_debugReg >> 8);
			case 0x1D:
			case 0x1F:
				return (uint8_t)_debugReg;

		default:
			return 0xFFu;
	}
}

void GenesisVdp::WriteByte(uint32_t addr, uint8_t val)
{
	_statusReadLatchValid = false;
	uint32_t reg = addr & 0x1Fu;

	switch(reg) {
		// Data port: $C00000-$C00003
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03: {
			if((addr & 1u) == 0u) {
				// High byte — stash in dedicated write buffer (does NOT touch _readBuf)
				_writeHi     = val;
				_writeHiData = true;
			} else {
				// Low byte — assemble word and commit
				uint16_t word = _writeHiData
				              ? (uint16_t)(((uint16_t)_writeHi << 8) | val)
				              : (uint16_t)(((uint16_t)0x00u    << 8) | val);
				_writeHiData = false;

				uint8_t cd = _codeReg & 0x0Fu;
#ifdef _DEBUG
				{
					static uint32_t sDataPortLogCount = 0;
					if(sDataPortLogCount < 128) {
						LogDebug("[MD Native][VDP] DATA write cd=$" + HexUtilities::ToHex(cd) +
							" addr=$" + HexUtilities::ToHex(_addrReg) +
							" word=$" + HexUtilities::ToHex(word));
						sDataPortLogCount++;
					}
				}
#endif

				if(_dmaType == DmaType::VramFill && !_dmaFillPend) {
					TriggerDmaFill(word);
					return;
				}

					switch(cd) {
						case 0x01:
							_vram[_addrReg & 0xFFFFu]        = (uint8_t)(word >> 8);
							_vram[(_addrReg + 1u) & 0xFFFFu] = (uint8_t)word;
#ifdef _DEBUG
							{
								static uint32_t sVramWriteLogCount = 0;
								if(sVramWriteLogCount < 64) {
									LogDebug("[MD Native][VDP] VRAM write addr=$" + HexUtilities::ToHex(_addrReg & 0xFFFFu) +
										" word=$" + HexUtilities::ToHex(word));
									sVramWriteLogCount++;
								}
							}
#endif
							break;
						case 0x03:
							CramWrite((_addrReg >> 1) & 0x3Fu, word);
#ifdef _DEBUG
							{
								static uint32_t sCramWriteLogCount = 0;
								if(sCramWriteLogCount < 64) {
									LogDebug("[MD Native][VDP] CRAM write idx=$" + HexUtilities::ToHex((_addrReg >> 1) & 0x3Fu) +
										" word=$" + HexUtilities::ToHex(word));
									sCramWriteLogCount++;
								}
							}
#endif
							break;
						case 0x05: VsramWrite((_addrReg >> 1) & 0x27u, word); break;
						default: break;
					}
				AdvanceAddr();
			}
			break;
		}

		// Control port: $C00004-$C00007
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07: {
			if((addr & 1u) == 0u) {
				// High byte — stash in dedicated control write buffer
				_writeHi     = val;
				_writeHiCtrl = true;
			} else {
				// Low byte — assemble word and dispatch
				uint16_t word = _writeHiCtrl
				              ? (uint16_t)(((uint16_t)_writeHi << 8) | val)
				              : (uint16_t)(((uint16_t)0x00u    << 8) | val);
				_writeHiCtrl = false;
				HandleControlWrite(word);
			}
			break;
		}

		// Debug register: $C0001C-$C0001F (mirrors)
		case 0x1C:
		case 0x1E:
			_debugReg = (uint16_t)((_debugReg & 0x00FFu) | ((uint16_t)val << 8));
			break;
		case 0x1D:
		case 0x1F:
			_debugReg = (uint16_t)((_debugReg & 0xFF00u) | val);
			break;

		default:
			break;
	}
}

// ===========================================================================
// DMA
// ===========================================================================

void GenesisVdp::TriggerDmaFill(uint16_t data)
{
	_dmaFillVal  = data;
	_dmaFillPend = true;
	// Actual fill executes through ConsumeInternalDma() in the backend scheduler.
}

void GenesisVdp::ExecDmaBus68k(uint32_t maxWords)
{
	if(!_backend || _dmaType != DmaType::Bus68k || _dmaLen == 0 || maxWords == 0) return;
	uint8_t  cd  = _codeReg & 0x0Fu;
	uint32_t len = (_dmaLen < maxWords) ? _dmaLen : maxWords;
	uint32_t src = _dmaSrc;

	// 68K source DMA increments only within the current 128KB source window.
	// When offset overflows, it wraps within that window (hardware quirk).
	uint32_t srcBase   = src & ~0x1FFFFu;
	uint32_t srcOffset = src &  0x1FFFFu;

	while(len > 0) {
		uint8_t  hi  = _backend->CpuBusRead8(srcBase |  srcOffset);
		uint8_t  lo  = _backend->CpuBusRead8(srcBase | ((srcOffset + 1u) & 0x1FFFFu));
		uint16_t word = ((uint16_t)hi << 8) | lo;
		srcOffset = (srcOffset + 2u) & 0x1FFFFu;

			switch(cd) {
				case 0x01:
					_vram[_addrReg & 0xFFFFu]        = (uint8_t)(word >> 8);
					_vram[(_addrReg + 1u) & 0xFFFFu] = (uint8_t)word;
					break;
				case 0x03: CramWrite ((_addrReg >> 1) & 0x3Fu, word); break;
				case 0x05: VsramWrite((_addrReg >> 1) & 0x27u, word); break;
				default: break;
			}
		AdvanceAddr();
		len--;
		_dmaLen--;
	}

	src = srcBase | srcOffset;
	_dmaSrc  = src;
	// Update DMA source registers for the next transfer chunk.
	uint32_t srcWords = src >> 1;
	_reg[21] = (uint8_t)(srcWords);
	_reg[22] = (uint8_t)(srcWords >> 8);
	_reg[23] = (_reg[23] & 0xC0u) | (uint8_t)((srcWords >> 16) & 0x7Fu);

	if(_dmaLen == 0) {
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaBusStartDelayMclk = 0;
		_dmaBusMclkRemainder = 0;
	}
}

void GenesisVdp::ExecDmaFill(uint32_t maxBytes)
{
	if(_dmaType != DmaType::VramFill || _dmaLen == 0 || maxBytes == 0) {
		return;
	}

	uint8_t cd = _codeReg & 0x0Fu;
	uint32_t len = (_dmaLen < maxBytes) ? _dmaLen : maxBytes;

	switch(cd) {
		case 0x01: {
			// VRAM fill is byte-oriented. The source comes from the upper byte of
			// the data-port word, matching the standard #$yy00 fill command form.
			uint8_t fillByte = (uint8_t)(_dmaFillVal >> 8);
			while(len > 0) {
				_vram[_addrReg & 0xFFFFu] = fillByte;
				AdvanceAddr();
				len--;
				_dmaLen--;
			}
			break;
		}

		case 0x03:
			while(len > 0) {
				CramWrite((_addrReg >> 1) & 0x3Fu, _dmaFillVal);
				AdvanceAddr();
				len--;
				_dmaLen--;
			}
			break;

		case 0x05:
			while(len > 0) {
				VsramWrite((_addrReg >> 1) & 0x27u, _dmaFillVal);
				AdvanceAddr();
				len--;
				_dmaLen--;
			}
			break;

		default:
			// Invalid DMA-fill destinations do not write, but the VDP still
			// consumes the programmed length and advances the address.
			while(len > 0) {
				AdvanceAddr();
				len--;
				_dmaLen--;
			}
			break;
	}

	if(_dmaLen == 0) {
		_dmaFillPend = false;
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaVdpMclkRemainder = 0;
	}
}

void GenesisVdp::ExecDmaCopy(uint32_t maxWords)
{
	if(_dmaType != DmaType::VramCopy || _dmaLen == 0 || maxWords == 0) {
		return;
	}

	// Copy from VRAM source to VRAM destination, one byte at a time
	uint32_t src = _dmaSrc;   // byte address in VRAM
	uint32_t len = (_dmaLen < maxWords) ? _dmaLen : maxWords;

	while(len > 0) {
		uint8_t val = _vram[src & 0xFFFFu];
		_vram[_addrReg & 0xFFFFu] = val;
		src++;
		AdvanceAddr();
		len--;
		_dmaLen--;
	}
	_dmaSrc = src;

	if(_dmaLen == 0) {
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaVdpMclkRemainder = 0;
	}
}

void GenesisVdp::ExecPendingDma()
{
	// Force-complete pending DMA (used by legacy call sites or debugger paths).
	if(_dmaType == DmaType::Bus68k) {
		ExecDmaBus68k(_dmaLen);
	} else if(_dmaType == DmaType::VramFill && _dmaFillPend) {
		ExecDmaFill(_dmaLen);
	} else if(_dmaType == DmaType::VramCopy) {
		ExecDmaCopy(_dmaLen);
	}
}

uint32_t GenesisVdp::ConsumeInternalDma(uint32_t masterClocks)
{
	if(masterClocks == 0u) {
		return 0u;
	}

	// Approximation for compatibility-first pacing:
	// VDP-internal DMA step budget. Fill is byte-oriented, copy is word-oriented.
	constexpr uint32_t DmaInternalStepMclk = 8u;

	if(_dmaType == DmaType::VramFill) {
		if(!_dmaFillPend || _dmaLen == 0) {
			return 0u;
		}

		uint32_t budget = masterClocks + _dmaVdpMclkRemainder;
		uint32_t bytes  = budget / DmaInternalStepMclk;
		_dmaVdpMclkRemainder = (uint8_t)(budget % DmaInternalStepMclk);
		if(bytes == 0u) {
			return 0u;
		}

		uint32_t before = _dmaLen;
		ExecDmaFill(bytes);
		uint32_t transferred = before - _dmaLen;
		return transferred * DmaInternalStepMclk;
	}

	if(_dmaType == DmaType::VramCopy) {
		if(_dmaLen == 0) {
			return 0u;
		}

		uint32_t budget = masterClocks + _dmaVdpMclkRemainder;
		uint32_t words  = budget / DmaInternalStepMclk;
		_dmaVdpMclkRemainder = (uint8_t)(budget % DmaInternalStepMclk);
		if(words == 0u) {
			return 0u;
		}

		uint32_t before = _dmaLen;
		ExecDmaCopy(words);
		uint32_t transferred = before - _dmaLen;
		return transferred * DmaInternalStepMclk;
	}

	return 0u;
}

uint32_t GenesisVdp::Consume68kBusDma(uint32_t masterClocks, uint32_t sliceStartMclk)
{
	if(!Is68kBusDmaActive() || masterClocks == 0u) {
		return 0u;
	}

	uint8_t cd = _codeReg & 0x0Fu;
	bool wordDest = (cd == 0x03u || cd == 0x05u);
	auto getWordPeriodMclk = [&](bool blanking) -> uint32_t {
		if(wordDest) {
			if(blanking) {
				// GenplusGX reference: CRAM/VSRAM DMA transfers 161/198 words per
				// line in H32/H40 when the display is blanked.
				return IsH40() ? 17u : 21u;
			}

			// Active-display CRAM/VSRAM DMA transfers 16/18 words per line.
			return IsH40() ? 190u : 214u;
		}

		// Preserve the existing VRAM approximation for now. The direct-color DMA
		// regression is on CRAM, so tighten that path first.
		return 16u;
	};

	uint32_t budget = masterClocks + _dmaBusMclkRemainder;
	uint32_t consumed = 0u;
	_dmaBusMclkRemainder = 0u;

	// Bus68k DMA acquires the bus before the first transfer.
	if(_dmaBusStartDelayMclk > 0u) {
		uint32_t delay = (_dmaBusStartDelayMclk < budget) ? _dmaBusStartDelayMclk : budget;
		_dmaBusStartDelayMclk = (uint8_t)(_dmaBusStartDelayMclk - delay);
		budget -= delay;
		consumed += delay;
		if(_dmaBusStartDelayMclk > 0u) {
			return consumed;
		}
	}

	if(!wordDest) {
		constexpr uint32_t Dma68kWordMclk = 16u;
		uint32_t words = budget / Dma68kWordMclk;
		_dmaBusMclkRemainder = (uint8_t)(budget % Dma68kWordMclk);
		if(words == 0u) {
			return consumed;
		}

		uint32_t before = _dmaLen;
		ExecDmaBus68k(words);
		uint32_t transferred = before - _dmaLen;
		return consumed + (transferred * Dma68kWordMclk);
	}

	uint32_t framePos = sliceStartMclk + consumed;
	uint32_t sliceEnd = sliceStartMclk + masterClocks;
	uint32_t phaseMclk = _dmaBusMclkRemainder;

	while(framePos < sliceEnd && _dmaLen > 0u) {
		uint32_t line = framePos / MCLKS_PER_LINE;
		uint32_t segEnd = std::min(sliceEnd, (line + 1u) * MCLKS_PER_LINE);
		bool blanking = !DispEnabled() || line >= ActiveHeight();
		uint32_t periodMclk = getWordPeriodMclk(blanking);
		uint32_t clocksRemaining = segEnd - framePos;

		while(clocksRemaining > 0u && _dmaLen > 0u) {
			uint32_t clocksToWord = (phaseMclk < periodMclk) ? (periodMclk - phaseMclk) : periodMclk;
			if(clocksToWord > clocksRemaining) {
				phaseMclk += clocksRemaining;
				framePos += clocksRemaining;
				clocksRemaining = 0u;
				break;
			}

			uint32_t nextPos = framePos + clocksToWord;
			// Keep CRAM/VSRAM DMA timing at word granularity, but do not attempt
			// per-word backdrop raster updates here. The native renderer is
			// scanline-based; rewriting already-rendered backdrop pixels inside the
			// line produces the diagonal "direct color DMA" smear seen in GenTest.
			// Let the current line keep the color it started with and apply the
			// updated CRAM contents on subsequent lines.
			ExecDmaBus68k(1u);
			framePos = nextPos;
			clocksRemaining -= clocksToWord;
			phaseMclk = 0u;

			if(_dmaLen == 0u) {
				_dmaBusMclkRemainder = 0u;
				return framePos - sliceStartMclk;
			}
		}
	}

	_dmaBusMclkRemainder = (uint8_t)phaseMclk;
	return masterClocks;
}

// ===========================================================================
// Rendering helpers
// ===========================================================================

uint16_t GenesisVdp::PlaneWidthTiles() const
{
	uint8_t sz = _reg[16] & 0x03u;
	switch(sz) {
		case 0x00: return 32;
		case 0x01: return 64;
		case 0x03: return 128;
		default:   return 32; // reserved value
	}
}

uint16_t GenesisVdp::PlaneHeightTiles() const
{
	uint8_t sz = (_reg[16] >> 4) & 0x03u;
	switch(sz) {
		case 0x00: return 32;
		case 0x01: return 64;
		case 0x03: return 128;
		default:   return 32; // reserved value
	}
}

uint16_t GenesisVdp::WindowBase() const
{
	// R3: Window name table base.
	// H40: aligned to 0x1000; H32: aligned to 0x800.
	if(IsH40()) {
		return (uint16_t)(_reg[3] & 0x3Cu) << 10;
	} else {
		return (uint16_t)(_reg[3] & 0x3Eu) << 10;
	}
}

uint16_t GenesisVdp::SpriteBase() const
{
	// R5: Sprite attribute table base.
	// H40: bits 6:1 × 0x200; H32: bits 5:0 × 0x200
	if(IsH40()) {
		return (uint16_t)(_reg[5] & 0x7Eu) << 9;
	} else {
		return (uint16_t)(_reg[5] & 0x7Fu) << 9;
	}
}

// H-scroll: returns how many pixels to shift the plane left (i.e., plane origin moves right)
// Positive = plane scrolls right (content moves left on screen)
uint16_t GenesisVdp::GetHScroll(uint16_t line, bool planeA) const
{
	uint8_t mode = _reg[11] & 0x03u;
	uint16_t tableBase = HScrollBase();
	uint32_t entryOffset;

	switch(mode) {
		case 0x00: // Full screen — one entry
		case 0x01: // Prohibited — treat as full
			entryOffset = 0;
			break;
		case 0x02: // Cell scroll (one entry per 8 scanlines)
			entryOffset = (line & ~7u) * 4u;
			break;
		case 0x03: // Line scroll
			entryOffset = (uint32_t)line * 4u;
			break;
		default:
			entryOffset = 0;
			break;
	}

	uint16_t byteAddr = (uint16_t)((tableBase + entryOffset) & 0xFFFFu);
	uint16_t scrollA = ((uint16_t)_vram[byteAddr] << 8) | _vram[(byteAddr + 1u) & 0xFFFFu];
	uint16_t scrollB = ((uint16_t)_vram[(byteAddr + 2u) & 0xFFFFu] << 8) | _vram[(byteAddr + 3u) & 0xFFFFu];
	return planeA ? (scrollA & 0x3FFu) : (scrollB & 0x3FFu);
}

// V-scroll: tileCol2 is the pair-of-columns index (0 = columns 0-1, 1 = columns 2-3, ...)
uint16_t GenesisVdp::GetVScroll(uint16_t tileCol2, bool planeA) const
{
	bool twoColumn = (_reg[11] & 0x04u) != 0;
	uint8_t vsramIdx;
	if(twoColumn) {
		// One entry pair per 2 columns: idx = tileCol2 * 2 (A), tileCol2 * 2 + 1 (B)
		uint8_t base = (uint8_t)((tileCol2 & 0x1Fu) * 2u);
		vsramIdx = planeA ? base : (base + 1u);
	} else {
		// Full scroll: VSRAM[0] = A, VSRAM[1] = B
		vsramIdx = planeA ? 0u : 1u;
	}
	return VsramRead(vsramIdx) & 0x3FFu;
}

// Returns true if screen pixel (x, line) is covered by the window plane.
//
// Per Genesis VDP hardware behaviour:
//   R17 (WHP): bit7 = right-side; bits 4:0 = cell column boundary (each unit = 2 tiles = 16px in H40)
//   R18 (WVP): bit7 = down-side;  bits 4:0 = cell row boundary
//
// The window covers two independent strips, OR-ed together:
//   Y strip — ALL columns on lines where the Y-condition is met.
//   X strip — ALL lines in columns where the X-condition is met.
// Together they produce an L-shaped (or full-screen) region, not a rectangle.
bool GenesisVdp::IsWindowPixel(uint16_t line, uint16_t x) const
{
	uint8_t  wx          = _reg[17];
	uint8_t  wy          = _reg[18];
	bool     windowRight = (wx & 0x80u) != 0;
	bool     windowDown  = (wy & 0x80u) != 0;
	// R11 HP4-HP0: horizontal boundary in units of 8 pixels (per VDP registers doc).
	uint16_t wx_pix      = (uint16_t)(wx & 0x1Fu) * 8u;
	uint16_t wy_cell     = (uint16_t)(wy & 0x1Fu);
	uint16_t line_cell   = line >> 3;

	bool covY = windowDown  ? (line_cell >= wy_cell) : (line_cell < wy_cell);
	bool covX = windowRight ? (x         >= wx_pix)  : (x         <  wx_pix);

	return covX || covY;
}

uint8_t GenesisVdp::FetchTilePixel(uint16_t tileBase, uint8_t row, uint8_t col) const
{
	uint16_t byteAddr = tileBase + (uint16_t)row * 4u + (col >> 1);
	uint8_t  byte     = _vram[byteAddr & 0xFFFFu];
	return (col & 1u) ? (byte & 0x0Fu) : (byte >> 4);
}

// ===========================================================================
// Rendering — per-plane
// Pixel encoding: bit7=priority, bits5:4=palette, bits3:0=color (0=transparent)
// ===========================================================================

void GenesisVdp::RenderPlaneB(uint16_t line, uint8_t* dst, uint16_t pixels) const
{
	bool     int2     = IsInterlace2();
	uint16_t planeW   = PlaneWidthTiles();
	uint16_t planeH   = PlaneHeightTiles();
	uint16_t hscroll  = GetHScroll(line, false);  // plane B

	// In interlace mode 2 each tile is 16 rows tall; name-table rows advance
	// every 16 display lines.  Outside interlace: normal 8-row tiles.
	uint16_t tilePixH = int2 ? 16u : 8u;
	uint16_t tileRow  = line / tilePixH;
	uint16_t pixRow   = line % tilePixH;
	// In interlace mode 2 the actual row within the 16-pixel tile depends
	// on which field we're in:  even pixels for field 0, odd for field 1.
	uint16_t intPixRow = int2 ? (uint16_t)(pixRow * 2u + (_interlaceField ? 1u : 0u)) : pixRow;

	uint16_t nameBase = ScrollBBase();
	uint32_t planePxH = (uint32_t)planeH * tilePixH;
	uint32_t planePxW = (uint32_t)planeW * 8u;

	for(uint16_t x = 0; x < pixels; x++) {
		uint16_t px = (uint16_t)(x - hscroll) & (uint16_t)(planePxW - 1u);

		uint16_t vscroll = GetVScroll(x >> 4, false);
		uint32_t py = ((uint32_t)tileRow * tilePixH + intPixRow + vscroll) % planePxH;

		uint16_t tc  = (px >> 3) & (planeW - 1u);
		uint16_t tr  = (uint16_t)(py / tilePixH) & (planeH - 1u);
		uint16_t tpx = px & 7u;
		uint16_t tpy = (uint16_t)(py % tilePixH);

		uint16_t nameAddr = nameBase + (uint16_t)((tr * planeW + tc) * 2u);
		uint16_t entry    = ((uint16_t)_vram[nameAddr & 0xFFFFu] << 8)
		                  | _vram[(nameAddr + 1u) & 0xFFFFu];

		bool     pri   = (entry >> 15) & 1;
		uint8_t  pal   = (uint8_t)((entry >> 13) & 3u);
		bool     vflip = (entry >> 12) & 1;
		bool     hflip = (entry >> 11) & 1;
		uint16_t tile  = entry & 0x7FFu;

		uint8_t col = hflip ? (7u - tpx) : (uint8_t)tpx;
		uint8_t row = vflip ? ((uint8_t)(tilePixH - 1u) - (uint8_t)tpy) : (uint8_t)tpy;

		// Interlace mode 2: each tile is 64 bytes (16 rows × 4 bytes/row)
		uint16_t tileBase = int2 ? (tile * 64u) : (tile * 32u);
		uint8_t  pix = FetchTilePixel(tileBase, row, col);

		dst[x] = (uint8_t)((pri ? 0x80u : 0x00u) | (pal << 4) | pix);
	}
}

void GenesisVdp::RenderPlaneA(uint16_t line, uint8_t* dst, uint16_t pixels) const
{
	bool     int2     = IsInterlace2();
	uint16_t planeW   = PlaneWidthTiles();
	uint16_t planeH   = PlaneHeightTiles();
	uint16_t hscroll  = GetHScroll(line, true);   // plane A

	uint16_t tilePixH  = int2 ? 16u : 8u;
	uint16_t tileRow   = line / tilePixH;
	uint16_t pixRow    = line % tilePixH;
	uint16_t intPixRow = int2 ? (uint16_t)(pixRow * 2u + (_interlaceField ? 1u : 0u)) : pixRow;

	uint16_t nameBase = ScrollABase();
	uint32_t planePxH = (uint32_t)planeH * tilePixH;
	uint32_t planePxW = (uint32_t)planeW * 8u;

	for(uint16_t x = 0; x < pixels; x++) {
		uint16_t px = (uint16_t)(x - hscroll) & (uint16_t)(planePxW - 1u);

		uint16_t vscroll = GetVScroll(x >> 4, true);
		uint32_t py = ((uint32_t)tileRow * tilePixH + intPixRow + vscroll) % planePxH;

		uint16_t tc  = (px >> 3) & (planeW - 1u);
		uint16_t tr  = (uint16_t)(py / tilePixH) & (planeH - 1u);
		uint16_t tpx = px & 7u;
		uint16_t tpy = (uint16_t)(py % tilePixH);

		uint16_t nameAddr = nameBase + (uint16_t)((tr * planeW + tc) * 2u);
		uint16_t entry    = ((uint16_t)_vram[nameAddr & 0xFFFFu] << 8)
		                  | _vram[(nameAddr + 1u) & 0xFFFFu];

		bool     pri   = (entry >> 15) & 1;
		uint8_t  pal   = (uint8_t)((entry >> 13) & 3u);
		bool     vflip = (entry >> 12) & 1;
		bool     hflip = (entry >> 11) & 1;
		uint16_t tile  = entry & 0x7FFu;

		uint8_t col = hflip ? (7u - tpx) : (uint8_t)tpx;
		uint8_t row = vflip ? ((uint8_t)(tilePixH - 1u) - (uint8_t)tpy) : (uint8_t)tpy;

		uint16_t tileBase = int2 ? (tile * 64u) : (tile * 32u);
		uint8_t  pix = FetchTilePixel(tileBase, row, col);

		dst[x] = (uint8_t)((pri ? 0x80u : 0x00u) | (pal << 4) | pix);
	}
}

void GenesisVdp::RenderWindow(uint16_t line, uint8_t* dst, uint16_t pixels) const
{
	bool     int2     = IsInterlace2();
	uint16_t nameBase = WindowBase();
	uint16_t cellW    = IsH40() ? 64u : 32u;   // window table width in cells
	uint16_t tilePixH = int2 ? 16u : 8u;
	uint16_t winLine  = line / tilePixH;
	uint16_t pixRow   = line % tilePixH;
	uint16_t intPixRow = int2 ? (uint16_t)(pixRow * 2u + (_interlaceField ? 1u : 0u)) : pixRow;

	for(uint16_t x = 0; x < pixels; x++) {
		if(!IsWindowPixel(line, x)) {
			dst[x] = 0;  // not window — transparent (keep plane A result)
			continue;
		}

		uint16_t tc  = x >> 3;
		uint16_t tpx = x & 7u;

		uint16_t nameAddr = nameBase + (uint16_t)((winLine * cellW + tc) * 2u);
		uint16_t entry    = ((uint16_t)_vram[nameAddr & 0xFFFFu] << 8)
		                  | _vram[(nameAddr + 1u) & 0xFFFFu];

		bool     pri   = (entry >> 15) & 1;
		uint8_t  pal   = (uint8_t)((entry >> 13) & 3u);
		bool     vflip = (entry >> 12) & 1;
		bool     hflip = (entry >> 11) & 1;
		uint16_t tile  = entry & 0x7FFu;

		uint8_t col = hflip ? (7u - tpx) : (uint8_t)tpx;
		uint8_t row = vflip ? ((uint8_t)(tilePixH - 1u) - (uint8_t)intPixRow) : (uint8_t)intPixRow;

		uint16_t tileBase = int2 ? (tile * 64u) : (tile * 32u);
		uint8_t  pix = FetchTilePixel(tileBase, row, col);

		// Mark window pixels with a special flag (bit 6) so compositor knows
		dst[x] = (uint8_t)((pri ? 0x80u : 0x00u) | 0x40u | (pal << 4) | pix);
	}
}

void GenesisVdp::RenderSprites(uint16_t line, uint8_t* dst, uint16_t pixels)
{
	bool     int2       = IsInterlace2();
	uint16_t sprBase    = SpriteBase();
	uint16_t maxSprites = IsH40() ? 80u : 64u;
	uint16_t maxPerLine = IsH40() ? 20u : 16u;
	uint16_t maxDots    = pixels;

	// In interlace mode 2 each sprite cell is 16 pixels tall; Y positions are
	// in virtual doubled-resolution lines, and tiles are 64 bytes each.
	uint16_t cellPixH = int2 ? 16u : 8u;
	// Effective display line in interlace: double the scanline and add field
	uint16_t effLine = int2 ? (uint16_t)(line * 2u + (_interlaceField ? 1u : 0u)) : line;

	uint8_t  count = 0;
	uint8_t  idx   = 0;  // start at sprite 0
	uint16_t dots  = 0;

	// Masking state:
	// - Any visible sprite with x!=0 arms masking for later x=0 sprites.
	// - If previous line hit sprite-dot limit, masking starts armed.
	bool maskArmed  = _prevLineDotOverflow;
	bool maskActive = false;

	// Traverse the full sprite chain so we can detect overflow even past the limit.
	for(uint8_t s = 0; s < maxSprites; s++) {
		uint16_t entryBase = sprBase + (uint16_t)idx * 8u;

		// Word 0: Y position (9-bit, bias 128)
		uint16_t w0 = ((uint16_t)_vram[(entryBase    ) & 0xFFFFu] << 8)
		            |             _vram[(entryBase + 1) & 0xFFFFu];
		// Word 1: size | link
		uint16_t w1 = ((uint16_t)_vram[(entryBase + 2) & 0xFFFFu] << 8)
		            |             _vram[(entryBase + 3) & 0xFFFFu];
		// Word 2: attributes
		uint16_t w2 = ((uint16_t)_vram[(entryBase + 4) & 0xFFFFu] << 8)
		            |             _vram[(entryBase + 5) & 0xFFFFu];
		// Word 3: X position (9-bit, bias 128)
		uint16_t w3 = ((uint16_t)_vram[(entryBase + 6) & 0xFFFFu] << 8)
		            |             _vram[(entryBase + 7) & 0xFFFFu];

		// In interlace mode 2 the Y position in the sprite attribute table is
		// already in doubled-resolution coordinates (bias still 128).
		int16_t  sprY      = (int16_t)(w0 & 0x1FFu) - 128;
		// Sprite word 1: bits[11:10] = HS (horizontal size), bits[9:8] = VS (vertical size)
		uint8_t  vertCells  = (uint8_t)(((w1 >>  8) & 3u) + 1u);  // VS: vertical cell count
		uint8_t  horizCells = (uint8_t)(((w1 >> 10) & 3u) + 1u);  // HS: horizontal cell count
		uint8_t  link      = (uint8_t)(w1 & 0x7Fu);

		bool     pri   = (w2 >> 15) & 1;
		uint8_t  pal   = (uint8_t)((w2 >> 13) & 3u);
		bool     vflip = (w2 >> 12) & 1;
		bool     hflip = (w2 >> 11) & 1;
		uint16_t tile  = w2 & 0x7FFu;
		uint16_t sprXRaw = (w3 & 0x1FFu);
		int16_t  sprX    = (int16_t)sprXRaw - 128;

		uint16_t sprH = (uint16_t)vertCells * cellPixH;

		// Is this sprite on the current (effective) scanline?
		if((int16_t)effLine >= sprY && (int16_t)effLine < (int16_t)(sprY + sprH)) {
			count++;

			if(count > maxPerLine) {
				// Sprite overflow: more sprites on this scanline than the hardware limit.
				// Set status bit 6 and stop rendering (hardware also stops here).
				_status |= 0x0040u;
				break;
			}

			if(sprXRaw != 0) {
				maskArmed = true;
			} else if(maskArmed) {
				maskActive = true;
			}

			uint16_t spriteDots = (uint16_t)horizCells * 8u;
			dots = (uint16_t)(dots + spriteDots);

			uint16_t drawDots = spriteDots;
			if(dots > maxDots) {
				drawDots = (uint16_t)(drawDots - (dots - maxDots));
			}

			uint16_t sprRow = (uint16_t)((int16_t)effLine - sprY);
			if(vflip) sprRow = (uint16_t)(sprH - 1u - sprRow);

			uint8_t cellRow = (uint8_t)(sprRow / cellPixH);
			uint8_t pixRow  = (uint8_t)(sprRow % cellPixH);

			uint16_t dotPos = 0;
			for(uint8_t cx = 0; cx < horizCells && dotPos < drawDots; cx++) {
				uint8_t cellCol = hflip ? (horizCells - 1u - cx) : cx;

				// Tile index: sprite tiles are in column-major order (vertical then horizontal)
				uint16_t tileIdx = tile + (uint16_t)(cellCol * vertCells) + cellRow;
				// Interlace mode 2: 64-byte tiles; normal: 32-byte tiles
				uint16_t tileBase = int2 ? (tileIdx * 64u) : (tileIdx * 32u);

				for(uint8_t px = 0; px < 8u && dotPos < drawDots; px++, dotPos++) {
					if(maskActive) continue;

					int16_t screenX = sprX + (int16_t)(cx * 8u + px);
					if(screenX < 0 || screenX >= (int16_t)pixels) continue;

					uint8_t col = hflip ? (7u - px) : px;
					uint8_t pix = FetchTilePixel(tileBase, pixRow, col);
					if(pix == 0) continue;  // transparent pixel — no collision

					if(dst[screenX] != 0) {
						// Two non-transparent sprite pixels overlap: sprite collision.
						_status |= 0x0020u;  // set status bit 5
						continue;            // first sprite wins
					}

					dst[screenX] = (uint8_t)((pri ? 0x80u : 0x00u) | (pal << 4) | pix);
				}
			}

			if(dots >= maxDots) {
				// If this line reached the sprite-dot budget, an x=0 mask in
				// the first slot can be effective on the next line.
				_prevLineDotOverflow = (dots >= pixels);
				return;
			}
		}

		if(link == 0 || link >= maxSprites) break;
		idx = link;
	}

	_prevLineDotOverflow = false;
}

void GenesisVdp::Composite(uint16_t line,
	const uint8_t* planeB, const uint8_t* planeA,
	const uint8_t* spr,    uint16_t pixels)
{
	if(!_fb) return;
	uint32_t* outLine = _fb + (uint32_t)line * _fbW;

	// Priority order (front to back):
	//   1. hi-pri window
	//   2. hi-pri sprite
	//   3. hi-pri plane A
	//   4. hi-pri plane B
	//   5. lo-pri window
	//   6. lo-pri sprite
	//   7. lo-pri plane A
	//   8. lo-pri plane B
	//   9. backdrop

	uint8_t bgIdx = _reg[7] & 0x3Fu;
	bool    shMode = ShadowHlEnabled();

	// Shadow/highlight shade codes
	enum : uint8_t { Shade_Shadow = 0, Shade_Normal = 1, Shade_Highlight = 2 };

	for(uint16_t x = 0; x < pixels; x++) {
		uint8_t pB = planeB[x];
		uint8_t pA = planeA[x];  // may have bit6 set = window
		uint8_t pS = spr[x];

		// Decode layer flags
		bool  winSrc = (pA & 0x40u) != 0;
		bool  winHi  = winSrc && ((pA & 0x80u) != 0);
		bool  winVis = winSrc && ((pA & 0x0Fu) != 0);
		bool  sprHi  = (pS & 0x80u) != 0;
		bool  sprVis = (pS & 0x0Fu) != 0;
		bool  pAHi   = !winSrc && ((pA & 0x80u) != 0);
		bool  pAVis  = !winSrc && ((pA & 0x0Fu) != 0);
		bool  pBHi   = (pB & 0x80u) != 0;
		bool  pBVis  = (pB & 0x0Fu) != 0;

		// Determine shade in shadow/highlight mode
		uint8_t shade = Shade_Shadow;
		bool    sprIsOp = false;  // sprite is a shadow/highlight operator

		if(shMode) {
			// Step 1: any hi-pri visible plane or window → normal
			if((winHi && winVis) || (pAHi && pAVis) || (pBHi && pBVis)) {
				shade = Shade_Normal;
			}
			// Step 2: sprite determines shade (operators are transparent)
			if(sprVis) {
				uint8_t sprPal   = (pS >> 4) & 3u;
				uint8_t sprColor = pS & 0x0Fu;
				if(!sprHi && sprPal == 3u) {
					if(sprColor == 14u) {
						// Shadow operator: force shadow, sprite is transparent
						shade    = Shade_Shadow;
						sprIsOp  = true;
					} else if(sprColor == 15u) {
						// Highlight operator: lift one brightness level, sprite is transparent.
						// Doc: "If the pixel is shadowed by the first rule, it will appear normal."
						// Shadow→Normal, Normal→Highlight.
						shade   = (shade == Shade_Shadow) ? Shade_Normal : Shade_Highlight;
						sprIsOp = true;
					}
					// else: lo-pri pal-3 regular sprite — shade stays as determined by planes
				} else if(sprHi) {
					// High-priority non-operator sprite → normal brightness
					shade = Shade_Normal;
				} else if(sprColor == 15u) {
					// Lo-pri sprite with color 15 in non-operator palette → always Normal.
					// Doc: "Pixels in sprites using colour 15 of palette lines 1, 2 or 3
					//        will always appear normal."
					shade = Shade_Normal;
				}
			}
		}

		// Select cramIdx using priority chain.
		// In S/H mode, operator sprites are transparent (skip them in color selection).
		uint8_t cramIdx = bgIdx;  // backdrop fallback
		bool    backdrop = true;

		if      (winHi && winVis)                    { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (sprHi && sprVis && !sprIsOp)       { cramIdx = (uint8_t)(((pS >> 4) & 3u) * 16u + (pS & 0x0Fu)); backdrop = false; }
		else if (pAHi  && pAVis)                    { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (pBHi  && pBVis)                    { cramIdx = (uint8_t)(((pB >> 4) & 3u) * 16u + (pB & 0x0Fu)); backdrop = false; }
		else if (!winHi && winVis)                  { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (!sprHi && sprVis && !sprIsOp)      { cramIdx = (uint8_t)(((pS >> 4) & 3u) * 16u + (pS & 0x0Fu)); backdrop = false; }
		else if (!pAHi  && pAVis)                   { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (!pBHi  && pBVis)                   { cramIdx = (uint8_t)(((pB >> 4) & 3u) * 16u + (pB & 0x0Fu)); backdrop = false; }

		cramIdx &= 0x3Fu;
		_lineBackdropMask[x] = backdrop ? 1u : 0u;

		if(shMode) {
			switch(shade) {
				case Shade_Shadow:    outLine[x] = _shadowPalette   [cramIdx]; break;
				case Shade_Highlight: outLine[x] = _highlightPalette[cramIdx]; break;
				default:              outLine[x] = _palette         [cramIdx]; break;
			}
		} else {
			outLine[x] = _palette[cramIdx];
		}
	}

	// Fill any framebuffer pixels beyond active display with black
	uint32_t black = 0xFF000000u;
	for(uint32_t x = pixels; x < _fbW; x++) {
		outLine[x] = black;
	}
	for(uint32_t x = pixels; x < std::min<uint32_t>(_fbW, 320u); x++) {
		_lineBackdropMask[x] = 0u;
	}
}

// ===========================================================================
// BeginLine / EndLine / RunLine
// ===========================================================================

void GenesisVdp::BeginLine(uint16_t line, uint32_t* fb, uint32_t fbW, uint32_t fbH)
{
	_fb   = fb;
	_fbW  = fbW;
	_fbH  = fbH;
	_scanline = line;

	uint16_t activeH = ActiveHeight();

	if(line == 0) {
		// Start of frame: reset V-blank, reload H-int counter
		_status &= ~0x0008u;  // clear V-blank
		_hintCounter = (int)HIntReload();
		_frameCount++;
		_prevLineDotOverflow = false;

		// Interlace field: toggle every frame; update status bit 4 (odd frame)
		if(IsInterlace()) {
			_interlaceField = !_interlaceField;
		} else {
			_interlaceField = false;
		}
		if(_interlaceField) _status |= 0x0010u;
		else                _status &= ~0x0010u;
	}

	// Clear H-blank at line start.
	_status &= ~0x0004u;

	if(line < activeH) {
		// Active display.
		_status &= ~0x0008u;

		if(DispEnabled()) {
			static uint8_t planeB[320];
			static uint8_t planeA[320];
			static uint8_t winPix[320];
			static uint8_t sprPix[320];

			uint16_t w = ActiveWidth();
			memset(planeB, 0, w);
			memset(planeA, 0, w);
			memset(winPix, 0, w);
			memset(sprPix, 0, w);

			RenderPlaneB (line, planeB, w);
			RenderPlaneA (line, planeA, w);
			RenderWindow (line, winPix, w);
			RenderSprites(line, sprPix, w);

				// Merge window into plane A (bit6 = window-coverage flag)
				for(uint16_t x = 0; x < w; x++) {
					if(winPix[x] & 0x40u) {
						planeA[x] = winPix[x];
					}
				}

			Composite(line, planeB, planeA, sprPix, w);
		} else {
			// Display disabled — fill with backdrop colour
			if(_fb) {
				uint32_t bg = _palette[_reg[7] & 0x3Fu];
				uint32_t* outLine = _fb + (uint32_t)line * _fbW;
				for(uint32_t x = 0; x < _fbW; x++) outLine[x] = bg;
				memset(_lineBackdropMask, 1, sizeof(_lineBackdropMask));
			}
		}
	} else if(line == activeH) {
		// First V-blank line.
		_status |= 0x0008u;
		if(VIntEnabled()) { _vintPending = true; _vintNew = true; }
		_vintFiredFrame = true;  // V-blank start processed; suppress further NextVIntMclk events
	} else {
		// Remaining V-blank lines.
		_status |= 0x0008u;
	}
}

void GenesisVdp::EndLine()
{
	// Enter H-blank at end of scanline.
	_status |= 0x0004u;

	// H-interrupt counter behaviour (per BlastEM / hardware reference):
	//   Active display (line < ActiveHeight): decrement; fire and reload at 0.
	//   V-blank (line >= ActiveHeight): reload every line; do NOT fire interrupt.
	if(_scanline < ActiveHeight()) {
		if(_hintCounter <= 0) {
			if(HIntEnabled()) _hintPending = true;
			_hintCounter = (int)HIntReload();
		} else {
			_hintCounter--;
		}
	} else {
		// V-blank — reset counter for the next active-display period.
		_hintCounter = (int)HIntReload();
	}
}

void GenesisVdp::RunLine(uint16_t line, uint32_t* fb, uint32_t fbW, uint32_t fbH)
{
	BeginLine(line, fb, fbW, fbH);
	EndLine();
}

bool GenesisVdp::ConsumeVInt()
{
	if(_vintNew) {
		_vintNew = false;
		return true;
	}
	return false;
}

bool GenesisVdp::ConsumeHInt()
{
	if(_hintPending) {
		_hintPending = false;
		return true;
	}
	return false;
}

// ===========================================================================
// Event-driven master-clock scheduler interface
// ===========================================================================

void GenesisVdp::BeginFrame(uint32_t* fb, uint32_t fbW, uint32_t fbH)
{
	_frameFb  = fb;
	_frameFbW = fbW;
	_frameFbH = fbH;
	_mclkPos        = 0;
	_lineBegun      = false;
	_vintFiredFrame = false;
	// Pre-load H-int counter so NextHIntMclk() is valid before AdvanceToMclk
	// calls BeginLine(0). BeginLine(0) will set this again (idempotent).
	_hintCounter = (int)HIntReload();
}

void GenesisVdp::AdvanceToMclk(uint32_t targetMclk)
{
	while(_mclkPos < targetMclk) {
		uint32_t currentLine = _mclkPos / MCLKS_PER_LINE;
		uint32_t lineEnd     = (currentLine + 1u) * MCLKS_PER_LINE;

		if(!_lineBegun) {
			BeginLine((uint16_t)currentLine, _frameFb, _frameFbW, _frameFbH);
			_lineBegun = true;
		}

		if(lineEnd <= targetMclk) {
			EndLine();
			_lineBegun = false;
			_mclkPos   = lineEnd;
		} else {
			_mclkPos = targetMclk;
			break;
		}
	}

	// If we stopped exactly at a line boundary without having begun the line,
	// call BeginLine now so that start-of-line events (e.g. V-int at VBlank)
	// are fired before the caller delivers interrupts.
	if(_mclkPos == targetMclk && !_lineBegun && (_mclkPos % MCLKS_PER_LINE == 0u)) {
		uint32_t line = _mclkPos / MCLKS_PER_LINE;
		if(line < (uint32_t)TotalScanlines()) {
			BeginLine((uint16_t)line, _frameFb, _frameFbW, _frameFbH);
			_lineBegun = true;
		}
	}
}

uint32_t GenesisVdp::NextVIntMclk() const
{
	if(!VIntEnabled() || _vintFiredFrame) return UINT32_MAX;
	uint32_t vblankMclk = (uint32_t)ActiveHeight() * MCLKS_PER_LINE;
	return (_mclkPos <= vblankMclk) ? vblankMclk : UINT32_MAX;
}

uint32_t GenesisVdp::NextHIntMclk() const
{
	if(!HIntEnabled()) return UINT32_MAX;
	uint32_t currentLine = _mclkPos / MCLKS_PER_LINE;

	// H-int only fires during active display lines (0 .. ActiveHeight()-1).
	// During V-blank the counter is reloaded each line but no interrupt is raised.
	if(currentLine >= (uint32_t)ActiveHeight()) return UINT32_MAX;

	uint32_t eventMclk  = (currentLine + (uint32_t)_hintCounter + 1u) * MCLKS_PER_LINE;
	uint32_t vblankMclk = (uint32_t)ActiveHeight() * MCLKS_PER_LINE;

	// If the counter roll-over would land in or past V-blank, the interrupt will
	// not fire this frame (it fires at REG_HINT lines from the next frame start).
	if(eventMclk > vblankMclk) return UINT32_MAX;

	return eventMclk;
}

// ===========================================================================
// Save / load state
// ===========================================================================

void GenesisVdp::SaveState(vector<uint8_t>& out) const
{
	AppV(out, _vram);
	AppV(out, _cram);
	AppV(out, _vsram);
	AppV(out, _reg);
	AppV(out, _ctrlPend);
	AppV(out, _ctrlFirst);
	AppV(out, _addrReg);
	AppV(out, _codeReg);
	AppV(out, _readBuf);
	AppV(out, _writeHi);
	AppV(out, _writeHiData);
	AppV(out, _writeHiCtrl);
	AppV(out, _status);
	AppV(out, _statusReadLatch);
	AppV(out, _statusReadLatchValid);
	AppV(out, _vintPending);
	AppV(out, _vintNew);
	AppV(out, _hintPending);
	AppV(out, _hintCounter);
	uint8_t dm = (uint8_t)_dmaType;
	AppV(out, dm);
	AppV(out, _dmaSrc);
	AppV(out, _dmaLen);
	AppV(out, _dmaFillVal);
	AppV(out, _dmaFillPend);
	AppV(out, _dmaBusStartDelayMclk);
	AppV(out, _dmaBusMclkRemainder);
	AppV(out, _dmaVdpMclkRemainder);
	AppV(out, _interlaceField);
	AppV(out, _scanline);
	AppV(out, _frameCount);
	AppV(out, _prevLineDotOverflow);
	AppV(out, _mclkPos);
	AppV(out, _lineBegun);
	AppV(out, _vintFiredFrame);
}

bool GenesisVdp::LoadState(const vector<uint8_t>& data, size_t& offset)
{
	if(!RdV(data, offset, _vram))        return false;
	if(!RdV(data, offset, _cram))        return false;
	if(!RdV(data, offset, _vsram))       return false;
	if(!RdV(data, offset, _reg))         return false;
	if(!RdV(data, offset, _ctrlPend))    return false;
	if(!RdV(data, offset, _ctrlFirst))   return false;
	if(!RdV(data, offset, _addrReg))     return false;
	if(!RdV(data, offset, _codeReg))     return false;
	if(!RdV(data, offset, _readBuf))     return false;
	if(!RdV(data, offset, _writeHi))     return false;
	if(!RdV(data, offset, _writeHiData)) return false;
	if(!RdV(data, offset, _writeHiCtrl)) return false;
	if(!RdV(data, offset, _status))      return false;
	if(!RdV(data, offset, _statusReadLatch)) return false;
	if(!RdV(data, offset, _statusReadLatchValid)) return false;
	if(!RdV(data, offset, _vintPending)) return false;
	if(!RdV(data, offset, _vintNew))     return false;
	if(!RdV(data, offset, _hintPending)) return false;
	if(!RdV(data, offset, _hintCounter)) return false;
	uint8_t dm = 0;
	if(!RdV(data, offset, dm))           return false;
	_dmaType = (DmaType)dm;
	if(!RdV(data, offset, _dmaSrc))      return false;
	if(!RdV(data, offset, _dmaLen))      return false;
	if(!RdV(data, offset, _dmaFillVal))  return false;
	if(!RdV(data, offset, _dmaFillPend)) return false;
	if(!RdV(data, offset, _dmaBusStartDelayMclk)) return false;
	if(!RdV(data, offset, _dmaBusMclkRemainder)) return false;
	if(!RdV(data, offset, _dmaVdpMclkRemainder)) return false;
	if(!RdV(data, offset, _interlaceField)) return false;
	if(!RdV(data, offset, _scanline))      return false;
	if(!RdV(data, offset, _frameCount))   return false;
	if(offset < data.size()) {
		if(!RdV(data, offset, _prevLineDotOverflow)) return false;
	} else {
		_prevLineDotOverflow = false;
	}
	if(!RdV(data, offset, _mclkPos))      return false;
	if(!RdV(data, offset, _lineBegun))    return false;
	if(!RdV(data, offset, _vintFiredFrame)) return false;

	// Debug register is not serialized; clear to power-on default on load.
	_debugReg = 0;

	// Rebuild expanded palette
	for(uint8_t i = 0; i < 64; i++) RefreshPalette(i);

	return true;
}
