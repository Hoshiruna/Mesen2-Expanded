#pragma once

#include "pch.h"

#include "Genesis/APU/GenesisDACchannel.h"
#include "Genesis/APU/GenesisFMchannel.h"
#include "Genesis/APU/GenesisPSGchannel.h"

class Emulator;
class GenesisNativeBackend;

class GenesisApu
{
public:
	static constexpr uint32_t NtscSampleRate = 53267;
	static constexpr uint32_t PalSampleRate = 52781;

	void Init(Emulator* emu, GenesisNativeBackend* backend, bool isPal);
	void Reset(bool isPal);

	uint8_t ReadYmStatus(uint8_t part);
	void WriteYm(uint8_t part, bool isAddr, uint8_t data);
	void WritePsg(uint8_t data);

	void RunLine(uint32_t masterClocksThisLine);
	void SyncToMasterClock(uint64_t masterClock);
	void FlushFrame();

	void SaveState(vector<uint8_t>& out) const;
	bool LoadState(const vector<uint8_t>& data, size_t& offset);

private:
	static constexpr uint32_t MaxSamplesPerFrame = 1200;

	void Advance(uint32_t masterClocks);

	Emulator* _emu = nullptr;
	GenesisNativeBackend* _backend = nullptr;
	bool _isPal = false;
	uint64_t _lastMasterClock = 0;
	uint32_t _ymSampleAcc = 0;
	GenesisFMchannel _fm;
	GenesisPSGchannel _psg;
	GenesisDACchannel _dac;
	int16_t _sampleBuf[MaxSamplesPerFrame * 2] = {};
	uint32_t _sampleCount = 0;
};
