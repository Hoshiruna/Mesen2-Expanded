#pragma once

#include "pch.h"

class GenesisDACchannel
{
public:
	void Reset();

	void WriteData(uint8_t data);
	void SetEnabled(bool enabled);
	void SetPan(uint8_t ymPanReg);
	bool IsEnabled() const;

	void Advance(uint32_t masterClocks);
	void MixSample(int32_t& outL, int32_t& outR, uint32_t samplePeriod) const;
	void ResetWindowAccumulator();

	void SaveState(vector<uint8_t>& out) const;
	bool LoadState(const vector<uint8_t>& data, size_t& offset);

private:
	uint8_t _data = 0;
	uint8_t _pan = 0;
	bool _enabled = false;
	int64_t _accumL = 0;
	int64_t _accumR = 0;
};
