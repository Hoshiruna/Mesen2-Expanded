#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Shared/MemoryType.h"

class LabelManager;
class EmuSettings;
class MemoryDumper;
struct GenesisCpuState;
class GenesisConsole;

class GenesisDisUtils
{
public:
	static void GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings);
	static EffectiveAddressInfo GetEffectiveAddress(DisassemblyInfo& info, GenesisConsole* console, GenesisCpuState& state);

	static uint8_t GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper);

	static bool IsJumpToSub(uint32_t opCode);
	static bool IsReturnInstruction(uint32_t opCode);
	static bool IsUnconditionalJump(uint32_t opCode);
	static bool IsConditionalJump(uint32_t opCode);
};
