#include "pch.h"
#include "Genesis/Debugger/GenesisDebugger.h"
#include "Genesis/Debugger/GenesisDisUtils.h"
#include "Genesis/Debugger/GenesisTraceLogger.h"
#include "Genesis/Debugger/GenesisEventManager.h"
#include "Genesis/Debugger/GenesisVdpTools.h"
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
	_ppuTools.reset(new GenesisVdpTools(debugger, _emu, _console));
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
		case StepType::Step:         step.StepCount = stepCount; break;
		case StepType::CpuCycleStep: step.CpuCycleStepCount = stepCount; break;
		case StepType::PpuStep:
		case StepType::PpuScanline:
		case StepType::PpuFrame:     step.PpuStepCount = stepCount; break;
		case StepType::SpecificScanline: step.BreakScanline = stepCount; break;

		case StepType::StepOut:
			step.BreakAddress      = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;

		case StepType::StepOver:
			if(GenesisDisUtils::IsJumpToSub((uint32_t)_prevOpWord)) {
				// Break at the instruction after the JSR/BSR
				uint32_t instrSz = _console->GetInstructionSize(_prevProgramCounter);
				if(instrSz < 2) instrSz = 2;
				step.BreakAddress      = (int32_t)((_prevProgramCounter + instrSz) & 0x00FFFFFFu);
				step.BreakStackPointer = (int64_t)_prevStackPointer;
			} else {
				step.StepCount = 1;
			}
			break;

		default: step.StepCount = 1; break;
	}
	_step.reset(new StepRequest(step));
}

void GenesisDebugger::ProcessInstruction()
{
	GenesisCpuState cpuState = _console->GetState().Cpu;
	uint32_t pc = cpuState.PC & 0x00FFFFFFu;
	AddressInfo relAddr = { (int32_t)pc, MemoryType::GenesisMemory };
	AddressInfo addressInfo = _console->GetAbsoluteAddress(relAddr);
	uint8_t value = _console->ReadMemory(MemoryType::GenesisMemory, pc);
	MemoryOperationInfo operation(pc, value, MemoryOperationType::ExecOpCode, MemoryType::GenesisMemory);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = cpuState.CycleCount;

	if(addressInfo.Address >= 0) {
		if(addressInfo.Type == MemoryType::GenesisPrgRom) {
			_codeDataLogger->SetCode(addressInfo.Address);
		}
		_disassembler->BuildCache(addressInfo, 0, CpuType::GenesisMain);
	}

	// Update callstack based on what the *previous* instruction was
	ProcessCallStackUpdates(addressInfo, pc);

	uint8_t lo = _console->ReadMemory(MemoryType::GenesisMemory, (pc + 1) & 0x00FFFFFFu);
	_prevProgramCounter = pc;
	_prevOpWord         = (uint16_t)((value << 8) | lo);
	_prevStackPointer   = cpuState.SP;

	_step->ProcessCpuExec();
	_debugger->ProcessBreakConditions(CpuType::GenesisMain, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void GenesisDebugger::ProcessCallStackUpdates(AddressInfo& destAddr, uint32_t destPc)
{
	uint32_t prevOp = (uint32_t)_prevOpWord;

	if(GenesisDisUtils::IsJumpToSub(prevOp)) {
		// JSR / BSR: previous instruction pushed return address and jumped
		uint32_t instrSz  = _console->GetInstructionSize(_prevProgramCounter);
		if(instrSz < 2) instrSz = 2;
		uint32_t returnPc = (_prevProgramCounter + instrSz) & 0x00FFFFFFu;

		AddressInfo srcRelAddr = { (int32_t)_prevProgramCounter, MemoryType::GenesisMemory };
		AddressInfo retRelAddr = { (int32_t)returnPc,            MemoryType::GenesisMemory };
		AddressInfo srcAddr = _console->GetAbsoluteAddress(srcRelAddr);
		AddressInfo retAddr = _console->GetAbsoluteAddress(retRelAddr);

		_callstackManager->Push(srcAddr, _prevProgramCounter,
		                        destAddr, destPc,
		                        retAddr,  returnPc,
		                        _prevStackPointer, StackFrameFlags::None);

	} else if(GenesisDisUtils::IsReturnInstruction(prevOp)) {
		// RTS / RTR / RTE: previous instruction returned
		GenesisCpuState cpuState = _console->GetState().Cpu;
		_callstackManager->Pop(destAddr, destPc, cpuState.SP);

		if(_step->BreakAddress == (int32_t)destPc &&
		   _step->BreakStackPointer == (int64_t)cpuState.SP) {
			_step->Break(BreakSource::CpuStep);
		}
	}
}

void GenesisDebugger::ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	addr &= 0x00FFFFFFu;
	AddressInfo relAddr = { (int32_t)addr, MemoryType::GenesisMemory };
	AddressInfo addressInfo = _console->GetAbsoluteAddress(relAddr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::GenesisMemory);
	InstructionProgress.LastMemOperation = operation;

	if(type == MemoryOperationType::ExecOpCode) {
		if(_traceLogger->IsEnabled()) {
			DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, addr, 0, CpuType::GenesisMain);
			GenesisCpuState cpuState = _console->GetState().Cpu;
			_traceLogger->Log(cpuState, disInfo, operation, addressInfo);
		}

		_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());
		if(_step->ProcessCpuCycle()) {
			_debugger->SleepUntilResume(CpuType::GenesisMain, BreakSource::CpuStep, &operation);
		}
	} else if(type == MemoryOperationType::ExecOperand) {
		if(addressInfo.Address >= 0 && addressInfo.Type == MemoryType::GenesisPrgRom) {
			_codeDataLogger->SetCode(addressInfo.Address);
		}

		if(_traceLogger->IsEnabled()) {
			_traceLogger->LogNonExec(operation, addressInfo);
		}

		_memoryAccessCounter->ProcessMemoryExec(addressInfo, _console->GetMasterClock());
		_step->ProcessCpuCycle();
		_debugger->ProcessBreakConditions(CpuType::GenesisMain, *_step.get(), _breakpointManager.get(), operation, addressInfo);
	} else {
		if(addressInfo.Address >= 0 && addressInfo.Type == MemoryType::GenesisPrgRom) {
			_codeDataLogger->SetData(addressInfo.Address);
		}

		if(_traceLogger->IsEnabled()) {
			_traceLogger->LogNonExec(operation, addressInfo);
		}

		_memoryAccessCounter->ProcessMemoryRead(addressInfo, _console->GetMasterClock());
		_step->ProcessCpuCycle();
		_debugger->ProcessBreakConditions(CpuType::GenesisMain, *_step.get(), _breakpointManager.get(), operation, addressInfo);
	}
}

void GenesisDebugger::ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	addr &= 0x00FFFFFFu;
	AddressInfo relAddr = { (int32_t)addr, MemoryType::GenesisMemory };
	AddressInfo addressInfo = _console->GetAbsoluteAddress(relAddr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::GenesisMemory);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Address >= 0) {
		_disassembler->InvalidateCache(addressInfo, CpuType::GenesisMain);
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}

	_memoryAccessCounter->ProcessMemoryWrite(addressInfo, _console->GetMasterClock());
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions(CpuType::GenesisMain, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void GenesisDebugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	addr &= 0x00FFFFFF;

	if(!updateDebuggerOnly) {
		_console->SetProgramCounter(addr);
	}

	_prevProgramCounter = addr;
	uint8_t hi = _console->ReadMemory(MemoryType::GenesisMemory, addr);
	uint8_t lo = _console->ReadMemory(MemoryType::GenesisMemory, (addr + 1) & 0x00FFFFFF);
	_prevOpWord = (uint16_t)((hi << 8) | lo);
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
	uint8_t hi = _console->ReadMemory(MemoryType::GenesisMemory, _prevProgramCounter);
	uint8_t lo = _console->ReadMemory(MemoryType::GenesisMemory, (_prevProgramCounter + 1) & 0x00FFFFFF);
	_prevOpWord = (uint16_t)((hi << 8) | lo);
}

StepBackConfig GenesisDebugger::GetStepBackConfig()
{
	uint64_t masterClock = _console->GetMasterClock();
	// NTSC: 3420 master clocks/scanline, 896040/frame; PAL similar
	return { masterClock, 3420, 896040 };
}

DebuggerFeatures GenesisDebugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq  = true;
	features.CallStack = true;
	features.StepOver  = true;
	features.StepOut   = true;
	features.StepBack  = true;
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

PpuTools* GenesisDebugger::GetPpuTools()
{
	return _ppuTools.get();
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
	(void)state;
}
