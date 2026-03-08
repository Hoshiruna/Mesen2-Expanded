#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/IDebugger.h"
#include "Genesis/GenesisTypes.h"

class Disassembler;
class Debugger;
class GenesisConsole;
class BreakpointManager;
class EmuSettings;
class Emulator;

enum class MemoryOperationType;

class GenesisZ80Debugger final : public IDebugger
{
	Debugger*        _debugger;
	Emulator*        _emu;
	GenesisConsole*  _console;
	EmuSettings*     _settings;

	unique_ptr<BreakpointManager> _breakpointManager;

	uint16_t _prevProgramCounter = 0;
	GenesisZ80State _cachedState = {};

public:
	GenesisZ80Debugger(Debugger* debugger);
	~GenesisZ80Debugger();

	void Reset() override;
	void Run() override;
	void Step(int32_t stepCount, StepType type) override;

	void ProcessInstruction();
	void ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type);
	void ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type);

	void SetProgramCounter(uint32_t addr, bool updateDebuggerOnly = false) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	uint64_t GetCpuCycleCount(bool forProfiler = false) override;
	void ResetPrevOpCode() override;

	DebuggerFeatures GetSupportedFeatures() override;

	BaseEventManager* GetEventManager() override;
	IAssembler*       GetAssembler() override;
	CallstackManager* GetCallstackManager() override;
	BreakpointManager* GetBreakpointManager() override;
	ITraceLogger*     GetTraceLogger() override;

	BaseState& GetState() override;
	void GetPpuState(BaseState& state) override;
	void SetPpuState(BaseState& state) override;
};
