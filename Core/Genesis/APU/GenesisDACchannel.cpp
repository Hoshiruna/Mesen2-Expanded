#include "pch.h"

#include "Genesis/APU/GenesisDACchannel.h"

namespace
{
	template<typename T>
	void AppendValue(vector<uint8_t>& out, const T& value)
	{
		size_t pos = out.size();
		out.resize(pos + sizeof(value));
		memcpy(out.data() + pos, &value, sizeof(value));
	}

	template<typename T>
	bool ReadValue(const vector<uint8_t>& data, size_t& offset, T& value)
	{
		if(offset + sizeof(value) > data.size()) {
			return false;
		}

		memcpy(&value, data.data() + offset, sizeof(value));
		offset += sizeof(value);
		return true;
	}
}

void GenesisDACchannel::Reset()
{
	_data = 0;
	_pan = 0;
	_enabled = false;
	ResetWindowAccumulator();
}

void GenesisDACchannel::WriteData(uint8_t data)
{
	_data = data;
}

void GenesisDACchannel::SetEnabled(bool enabled)
{
	_enabled = enabled;
}

void GenesisDACchannel::SetPan(uint8_t ymPanReg)
{
	_pan = (uint8_t)((ymPanReg >> 6) & 0x03u);
}

bool GenesisDACchannel::IsEnabled() const
{
	return _enabled;
}

void GenesisDACchannel::Advance(uint32_t masterClocks)
{
	if(!_enabled || masterClocks == 0u) {
		return;
	}

	int32_t dacSample = ((int32_t)_data - 128) << 6;
	if(_pan & 0x02u) _accumL += (int64_t)dacSample * masterClocks;
	if(_pan & 0x01u) _accumR += (int64_t)dacSample * masterClocks;
}

void GenesisDACchannel::MixSample(int32_t& outL, int32_t& outR, uint32_t samplePeriod) const
{
	if(samplePeriod == 0u) {
		return;
	}

	outL += (int32_t)(_accumL / (int64_t)samplePeriod);
	outR += (int32_t)(_accumR / (int64_t)samplePeriod);
}

void GenesisDACchannel::ResetWindowAccumulator()
{
	_accumL = 0;
	_accumR = 0;
}

void GenesisDACchannel::SaveState(vector<uint8_t>& out) const
{
	AppendValue(out, _data);
	AppendValue(out, _pan);
	AppendValue(out, _enabled);
	AppendValue(out, _accumL);
	AppendValue(out, _accumR);
}

bool GenesisDACchannel::LoadState(const vector<uint8_t>& data, size_t& offset)
{
	return ReadValue(data, offset, _data)
		&& ReadValue(data, offset, _pan)
		&& ReadValue(data, offset, _enabled)
		&& ReadValue(data, offset, _accumL)
		&& ReadValue(data, offset, _accumR);
}
