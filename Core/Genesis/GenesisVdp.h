#pragma once
#include "pch.h"
#include "Genesis/GenesisTypes.h"
#include <cstdio>

class Emulator;
class GenesisNativeBackend;

// ---------------------------------------------------------------------------
// GenesisVdp — Sega Mega Drive VDP
//
// Implements:
//   - Register file R0-R23
//   - VRAM/CRAM/VSRAM with correct control-port state machine
//   - DMA: 68K→VDP, VRAM fill, VRAM copy
//   - Scanline renderer: plane B, plane A, window, sprites
//   - H-blank (level 4) and V-blank (level 6) interrupt generation
//   - H/V counter
// ---------------------------------------------------------------------------
class GenesisVdp
{
public:
	static constexpr uint16_t LinesNtsc    = 262;
	static constexpr uint16_t LinesPal     = 313;
	static constexpr uint32_t MCLKS_PER_LINE = 3420u; // master clocks per scanline

	// V-blank status flag (bit 3) goes active at hslot 167 (H40) / 135 (H32)
	// within the first V-blank line — NOT at the start of the line where V-int fires.
	static constexpr uint16_t VBLANK_START_SLOT_H40 = 167u;
	static constexpr uint16_t VBLANK_START_SLOT_H32 = 135u;

private:
	Emulator*             _emu     = nullptr;
	GenesisNativeBackend* _backend = nullptr;
	bool _isPal = false;

	// -----------------------------------------------------------------------
	// VDP memory (owned by VDP)
	// -----------------------------------------------------------------------
	uint8_t  _vram[0x10000] = {};   // 64 KB VRAM
	uint8_t  _cram[0x80]    = {};   // 64 × 2 bytes (9-bit RGB, big-endian pairs)
	uint8_t  _vsram[0x50]   = {};   // 40 × 2 bytes

	// Expanded ARGB8888 palette — updated whenever CRAM changes
	uint32_t _palette[64]          = {};
	uint32_t _shadowPalette[64]    = {};   // same as _palette but at half brightness
	uint32_t _highlightPalette[64] = {};   // same as _palette but 1.5× brightness

	// -----------------------------------------------------------------------
	// Registers R0-R23
	// -----------------------------------------------------------------------
	uint8_t _reg[24] = {};

	// -----------------------------------------------------------------------
	// Control port state machine
	// -----------------------------------------------------------------------
	bool     _ctrlPend = false;  // waiting for second word of address command
	uint16_t _ctrlFirst = 0;     // first word buffer
	uint16_t _addrReg  = 0;      // 16-bit VDP address register
	uint8_t  _codeReg  = 0;      // 6-bit code (CD5:CD0)

	// Read-ahead buffer — primed whenever a read address/code is set
	uint16_t _readBuf = 0;

	// Byte-write accumulator — holds the high byte of a pending word write
	// to the data or control port.  Separate from _readBuf to avoid aliasing.
	uint8_t  _writeHi     = 0;
	bool     _writeHiData = false;  // pending high byte for data port
	bool     _writeHiCtrl = false;  // pending high byte for control port

	// -----------------------------------------------------------------------
	// Status register
	//   bit 9 : FIFO empty
	//   bit 8 : FIFO full
	//   bit 7 : V-int pending
	//   bit 6 : sprite overflow
	//   bit 5 : sprite collision
	//   bit 4 : odd frame (interlace)
	//   bit 3 : V-blank
	//   bit 2 : H-blank
	//   bit 1 : DMA busy
	//   bit 0 : PAL
	// -----------------------------------------------------------------------
	uint16_t _status = 0x3400;
	uint16_t _debugReg = 0;

	// -----------------------------------------------------------------------
	// Interrupt state
	// -----------------------------------------------------------------------
	bool _vintPending = false; // VDP status bit 7: set at VBlank, cleared when CPU reads status
	bool _vintNew     = false; // one-shot: set at VBlank, cleared by ConsumeVInt() → delivers IRQ6
	bool _hintPending = false;
	int  _hintCounter = 0;   // counts down each active line; fires at 0
	uint16_t _statusReadLatch = 0; // latched status word for control-port high->low byte reads
	bool _statusReadLatchValid = false;

	// -----------------------------------------------------------------------
	// DMA
	// -----------------------------------------------------------------------
	enum class DmaType : uint8_t { None, Bus68k, VramFill, VramCopy };
	DmaType  _dmaType    = DmaType::None;
	uint32_t _dmaSrc     = 0;    // byte address (Bus68k) or VRAM addr (copy)
	uint32_t _dmaLen     = 0;    // word count remaining
	uint16_t _dmaAddr    = 0;    // latched DMA destination address register
	uint8_t  _dmaCode    = 0;    // latched DMA command code (CD5:CD0)
	uint16_t _dmaFillVal = 0;    // fill word (written via data port)
	bool     _dmaFillPend = false; // fill word received, waiting to execute
	uint8_t  _dmaBusStartDelayMclk = 0; // startup latency before first 68K DMA word
	uint8_t  _dmaBusMclkRemainder = 0; // leftover master clocks for 68K DMA pacing
	uint8_t  _dmaVdpMclkRemainder = 0; // leftover master clocks for internal VDP DMA pacing

	// -----------------------------------------------------------------------
	// FIFO — 4-entry write queue
	// Data port writes enqueue here; entries drain at external slots.
	// -----------------------------------------------------------------------
	struct FifoEntry {
		uint16_t data    = 0;
		uint16_t addr    = 0;   // latched _addrReg at write time
		uint8_t  code    = 0;   // latched _codeReg at write time (CD3:CD0)
		uint8_t  pad     = 0;
	};
	FifoEntry _fifo[4]   = {};
	uint8_t   _fifoRead  = 0;  // ring read index
	uint8_t   _fifoWrite = 0;  // ring write index
	uint8_t   _fifoCount = 0;  // entries currently in FIFO

	// -----------------------------------------------------------------------
	// Interlace field tracking
	// -----------------------------------------------------------------------
	bool     _interlaceField = false;  // false = even/field-0, true = odd/field-1

	// -----------------------------------------------------------------------
	// Scanline / frame counters
	// -----------------------------------------------------------------------
	uint16_t _scanline   = 0;
	uint32_t _frameCount = 0;
	FILE*    _dmaTraceFile = nullptr;  // temporary DMA debug trace
	// Sprite mask quirk: if the previous scanline ended in sprite-dot overflow,
	// an x=0 mask in slot 1 can become effective on the next line.
	bool     _prevLineDotOverflow = false;

	// -----------------------------------------------------------------------
	// Output framebuffer (set per-frame by RunLine caller)
	// -----------------------------------------------------------------------
	uint32_t* _fb  = nullptr;
	uint32_t  _fbW = 320;
	uint32_t  _fbH = 224;

	// -----------------------------------------------------------------------
	// Master-clock timeline (event-driven scheduler)
	// -----------------------------------------------------------------------
	uint32_t  _mclkPos        = 0;      // master-clock position within current frame
	bool      _lineBegun      = false;  // BeginLine called for _mclkPos/MCLKS_PER_LINE
	bool      _vintFiredFrame = false;  // V-int already fired this frame
	uint32_t  _vblankSetMclk  = UINT32_MAX; // mclk at which V-blank flag (status bit 3) should go active
	uint32_t* _frameFb        = nullptr;
	uint32_t  _frameFbW       = 320;
	uint32_t  _frameFbH       = 224;
	uint16_t  _lineRenderX    = 0;      // pixels already rasterized on current active line
	uint8_t   _lineBackdropMask[320] = {};

	// -----------------------------------------------------------------------
	// Slot-accurate rendering 
	// -----------------------------------------------------------------------
public:
	enum class SlotOp : uint8_t {
		ReadMapScrollA,   // read plane A nametable entries for a column
		ExternalSlot,     // FIFO drain / DMA fill / DMA copy step
		RenderMap1,       // render col_1 of plane A into _tmpBufA
		RenderMap2,       // render col_2 of plane A into _tmpBufA+8
		ReadMapScrollB,   // read plane B nametable entries for a column
		ReadSpriteX,      // read sprite X position / build draw list
		RenderMap3,       // render col_1 of plane B into _tmpBufB
		RenderMapOutput,  // render col_2 of B, composite 16px, advance bufs
		SpriteRender,     // render_sprite_cells + scan_sprite_table
		HScrollLoad,      // load hscroll_a/b from HSCROLL table
		Refresh,          // DRAM refresh (no external slot / no DMA src)
		ClearLinebuf,     // memset linebuf, reset sprite_draws
		Nop               // idle / reserved slot
	};

	struct SlotDescriptor {
		uint8_t  hslot;    // H-counter value for this slot
		SlotOp   op;       // primary operation
		int16_t  column;   // column parameter for map ops (-1 = N/A)
	};

	static constexpr uint16_t SLOT_COUNT_H40 = 210u;
	static constexpr uint16_t SLOT_COUNT_H32 = 171u;
	static constexpr uint8_t  BG_START_SLOT  = 6u;
	static constexpr uint8_t  SCROLL_BUF_SIZE = 32u;
	static constexpr uint8_t  SCROLL_BUF_MASK = 31u;
	static constexpr uint8_t  SCROLL_BUF_DRAW = 16u;
	static constexpr uint16_t LINEBUF_SIZE    = 347u;  // 320 + 27 border
	static constexpr uint8_t  MAX_SPRITES_LINE_H40 = 20u;
	static constexpr uint8_t  MAX_SPRITES_LINE_H32 = 16u;
	static constexpr uint8_t  MAX_DRAWS_H40 = 40u;
	static constexpr uint8_t  MAX_DRAWS_H32 = 32u;

private:
	static const SlotDescriptor kSlotTableH40[SLOT_COUNT_H40];
	static const SlotDescriptor kSlotTableH32[SLOT_COUNT_H32];

	// --- Slot machine state ---
	uint16_t _slotIndex   = 0;    // index into kSlotTableH40/H32
	uint16_t _slotCycles  = 16;   // mclks per slot (16 for H40, 20 for H32)

	// --- Circular plane tile buffers (tmp_buf_a/b) ---
	uint8_t  _tmpBufA[SCROLL_BUF_SIZE] = {};
	uint8_t  _tmpBufB[SCROLL_BUF_SIZE] = {};
	uint8_t  _bufAOff = 0;
	uint8_t  _bufBOff = 0;

	// --- Column render scratch ---
	uint16_t _col1 = 0;           // nametable entry for first tile of column
	uint16_t _col2 = 0;           // nametable entry for second tile of column
	uint16_t _colB1 = 0;          // plane B nametable entry 1
	uint16_t _colB2 = 0;          // plane B nametable entry 2
	uint8_t  _vOffsetA = 0;       // vertical pixel offset within tile (plane A)
	uint8_t  _vOffsetB = 0;       // vertical pixel offset within tile (plane B)
	bool     _windowActive = false; // current column is inside window region
	uint16_t _vscrollLatch[2] = {};  // latched vscroll values (plane A, plane B)
	uint16_t _hscrollA = 0;
	uint16_t _hscrollAFine = 0;   // fine H-scroll component (lower 3-4 bits)
	uint16_t _hscrollB = 0;
	uint16_t _hscrollBFine = 0;

	// --- Sprite rendering state ---
	uint8_t  _linebuf[LINEBUF_SIZE]     = {};  // sprite pixel buffer
	uint8_t  _compositebuf[LINEBUF_SIZE] = {};  // composited CRAM index buffer

	struct SpriteInfo {
		uint16_t index = 0;  // SAT entry index
		int16_t  y     = 0;  // screen Y position
		uint8_t  size  = 0;  // SAT raw size nibble: HHVV, each field encoded as (cells-1)
	};

	struct SpriteDraw {
		int16_t  xPos    = 0;
		uint16_t address = 0;  // tile data VRAM address
		uint8_t  palPri  = 0;  // palette + priority bits
		uint8_t  hFlip   = 0;
		uint8_t  width   = 0;  // cells wide
		uint8_t  height  = 0;  // cells tall
		uint16_t baseTile = 0; // SAT tile index (11-bit)
		uint8_t  cellRow  = 0; // sprite row in cells
		uint8_t  pixRow   = 0; // pixel row inside current cell
		uint8_t  satIndex = 0; // SAT entry index for diagnostics
	};

	SpriteInfo  _spriteInfoList[MAX_SPRITES_LINE_H40] = {};
	SpriteDraw  _spriteDrawList[MAX_DRAWS_H40] = {};
	uint8_t     _sprInfoCount  = 0;   // sprites found during scan
	uint8_t     _sprDraws      = 0;   // first populated draw index
	int8_t      _sprCurSlot    = 0;   // current sprite info entry for ReadSpriteX
	uint8_t     _sprRenderIdx  = 0;   // current draw entry for SpriteRender
	uint8_t     _sprRenderCell = 0;   // current cell within sprite being rendered
	uint8_t     _sprScanLink   = 0;   // current SAT link for scan_sprite_table
	bool        _sprScanDone   = false;
	bool        _sprCanMask    = false; // non-zero X sprite seen (carries across lines, NOT reset per line)
	bool        _sprMasked     = false; // X=0 mask active
	uint8_t     _maxSpritesLine = MAX_SPRITES_LINE_H40;
	uint8_t     _maxDrawsLine   = MAX_DRAWS_H40;
	uint8_t     _sprCellBudget  = 40;   // max sprite cells processed this line (H40=40, H32=32)

	// SAT cache: snapshot of the sprite attribute table taken at EndLine().
	// BeginLine()'s batch sprite scan reads from this cache so that the scan
	// sees the SAT state from the *previous* line (before the 68K ran the
	// current line's cycles), matching hardware timing for mid-frame SAT
	// rewrites used by sprite-multiplexing games.
	static constexpr uint16_t SAT_CACHE_SIZE = 80u * 8u; // 80 entries × 8 bytes
	uint8_t  _satCache[SAT_CACHE_SIZE] = {};
	void CacheSpriteTable();

	// --- Slot operation methods ---
	void DispatchSlot(const SlotDescriptor& slot);
	void SlotReadMapScrollA(int16_t column);
	void SlotReadMapScrollB(int16_t column);
	void SlotRenderMap1();
	void SlotRenderMap2();
	void SlotRenderMap3();
	void SlotRenderMapOutput(int16_t column);
	void SlotReadSpriteX();
	void SlotSpriteRender();
	void SlotScanSpriteTable();
	void SlotExternalSlot();
	void SlotClearLinebuf();
	void SlotHScrollLoad();
	void FifoDrainOne();       // drain one FIFO entry (execute the write)
	void FifoUpdateStatus();   // update status bits 9:8 from _fifoCount
	void RunDmaSrc();          // feed one Bus68k DMA word into FIFO (called at external slots)
	void FlushCompositeBuf(uint16_t line);

	// -----------------------------------------------------------------------
	// Internal helpers
	// -----------------------------------------------------------------------

	// VRAM: word-width internal bus, big-endian pairs in _vram[]
	uint16_t VramRead (uint16_t wordAddr) const;
	void     VramWrite(uint16_t wordAddr, uint16_t value);

	// CRAM / VSRAM
	uint16_t CramRead (uint8_t idx) const;
	void     CramWrite(uint8_t idx, uint16_t value);
	uint16_t VsramRead (uint8_t idx) const;
	void     VsramWrite(uint8_t idx, uint16_t value);

	// Palette expansion
	void     RefreshPalette(uint8_t idx);
	static uint32_t CramWordToArgb         (uint16_t w);
	static uint32_t CramWordToArgbShadow   (uint16_t w);
	static uint32_t CramWordToArgbHighlight(uint16_t w);

	// Control port helpers
	void     AdvanceAddr();
	void     PrimeBuf();
	void     HandleControlWrite(uint16_t word);
	void     WriteReg(uint8_t r, uint8_t val);
	void     BeginOperation();   // called after address/code are finalized

	// DMA
	void     ExecDma();
	void     ExecDmaBus68k(uint32_t maxWords);
	void     ExecDmaFill(uint32_t maxBytes);
	void     ExecDmaCopy(uint32_t maxWords);
	void     AdvanceDmaAddr();
	void     StartDmaIfArmed();

	// -----------------------------------------------------------------------
	// Rendering
	// -----------------------------------------------------------------------
	void RenderScanline(uint16_t line);
	void RenderBackdrop(uint16_t line);

	// Per-pixel encoding:
	//   bit 7   : priority flag
	//   bits 6:4: palette (0-3) — note only 2 bits needed, stored in 4:3
	//   bits 3:0: color index within palette (0 = transparent for planes/sprites)
	// Full CRAM index = ((pix >> 4) & 0xF) * 16 + (pix & 0xF)
	//   but palette is 2 bits (bits 5:4) and color index 4 bits (3:0), so:
	//   CRAM index = ((pix >> 4) & 3) * 16 + (pix & 0xF)

	void RenderPlaneB (uint16_t line, uint8_t* dst, uint16_t pixels) const;
	void RenderPlaneA (uint16_t line, uint8_t* dst, uint16_t pixels) const;
	void RenderWindow (uint16_t line, uint8_t* dst, uint16_t pixels) const;
	void RenderSprites(uint16_t line, uint8_t* dst, uint16_t pixels);
	void Composite    (uint16_t line,
	                   const uint8_t* planeB, const uint8_t* planeA,
	                   const uint8_t* spr,    uint16_t pixels);

	// Render one 8-wide name-table row into dst[0..7]
	// Returns true if any pixel in this column is covered by the window plane.
	void RenderNameTableCol(uint16_t nameWord, uint8_t tileRow,
	                         uint8_t* dst, bool toWindow) const;

	// Fetch one pixel's 4-bit color index from a tile
	uint8_t FetchTilePixel(uint16_t tileBase, uint8_t row, uint8_t col) const;

	// H/V counter — accurate non-linear mapping
	uint8_t  VCounterValue(uint32_t scanline) const;
	uint16_t ReadHVCounter() const;
	uint16_t _hvLatch = 0;    // latched HV value (R0 bit 1)

	// H/V scroll queries
	uint16_t GetHScrollRaw(uint16_t line, bool planeA) const;
	uint16_t GetHScroll(uint16_t line, bool planeA) const;
	uint16_t GetVScroll(uint16_t tileCol2, bool planeA) const; // tileCol2 = tile column / 2

	// Window coverage test
	bool IsWindowPixel(uint16_t line, uint16_t x) const;

	// Mode queries
	bool     IsH40()             const { return (_reg[12] & 0x01) != 0; }
	bool     IsV30()             const { return (_reg[1]  & 0x08) != 0; }
	bool     DispEnabled()       const { return (_reg[1]  & 0x40) != 0; }
	bool     VIntEnabled()       const { return (_reg[1]  & 0x20) != 0; }
	bool     HIntEnabled()       const { return (_reg[0]  & 0x10) != 0; }
	bool     DmaEnabled()        const { return (_reg[1]  & 0x10) != 0; }
	bool     ShadowHlEnabled()   const { return (_reg[12] & 0x08) != 0; }
	// Interlace mode from R12 bits [2:1] (LS1-LS0):
	//   00 = no interlace, 01 = interlace normal, 10 = no interlace, 11 = interlace double.
	// Interlace is active only when LS0 (bit 1) is set; LS1 alone (0x04) means no interlace.
	uint8_t  InterlaceMode()     const { return _reg[12] & 0x06u; }  // raw LS1:LS0 bits
	bool     IsInterlace()       const { return (_reg[12] & 0x02u) != 0; } // LS0 must be set
	bool     IsInterlace2()      const { return (_reg[12] & 0x06u) == 0x06u; } // both LS1+LS0

	uint16_t PlaneWidthTiles()  const;
	uint16_t PlaneHeightTiles() const;

	uint16_t ScrollABase() const { return (uint16_t)(_reg[2] & 0x38) << 10; }
	uint16_t ScrollBBase() const { return (uint16_t)(_reg[4] & 0x07) << 13; }
	uint16_t WindowBase()  const;
	uint16_t SpriteBase()  const;
	uint16_t HScrollBase() const { return (uint16_t)(_reg[13] & 0x3F) << 10; }
	uint8_t  AutoInc()     const { return _reg[15]; }
	uint8_t  HIntReload()  const { return _reg[10]; }

public:
	void Init(Emulator* emu, GenesisNativeBackend* backend, bool isPal);
	void Reset(bool isPal);

	// Event-driven master-clock scheduler interface
	// Call BeginFrame once per frame before the event loop.
	// AdvanceToMclk processes all scanlines up to targetMclk.
	// NextVIntMclk / NextHIntMclk predict the next interrupt event point.
	void     BeginFrame(uint32_t* fb, uint32_t fbW, uint32_t fbH);
	void     AdvanceToMclk(uint32_t targetMclk);
	uint32_t NextVIntMclk() const;
	uint32_t NextHIntMclk() const;
	uint32_t GetMclkPos()   const { return _mclkPos; }

	uint16_t ActiveWidth()  const { return (_reg[12] & 0x01) ? 320u : 256u; }
	uint16_t ActiveHeight() const { return (_reg[1]  & 0x08) ? 240u : 224u; }
	uint16_t TotalScanlines() const { return _isPal ? LinesPal : LinesNtsc; }

	// Called by GenesisNativeBackend::ReadCartBus / WriteCartBus
	// addr is the raw 24-bit bus address (in $C00000-$C0001F range)
	uint8_t ReadByte (uint32_t addr);
	void    WriteByte(uint32_t addr, uint8_t val);

	// Called once per scanline from the backend's RunFrame loop.
	// Renders line `line` into `fb` (ARGB8888, stride `fbW`).
	// Returns true when this call crosses into VBlank (line == ActiveHeight()-1 done).
	// Caller should check ConsumeVInt() / ConsumeHInt() after each call.
	void BeginLine(uint16_t line, uint32_t* fb, uint32_t fbW, uint32_t fbH);
	void EndLine();
	void RunLine(uint16_t line, uint32_t* fb, uint32_t fbW, uint32_t fbH);

	// Interrupt consumption — call after RunLine
	bool ConsumeVInt();
	bool ConsumeHInt();

	// DMA — backend calls these
	bool HasPendingDma()  const { return _dmaType != DmaType::None || _dmaFillPend; }
	bool Is68kBusDmaActive() const { return _dmaType == DmaType::Bus68k && _dmaLen > 0; }
	uint32_t Consume68kBusDma(uint32_t masterClocks, uint32_t sliceStartMclk); // returns consumed 68K-bus lock clocks
	uint32_t ConsumeInternalDma(uint32_t masterClocks); // returns consumed internal-DMA clocks
	void TriggerDmaFill(uint16_t data);    // called on data-port write when dma-fill armed
	void ExecPendingDma();                  // public entry point for backend

	// Debugger accessors
	uint8_t*  Vram()  { return _vram; }
	uint8_t*  Cram()  { return _cram; }
	uint8_t*  Vsram() { return _vsram; }

	uint16_t GetScanline()   const { return _scanline; }
	uint16_t GetHClock()     const { return (uint16_t)(_mclkPos % MCLKS_PER_LINE); }
	uint16_t GetVClock()     const { return _scanline; }
	uint16_t GetHVCounter()  const { return ReadHVCounter(); }
	uint16_t GetStatus()     const { return _status; }
	uint32_t GetFrameCount() const { return _frameCount; }
	bool     IsDisplayEnabled() const { return DispEnabled(); }
	bool     IsBlanking() const { return !DispEnabled() || (_status & 0x0008u) != 0; }
	void     GetDebugState(GenesisVdpDebugState& state) const;
	bool     GetDebugTraceLines(GenesisTraceBufferKind kind, vector<string>& lines) const;
	uint8_t  GetRegister(uint8_t index) const { return index < 24 ? _reg[index] : 0; }
	void     GetRegisters(uint8_t regs[24]) const { memcpy(regs, _reg, 24); }
	uint8_t  GetDmaType() const { return (uint8_t)_dmaType; }
	uint32_t GetDmaSource() const { return _dmaSrc; }
	uint32_t GetDmaLength() const { return _dmaLen; }
	uint16_t GetDmaFillValue() const { return _dmaFillVal; }
	bool     IsDmaFillPending() const { return _dmaFillPend; }
	bool     IsVIntPending() const { return _vintPending; }
	bool     IsHIntPending() const { return _hintPending; }

	// Save / load state helpers (called by GenesisNativeBackend::SaveState/LoadState)
	void SaveState(vector<uint8_t>& out) const;
	bool LoadState(const vector<uint8_t>& data, size_t& offset);
};
