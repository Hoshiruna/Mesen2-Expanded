#pragma once

#include "pch.h"
#include "Genesis/GenesisI2cEeprom.h"
#include "Genesis/GenesisCpu68k.h"
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisCpuZ80.h"
#include "Genesis/APU/GenesisApu.h"
#include "Shared/MemoryType.h"

class Emulator;
class IGenesisPlatformCallbacks;

// ---------------------------------------------------------------------------
// GenesisNativeBackend
//
// Native core: correct cart bus map + native M68000 interpreter.
// VDP/APU are integrated with scanline-based scheduling.
// ---------------------------------------------------------------------------
class GenesisNativeBackend final
{
public:
	// SRAM bus width exposed by the cart header.
	enum class SramMode : uint8_t { None, Word, UpperByte, LowerByte };

	// Cart mapper type determined at load time.
	enum class MapperType : uint8_t { Linear, SSF2 };

	// Decoded 68K bus region used by the bus dispatch layer.
	enum class BusRegion : uint8_t {
		Cart,
		CartOverflow,
		Z80Space,
		IoRegs,
		MemoryMode,
		Z80BusReq,
		Z80Reset,
		MapperRegs,
		Tmss,       // $A14000–$A14003 TMSS unlock registers
		TmssCart,   // $A14101 TMSS/cartridge register
		VdpPorts,
		WorkRam,
		OpenBus
	};

	private:
		enum class ExecContext : uint8_t { None, Cpu68k, Z80 };

		// -----------------------------------------------------------------------
			// Save-state identity
			// -----------------------------------------------------------------------
			static constexpr uint32_t NativeStateMagic   = 0x314E444Du; // MDN1
			static constexpr uint32_t NativeStateVersion = 36;          // Refresh-delay parity state

	// -----------------------------------------------------------------------
	// Platform callbacks / emulator
	// -----------------------------------------------------------------------
	IGenesisPlatformCallbacks* _callbacks = nullptr;
	Emulator*                  _emu       = nullptr;

	// -----------------------------------------------------------------------
	// CPU
	// -----------------------------------------------------------------------
	GenesisCpu68k _cpu;
	GenesisCpuState _cpuState = {};
	uint32_t _cpuUsp = 0;
	uint8_t  _cpuPendingIrq = 0;

	// -----------------------------------------------------------------------
	// Memory regions
	// -----------------------------------------------------------------------
	vector<uint8_t> _rom;
	vector<uint8_t> _workRam;   // 64 KB
	vector<uint8_t> _saveRam;
	bool _cpuTestBusEnabled = false;
	vector<uint8_t> _cpuTestBus;
	uint16_t _openBusValue = 0xFFFFu;
	mutable uint32_t _refreshCounterMclk = 0;
	mutable uint64_t _refreshLastAccessClock = 0;

	// -----------------------------------------------------------------------
	// Cart / mapper state
	// -----------------------------------------------------------------------
	MapperType _mapperType  = MapperType::Linear;

	// SSF2 bank table: 8 windows of 512 KB each covering $000000-$3FFFFF.
	// romBank[n] = physical 512 KB page mapped into window n.
	uint8_t _romBank[8]  = {};
	bool    _ramEnable   = false;
	bool    _ramWritable = true;

	// SRAM window on the 68K bus (from ROM header or defaults).
	uint32_t _sramStart = 0;
	uint32_t _sramEnd   = 0;
	SramMode _sramMode  = SramMode::None;

	// EEPROM
	GenesisI2cEeprom _eeprom;
	bool     _hasEeprom    = false;
	uint32_t _eepromBusStart = 0;
	uint32_t _eepromBusEnd   = 0;
	// Pin mapping (4-bit values: bit 3 selects byte lane, bits 2-0 select bit).
	uint8_t  _rsda = 0;    // read SDA
	uint8_t  _wsda = 0;    // write SDA
	uint8_t  _wscl = 1;    // write SCL

	// -----------------------------------------------------------------------
	// VDP (owns VRAM/CRAM/VSRAM)
	// -----------------------------------------------------------------------
	GenesisVdp _vdp;

	// -----------------------------------------------------------------------
		// Z80 sound CPU + APU
		// -----------------------------------------------------------------------
		GenesisCpuZ80 _z80;
		GenesisApu    _apu;
	static constexpr uint16_t Z80BusReqAckDelayMclk = 45; // ~3 Z80 cycles
	static constexpr uint16_t Z80BusResumeDelayMclk = 15; // ~1 Z80 cycle

	// -----------------------------------------------------------------------
	// Video / timing
	// -----------------------------------------------------------------------
	vector<uint32_t> _frameBuffer;
	uint32_t _frameWidth  = 320;
	uint32_t _frameHeight = 224;

		bool     _isPal      = false;
		uint64_t _masterClock = 0;
		uint8_t  _cpuClockRemainder = 0; // master-clock remainder for 68K /7 divider
		uint8_t  _z80ClockRemainder = 0; // master-clock remainder for Z80 /15 divider
		uint64_t _sliceStartMasterClock = 0;
		uint32_t _sliceMasterClocks = 0;
		uint32_t _slice68kStartMclk = 0;
		uint32_t _apuSliceSyncedMclk = 0;
		ExecContext _execContext = ExecContext::None;
		uint64_t _diag68kSliceOverrunCycles = 0;
		uint32_t _diag68kSliceOverrunCount = 0;
		uint32_t _diag68kMaxSliceOverrun = 0;
		uint64_t _diagFrameCounter = 0;

	// -----------------------------------------------------------------------
	// I/O registers
	// -----------------------------------------------------------------------
	bool     _z80BusRequest = false;
	bool     _z80Reset = true;
	bool     _z80BusAck = false; // true when Z80 has released its bus to 68K
	uint16_t _z80BusReqDelayMclk = 0;
	uint16_t _z80ResumeDelayMclk = 0;
	uint64_t _z80BusReqReadyClock = 0;
	uint64_t _z80ResumeReadyClock = 0;
	uint32_t _z80SliceSyncedMclk = 0;
	uint8_t  _z80SliceClockRemainder = 0;
	uint8_t  _ioData[3] = { 0x7F, 0x7F, 0x7F };
	uint8_t  _ioCtrl[3] = { 0x00, 0x00, 0x00 };
	// MD I/O registers 0x07-0x0F (TxData/RxData/Serial control).
	// These are mostly unused by games, but startup/runtime probes rely on
	// sensible reset values and masking behavior.
	uint8_t  _ioExt[9] = { 0xFF, 0x00, 0x00, 0xFF, 0x00, 0x00, 0xFB, 0x00, 0x00 };
	uint8_t  _ioPadState[2] = { 0x40, 0x40 }; // TH input state (bit6)
	uint8_t  _ioPadCounter[2] = { 0, 0 };     // 6-button TH pulse counter
	uint8_t  _ioPadTimeout[2] = { 0, 0 };     // 6-button counter timeout (scanline ticks)
	uint64_t _ioPadLatency[2] = { 0, 0 };     // TH input-switch latency (68K cycles)
	bool     _ioSixButton[2] = { false, false };
	bool     _preferSixButton = false;
	uint32_t _bootStallFrames = 0;
	uint32_t _bootInjectFrames = 0;
	uint32_t _bootInjectCount = 0;

	string   _disasmText;

	// -----------------------------------------------------------------------
	// Internal helpers
	// -----------------------------------------------------------------------
	void     ParseCartHeader();
	void     InitBankTable();
	BusRegion DecodeBusRegion(uint32_t address) const;
	bool     IsZ80BusGranted() const;
		uint8_t  GetOpenBusByte(uint32_t address) const;
		void     UpdateOpenBusByte(uint32_t address, uint8_t value);
		bool     IsRefreshFreeAccess(BusRegion region) const;
		uint8_t  GetRefreshWaitStates(BusRegion region) const;
		void     AdvanceZ80BusArbitration(uint32_t masterClocks);
		void     SyncZ80ToSliceOffset(uint32_t offsetMclk);
		void     SyncZ80ToCurrentExecution();
		void     UpdateZ80ArbitrationState(uint64_t accessClock);
		uint64_t GetCurrentExecutionMasterClock() const;
		uint32_t GetRemainingZ80CycleMclk() const;
		void     UpdateFrameGeometry();
		void     DeliverPendingVdpInterrupts();
		uint32_t GetCurrentSliceOffsetMclk() const;
		void     SyncApuToCurrentExecution();
		void     SyncApuToSliceOffset(uint32_t offsetMclk);
		void     RunMasterClockSlice(uint32_t masterClocks);
		uint8_t  ReadCartBus(uint32_t address);
		void     WriteCartBus(uint32_t address, uint8_t value);

	uint16_t ReadCartBusWord(uint32_t address);
	void     WriteCartBusWord(uint32_t address, uint16_t value);
	uint8_t  ReadIoRegister(uint32_t address);
	void     WriteIoRegister(uint32_t address, uint8_t value);
	uint8_t  ReadGamepadPort(uint32_t port, uint32_t buttons, bool connected);
	void     WriteGamepadPort(uint32_t port, uint8_t data, uint8_t mask, bool connected);

	// EEPROM bus helpers (byte-level, translates pin-mapped word protocol).
	uint8_t  EepromRead(uint32_t address) const;
	void     EepromWrite(uint32_t address, uint8_t value);

public:
	GenesisNativeBackend(Emulator* emu, IGenesisPlatformCallbacks* callbacks);
	~GenesisNativeBackend() = default;

	// CPU bus access — called by GenesisCpu68k
	uint8_t CpuBusRead8 (uint32_t address);
	void    CpuBusWrite8(uint32_t address, uint8_t value);
	uint8_t CpuBusWaitStates(uint32_t address, bool isWrite) const;
	uint16_t GetOpenBusWord() const { return _openBusValue; }

	// Z80 ROM window access — called by GenesisCpuZ80
	uint8_t ReadBusForZ80 (uint32_t physAddr);
	void    WriteBusForZ80(uint32_t physAddr, uint8_t val);
	uint8_t GetZ80To68kBusPenaltyCycles() const;
	bool    RaiseVdpIrq(uint8_t level);
	void    VdpInterruptAcknowledge();

	// Core interface -------------------------------------------------------

	bool LoadRom(const vector<uint8_t>& romData, const char* region,
	             const uint8_t* saveRamData, uint32_t saveRamSize,
	             const uint8_t* saveEepromData, uint32_t saveEepromSize);

	void RunFrame();
	void SyncSaveData();

	const uint8_t* GetMemoryPointer(MemoryType type, uint32_t& size);
	const uint8_t* GetSaveEeprom(uint32_t& size);

	bool     IsPAL()            const;
	double   GetFps()           const;
	uint32_t GetFrameCount()    const;
	uint64_t GetMasterClock()   const;
	uint32_t GetMasterClockRate() const;

	void GetCpuState(GenesisCpuState& state) const;
	void GetVdpState(GenesisVdpState& state) const;
	void GetVdpRegisters(uint8_t regs[24]) const;
	bool GetVdpDebugState(GenesisVdpDebugState& state) const;
	bool GetVdpTraceLines(GenesisTraceBufferKind kind, vector<string>& lines) const;
	void GetFrameSize(uint32_t& width, uint32_t& height) const;
	bool GetBackendDebugState(GenesisBackendState& state) const;

	uint8_t ReadMemory(MemoryType type, uint32_t address);
	void    WriteMemory(MemoryType type, uint32_t address, uint8_t value);

	bool        SetProgramCounter(uint32_t address);
	uint32_t    GetInstructionSize(uint32_t address);
	const char* DisassembleInstruction(uint32_t address);

	bool SaveState(vector<uint8_t>& outState);
	bool LoadState(const vector<uint8_t>& state);

	// Z80 debug interface (called from GenesisCpuZ80 via _backend pointer)
	void Z80ProcessInstruction();
	void Z80ProcessRead(uint16_t addr, uint8_t& value, MemoryOperationType opType);
	bool Z80ProcessWrite(uint16_t addr, uint8_t& value, MemoryOperationType opType);

	// Z80 state for GenesisZ80Debugger
	GenesisZ80State GetZ80DebugState() const;
	void            SetZ80ProgramCounter(uint16_t addr);

		void EnableCpuTestBus();
		void DisableCpuTestBus();
		void ClearCpuTestBus(uint8_t fillValue = 0);
		void SetCpuTestBusByte(uint32_t address, uint8_t value);
		uint8_t GetCpuTestBusByte(uint32_t address) const;
		void SetCpuStateForTest(const GenesisCpuState& state, uint8_t pendingIrq = 0);
		void GetCpuStateForTest(GenesisCpuState& state) const;
		int32_t RunCpuInstructionForTest();
		uint8_t GetVdpRegister(uint8_t index) const;
		uint16_t GetHVCounter() const;
};
