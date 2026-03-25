#include "pch.h"
#include <cmath>

#include "Genesis/APU/GenesisDACchannel.h"
#include "Genesis/APU/GenesisFMchannel.h"
#include "Genesis/GenesisNativeBackend.h"

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

	static constexpr double kPi = 3.14159265358979323846;
	static constexpr uint8_t kOpnFKeyTable[16] = {
		0, 0, 0, 0, 0, 0, 0, 1,
		2, 3, 3, 3, 3, 3, 3, 3
	};
	static constexpr uint8_t kLfoStep[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };
}

bool GenesisFMchannel::_tablesReady = false;
int16_t GenesisFMchannel::_sinTable[512] = {};
int16_t GenesisFMchannel::_expTable[256] = {};
uint8_t GenesisFMchannel::_counterShiftTable[64] = {};
uint8_t GenesisFMchannel::_attenuationIncrementTable[64][8] = {};
uint8_t GenesisFMchannel::_detunePhaseIncrementTable[32][4] = {};

GenesisFMchannel::GenesisFMchannel()
{
	if(!_tablesReady) {
		BuildTables();
		_tablesReady = true;
	}
}

void GenesisFMchannel::BuildTables()
{
	static const uint8_t counterShiftTable[64] = {
		11, 11, 11, 11, 10, 10, 10, 10,
		9, 9, 9, 9, 8, 8, 8, 8,
		7, 7, 7, 7, 6, 6, 6, 6,
		5, 5, 5, 5, 4, 4, 4, 4,
		3, 3, 3, 3, 2, 2, 2, 2,
		1, 1, 1, 1, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	memcpy(_counterShiftTable, counterShiftTable, sizeof(counterShiftTable));

	static const uint8_t attenuationIncrementTable[64][8] = {
		{0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, {0,1,0,1,0,1,0,1}, {0,1,0,1,0,1,0,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,0,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,0,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
		{1,1,1,1,1,1,1,1}, {1,1,1,2,1,1,1,2}, {1,2,1,2,1,2,1,2}, {1,2,2,2,1,2,2,2},
		{2,2,2,2,2,2,2,2}, {2,2,2,4,2,2,2,4}, {2,4,2,4,2,4,2,4}, {2,4,4,4,2,4,4,4},
		{4,4,4,4,4,4,4,4}, {4,4,4,8,4,4,4,8}, {4,8,4,8,4,8,4,8}, {4,8,8,8,4,8,8,8},
		{8,8,8,8,8,8,8,8}, {8,8,8,8,8,8,8,8}, {8,8,8,8,8,8,8,8}, {8,8,8,8,8,8,8,8}
	};
	memcpy(_attenuationIncrementTable, attenuationIncrementTable, sizeof(attenuationIncrementTable));

	static const uint8_t detunePhaseIncrementTable[32][4] = {
		{0,0,1,2}, {0,0,1,2}, {0,0,1,2}, {0,0,1,2},
		{0,1,2,2}, {0,1,2,3}, {0,1,2,3}, {0,1,2,3},
		{0,1,2,4}, {0,1,3,4}, {0,1,3,4}, {0,1,3,5},
		{0,2,4,5}, {0,2,4,6}, {0,2,4,6}, {0,2,5,7},
		{0,2,5,8}, {0,3,6,8}, {0,3,6,9}, {0,3,7,10},
		{0,4,8,11}, {0,4,8,12}, {0,4,9,13}, {0,5,10,14},
		{0,5,11,16}, {0,6,12,17}, {0,6,13,19}, {0,7,14,20},
		{0,8,16,22}, {0,8,16,22}, {0,8,16,22}, {0,8,16,22}
	};
	memcpy(_detunePhaseIncrementTable, detunePhaseIncrementTable, sizeof(detunePhaseIncrementTable));

	for(int i = 0; i < 512; i++) {
		double s = std::sin((i + 0.5) * kPi / 512.0);
		_sinTable[i] = (int16_t)(-std::log2(s) * 256.0 + 0.5);
	}

	for(int i = 0; i < 256; i++) {
		_expTable[i] = (int16_t)((std::pow(2.0, (255 - i) / 256.0)) * 1024.0 + 0.5);
	}
}

void GenesisFMchannel::Init(GenesisNativeBackend* backend)
{
	_backend = backend;
	Reset();
}

void GenesisFMchannel::Reset()
{
	_ym = {};
	_ymAddrLatch[0] = 0;
	_ymAddrLatch[1] = 0;
	_ymCh6PanReg = 0xC0;
	_currentMasterClock = _backend ? _backend->GetMasterClock() : 0;
	_busyUntilMasterClock = 0;
	_ymInternalAcc = 0;
	ResetWindowAccumulators();

	for(int c = 0; c < 6; c++) {
		for(int o = 0; o < 4; o++) {
			_ym.ch[c].op[o].egLevel = 1023;
			_ym.ch[c].op[o].egState = YmOp::Off;
			_ym.ch[c].op[o].tl = 127;
			_ym.ch[c].op[o].mul = 1;
			_ym.ch[c].op[o].rr = 15;
		}
		_ym.ch[c].lr = 3;
	}
}

void GenesisFMchannel::SyncCoreToMasterClock(uint64_t masterClock)
{
	if(masterClock < _currentMasterClock) {
		_currentMasterClock = masterClock;
	}
}

uint8_t GenesisFMchannel::ReadStatus(uint8_t part, uint64_t masterClock)
{
	(void)part;
	if(masterClock > _currentMasterClock) {
		Advance((uint32_t)(masterClock - _currentMasterClock));
	}

	uint8_t status = 0;
	if(_ym.timerAOverflow) status |= 0x01;
	if(_ym.timerBOverflow) status |= 0x02;
	if(_currentMasterClock < _busyUntilMasterClock) status |= 0x80;
	return status;
}

void GenesisFMchannel::YmUpdateDacRouting(GenesisDACchannel& dac)
{
	dac.SetPan(_ymCh6PanReg);
}

void GenesisFMchannel::RefreshDacRouting(GenesisDACchannel& dac)
{
	YmUpdateDacRouting(dac);
}

void GenesisFMchannel::Write(uint8_t part, bool isAddr, uint8_t data, uint64_t masterClock, GenesisDACchannel& dac)
{
	if(masterClock > _currentMasterClock) {
		Advance((uint32_t)(masterClock - _currentMasterClock));
	}

	if(isAddr) {
		_ymAddrLatch[part & 0x01] = data;
		return;
	}

	uint8_t reg = _ymAddrLatch[part & 0x01];
	uint64_t alignedClock = ((_currentMasterClock + YmInternalPeriod - 1) / YmInternalPeriod) * YmInternalPeriod;
	_busyUntilMasterClock = alignedClock + (uint64_t)YmInternalPeriod * 32u;

	if((part & 0x01) == 0 && reg == 0x2A) {
		dac.WriteData(data);
	} else if((part & 0x01) == 0 && reg == 0x2B) {
		dac.SetEnabled((data & 0x80u) != 0);
	} else if((part & 0x01) == 1 && reg == 0xB6) {
		_ymCh6PanReg = data;
		dac.SetPan(data);
	}

	YmHandleRegWrite(part & 0x01, reg, data);
}

uint32_t GenesisFMchannel::ClampAdvanceStep(uint32_t step) const
{
	uint32_t ymInternalNext = YmInternalPeriod - (_ymInternalAcc % YmInternalPeriod);
	if(step > ymInternalNext) {
		step = ymInternalNext;
	}
	return step;
}

void GenesisFMchannel::Advance(uint32_t masterClocks)
{
	if(masterClocks == 0u) {
		return;
	}

	_currentMasterClock += masterClocks;
	YmStepTimers(masterClocks);
	_ymInternalAcc += masterClocks;

	while(_ymInternalAcc >= YmInternalPeriod) {
		int32_t ymL = 0;
		int32_t ymR = 0;
		_ymInternalAcc -= YmInternalPeriod;
		YmClock(ymL, ymR);
		_ymAccumL += (int64_t)ymL * YmInternalPeriod;
		_ymAccumR += (int64_t)ymR * YmInternalPeriod;
	}
}

void GenesisFMchannel::MixSample(int32_t& outL, int32_t& outR, uint32_t samplePeriod)
{
	if(samplePeriod == 0u) {
		return;
	}

	outL += (int32_t)(_ymAccumL / (int64_t)samplePeriod);
	outR += (int32_t)(_ymAccumR / (int64_t)samplePeriod);
}

void GenesisFMchannel::ResetWindowAccumulators()
{
	_ymAccumL = 0;
	_ymAccumR = 0;
}

void GenesisFMchannel::SaveState(vector<uint8_t>& out) const
{
	static constexpr uint8_t YmStateVersion = 1;

	AppendValue(out, YmStateVersion);
	AppendValue(out, _ym);
	AppendValue(out, _ymAddrLatch);
	AppendValue(out, _ymCh6PanReg);
	AppendValue(out, _currentMasterClock);
	AppendValue(out, _busyUntilMasterClock);
	AppendValue(out, _ymInternalAcc);
	AppendValue(out, _ymAccumL);
	AppendValue(out, _ymAccumR);
}

bool GenesisFMchannel::LoadState(const vector<uint8_t>& data, size_t& offset, GenesisDACchannel& dac)
{
	static constexpr uint8_t YmStateVersion = 1;

	uint8_t version = 0;
	if(!ReadValue(data, offset, version) || version != YmStateVersion) return false;
	if(!ReadValue(data, offset, _ym)) return false;
	if(!ReadValue(data, offset, _ymAddrLatch)) return false;
	if(!ReadValue(data, offset, _ymCh6PanReg)) return false;
	if(!ReadValue(data, offset, _currentMasterClock)) return false;
	if(!ReadValue(data, offset, _busyUntilMasterClock)) return false;
	if(!ReadValue(data, offset, _ymInternalAcc)) return false;
	if(!ReadValue(data, offset, _ymAccumL)) return false;
	if(!ReadValue(data, offset, _ymAccumR)) return false;

	YmUpdateDacRouting(dac);
	return true;
}

void GenesisFMchannel::YmHandleRegWrite(uint8_t part, uint8_t reg, uint8_t data)
{
	if(part == 0) {
		if(reg == 0x22) {
			_ym.lfoEnable = (data & 0x08) != 0;
			_ym.lfoFreq = data & 0x07;
		} else if(reg == 0x24) {
			_ym.timerALatch = (uint16_t)((_ym.timerALatch & 0x0003u) | ((uint16_t)data << 2));
		} else if(reg == 0x25) {
			_ym.timerALatch = (uint16_t)((_ym.timerALatch & 0x03FCu) | (data & 0x03u));
		} else if(reg == 0x26) {
			_ym.timerBLatch = data;
		} else if(reg == 0x27) {
			bool timerALoad = (data & 0x01) != 0;
			bool timerBLoad = (data & 0x02) != 0;
			if(timerALoad) _ym.timerAAccum = 0;
			if(timerBLoad) _ym.timerBAccum = 0;
			_ym.timerALoad = timerALoad;
			_ym.timerBLoad = timerBLoad;
			_ym.timerAEnable = (data & 0x04) != 0;
			_ym.timerBEnable = (data & 0x08) != 0;
			if(data & 0x10) _ym.timerAOverflow = false;
			if(data & 0x20) _ym.timerBOverflow = false;
			_ym.ch3Mode = (data >> 6) & 0x03u;
			YmRefreshPhaseIncs(2);
		} else if(reg == 0x28) {
			YmKeyEvent(data);
			return;
		} else if(reg == 0x2A) {
			_ym.dacData = data;
		} else if(reg == 0x2B) {
			_ym.dacEnable = (data & 0x80) != 0;
		}
	}

	if(reg < 0x30) {
		return;
	}

	int chOff = reg & 0x03;
	if(chOff == 0x03) {
		return;
	}

	int ch = chOff + part * 3;
	static const int kSlotMap[4] = { 0, 2, 1, 3 };
	int slot = (reg >> 2) & 0x03;
	int op = kSlotMap[slot];
	YmOp& o = _ym.ch[ch].op[op];
	YmCh& c = _ym.ch[ch];

	switch(reg & 0xF0) {
		case 0x30:
			o.dt = (data >> 4) & 0x07;
			o.mul = data & 0x0F;
			YmUpdatePhaseInc(ch, op);
			break;

		case 0x40:
			o.tl = data & 0x7F;
			break;

		case 0x50:
			o.rs = (data >> 6) & 0x03;
			o.ar = data & 0x1F;
			break;

		case 0x60:
			o.amOn = (data & 0x80) != 0;
			o.d1r = data & 0x1F;
			break;

		case 0x70:
			o.d2r = data & 0x1F;
			break;

		case 0x80:
			o.sl = (data >> 4) & 0x0F;
			o.rr = data & 0x0F;
			break;

		case 0x90:
			o.ssgEg = data & 0x0F;
			break;

		case 0xA0: {
			uint8_t subReg = reg & 0x0F;
			if(subReg < 3) {
				c.fnum = (uint16_t)((c.fnum & 0x700) | data);
				YmRefreshPhaseIncs(ch);
			} else if(subReg >= 4 && subReg <= 6) {
				c.block = (data >> 3) & 0x07;
				c.fnum = (uint16_t)((c.fnum & 0x0FF) | (((uint16_t)(data & 0x07)) << 8));
				YmRefreshPhaseIncs(ch);
			} else if(part == 0 && subReg >= 8 && subReg <= 10) {
				int ch3Slot = subReg - 8;
				_ym.ch3Fnum[ch3Slot] = (uint16_t)((_ym.ch3Fnum[ch3Slot] & 0x700) | data);
				YmRefreshPhaseIncs(2);
			} else if(part == 0 && subReg >= 12 && subReg <= 14) {
				int ch3Slot = subReg - 12;
				_ym.ch3Block[ch3Slot] = (data >> 3) & 0x07;
				_ym.ch3Fnum[ch3Slot] = (uint16_t)((_ym.ch3Fnum[ch3Slot] & 0x0FF) | (((uint16_t)(data & 0x07)) << 8));
				YmRefreshPhaseIncs(2);
			}
			break;
		}

		case 0xB0: {
			uint8_t subReg = reg & 0x0F;
			if(subReg < 3) {
				c.alg = data & 0x07;
				c.fb = (data >> 3) & 0x07;
			} else if(subReg >= 4 && subReg <= 6) {
				c.lr = (data >> 6) & 0x03;
				c.ams = (data >> 4) & 0x03;
				c.pms = data & 0x07;
			}
			break;
		}
	}
}

void GenesisFMchannel::YmGetPhaseState(int ch, int op, uint16_t& fnum, uint8_t& block, uint8_t& keyCode) const
{
	const YmCh& c = _ym.ch[ch];
	fnum = c.fnum;
	block = c.block;

	if(ch == 2 && _ym.ch3Mode != 0) {
		switch(op) {
			case 0: fnum = _ym.ch3Fnum[1]; block = _ym.ch3Block[1]; break;
			case 1: fnum = _ym.ch3Fnum[2]; block = _ym.ch3Block[2]; break;
			case 2: fnum = _ym.ch3Fnum[0]; block = _ym.ch3Block[0]; break;
			default: break;
		}
	}

	keyCode = (uint8_t)((block << 2) | kOpnFKeyTable[(fnum >> 7) & 0x0F]);
}

void GenesisFMchannel::YmKeyEvent(uint8_t data)
{
	int ch = data & 0x07;
	if(ch == 3) ch = 5;
	else if(ch > 3) ch--;
	if(ch >= 6) return;

	for(int op = 0; op < 4; op++) {
		bool opOn = (data & (0x10 << op)) != 0;
		if(opOn) {
			YmKeyOn(ch, op);
		} else {
			YmKeyOff(ch, op);
		}
	}
}

void GenesisFMchannel::YmUpdatePhaseInc(int ch, int op)
{
	YmOp& o = _ym.ch[ch].op[op];
	uint16_t fnum = 0;
	uint8_t block = 0;
	uint8_t kc = 0;
	YmGetPhaseState(ch, op, fnum, block, kc);

	uint32_t baseInc = ((uint32_t)fnum << block) >> 1;
	uint32_t scaledInc = (o.mul == 0) ? (baseInc >> 1) : (baseInc * o.mul);
	scaledInc = (scaledInc + 12u) / 24u;
	o.phaseInc = (scaledInc == 0 && baseInc != 0) ? 1u : scaledInc;

	uint32_t dtVal = _detunePhaseIncrementTable[kc][o.dt & 0x03];
	if(o.dt & 0x04) {
		o.phaseInc = (o.phaseInc > dtVal) ? (o.phaseInc - dtVal) : 0u;
	} else {
		o.phaseInc += dtVal;
	}
}

void GenesisFMchannel::YmRefreshPhaseIncs(int ch)
{
	YmUpdatePhaseInc(ch, 0);
	YmUpdatePhaseInc(ch, 1);
	YmUpdatePhaseInc(ch, 2);
	YmUpdatePhaseInc(ch, 3);
}

void GenesisFMchannel::YmStepTimers(uint32_t masterClocks)
{
	uint32_t aPeriodTicks = 1024u - (_ym.timerALatch & 0x03FFu);
	uint32_t bPeriodTicks = 256u - (uint32_t)_ym.timerBLatch;
	if(aPeriodTicks == 0) aPeriodTicks = 1024u;
	if(bPeriodTicks == 0) bPeriodTicks = 256u;

	uint32_t aPeriod = aPeriodTicks * 24u;
	uint32_t bPeriod = bPeriodTicks * 384u;

	if(_ym.timerALoad) {
		_ym.timerAAccum += masterClocks;
		while(_ym.timerAAccum >= aPeriod) {
			_ym.timerAAccum -= aPeriod;
			if(_ym.timerAEnable) {
				_ym.timerAOverflow = true;
			}
		}
	}

	if(_ym.timerBLoad) {
		_ym.timerBAccum += masterClocks;
		while(_ym.timerBAccum >= bPeriod) {
			_ym.timerBAccum -= bPeriod;
			if(_ym.timerBEnable) {
				_ym.timerBOverflow = true;
			}
		}
	}
}

void GenesisFMchannel::YmKeyOn(int ch, int op)
{
	YmOp& o = _ym.ch[ch].op[op];
	if(!o.keyOn) {
		o.keyOn = true;
		o.phase = 0;
		o.egState = YmOp::Attack;

		if(YmEgRate(o, ch, op) >= 62u) {
			o.egLevel = 0;
			o.egState = YmOp::Decay;
		}
	}
}

void GenesisFMchannel::YmKeyOff(int ch, int op)
{
	YmOp& o = _ym.ch[ch].op[op];
	if(o.keyOn) {
		o.keyOn = false;
		if(o.egState != YmOp::Off) {
			o.egState = YmOp::Release;
		}
	}
}

uint32_t GenesisFMchannel::YmEgRate(const YmOp& o, int ch, int op) const
{
	uint16_t fnum = 0;
	uint8_t block = 0;
	uint8_t kc = 0;
	YmGetPhaseState(ch, op, fnum, block, kc);
	uint8_t shift = (uint8_t)(3 - o.rs);
	uint8_t rks = kc >> shift;

	uint8_t rate = 0;
	switch(o.egState) {
		case YmOp::Attack: rate = o.ar; break;
		case YmOp::Decay: rate = o.d1r; break;
		case YmOp::Sustain: rate = o.d2r; break;
		case YmOp::Release: rate = (uint8_t)((o.rr << 1) | 1); break;
		default: return 0;
	}

	if(rate == 0) {
		return 0;
	}

	uint32_t effRate = (uint32_t)rate * 2u + rks;
	return effRate > 63u ? 63u : effRate;
}

void GenesisFMchannel::YmStepEnvelope(YmOp& o, int ch, int op)
{
	if(o.egState == YmOp::Off) {
		return;
	}

	uint32_t rate = YmEgRate(o, ch, op);
	if(rate == 0) {
		return;
	}

	uint32_t counterShift = _counterShiftTable[rate];
	uint32_t cycleMask = (1u << counterShift) - 1u;
	if((_ym.egCounter & cycleMask) != 0u) {
		return;
	}

	uint32_t updateCycle = (_ym.egCounter >> counterShift) & 0x07u;
	uint32_t attenuationIncrement = _attenuationIncrementTable[rate][updateCycle];

	switch(o.egState) {
		case YmOp::Attack:
			if(rate >= 62u) {
				o.egLevel = 0;
				o.egState = YmOp::Decay;
			} else if(o.egLevel > 0u) {
				uint32_t delta = ((~o.egLevel) * attenuationIncrement) >> 4;
				o.egLevel = (o.egLevel + delta) & 0x3FFu;
				if(o.egLevel == 0u) {
					o.egState = YmOp::Decay;
				}
			}
			break;

		case YmOp::Decay: {
			o.egLevel += attenuationIncrement;
			uint32_t sl = o.sl == 15 ? 1023u : (uint32_t)o.sl << 5;
			if(o.egLevel >= sl) {
				o.egLevel = sl;
				o.egState = YmOp::Sustain;
			}
			break;
		}

		case YmOp::Sustain:
			if(o.d2r > 0) {
				o.egLevel += (o.ssgEg & 0x08) ? (attenuationIncrement * 4u) : attenuationIncrement;
				if(o.egLevel >= 1023u) {
					o.egLevel = 1023u;
					o.egState = YmOp::Off;
				}
			}
			break;

		case YmOp::Release:
			o.egLevel += attenuationIncrement;
			if(o.egLevel >= 1023u) {
				o.egLevel = 1023u;
				o.egState = YmOp::Off;
			}
			break;

		default:
			break;
	}
}

int32_t GenesisFMchannel::YmCalcOp(YmOp& o, int32_t modIn)
{
	o.phase = (o.phase + o.phaseInc) & 0x000FFFFFu;
	uint32_t phaseOut = (o.phase >> 10) & 0x3FFu;

	uint32_t sinPhase = phaseOut & 0x1FFu;
	bool sinNeg = (phaseOut & 0x200u) != 0;
	int16_t sinVal = _sinTable[sinPhase];

	uint32_t envAtt = (uint32_t)(o.tl << 3) + o.egLevel;
	if(envAtt > 1023u) envAtt = 1023u;

	uint32_t logAmp = (uint32_t)sinVal + (envAtt << 2);
	uint32_t expIdx = logAmp & 0xFFu;
	uint32_t expShift = logAmp >> 8;
	int32_t result = 0;
	if(expShift < 12u) {
		result = (int32_t)(_expTable[expIdx]) >> expShift;
	}

	if(modIn != 0) {
		uint32_t modPhase = (o.phase + (uint32_t)(modIn >> 1)) & 0x000FFFFFu;
		uint32_t mp = (modPhase >> 10) & 0x3FFu;
		bool mpNeg = (mp & 0x200u) != 0;
		int16_t ms = _sinTable[mp & 0x1FFu];
		uint32_t ml = (uint32_t)ms + (envAtt << 2);
		uint32_t mShift = ml >> 8;
		result = (mShift < 12u) ? (int32_t)(_expTable[ml & 0xFFu]) >> mShift : 0;
		if(mpNeg) result = -result;
		o.output = result;
		return result;
	}

	if(sinNeg) result = -result;
	o.output = result;
	return result;
}

void GenesisFMchannel::YmCalcSample(int32_t& outL, int32_t& outR)
{
	for(int ch = 0; ch < 6; ch++) {
		YmCh& c = _ym.ch[ch];

		int32_t fbMod = 0;
		if(c.fb > 0) {
			fbMod = (c.fbBuf[0] + c.fbBuf[1]) >> (9 - c.fb);
		}

		int32_t op1 = YmCalcOp(c.op[0], fbMod);
		c.fbBuf[1] = c.fbBuf[0];
		c.fbBuf[0] = op1;

		int32_t out = 0;
		switch(c.alg) {
			case 0: { int32_t o2 = YmCalcOp(c.op[1], op1); int32_t o3 = YmCalcOp(c.op[2], o2); out = YmCalcOp(c.op[3], o3); break; }
			case 1: { int32_t o2 = YmCalcOp(c.op[1], 0); int32_t o3 = YmCalcOp(c.op[2], op1 + o2); out = YmCalcOp(c.op[3], o3); break; }
			case 2: { int32_t o2 = YmCalcOp(c.op[1], 0); int32_t o3 = YmCalcOp(c.op[2], o2); out = YmCalcOp(c.op[3], op1 + o3); break; }
			case 3: { int32_t o2 = YmCalcOp(c.op[1], op1); int32_t o3 = YmCalcOp(c.op[2], 0); out = YmCalcOp(c.op[3], o2 + o3); break; }
			case 4: { int32_t o2 = YmCalcOp(c.op[1], op1); int32_t o3 = YmCalcOp(c.op[2], 0); out = o2 + YmCalcOp(c.op[3], o3); break; }
			case 5: { int32_t o2 = YmCalcOp(c.op[1], op1); int32_t o3 = YmCalcOp(c.op[2], op1); int32_t o4 = YmCalcOp(c.op[3], op1); out = o2 + o3 + o4; break; }
			case 6: { int32_t o2 = YmCalcOp(c.op[1], op1); int32_t o3 = YmCalcOp(c.op[2], 0); int32_t o4 = YmCalcOp(c.op[3], 0); out = o2 + o3 + o4; break; }
			case 7: { YmCalcOp(c.op[1], 0); YmCalcOp(c.op[2], 0); YmCalcOp(c.op[3], 0); out = op1 + c.op[1].output + c.op[2].output + c.op[3].output; break; }
		}

		if(ch == 5 && _ym.dacEnable) {
			// Exodus models the DAC as replacing channel 6's FM output rather than as
			// a separate post-mix source.
			out = ((int32_t)_ym.dacData - 0x80) << 6;
		} else {
			out >>= 2;
		}

		if(c.lr & 0x02) outL += out;
		if(c.lr & 0x01) outR += out;
	}
}

void GenesisFMchannel::YmClock(int32_t& outL, int32_t& outR)
{
	if(_ym.lfoEnable) {
		_ym.lfoPhase++;
		if(_ym.lfoPhase >= kLfoStep[_ym.lfoFreq & 0x07]) {
			_ym.lfoPhase = 0;
		}
	} else {
		_ym.lfoPhase = 0;
	}

	_ym.egTimer++;
	if(_ym.egTimer >= 3) {
		_ym.egTimer = 0;
		_ym.egCounter++;
		if(_ym.egCounter == 4096) {
			_ym.egCounter = 1;
		}

		for(int ch = 0; ch < 6; ch++) {
			for(int op = 0; op < 4; op++) {
				YmStepEnvelope(_ym.ch[ch].op[op], ch, op);
			}
		}
	}

	YmCalcSample(outL, outR);
}
