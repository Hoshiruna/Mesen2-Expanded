#include "pch.h"
#include "Genesis/Debugger/GenesisZ80Debugger.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/StepBackManager.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MemoryOperationType.h"

GenesisZ80Debugger::GenesisZ80Debugger(Debugger* debugger) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_emu      = debugger->GetEmulator();
	_console  = (GenesisConsole*)debugger->GetConsole();
	_settings = _emu->GetSettings();

	_breakpointManager.reset(new BreakpointManager(debugger, this, CpuType::GenesisZ80, nullptr));
	_step.reset(new StepRequest());
}

GenesisZ80Debugger::~GenesisZ80Debugger()
{
}

void GenesisZ80Debugger::Reset()
{
	_prevProgramCounter = 0;
}

void GenesisZ80Debugger::Run()
{
	_step.reset(new StepRequest());
}

void GenesisZ80Debugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);
	switch(type) {
		case StepType::Step:         step.StepCount = stepCount; break;
		case StepType::CpuCycleStep: step.CpuCycleStepCount = stepCount; break;
		default:                     step.StepCount = 1; break;
	}
	_step.reset(new StepRequest(step));
}

void GenesisZ80Debugger::ProcessInstruction()
{
	_cachedState = _console->GetZ80DebugState();
	uint16_t pc  = _cachedState.PC;

	AddressInfo addressInfo = { (int32_t)pc, MemoryType::GenesisAudioRam };
	uint8_t value = (pc < 0x2000) ? _console->ReadMemory(MemoryType::GenesisAudioRam, pc) : 0;
	MemoryOperationInfo operation(pc, value, MemoryOperationType::ExecOpCode, MemoryType::GenesisAudioRam);

	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = _cachedState.CycleCount;

	_prevProgramCounter = pc;
	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(CpuType::GenesisZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void GenesisZ80Debugger::ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = { (int32_t)addr, MemoryType::GenesisAudioRam };
	MemoryOperationInfo operation(addr, value, type, MemoryType::GenesisAudioRam);
	_debugger->ProcessBreakConditions(CpuType::GenesisZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void GenesisZ80Debugger::ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = { (int32_t)addr, MemoryType::GenesisAudioRam };
	MemoryOperationInfo operation(addr, value, type, MemoryType::GenesisAudioRam);
	_debugger->ProcessBreakConditions(CpuType::GenesisZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void GenesisZ80Debugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	if(!updateDebuggerOnly) {
		_console->SetZ80ProgramCounter((uint16_t)(addr & 0xFFFF));
	}
	_prevProgramCounter = (uint16_t)(addr & 0xFFFF);
}

uint32_t GenesisZ80Debugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : _cachedState.PC;
}

uint64_t GenesisZ80Debugger::GetCpuCycleCount(bool forProfiler)
{
	return _cachedState.CycleCount;
}

void GenesisZ80Debugger::ResetPrevOpCode()
{
	_prevProgramCounter = 0;
}

DebuggerFeatures GenesisZ80Debugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.StepOver          = false;
	features.StepOut           = false;
	features.StepBack          = false;
	features.CallStack         = false;
	features.ChangeProgramCounter = true;
	return features;
}

BaseEventManager* GenesisZ80Debugger::GetEventManager()   { return nullptr; }
IAssembler*       GenesisZ80Debugger::GetAssembler()       { return nullptr; }
CallstackManager* GenesisZ80Debugger::GetCallstackManager(){ return nullptr; }
BreakpointManager* GenesisZ80Debugger::GetBreakpointManager() { return _breakpointManager.get(); }
ITraceLogger*     GenesisZ80Debugger::GetTraceLogger()     { return nullptr; }

BaseState& GenesisZ80Debugger::GetState()
{
	_cachedState = _console->GetZ80DebugState();
	return _cachedState;
}

void GenesisZ80Debugger::GetPpuState(BaseState& state) { (void)state; }
void GenesisZ80Debugger::SetPpuState(BaseState& state) { (void)state; }
