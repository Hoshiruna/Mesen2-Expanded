#pragma once

#include "pch.h"
#include "Genesis/GenesisTypes.h"
#include "Shared/MemoryType.h"
#include "Shared/SettingTypes.h"

class IGenesisCoreBackend
{
public:
	virtual ~IGenesisCoreBackend() = default;

	virtual GenesisCoreType GetCoreType() const = 0;

	virtual bool LoadRom(const vector<uint8_t>& romData, const char* region,
	                     const uint8_t* saveRamData, uint32_t saveRamSize,
	                     const uint8_t* saveEepromData, uint32_t saveEepromSize) = 0;

	virtual void RunFrame() = 0;
	virtual void SyncSaveData() = 0;

	virtual const uint8_t* GetMemoryPointer(MemoryType type, uint32_t& size) = 0;
	virtual const uint8_t* GetSaveEeprom(uint32_t& size) = 0;

	virtual bool IsPAL() const = 0;
	virtual double GetFps() const = 0;
	virtual uint64_t GetMasterClock() const = 0;
	virtual uint32_t GetMasterClockRate() const = 0;

	virtual void GetCpuState(GenesisCpuState& state) const = 0;
	virtual void GetVdpState(GenesisVdpState& state) const = 0;
	virtual void GetVdpRegisters(uint8_t regs[24]) const = 0;
	virtual bool GetVdpDebugState(GenesisVdpDebugState& state) const { state = {}; return false; }
	virtual bool GetVdpTraceLines(GenesisTraceBufferKind kind, vector<string>& lines) const { lines.clear(); return false; }
	virtual void GetFrameSize(uint32_t& width, uint32_t& height) const = 0;
	virtual bool GetBackendDebugState(GenesisBackendState& state) const { state = {}; return false; }

	virtual uint8_t ReadMemory(MemoryType type, uint32_t address) = 0;
	virtual void WriteMemory(MemoryType type, uint32_t address, uint8_t value) = 0;

	virtual bool SetProgramCounter(uint32_t address) = 0;
	virtual uint32_t GetInstructionSize(uint32_t address) = 0;
	virtual const char* DisassembleInstruction(uint32_t address) = 0;

	virtual bool SaveState(vector<uint8_t>& outState) = 0;
	virtual bool LoadState(const vector<uint8_t>& state) = 0;
};
