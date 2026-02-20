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
