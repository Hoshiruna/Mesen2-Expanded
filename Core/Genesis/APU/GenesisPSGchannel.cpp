#include "pch.h"
#include <algorithm>

#include "Genesis/APU/GenesisPSGchannel.h"

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

void GenesisPSGchannel::Reset()
{
	_state = {};
	for(int i = 0; i < 4; i++) {
		_state.polarity[i] = 1;
	}
	_state.latch = 3;
	_state.noiseShiftWidth = 15;
	_state.noiseBitMask = 0x09;
	_state.noiseShiftValue = 1u << _state.noiseShiftWidth;
	for(int i = 0; i < 3; i++) {
		_state.regs[i * 2 + 1] = 0x0F;
		_state.freqInc[i] = 15 * 16;
	}
	_state.regs[7] = 0x0F;
	_state.freqInc[3] = 16 * 15 * 16;
	ResetWindowAccumulator();
}

void GenesisPSGchannel::Write(uint8_t data)
{
	static constexpr int32_t PsgStepMclk = 15 * 16;
	static constexpr int32_t PsgZeroFreqInc = PsgStepMclk;

	int index = 0;
	if(data & 0x80) {
		_state.latch = index = (data >> 4) & 0x07;
	} else {
		index = _state.latch;
	}

	switch(index) {
		case 0:
		case 2:
		case 4: {
			int32_t regValue = 0;
			if(data & 0x80) {
				regValue = (_state.regs[index] & 0x3F0) | (data & 0x0F);
			} else {
				regValue = (_state.regs[index] & 0x00F) | ((data & 0x3F) << 4);
			}

			_state.regs[index] = regValue;
			_state.freqInc[index >> 1] = regValue ? (regValue * PsgStepMclk) : PsgZeroFreqInc;
			if(index == 4 && (_state.regs[6] & 0x03) == 0x03) {
				_state.freqInc[3] = _state.freqInc[2];
			}
			break;
		}

		case 6: {
			int32_t noiseFreq = data & 0x03;
			_state.regs[6] = data & 0x07;
			if(noiseFreq == 0x03) {
				_state.freqInc[3] = _state.freqInc[2];
				_state.freqCounter[3] = _state.freqCounter[2];
			} else {
				_state.freqInc[3] = (0x10 << noiseFreq) * PsgStepMclk;
			}
			_state.noiseShiftValue = 1u << _state.noiseShiftWidth;
			break;
		}

		default: {
			static const int16_t kPsgVolTable[16] = {
				2800, 2224, 1766, 1403, 1114, 885, 703, 558,
				443, 352, 280, 222, 177, 140, 111, 0
			};

			int channel = index >> 1;
			int32_t volume = data & 0x0F;
			_state.regs[index] = volume;
			if(channel < 4) {
				_state.chanOut[channel] = kPsgVolTable[volume];
			}
			break;
		}
	}
}

void GenesisPSGchannel::Advance(uint32_t masterClocks)
{
	static const uint8_t kNoiseFeedback[10] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0 };

	for(int i = 0; i < 4; i++) {
		if(i < 3) {
			uint32_t remaining = masterClocks;
			int32_t counter = _state.freqCounter[i];
			int32_t polarity = _state.polarity[i];
			int32_t period = std::max(_state.freqInc[i], 1);
			int32_t level = _state.chanOut[i];

			while(remaining > 0u) {
				while(counter <= 0) {
					polarity = -polarity;
					counter += period;
				}

				uint32_t run = std::min<uint32_t>(remaining, (uint32_t)counter);
				_sampleAccum += (int64_t)(polarity < 0 ? -level : level) * run;
				remaining -= run;
				counter -= (int32_t)run;
			}

			_state.freqCounter[i] = counter;
			_state.polarity[i] = (int8_t)polarity;
		} else {
			uint32_t remaining = masterClocks;
			int32_t counter = _state.freqCounter[i];
			int32_t polarity = _state.polarity[i];
			uint32_t shiftValue = _state.noiseShiftValue;
			int32_t period = std::max(_state.freqInc[3], 1);
			int32_t level = _state.chanOut[3];

			while(remaining > 0u) {
				while(counter <= 0) {
					polarity = -polarity;
					if(polarity > 0) {
						if(_state.regs[6] & 0x04) {
							shiftValue = (shiftValue >> 1) | (kNoiseFeedback[shiftValue & _state.noiseBitMask] << _state.noiseShiftWidth);
						} else {
							uint32_t shiftOutput = shiftValue & 0x01;
							shiftValue = (shiftValue >> 1) | (shiftOutput << _state.noiseShiftWidth);
						}
					}

					counter += period;
				}

				uint32_t run = std::min<uint32_t>(remaining, (uint32_t)counter);
				_sampleAccum += (int64_t)((shiftValue & 1u) ? level : 0) * run;
				remaining -= run;
				counter -= (int32_t)run;
			}

			_state.freqCounter[i] = counter;
			_state.polarity[i] = (int8_t)polarity;
			_state.noiseShiftValue = shiftValue;
		}
	}
}

void GenesisPSGchannel::MixSample(int32_t& outL, int32_t& outR, uint32_t samplePeriod) const
{
	int32_t psgSample = samplePeriod ? (int32_t)(_sampleAccum / (int64_t)samplePeriod) : 0;
	outL += psgSample;
	outR += psgSample;
}

void GenesisPSGchannel::ResetWindowAccumulator()
{
	_sampleAccum = 0;
}

void GenesisPSGchannel::SaveState(vector<uint8_t>& out) const
{
	AppendValue(out, _state);
	AppendValue(out, _sampleAccum);
}

bool GenesisPSGchannel::LoadState(const vector<uint8_t>& data, size_t& offset)
{
	return ReadValue(data, offset, _state)
		&& ReadValue(data, offset, _sampleAccum);
}
