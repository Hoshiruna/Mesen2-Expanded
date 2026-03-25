#pragma once
#include "pch.h"
#include "Shared/BaseState.h"
#include "Shared/MemoryType.h"

// M68000 CPU state (subset exposed to Mesen2 debugger)
struct GenesisCpuState : public BaseState
{
	uint64_t CycleCount = 0;
	uint32_t PC = 0;
	uint32_t SP = 0;    // A7

	uint32_t D[8] = {};
	uint32_t A[8] = {};

	uint16_t SR = 0;    // Status register (CCR + supervisor bits)

	bool Stopped = false;

	// padding byte inserted by compiler before USP, preserved here for clarity
	uint32_t USP = 0;   // User Stack Pointer (saved when in supervisor mode)
};

struct GenesisVdpState : public BaseState
{
	uint32_t FrameCount = 0;
	uint16_t HClock = 0;
	uint16_t VClock = 0;
	uint16_t Width  = 320;
	uint16_t Height = 224;
	bool     PAL    = false;
};

struct GenesisState : public BaseState
{
	GenesisCpuState Cpu;
	GenesisVdpState Vdp;
};

struct GenesisBackendState : public BaseState
{
	uint64_t MasterClock = 0;
	uint32_t FrameWidth = 0;
	uint32_t FrameHeight = 0;
	uint32_t ActiveWidth = 0;
	uint32_t ActiveHeight = 0;
	uint32_t VdpMclkPos = 0;
	uint32_t DmaSource = 0;
	uint32_t DmaLength = 0;
	uint16_t Scanline = 0;
	uint16_t HClock = 0;
	uint16_t VdpStatus = 0;
	uint16_t DmaFillValue = 0;
	uint8_t  DmaType = 0;
	uint8_t  CpuPendingIrq = 0;
	uint8_t  VintPending = 0;
	uint8_t  HintPending = 0;
	uint8_t  DmaFillPending = 0;
	uint8_t  DisplayEnabled = 0;
	uint8_t  Z80BusRequest = 0;
	uint8_t  Z80Reset = 0;
	uint8_t  Z80BusAck = 0;
	uint8_t  PAL = 0;
};

enum class GenesisTraceBufferKind : uint8_t
{
	Dma,
	Sprite,
	Compose,
	Scroll,
	HScrollDma
};

static constexpr uint32_t GenesisDebugScrollBufferSize = 32;
static constexpr uint32_t GenesisDebugLineBufferSize = 347;
static constexpr uint32_t GenesisDebugMaxSpritesLine = 20;
static constexpr uint32_t GenesisDebugMaxSpriteDraws = 40;

struct GenesisSpriteInfoDebugState : public BaseState
{
	uint16_t Index = 0;
	int16_t  Y = 0;
	uint8_t  Size = 0;
};

struct GenesisSpriteDrawDebugState : public BaseState
{
	int16_t  XPos = 0;
	uint16_t Address = 0;
	uint8_t  PalPri = 0;
	uint8_t  HFlip = 0;
	uint8_t  Width = 0;
	uint8_t  Height = 0;
	uint16_t BaseTile = 0;
	uint8_t  CellRow = 0;
	uint8_t  PixRow = 0;
	uint8_t  SatIndex = 0;
};

struct GenesisVdpDebugState : public BaseState
{
	uint32_t FrameCount = 0;
	uint16_t Scanline = 0;
	uint16_t HClock = 0;
	uint16_t VClock = 0;
	uint16_t HvCounter = 0;
	uint16_t Status = 0;
	uint16_t ActiveWidth = 0;
	uint16_t ActiveHeight = 0;
	uint8_t  Regs[24] = {};
	bool     IsH40 = false;
	bool     Interlace2 = false;
	bool     DisplayEnabled = false;
	bool     ShadowHighlightEnabled = false;

	uint16_t SlotIndex = 0;
	uint16_t SlotCycles = 0;

	uint8_t  TmpBufA[GenesisDebugScrollBufferSize] = {};
	uint8_t  TmpBufB[GenesisDebugScrollBufferSize] = {};
	uint8_t  BufAOff = 0;
	uint8_t  BufBOff = 0;

	uint16_t Col1 = 0;
	uint16_t Col2 = 0;
	uint16_t ColB1 = 0;
	uint16_t ColB2 = 0;
	uint8_t  VOffsetA = 0;
	uint8_t  VOffsetB = 0;
	bool     WindowActive = false;
	uint16_t VscrollLatch[2] = {};
	uint16_t HscrollA = 0;
	uint16_t HscrollAFine = 0;
	uint16_t HscrollB = 0;
	uint16_t HscrollBFine = 0;

	uint8_t  Linebuf[GenesisDebugLineBufferSize] = {};
	uint8_t  Compositebuf[GenesisDebugLineBufferSize] = {};

	uint8_t  SprInfoCount = 0;
	uint8_t  SprDraws = 0;
	int8_t   SprCurSlot = 0;
	uint8_t  SprRenderIdx = 0;
	uint8_t  SprRenderCell = 0;
	uint8_t  SprScanLink = 0;
	bool     SprScanDone = false;
	bool     SprCanMask = false;
	bool     SprMasked = false;
	uint8_t  MaxSpritesLine = 0;
	uint8_t  MaxDrawsLine = 0;
	uint8_t  SprCellBudget = 0;
	bool     PrevLineDotOverflow = false;

	GenesisSpriteInfoDebugState SpriteInfos[GenesisDebugMaxSpritesLine] = {};
	GenesisSpriteDrawDebugState SpriteDrawList[GenesisDebugMaxSpriteDraws] = {};
};

// Z80 state exposed to the Mesen2 debugger
struct GenesisZ80State : public BaseState
{
	uint64_t CycleCount = 0;
	uint16_t PC = 0;
	uint16_t SP = 0;
	uint16_t IX = 0, IY = 0;
	uint8_t  A = 0, F = 0;
	uint8_t  B = 0, C = 0, D = 0, E = 0, H = 0, L = 0;
	uint8_t  A2 = 0, F2 = 0;
	uint8_t  B2 = 0, C2 = 0, D2 = 0, E2 = 0, H2 = 0, L2 = 0;
	uint8_t  I = 0, R = 0;
	bool     IFF1 = false, IFF2 = false;
	uint8_t  IM = 0;
	bool     Halted = false;
	uint16_t BankReg = 0;
};

// Button bitmask for a Genesis 3-button / 6-button pad
namespace GenesisButton
{
	enum GenesisButton : uint32_t
	{
		Up    = 0x0001,
		Down  = 0x0002,
		Left  = 0x0004,
		Right = 0x0008,
		A     = 0x0010,
		B     = 0x0020,
		C     = 0x0040,
		Start = 0x0080,
		X     = 0x0100,
		Y     = 0x0200,
		Z     = 0x0400,
		Mode  = 0x0800,
	};
}
