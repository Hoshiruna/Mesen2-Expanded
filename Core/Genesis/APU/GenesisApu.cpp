#include "pch.h"

#include "Genesis/APU/GenesisApu.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Emulator.h"

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

void GenesisApu::Init(Emulator* emu, GenesisNativeBackend* backend, bool isPal)
{
	_emu = emu;
	_backend = backend;
	_fm.Init(backend);
	Reset(isPal);
}

void GenesisApu::Reset(bool isPal)
{
	_isPal = isPal;
	_lastMasterClock = _backend ? _backend->GetMasterClock() : 0;
	_ymSampleAcc = 0;
	_sampleCount = 0;
	_psg.Reset();
	_dac.Reset();
	_fm.Reset();
	_fm.RefreshDacRouting(_dac);
}

uint8_t GenesisApu::ReadYmStatus(uint8_t part)
{
	uint64_t masterClock = _backend ? _backend->GetMasterClock() : _lastMasterClock;
	SyncToMasterClock(masterClock);
	return _fm.ReadStatus(part, masterClock);
}

void GenesisApu::WriteYm(uint8_t part, bool isAddr, uint8_t data)
{
	uint64_t masterClock = _backend ? _backend->GetMasterClock() : _lastMasterClock;
	SyncToMasterClock(masterClock);
	_fm.Write(part, isAddr, data, masterClock, _dac);
}

void GenesisApu::WritePsg(uint8_t data)
{
	uint64_t masterClock = _backend ? _backend->GetMasterClock() : _lastMasterClock;
	SyncToMasterClock(masterClock);
	_psg.Write(data);
}

void GenesisApu::Advance(uint32_t masterClocks)
{
	uint32_t remaining = masterClocks;
	while(remaining > 0u) {
		uint32_t step = remaining;
		uint32_t ymNext = GenesisFMchannel::YmPeriod - (_ymSampleAcc % GenesisFMchannel::YmPeriod);
		if(step > ymNext) {
			step = ymNext;
		}
		step = _fm.ClampAdvanceStep(step);

		_ymSampleAcc += step;
		remaining -= step;

		_dac.Advance(step);
		_psg.Advance(step);
		_fm.Advance(step);

		if(_ymSampleAcc >= GenesisFMchannel::YmPeriod) {
			_ymSampleAcc -= GenesisFMchannel::YmPeriod;

			if(_sampleCount + 2 <= MaxSamplesPerFrame * 2) {
				int32_t l = 0;
				int32_t r = 0;
				_psg.MixSample(l, r, GenesisFMchannel::YmPeriod);
				_fm.MixSample(l, r, GenesisFMchannel::YmPeriod);
				_dac.MixSample(l, r, GenesisFMchannel::YmPeriod);

				l = l > 32767 ? 32767 : (l < -32768 ? -32768 : l);
				r = r > 32767 ? 32767 : (r < -32768 ? -32768 : r);
				_sampleBuf[_sampleCount] = (int16_t)l;
				_sampleBuf[_sampleCount + 1] = (int16_t)r;
				_sampleCount += 2;
			}

			_psg.ResetWindowAccumulator();
			_dac.ResetWindowAccumulator();
			_fm.ResetWindowAccumulators();
		}
	}
}

void GenesisApu::RunLine(uint32_t masterClocksThisLine)
{
	SyncToMasterClock(_lastMasterClock + masterClocksThisLine);
}

void GenesisApu::SyncToMasterClock(uint64_t masterClock)
{
	if(masterClock <= _lastMasterClock) {
		_fm.SyncCoreToMasterClock(masterClock);
		return;
	}

	_fm.SyncCoreToMasterClock(masterClock);
	uint64_t delta = masterClock - _lastMasterClock;
	while(delta > 0u) {
		uint32_t step = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
		Advance(step);
		delta -= step;
	}

	_lastMasterClock = masterClock;
}

void GenesisApu::FlushFrame()
{
	if(!_emu || _sampleCount == 0) {
		_sampleCount = 0;
		return;
	}

	uint32_t rate = _isPal ? PalSampleRate : NtscSampleRate;
	_emu->GetSoundMixer()->PlayAudioBuffer(_sampleBuf, _sampleCount / 2, rate);
	_sampleCount = 0;
}

void GenesisApu::SaveState(vector<uint8_t>& out) const
{
	static constexpr uint8_t ApuStateVersion = 1;

	AppendValue(out, ApuStateVersion);
	AppendValue(out, _isPal);
	AppendValue(out, _lastMasterClock);
	AppendValue(out, _ymSampleAcc);
	_psg.SaveState(out);
	_dac.SaveState(out);
	_fm.SaveState(out);
}

bool GenesisApu::LoadState(const vector<uint8_t>& data, size_t& offset)
{
	static constexpr uint8_t ApuStateVersion = 1;

	uint8_t version = 0;
	if(!ReadValue(data, offset, version) || version != ApuStateVersion) return false;
	if(!ReadValue(data, offset, _isPal)) return false;
	if(!ReadValue(data, offset, _lastMasterClock)) return false;
	if(!ReadValue(data, offset, _ymSampleAcc)) return false;
	if(!_psg.LoadState(data, offset)) return false;
	if(!_dac.LoadState(data, offset)) return false;
	if(!_fm.LoadState(data, offset, _dac)) return false;

	_sampleCount = 0;
	return true;
}
