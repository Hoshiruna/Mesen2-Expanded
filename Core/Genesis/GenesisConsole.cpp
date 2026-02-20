#include "pch.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/GenesisDefaultVideoFilter.h"
#include "Shared/BatteryManager.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/RenderedFrame.h"
#include "Utilities/Serializer.h"
#include "Utilities/StringUtilities.h"
#include <cctype>

GenesisConsole* GenesisConsole::_activeConsole = nullptr;

namespace
{
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
	if(_impl) {
		GenesisAresDestroy(_impl);
		_impl = nullptr;
	}
	if(_activeConsole == this) {
		_activeConsole = nullptr;
	}
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
	if(!_impl) {
		return;
	}

	uint32_t size = 0;
	const uint8_t* ptr = nullptr;

	ptr = GenesisAresGetVRam(_impl, &size);
	_emu->RegisterMemory(MemoryType::GenesisVideoRam, const_cast<uint8_t*>(ptr), ptr ? size : 0);

	ptr = GenesisAresGetCRam(_impl, &size);
	_emu->RegisterMemory(MemoryType::GenesisColorRam, const_cast<uint8_t*>(ptr), ptr ? size : 0);

	ptr = GenesisAresGetVSRam(_impl, &size);
	_emu->RegisterMemory(MemoryType::GenesisVScrollRam, const_cast<uint8_t*>(ptr), ptr ? size : 0);

	ptr = GenesisAresGetSaveRam(_impl, &size);
	_emu->RegisterMemory(MemoryType::GenesisSaveRam, const_cast<uint8_t*>(ptr), ptr ? size : 0);
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

	// Create the Ares impl if not yet created
	if(!_impl) {
		_impl = GenesisAresCreate(this);
	}

	// Create control manager
	_controlManager.reset(new GenesisControlManager(_emu, this));

	vector<uint8_t> saveRamData = _emu->GetBatteryManager()->LoadBattery(".sav");
	vector<uint8_t> saveEepromData = _emu->GetBatteryManager()->LoadBattery(".eeprom");

	// Load into Ares
	if(!GenesisAresLoadRom(
		_impl, romData.data(), (uint32_t)romData.size(), regionStr,
		saveRamData.empty() ? nullptr : saveRamData.data(), (uint32_t)saveRamData.size(),
		saveEepromData.empty() ? nullptr : saveEepromData.data(), (uint32_t)saveEepromData.size()
	)) {
		return LoadRomResult::Failure;
	}

	_isPAL = GenesisAresIsPAL(_impl);

	// Allocate initial frame buffer
	_frameBuffer.resize(512 * 240, 0xFF000000);

	// Register physical memory regions for the debugger
	uint32_t romSize = 0;
	const uint8_t* romPtr = GenesisAresGetRom(_impl, &romSize);
	if(romPtr && romSize > 0) {
		_emu->RegisterMemory(MemoryType::GenesisPrgRom, const_cast<uint8_t*>(romPtr), romSize);
	}

	uint32_t wramSize = 0;
	const uint8_t* wramPtr = GenesisAresGetWorkRam(_impl, &wramSize);
	if(wramPtr && wramSize > 0) {
		_emu->RegisterMemory(MemoryType::GenesisWorkRam, const_cast<uint8_t*>(wramPtr), wramSize);
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
	if(_impl) {
		uint32_t coreWidth = 0;
		uint32_t coreHeight = 0;
		GenesisAresGetFrameSize(_impl, &coreWidth, &coreHeight);
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

	_frameWidth  = targetWidth;
	_frameHeight = targetHeight;
	_frameCount++;

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
	return _controlManager->GetButtonsForAres(port);
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
	_controlManager->UpdateControlDevices();
	_controlManager->UpdateInputState();

	GenesisAresRunFrame(_impl);
	RefreshDebuggerMemoryViews();

	if(!_frameBuffer.empty() && _frameWidth > 0 && _frameHeight > 0) {
		RenderedFrame frame((void*)_frameBuffer.data(), _frameWidth, _frameHeight, 1.0, _frameCount);
		_emu->GetVideoDecoder()->UpdateFrame(frame, false, false);
	}

	_emu->ProcessEndOfFrame();
}

void GenesisConsole::SaveBattery()
{
	if(!_impl) {
		return;
	}

	GenesisAresSyncSaveData(_impl);

	uint32_t saveRamSize = 0;
	const uint8_t* saveRam = GenesisAresGetSaveRam(_impl, &saveRamSize);
	if(saveRam && saveRamSize > 0) {
		_emu->GetBatteryManager()->SaveBattery(".sav", const_cast<uint8_t*>(saveRam), saveRamSize);
	}

	uint32_t eepromSize = 0;
	const uint8_t* eeprom = GenesisAresGetSaveEeprom(_impl, &eepromSize);
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
	return { CpuType::GenesisMain };
}

RomFormat GenesisConsole::GetRomFormat()
{
	return RomFormat::MegaDrive;
}

double GenesisConsole::GetFps()
{
	if(!_impl) return 60.0;
	return GenesisAresGetFps(_impl);
}

PpuFrameInfo GenesisConsole::GetPpuFrame()
{
	PpuFrameInfo frame = {};
	frame.FirstScanline = 0;
	frame.FrameCount    = _frameCount;
	frame.Width         = _frameWidth;
	frame.Height        = _frameHeight;
	frame.ScanlineCount = _isPAL ? 313 : 262;
	frame.CycleCount    = 3420;  // Master clocks per scanline (approx)
	frame.FrameBufferSize = _frameWidth * _frameHeight * sizeof(uint32_t);
	frame.FrameBuffer   = (uint8_t*)_frameBuffer.data();
	return frame;
}

BaseVideoFilter* GenesisConsole::GetVideoFilter(bool getDefaultFilter)
{
	return new GenesisDefaultVideoFilter(_emu);
}

uint64_t GenesisConsole::GetMasterClock()
{
	if(!_impl) return 0;
	return GenesisAresGetMasterClock(_impl);
}

uint32_t GenesisConsole::GetMasterClockRate()
{
	if(!_impl) return 53693175;
	return GenesisAresGetMasterClockRate(_impl);
}

AudioTrackInfo GenesisConsole::GetAudioTrackInfo()
{
	return AudioTrackInfo();
}

void GenesisConsole::ProcessAudioPlayerAction(AudioPlayerActionParams p)
{
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
			return relAddress;
		default:
			return { -1, MemoryType::None };
	}
}

AddressInfo GenesisConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	switch(absAddress.Type) {
		case MemoryType::GenesisMemory:
		case MemoryType::GenesisWorkRam:
		case MemoryType::GenesisPrgRom:
		case MemoryType::GenesisSaveRam:
		case MemoryType::GenesisVideoRam:
		case MemoryType::GenesisColorRam:
		case MemoryType::GenesisVScrollRam:
			return absAddress;
		default:
			return { -1, MemoryType::None };
	}
}

GenesisState GenesisConsole::GetState()
{
	GenesisState state;

	if(_impl) {
		uint32_t d[8], a[8];
		uint16_t sr;
		GenesisAresGetCpuState(_impl,
			&state.Cpu.PC, &state.Cpu.SP,
			d, a, &sr, &state.Cpu.CycleCount);
		for(int i = 0; i < 8; i++) {
			state.Cpu.D[i] = d[i];
			state.Cpu.A[i] = a[i];
		}
		state.Cpu.SR = sr;

		uint32_t w, h;
		GenesisAresGetFrameSize(_impl, &w, &h);
		state.Vdp.Width      = (uint16_t)w;
		state.Vdp.Height     = (uint16_t)h;
		state.Vdp.FrameCount = _frameCount;
		state.Vdp.PAL        = _isPAL;
	}

	return state;
}

void GenesisConsole::GetConsoleState(BaseState& state, ConsoleType consoleType)
{
	(GenesisState&)state = GetState();
}

void GenesisConsole::Serialize(Serializer& s)
{
	if(!_impl) return;

	if(s.IsSaving()) {
		uint32_t stateSize = 0;
		uint8_t* stateData = GenesisAresSaveState(_impl, &stateSize);
		vector<uint8_t> stateVec;
		if(stateData && stateSize > 0) {
			stateVec.assign(stateData, stateData + stateSize);
			GenesisAresFreeStateData(stateData);
		}
		SVVector(stateVec);
	} else {
		vector<uint8_t> stateVec;
		SVVector(stateVec);
		if(!stateVec.empty()) {
			GenesisAresLoadState(_impl, stateVec.data(), (uint32_t)stateVec.size());
		}
	}
}
