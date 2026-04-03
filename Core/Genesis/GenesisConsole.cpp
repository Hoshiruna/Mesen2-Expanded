#include "pch.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/GenesisDefaultVideoFilter.h"
#include "Shared/BatteryManager.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/EventType.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/RenderedFrame.h"
#include "Utilities/Serializer.h"
#include "Utilities/StringUtilities.h"
#include <cctype>

GenesisConsole* GenesisConsole::_activeConsole = nullptr;

namespace
{
	constexpr uint32_t NativeStateFormatMarker = 0x54444D4E; // NMDT

	bool HasSegaHeader(const vector<uint8_t>& romData)
	{
		if(romData.size() < 0x104) {
			return false;
		}
		return romData[0x100] == 'S' && romData[0x101] == 'E' &&
			romData[0x102] == 'G' && romData[0x103] == 'A';
	}

	bool IsLikelySmdImage(const vector<uint8_t>& romData, const string& extension)
	{
		if(romData.size() <= 0x200) {
			return false;
		}

		if(extension == ".smd") {
			return true;
		}

		// Typical SMD payload size = 512-byte header + 16KB interleaved blocks.
		if((romData.size() & 0x3fff) != 0x200) {
			return false;
		}

		// If the plain header already looks valid, avoid false positives.
		return !HasSegaHeader(romData);
	}

	void DecodeSmdToLinear(vector<uint8_t>& romData)
	{
		if(romData.size() <= 0x200) {
			return;
		}

		size_t payloadSize = romData.size() - 0x200;
		if((payloadSize & 0x3fff) != 0) {
			// Fallback: strip 512-byte copier header when block layout is irregular.
			romData.erase(romData.begin(), romData.begin() + 0x200);
			return;
		}

		vector<uint8_t> decoded(payloadSize);
		const uint8_t* src = romData.data() + 0x200;

		for(size_t block = 0; block < payloadSize; block += 0x4000) {
			const uint8_t* in = src + block;
			uint8_t* out = decoded.data() + block;
			for(size_t i = 0; i < 0x2000; i++) {
				out[(i << 1)] = in[0x2000 + i];
				out[(i << 1) + 1] = in[i];
			}
		}

		romData.swap(decoded);
	}

	ConsoleRegion DetectRegionFromHeader(const vector<uint8_t>& romData)
	{
		if(romData.size() < 0x200) {
			return ConsoleRegion::Auto;
		}

		bool foundAny = false;
		bool hasJ = false;
		bool hasU = false;
		bool hasE = false;

		for(size_t i = 0x1f0; i < 0x200; i++) {
			unsigned char ch = (unsigned char)romData[i];
			if(ch == 0 || ch == ' ') {
				continue;
			}

			ch = (unsigned char)std::toupper(ch);
			if(ch == 'J') { hasJ = true; foundAny = true; continue; }
			if(ch == 'U') { hasU = true; foundAny = true; continue; }
			if(ch == 'E') { hasE = true; foundAny = true; continue; }

			// Some dumps store region as a hex bitmask (J=1, U=4, E=8).
			if((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')) {
				int value = (ch <= '9') ? (ch - '0') : (10 + (ch - 'A'));
				hasJ = hasJ || ((value & 0x1) != 0);
				hasU = hasU || ((value & 0x4) != 0);
				hasE = hasE || ((value & 0x8) != 0);
				foundAny = true;
			}
		}

		if(!foundAny) return ConsoleRegion::Auto;
		if(hasU) return ConsoleRegion::Ntsc;
		if(hasJ) return ConsoleRegion::NtscJapan;
		if(hasE) return ConsoleRegion::Pal;
		return ConsoleRegion::Auto;
	}
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

GenesisConsole::GenesisConsole(Emulator* emu)
{
	_emu = emu;
	_activeConsole = this;
}

GenesisConsole::~GenesisConsole()
{
	_backend.reset();
	if(_activeConsole == this) {
		_activeConsole = nullptr;
	}
}

void GenesisConsole::CreateBackend(GenesisCoreType coreType)
{
	(void)coreType;
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		return;
	}

	_backend.reset(new GenesisNativeBackend(_emu, this));
}

// ---------------------------------------------------------------------------
// LoadRom
// ---------------------------------------------------------------------------

void GenesisConsole::DetermineRegion(const string& filename, const vector<uint8_t>& romData)
{
	string lower = StringUtilities::ToLower(filename);

	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();

	ConsoleRegion region = cfg.Region;
	if(region == ConsoleRegion::Auto) {
		region = DetectRegionFromHeader(romData);
		if(region == ConsoleRegion::Auto) {
			if(lower.find("(europe)") != string::npos || lower.find("(e)") != string::npos ||
			   lower.find("(pal)") != string::npos) {
				region = ConsoleRegion::Pal;
			} else if(lower.find("(japan)") != string::npos || lower.find("(j)") != string::npos) {
				region = ConsoleRegion::NtscJapan;
			} else {
				region = ConsoleRegion::Ntsc;
			}
		}
	}

	_region = region;
	_isPAL  = (region == ConsoleRegion::Pal);
}

void GenesisConsole::RefreshDebuggerMemoryViews()
{
	if(!_backend) {
		return;
	}

	auto registerMemory = [this](MemoryType type) {
		uint32_t size = 0;
		const uint8_t* ptr = _backend->GetMemoryPointer(type, size);
		_emu->RegisterMemory(type, const_cast<uint8_t*>(ptr), ptr ? size : 0);
	};

	registerMemory(MemoryType::GenesisVideoRam);
	registerMemory(MemoryType::GenesisColorRam);
	registerMemory(MemoryType::GenesisVScrollRam);
	registerMemory(MemoryType::GenesisSaveRam);
}

LoadRomResult GenesisConsole::LoadRom(VirtualFile& romFile)
{
	vector<uint8_t> romData;
	romFile.ReadFile(romData);

	if(romData.size() < 0x200) {
		return LoadRomResult::Failure;
	}

	// Decode Super Magic Drive interleaved dumps to linear ROM bytes.
	string ext = StringUtilities::ToLower(romFile.GetFileExtension());
	if(IsLikelySmdImage(romData, ext)) {
		DecodeSmdToLinear(romData);
	}

	// Determine region
	DetermineRegion(romFile.GetFileName(), romData);

	const char* regionStr;
	if(_region == ConsoleRegion::NtscJapan) {
		regionStr = "NTSC-J";
	} else if(_isPAL) {
		regionStr = "PAL";
	} else {
		regionStr = "NTSC-U";
	}

	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();
	if(cfg.Port1.Type == ControllerType::None) {
		cfg.Port1.Type = ControllerType::GenesisController;
	}
	if(cfg.Port2.Type == ControllerType::None) {
		cfg.Port2.Type = ControllerType::GenesisController;
	}
	CreateBackend(cfg.CoreType);

	// Create control manager
	_controlManager.reset(new GenesisControlManager(_emu, this));

	vector<uint8_t> saveRamData = _emu->GetBatteryManager()->LoadBattery(".sav");
	vector<uint8_t> saveEepromData = _emu->GetBatteryManager()->LoadBattery(".eeprom");

	if(!_backend || !_backend->LoadRom(
		romData, regionStr,
		saveRamData.empty() ? nullptr : saveRamData.data(), (uint32_t)saveRamData.size(),
		saveEepromData.empty() ? nullptr : saveEepromData.data(), (uint32_t)saveEepromData.size()
	)) {
		return LoadRomResult::Failure;
	}

	_isPAL = _backend->IsPAL();

	// Allocate initial frame buffer
	_frameBuffer.resize(512 * 240, 0xFF000000);

	// Register physical memory regions for the debugger
	uint32_t romSize = 0;
	const uint8_t* romPtr = _backend->GetMemoryPointer(MemoryType::GenesisPrgRom, romSize);
	if(romPtr && romSize > 0) {
		_emu->RegisterMemory(MemoryType::GenesisPrgRom, const_cast<uint8_t*>(romPtr), romSize);
	}

	uint32_t wramSize = 0;
	const uint8_t* wramPtr = _backend->GetMemoryPointer(MemoryType::GenesisWorkRam, wramSize);
	if(wramPtr && wramSize > 0) {
		_emu->RegisterMemory(MemoryType::GenesisWorkRam, const_cast<uint8_t*>(wramPtr), wramSize);
	}

	// Register Z80 RAM if native backend
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		uint32_t audioRamSize = 0;
		const uint8_t* audioRam = _backend->GetMemoryPointer(MemoryType::GenesisAudioRam, audioRamSize);
		if(audioRam && audioRamSize > 0) {
			_emu->RegisterMemory(MemoryType::GenesisAudioRam, const_cast<uint8_t*>(audioRam), audioRamSize);
		}
	}

	RefreshDebuggerMemoryViews();

	return LoadRomResult::Success;
}

// ---------------------------------------------------------------------------
// IGenesisPlatformCallbacks
// ---------------------------------------------------------------------------

void GenesisConsole::OnVideoFrame(const uint32_t* pixels, uint32_t pitch,
                                  uint32_t width, uint32_t height)
{
	uint32_t targetWidth = width;
	uint32_t targetHeight = height;
	if(_backend) {
		uint32_t coreWidth = 0;
		uint32_t coreHeight = 0;
		_backend->GetFrameSize(coreWidth, coreHeight);
		if(coreWidth > 0 && coreHeight > 0) {
			targetWidth = coreWidth;
			targetHeight = coreHeight;
		}
	}

	uint32_t xStep = (targetWidth > 0) ? (width / targetWidth) : 0;
	uint32_t yStep = (targetHeight > 0) ? (height / targetHeight) : 0;
	bool canResample = xStep >= 1 && yStep >= 1
		&& (targetWidth * xStep == width)
		&& (targetHeight * yStep == height);
	if(!canResample) {
		targetWidth = width;
		targetHeight = height;
		xStep = 1;
		yStep = 1;
	}

	_frameWidth = targetWidth;
	_frameHeight = targetHeight;

	uint32_t pitchPixels = pitch / sizeof(uint32_t);
	uint32_t needed = _frameWidth * _frameHeight;
	if(_frameBuffer.size() < needed) {
		_frameBuffer.resize(needed);
	}

	for(uint32_t y = 0; y < _frameHeight; y++) {
		const uint32_t* src = pixels + (y * yStep) * pitchPixels;
		uint32_t* dst = _frameBuffer.data() + y * _frameWidth;
		if(xStep == 1) {
			memcpy(dst, src, _frameWidth * sizeof(uint32_t));
		} else {
			for(uint32_t x = 0; x < _frameWidth; x++) {
				dst[x] = src[x * xStep];
			}
		}
	}
}

void GenesisConsole::OnAudioSamples(const int16_t* samples, uint32_t pairCount, uint32_t sourceRate)
{
	if(pairCount == 0) return;
	if(sourceRate == 0) {
		sourceRate = GetAudioSampleRate();
	}
	// PlayAudioBuffer expects stereo interleaved int16, and sampleCount = pair count.
	_emu->GetSoundMixer()->PlayAudioBuffer(
		const_cast<int16_t*>(samples),
		pairCount,
		sourceRate
	);
}

uint32_t GenesisConsole::GetAudioSampleRate()
{
	uint32_t sampleRate = _emu->GetSettings()->GetAudioConfig().SampleRate;
	_audioSampleRate = sampleRate ? sampleRate : 48000;
	return _audioSampleRate;
}

uint32_t GenesisConsole::GetControllerButtons(int port)
{
	if(!_controlManager) return 0;
	return _controlManager->GetButtonsForPort(port);
}

bool GenesisConsole::IsControllerConnected(int port)
{
	if(!_controlManager) return false;
	return _controlManager->IsPortConnected(port);
}

// ---------------------------------------------------------------------------
// IConsole
// ---------------------------------------------------------------------------

void GenesisConsole::Reset()
{
	_emu->ReloadRom(true);
}

void GenesisConsole::RunFrame()
{
	if(!_backend || !_controlManager) {
		return;
	}

	_controlManager->UpdateControlDevices();
	_controlManager->UpdateInputState();

	_backend->RunFrame();
	RefreshDebuggerMemoryViews();

	// Notify debugger (and Lua event callbacks) that the frame is complete.
	_emu->ProcessEvent(EventType::EndFrame, CpuType::GenesisMain);

	if(!_frameBuffer.empty() && _frameWidth > 0 && _frameHeight > 0) {
		RenderedFrame frame((void*)_frameBuffer.data(), _frameWidth, _frameHeight, 1.0, _backend->GetFrameCount());
		_emu->GetVideoDecoder()->UpdateFrame(frame, false, false);
	}

	_emu->ProcessEndOfFrame();
}

void GenesisConsole::SaveBattery()
{
	if(!_backend) {
		return;
	}

	_backend->SyncSaveData();

	uint32_t saveRamSize = 0;
	const uint8_t* saveRam = _backend->GetMemoryPointer(MemoryType::GenesisSaveRam, saveRamSize);
	if(saveRam && saveRamSize > 0) {
		_emu->GetBatteryManager()->SaveBattery(".sav", const_cast<uint8_t*>(saveRam), saveRamSize);
	}

	uint32_t eepromSize = 0;
	const uint8_t* eeprom = _backend->GetSaveEeprom(eepromSize);
	if(eeprom && eepromSize > 0) {
		_emu->GetBatteryManager()->SaveBattery(".eeprom", const_cast<uint8_t*>(eeprom), eepromSize);
	}
}

BaseControlManager* GenesisConsole::GetControlManager()
{
	return _controlManager.get();
}

ConsoleRegion GenesisConsole::GetRegion()
{
	return _region;
}

ConsoleType GenesisConsole::GetConsoleType()
{
	return ConsoleType::Genesis;
}

vector<CpuType> GenesisConsole::GetCpuTypes()
{
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		return { CpuType::GenesisMain, CpuType::GenesisZ80 };
	}
	return { CpuType::GenesisMain };
}

RomFormat GenesisConsole::GetRomFormat()
{
	return RomFormat::MegaDrive;
}

double GenesisConsole::GetFps()
{
	return _backend ? _backend->GetFps() : 60.0;
}

PpuFrameInfo GenesisConsole::GetPpuFrame()
{
	PpuFrameInfo frame = {};
	frame.FirstScanline = 0;
	frame.FrameCount = _backend ? _backend->GetFrameCount() : 0;
	frame.Width = _frameWidth;
	frame.Height = _frameHeight;
	frame.ScanlineCount = _isPAL ? 313 : 262;
	frame.CycleCount = 3420;  // Master clocks per scanline (approx)
	frame.FrameBufferSize = _frameWidth * _frameHeight * sizeof(uint32_t);
	frame.FrameBuffer = (uint8_t*)_frameBuffer.data();
	return frame;
}

BaseVideoFilter* GenesisConsole::GetVideoFilter(bool getDefaultFilter)
{
	(void)getDefaultFilter;
	return new GenesisDefaultVideoFilter(_emu);
}

uint64_t GenesisConsole::GetMasterClock()
{
	return _backend ? _backend->GetMasterClock() : 0;
}

uint32_t GenesisConsole::GetMasterClockRate()
{
	return _backend ? _backend->GetMasterClockRate() : 53693175;
}

AudioTrackInfo GenesisConsole::GetAudioTrackInfo()
{
	return AudioTrackInfo();
}

void GenesisConsole::ProcessAudioPlayerAction(AudioPlayerActionParams p)
{
	(void)p;
}

AddressInfo GenesisConsole::GetAbsoluteAddress(AddressInfo& relAddress)
{
	// For the Genesis debugger we treat the address spaces simply:
	// RelAddress in GenesisMemory maps 1:1 to the bus address.
	switch(relAddress.Type) {
		case MemoryType::GenesisMemory:
		case MemoryType::GenesisWorkRam:
		case MemoryType::GenesisPrgRom:
		case MemoryType::GenesisSaveRam:
		case MemoryType::GenesisVideoRam:
		case MemoryType::GenesisColorRam:
		case MemoryType::GenesisVScrollRam:
		case MemoryType::GenesisAudioRam:
			return relAddress;
		default:
			return { -1, MemoryType::None };
	}
}

AddressInfo GenesisConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	(void)cpuType;
	switch(absAddress.Type) {
		case MemoryType::GenesisMemory:
		case MemoryType::GenesisWorkRam:
		case MemoryType::GenesisPrgRom:
		case MemoryType::GenesisSaveRam:
		case MemoryType::GenesisVideoRam:
		case MemoryType::GenesisColorRam:
		case MemoryType::GenesisVScrollRam:
		case MemoryType::GenesisAudioRam:
			return absAddress;
		default:
			return { -1, MemoryType::None };
	}
}

GenesisState GenesisConsole::GetState()
{
	GenesisState state;

	if(_backend) {
		_backend->GetCpuState(state.Cpu);
		_backend->GetVdpState(state.Vdp);
	}

	return state;
}

uint8_t GenesisConsole::ReadMemory(MemoryType type, uint32_t address)
{
	return _backend ? _backend->ReadMemory(type, address) : 0;
}

void GenesisConsole::WriteMemory(MemoryType type, uint32_t address, uint8_t value)
{
	if(_backend) {
		_backend->WriteMemory(type, address, value);
	}
}

bool GenesisConsole::SetProgramCounter(uint32_t address)
{
	return _backend ? _backend->SetProgramCounter(address) : false;
}

uint32_t GenesisConsole::GetInstructionSize(uint32_t address)
{
	return _backend ? _backend->GetInstructionSize(address) : 2;
}

const char* GenesisConsole::DisassembleInstruction(uint32_t address)
{
	return _backend ? _backend->DisassembleInstruction(address) : "";
}

void GenesisConsole::GetConsoleState(BaseState& state, ConsoleType consoleType)
{
	(void)consoleType;
	(GenesisState&)state = GetState();
}

SaveStateCompatInfo GenesisConsole::ValidateSaveStateCompatibility(ConsoleType stateConsoleType)
{
	if(stateConsoleType != ConsoleType::Genesis) {
		return {};  // cross-console load: incompatible
	}

	// Same-console load: report compatible here; Serialize() enforces
	// the native state format marker and rejects mismatches with SetErrorFlag.
	return { true, "", "" };
}

void GenesisConsole::Serialize(Serializer& s)
{
	if(!_backend) {
		return;
	}

	vector<uint8_t> stateVec;

	if(_backend->GetCoreType() == GenesisCoreType::Native) {
		uint32_t stateFormatMarker = s.IsSaving() ? NativeStateFormatMarker : 0;
		SV(stateFormatMarker);
		if(!s.IsSaving() && stateFormatMarker != NativeStateFormatMarker) {
			// Reject legacy Genesis states that predate the native format marker.
			s.SetErrorFlag();
			return;
		}
	}

	if(s.IsSaving()) {
		if(!_backend->SaveState(stateVec)) {
			stateVec.clear();
		}
		SVVector(stateVec);
	} else {
		SVVector(stateVec);
		if(!stateVec.empty() && !_backend->LoadState(stateVec)) {
			s.SetErrorFlag();
		}
	}
}

GenesisZ80State GenesisConsole::GetZ80DebugState()
{
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		auto* nb = static_cast<GenesisNativeBackend*>(_backend.get());
		return nb->GetZ80DebugState();
	}
	return {};
}

void GenesisConsole::SetZ80ProgramCounter(uint16_t addr)
{
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		auto* nb = static_cast<GenesisNativeBackend*>(_backend.get());
		nb->SetZ80ProgramCounter(addr);
	}
}

uint8_t GenesisConsole::GetVdpRegister(uint8_t index) const
{
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		auto* nb = static_cast<GenesisNativeBackend*>(_backend.get());
		return nb->GetVdpRegister(index);
	}

	return 0;
}

uint16_t GenesisConsole::GetHVCounter() const
{
	if(_backend && _backend->GetCoreType() == GenesisCoreType::Native) {
		auto* nb = static_cast<GenesisNativeBackend*>(_backend.get());
		return nb->GetHVCounter();
	}

	return 0;
}

void GenesisConsole::GetVdpRegisters(uint8_t regs[24]) const
{
	if(_backend) {
		_backend->GetVdpRegisters(regs);
	} else {
		memset(regs, 0, 24);
	}
}

bool GenesisConsole::GetVdpDebugState(GenesisVdpDebugState& state) const
{
	if(_backend) {
		return _backend->GetVdpDebugState(state);
	}

	state = {};
	return false;
}

bool GenesisConsole::GetVdpTraceLines(GenesisTraceBufferKind kind, vector<string>& lines) const
{
	if(_backend) {
		return _backend->GetVdpTraceLines(kind, lines);
	}

	lines.clear();
	return false;
}

bool GenesisConsole::GetBackendDebugState(GenesisBackendState& state) const
{
	if(_backend) {
		return _backend->GetBackendDebugState(state);
	}

	state = {};
	return false;
}

ShortcutState GenesisConsole::IsShortcutAllowed(EmulatorShortcut shortcut, uint32_t shortcutParam)
{
	(void)shortcut;
	(void)shortcutParam;
	return ShortcutState::Default;
}
