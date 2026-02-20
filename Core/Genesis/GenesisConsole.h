#pragma once
#include "pch.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/SettingTypes.h"
#include "Genesis/GenesisTypes.h"
#include "Genesis/GenesisAresCore.h"

class Emulator;
class VirtualFile;
class BaseControlManager;
class GenesisControlManager;
class BaseVideoFilter;

// ---------------------------------------------------------------------------
// GenesisConsole
//
// Wraps the Ares Mega Drive core via the GenesisAresCore.h bridge API.
// This class never includes any ares/nall headers.
// ---------------------------------------------------------------------------
class GenesisConsole final : public IConsole, public IGenesisPlatformCallbacks
{
private:
	static GenesisConsole* _activeConsole;

	Emulator*                    _emu = nullptr;
	GenesisAresImpl*             _impl = nullptr;
	unique_ptr<GenesisControlManager> _controlManager;

	// Last rendered frame (ARGB8888)
	vector<uint32_t>             _frameBuffer;
	uint32_t                     _frameWidth  = 320;
	uint32_t                     _frameHeight = 224;
	uint32_t                     _frameCount  = 0;

	ConsoleRegion                _region = ConsoleRegion::Ntsc;
	bool                         _isPAL  = false;

	// Audio sample rate requested from Mesen audio settings.
	uint32_t                     _audioSampleRate = 48000;

	void DetermineRegion(const string& filename, const vector<uint8_t>& romData);
	void RefreshDebuggerMemoryViews();

	// IGenesisPlatformCallbacks -----------------------------------------------
	void OnVideoFrame(const uint32_t* pixels, uint32_t pitch,
	                  uint32_t width, uint32_t height) override;
	void OnAudioSamples(const int16_t* samples, uint32_t pairCount, uint32_t sourceRate) override;
	uint32_t GetAudioSampleRate() override;
	uint32_t GetControllerButtons(int port) override;

public:
	static vector<string> GetSupportedExtensions() { return { ".md", ".bin", ".gen", ".smd" }; }
	static vector<string> GetSupportedSignatures() { return {}; }

	GenesisConsole(Emulator* emu);
	~GenesisConsole();
	static GenesisConsole* GetActiveConsole() { return _activeConsole; }

	// IConsole ----------------------------------------------------------------
	LoadRomResult LoadRom(VirtualFile& romFile) override;
	void Reset() override;
	void RunFrame() override;
	void SaveBattery() override;

	BaseControlManager* GetControlManager() override;
	ConsoleRegion GetRegion() override;
	ConsoleType GetConsoleType() override;
	vector<CpuType> GetCpuTypes() override;
	RomFormat GetRomFormat() override;

	double GetFps() override;
	PpuFrameInfo GetPpuFrame() override;
	BaseVideoFilter* GetVideoFilter(bool getDefaultFilter) override;

	uint64_t GetMasterClock() override;
	uint32_t GetMasterClockRate() override;

	AudioTrackInfo GetAudioTrackInfo() override;
	void ProcessAudioPlayerAction(AudioPlayerActionParams p) override;

	AddressInfo GetAbsoluteAddress(AddressInfo& relAddress) override;
	AddressInfo GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType) override;
	void GetConsoleState(BaseState& state, ConsoleType consoleType) override;

	void Serialize(Serializer& s) override;

	// Accessor for debugger ---------------------------------------------------
	GenesisAresImpl* GetAresImpl() { return _impl; }
	GenesisState GetState();
};
