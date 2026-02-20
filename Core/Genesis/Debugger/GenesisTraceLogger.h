#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "Genesis/GenesisTypes.h"

class DisassemblyInfo;
class Debugger;

class GenesisTraceLogger : public BaseTraceLogger<GenesisTraceLogger, GenesisCpuState>
{
protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	GenesisTraceLogger(Debugger* debugger, IDebugger* cpuDebugger);

	void GetTraceRow(string& output, GenesisCpuState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(GenesisCpuState& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(GenesisCpuState& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(GenesisCpuState& state) { return (uint8_t)(state.SP & 0xFF); }
};
