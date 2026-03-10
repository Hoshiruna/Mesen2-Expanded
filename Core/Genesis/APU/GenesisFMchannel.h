#pragma once

#include "pch.h"

class GenesisDACchannel;
class GenesisNativeBackend;

class GenesisFMchannel
{
public:
	static constexpr uint32_t YmPeriod = 7 * 144;
	static constexpr uint32_t YmInternalPeriod = 7 * 6;

	GenesisFMchannel();

	void Init(GenesisNativeBackend* backend);
	void Reset();

	void SyncCoreToMasterClock(uint64_t masterClock);
	uint8_t ReadStatus(uint8_t part, uint64_t masterClock);
	void Write(uint8_t part, bool isAddr, uint8_t data, uint64_t masterClock, GenesisDACchannel& dac);

	uint32_t ClampAdvanceStep(uint32_t step) const;
	void Advance(uint32_t masterClocks);
	void MixSample(int32_t& outL, int32_t& outR, uint32_t samplePeriod);
	void ResetWindowAccumulators();
	void RefreshDacRouting(GenesisDACchannel& dac);

	void SaveState(vector<uint8_t>& out) const;
	bool LoadState(const vector<uint8_t>& data, size_t& offset, GenesisDACchannel& dac);

private:
	struct YmOp
	{
		uint8_t dt = 0, mul = 1;
		uint8_t tl = 127;
		uint8_t rs = 0, ar = 0;
		bool amOn = false;
		uint8_t d1r = 0, d2r = 0;
		uint8_t sl = 0, rr = 0;
		uint8_t ssgEg = 0;
		uint32_t phase = 0;
		uint32_t phaseInc = 0;
		enum EgState : uint8_t { Attack = 0, Decay = 1, Sustain = 2, Release = 3, Off = 4 };
		EgState egState = Off;
		uint32_t egLevel = 1023;
		bool keyOn = false;
		int32_t output = 0;
	};

	struct YmCh
	{
		YmOp op[4];
		uint16_t fnum = 0;
		uint8_t block = 0;
		uint8_t alg = 0;
		uint8_t fb = 0;
		uint8_t pms = 0, ams = 0;
		uint8_t lr = 3;
		int32_t fbBuf[2] = {};
	};

		struct Ym2612State
		{
			YmCh ch[6];
			bool dacEnable = false;
			uint8_t dacData = 0;
			bool lfoEnable = false;
		uint8_t lfoFreq = 0;
		uint32_t lfoPhase = 0;
		uint8_t egTimer = 0;
		uint16_t timerALatch = 0;
		uint8_t timerBLatch = 0;
			uint32_t timerAAccum = 0;
			uint32_t timerBAccum = 0;
			bool timerALoad = false;
			bool timerBLoad = false;
			bool timerAEnable = false;
			bool timerBEnable = false;
			bool timerAOverflow = false;
			bool timerBOverflow = false;
			uint8_t ch3Mode = 0;
			uint16_t ch3Fnum[3] = {};
			uint8_t ch3Block[3] = {};
			uint16_t egCounter = 0;
		} _ym;

		GenesisNativeBackend* _backend = nullptr;
		uint8_t _ymAddrLatch[2] = {};
		uint8_t _ymCh6PanReg = 0xC0;
		uint64_t _currentMasterClock = 0;
		uint64_t _busyUntilMasterClock = 0;
		uint32_t _ymInternalAcc = 0;
		int64_t _ymAccumL = 0;
		int64_t _ymAccumR = 0;

	static bool _tablesReady;
	static int16_t _sinTable[512];
	static int16_t _expTable[256];
	static uint8_t _counterShiftTable[64];
	static uint8_t _attenuationIncrementTable[64][8];
	static uint8_t _detunePhaseIncrementTable[32][4];
	static void BuildTables();

	void YmUpdateDacRouting(GenesisDACchannel& dac);
	void YmHandleRegWrite(uint8_t part, uint8_t reg, uint8_t data);
	void YmGetPhaseState(int ch, int op, uint16_t& fnum, uint8_t& block, uint8_t& keyCode) const;
	void YmKeyEvent(uint8_t data);
	void YmUpdatePhaseInc(int ch, int op);
	void YmRefreshPhaseIncs(int ch);
	void YmStepTimers(uint32_t masterClocks);
	void YmKeyOn(int ch, int op);
	void YmKeyOff(int ch, int op);
	int32_t YmCalcOp(YmOp& o, int32_t modIn);
	void YmCalcSample(int32_t& outL, int32_t& outR);
	void YmClock(int32_t& outL, int32_t& outR);
	void YmStepEnvelope(YmOp& o, int ch, int op);
	uint32_t YmEgRate(const YmOp& o, int ch, int op) const;
};
