#include "pch.h"
#include "Genesis/Debugger/GenesisTraceLogger.h"
#include "Genesis/GenesisTypes.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/HexUtilities.h"

GenesisTraceLogger::GenesisTraceLogger(Debugger* debugger, IDebugger* cpuDebugger)
	: BaseTraceLogger(debugger, cpuDebugger, CpuType::GenesisMain)
{
}

RowDataType GenesisTraceLogger::GetFormatTagType(string& tag)
{
	// Map M68000 register names to RowDataType slots
	// We reuse R0-R15 for D0-D7 and A0-A7
	if(tag == "D0") return RowDataType::R0;
	if(tag == "D1") return RowDataType::R1;
	if(tag == "D2") return RowDataType::R2;
	if(tag == "D3") return RowDataType::R3;
	if(tag == "D4") return RowDataType::R4;
	if(tag == "D5") return RowDataType::R5;
	if(tag == "D6") return RowDataType::R6;
	if(tag == "D7") return RowDataType::R7;
	if(tag == "A0") return RowDataType::R8;
	if(tag == "A1") return RowDataType::R9;
	if(tag == "A2") return RowDataType::R10;
	if(tag == "A3") return RowDataType::R11;
	if(tag == "A4") return RowDataType::R12;
	if(tag == "A5") return RowDataType::R13;
	if(tag == "A6") return RowDataType::R14;
	if(tag == "A7") return RowDataType::R15;  // A7 = SP
	if(tag == "SR") return RowDataType::PS;
	return RowDataType::Text;
}

void GenesisTraceLogger::GetTraceRow(string& output, GenesisCpuState& cpuState, TraceLogPpuState& ppuState, DisassemblyInfo& disassemblyInfo)
{
	for(RowPart& rowPart : _rowParts) {
		switch(rowPart.DataType) {
			case RowDataType::R0:  WriteIntValue(output, cpuState.D[0], rowPart); break;
			case RowDataType::R1:  WriteIntValue(output, cpuState.D[1], rowPart); break;
			case RowDataType::R2:  WriteIntValue(output, cpuState.D[2], rowPart); break;
			case RowDataType::R3:  WriteIntValue(output, cpuState.D[3], rowPart); break;
			case RowDataType::R4:  WriteIntValue(output, cpuState.D[4], rowPart); break;
			case RowDataType::R5:  WriteIntValue(output, cpuState.D[5], rowPart); break;
			case RowDataType::R6:  WriteIntValue(output, cpuState.D[6], rowPart); break;
			case RowDataType::R7:  WriteIntValue(output, cpuState.D[7], rowPart); break;
			case RowDataType::R8:  WriteIntValue(output, cpuState.A[0], rowPart); break;
			case RowDataType::R9:  WriteIntValue(output, cpuState.A[1], rowPart); break;
			case RowDataType::R10: WriteIntValue(output, cpuState.A[2], rowPart); break;
			case RowDataType::R11: WriteIntValue(output, cpuState.A[3], rowPart); break;
			case RowDataType::R12: WriteIntValue(output, cpuState.A[4], rowPart); break;
			case RowDataType::R13: WriteIntValue(output, cpuState.A[5], rowPart); break;
			case RowDataType::R14: WriteIntValue(output, cpuState.A[6], rowPart); break;
			case RowDataType::R15: WriteIntValue(output, cpuState.A[7], rowPart); break;
			case RowDataType::PS:  WriteIntValue(output, cpuState.SR, rowPart); break;
			default: ProcessSharedTag(rowPart, output, cpuState, ppuState, disassemblyInfo); break;
		}
	}
}

void GenesisTraceLogger::LogPpuState()
{
	_ppuState[_currentPos] = { 0, 0, 0, 0 };
}
