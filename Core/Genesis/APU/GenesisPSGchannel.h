#pragma once

#include "pch.h"

class GenesisPSGchannel
{
public:
	void Reset();
	void Write(uint8_t data);
	void Advance(uint32_t masterClocks);
	void MixSample(int32_t& outL, int32_t& outR, uint32_t samplePeriod) const;
	void ResetWindowAccumulator();

	void SaveState(vector<uint8_t>& out) const;
	bool LoadState(const vector<uint8_t>& data, size_t& offset);

private:
	struct PsgState
	{
		int32_t regs[8] = {};
		int32_t freqInc[4] = {};
		int32_t freqCounter[4] = {};
		int16_t chanOut[4] = {};
		int8_t polarity[4] = { -1, -1, -1, -1 };
		uint32_t noiseShiftValue = 1u << 15;
		uint8_t latch = 3;
		uint8_t noiseShiftWidth = 15;
		uint8_t noiseBitMask = 0x09;
	} _state;

	int64_t _sampleAccum = 0;
};
