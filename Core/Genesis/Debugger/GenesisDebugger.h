#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/IDebugger.h"
#include "Genesis/GenesisTypes.h"

class Disassembler;
class Debugger;
class GenesisTraceLogger;
class GenesisConsole;
class GenesisEventManager;
class CallstackManager;
class MemoryAccessCounter;
class BreakpointManager;
class EmuSettings;
class Emulator;
class CodeDataLogger;

enum class MemoryOperationType;

class GenesisDebugger final : public IDebugger
{
	Debugger* _debugger;
	Emulator* _emu;
	GenesisConsole* _console;
	EmuSettings* _settings;
	Disassembler* _disassembler;
	MemoryAccessCounter* _memoryAccessCounter;

	unique_ptr<GenesisEventManager> _eventManager;
	unique_ptr<CallstackManager>    _callstackManager;
	unique_ptr<CodeDataLogger>      _codeDataLogger;
	unique_ptr<BreakpointManager>   _breakpointManager;
	unique_ptr<GenesisTraceLogger>  _traceLogger;

	uint32_t _prevProgramCounter = 0;
	uint16_t _prevOpWord = 0;

	string _cdlFile;

public:
	GenesisDebugger(Debugger* debugger);
	~GenesisDebugger();

	void Reset() override;
	void Run() override;
	void Step(int32_t stepCount, StepType type) override;

	void ProcessInstruction();

	void SetProgramCounter(uint32_t addr, bool updateDebuggerOnly = false) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	uint64_t GetCpuCycleCount(bool forProfiler = false) override;
	void ResetPrevOpCode() override;

	DebuggerFeatures GetSupportedFeatures() override;

	BaseEventManager* GetEventManager() override;
	IAssembler* GetAssembler() override;
	CallstackManager* GetCallstackManager() override;
	BreakpointManager* GetBreakpointManager() override;
	ITraceLogger* GetTraceLogger() override;

	BaseState& GetState() override;
	void GetPpuState(BaseState& state) override;
	void SetPpuState(BaseState& state) override;

private:
	GenesisState _cachedState;
};
