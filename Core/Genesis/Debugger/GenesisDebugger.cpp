#include "pch.h"
#include "Genesis/Debugger/GenesisDebugger.h"
#include "Genesis/Debugger/GenesisDisUtils.h"
#include "Genesis/Debugger/GenesisTraceLogger.h"
#include "Genesis/Debugger/GenesisEventManager.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/CodeDataLogger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/StepBackManager.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/MemoryDumper.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/MemoryOperationType.h"

GenesisDebugger::GenesisDebugger(Debugger* debugger) : IDebugger(debugger->GetEmulator())
{
	_debugger            = debugger;
	_emu                 = debugger->GetEmulator();
	_console             = (GenesisConsole*)debugger->GetConsole();
	_settings            = _emu->GetSettings();
	_disassembler        = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();

	_eventManager.reset(new GenesisEventManager(debugger, _console));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_breakpointManager.reset(new BreakpointManager(debugger, this, CpuType::GenesisMain, _eventManager.get()));
	_traceLogger.reset(new GenesisTraceLogger(debugger, this));
	uint32_t romSize = (uint32_t)_emu->GetMemory(MemoryType::GenesisPrgRom).Size;
	if(romSize == 0) romSize = 1;  // CDL requires non-zero size
	_codeDataLogger.reset(new CodeDataLogger(debugger, MemoryType::GenesisPrgRom, romSize, CpuType::GenesisMain, _emu->GetCrc32()));
	_step.reset(new StepRequest());
}

GenesisDebugger::~GenesisDebugger()
{
}

void GenesisDebugger::Reset()
{
	_prevProgramCounter = 0;
}

void GenesisDebugger::Run()
{
	_step.reset(new StepRequest());
}

void GenesisDebugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);
	switch(type) {
		case StepType::Step:          step.StepCount = stepCount; break;
		case StepType::CpuCycleStep:  step.CpuCycleStepCount = stepCount; break;
		case StepType::PpuStep:
		case StepType::PpuScanline:
		case StepType::PpuFrame:      step.PpuStepCount = stepCount; break;
		case StepType::SpecificScanline: step.BreakScanline = stepCount; break;
		default: step.StepCount = 1; break;
	}
	_step.reset(new StepRequest(step));
}

void GenesisDebugger::ProcessInstruction()
{
	// Called hypothetically when a CPU instruction is about to execute.
	// Since Ares runs internally, this is a no-op for now.
}

void GenesisDebugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	addr &= 0x00FFFFFF;

	if(!updateDebuggerOnly) {
		GenesisAresImpl* impl = _console->GetAresImpl();
		if(impl) {
			GenesisAresSetProgramCounter(impl, addr);
		}
	}

	_prevProgramCounter = addr;
	GenesisAresImpl* impl = _console->GetAresImpl();
	if(impl) {
		uint8_t hi = GenesisAresReadMemory(impl, addr);
		uint8_t lo = GenesisAresReadMemory(impl, (addr + 1) & 0x00FFFFFF);
		_prevOpWord = (uint16_t)((hi << 8) | lo);
	}
}

uint32_t GenesisDebugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : _console->GetState().Cpu.PC;
}

uint64_t GenesisDebugger::GetCpuCycleCount(bool forProfiler)
{
	return _console->GetState().Cpu.CycleCount;
}

void GenesisDebugger::ResetPrevOpCode()
{
	_prevProgramCounter = _console->GetState().Cpu.PC;
	GenesisAresImpl* impl = _console->GetAresImpl();
	if(impl) {
		uint8_t hi = GenesisAresReadMemory(impl, _prevProgramCounter);
		uint8_t lo = GenesisAresReadMemory(impl, (_prevProgramCounter + 1) & 0x00FFFFFF);
		_prevOpWord = (uint16_t)((hi << 8) | lo);
	} else {
		_prevOpWord = 0;
	}
}

DebuggerFeatures GenesisDebugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq  = true;
	features.CallStack = false;
	features.StepOver  = false;
	features.StepOut   = false;
	features.StepBack  = false;
	features.ChangeProgramCounter = AllowChangeProgramCounter;

	// M68000 autovector IRQ addresses in exception table
	features.CpuVectors[0] = { "IRQ 1", 0x64, VectorType::Indirect };
	features.CpuVectors[1] = { "IRQ 2", 0x68, VectorType::Indirect };
	features.CpuVectors[2] = { "IRQ 3", 0x6C, VectorType::Indirect };
	features.CpuVectors[3] = { "IRQ 4", 0x70, VectorType::Indirect };
	features.CpuVectors[4] = { "IRQ 5", 0x74, VectorType::Indirect };
	features.CpuVectors[5] = { "IRQ 6", 0x78, VectorType::Indirect };
	features.CpuVectors[6] = { "IRQ 7", 0x7C, VectorType::Indirect };
	features.CpuVectorCount = 7;

	return features;
}

BaseEventManager* GenesisDebugger::GetEventManager()
{
	return _eventManager.get();
}

IAssembler* GenesisDebugger::GetAssembler()
{
	return nullptr;  // No assembler for Genesis yet
}

CallstackManager* GenesisDebugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* GenesisDebugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

ITraceLogger* GenesisDebugger::GetTraceLogger()
{
	return _traceLogger.get();
}

BaseState& GenesisDebugger::GetState()
{
	_cachedState = _console->GetState();
	return _cachedState.Cpu;
}

void GenesisDebugger::GetPpuState(BaseState& state)
{
	GenesisState fullState = _console->GetState();
	(GenesisVdpState&)state = fullState.Vdp;
}

void GenesisDebugger::SetPpuState(BaseState& state)
{
	// Cannot directly set Ares VDP state
}
