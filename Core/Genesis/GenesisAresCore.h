#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// Abstract callbacks from the Ares core back into Mesen2.
// This class must NOT include any ares/nall headers.
// ---------------------------------------------------------------------------
class IGenesisPlatformCallbacks
{
public:
	virtual ~IGenesisPlatformCallbacks() = default;

	// Called once per rendered frame with an ARGB8888 pixel buffer.
	virtual void OnVideoFrame(const uint32_t* pixels, uint32_t pitch,
	                          uint32_t width, uint32_t height) = 0;

	// Called repeatedly during a frame as audio streams produce samples.
	// samples[] is interleaved stereo, int16 range [-32768,32767].
	// sourceRate is the stream mix sample rate.
	virtual void OnAudioSamples(const int16_t* samples, uint32_t pairCount, uint32_t sourceRate) = 0;

	// Preferred output sample rate for stream resampling/mixing.
	virtual uint32_t GetAudioSampleRate() = 0;

	// Called to query current button state for a controller port (0 or 1).
	// Returns a bitmask using GenesisButton flags.
	virtual uint32_t GetControllerButtons(int port) = 0;
};

// ---------------------------------------------------------------------------
// Opaque implementation handle
// ---------------------------------------------------------------------------
struct GenesisAresImpl;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
GenesisAresImpl* GenesisAresCreate(IGenesisPlatformCallbacks* callbacks);
void             GenesisAresDestroy(GenesisAresImpl* impl);

// ---------------------------------------------------------------------------
// ROM loading
// Load a raw ROM image (no header stripping performed here).
// region: "NTSC-J", "NTSC-U", or "PAL"
// saveRam/saveEeprom blobs can be null/0.
// Returns true on success.
// ---------------------------------------------------------------------------
bool GenesisAresLoadRom(GenesisAresImpl* impl,
                          const uint8_t* romData, uint32_t romSize,
                          const char* region,
                          const uint8_t* saveRamData, uint32_t saveRamSize,
                          const uint8_t* saveEepromData, uint32_t saveEepromSize);

// Unload the current ROM (called before loading a new one or on shutdown).
void GenesisAresUnload(GenesisAresImpl* impl);

// ---------------------------------------------------------------------------
// Emulation
// Run until exactly one video frame has been produced (VDP vblank).
// ---------------------------------------------------------------------------
void GenesisAresRunFrame(GenesisAresImpl* impl);

// Hard reset (equivalent to power cycle).
void GenesisAresPower(GenesisAresImpl* impl, bool reset);

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------
double   GenesisAresGetFps(GenesisAresImpl* impl);
bool     GenesisAresIsPAL(GenesisAresImpl* impl);
uint64_t GenesisAresGetMasterClock(GenesisAresImpl* impl);
uint32_t GenesisAresGetMasterClockRate(GenesisAresImpl* impl);

// Fill out CPU state (M68000).
void GenesisAresGetCpuState(GenesisAresImpl* impl,
                              uint32_t* pc, uint32_t* sp,
                              uint32_t d[8], uint32_t a[8],
                              uint16_t* sr, uint64_t* cycles);

// Current VDP frame counter (increments every vblank).
uint32_t GenesisAresGetFrameCount(GenesisAresImpl* impl);

// Frame dimensions (may change between frames if mode switch occurred).
void GenesisAresGetFrameSize(GenesisAresImpl* impl,
                              uint32_t* width, uint32_t* height);

// ---------------------------------------------------------------------------
// Memory access (for debugger)
// ---------------------------------------------------------------------------
uint8_t GenesisAresReadMemory(GenesisAresImpl* impl, uint32_t address);
void    GenesisAresWriteMemory(GenesisAresImpl* impl, uint32_t address, uint8_t value);

// Return pointer + size for various memory regions (VRAM, CRAM, WorkRAM, ROM).
// Pointers remain valid until the next GenesisAresRunFrame call.
const uint8_t* GenesisAresGetVRam(GenesisAresImpl* impl, uint32_t* size);
const uint8_t* GenesisAresGetCRam(GenesisAresImpl* impl, uint32_t* size);
const uint8_t* GenesisAresGetVSRam(GenesisAresImpl* impl, uint32_t* size);
const uint8_t* GenesisAresGetWorkRam(GenesisAresImpl* impl, uint32_t* size);
const uint8_t* GenesisAresGetRom(GenesisAresImpl* impl, uint32_t* size);
const uint8_t* GenesisAresGetSaveRam(GenesisAresImpl* impl, uint32_t* size);
const uint8_t* GenesisAresGetSaveEeprom(GenesisAresImpl* impl, uint32_t* size);
void           GenesisAresSyncSaveData(GenesisAresImpl* impl);

uint8_t GenesisAresReadVRam(GenesisAresImpl* impl, uint32_t address);
uint8_t GenesisAresReadCRam(GenesisAresImpl* impl, uint32_t address);
uint8_t GenesisAresReadVSRam(GenesisAresImpl* impl, uint32_t address);
void    GenesisAresWriteVRam(GenesisAresImpl* impl, uint32_t address, uint8_t value);
void    GenesisAresWriteCRam(GenesisAresImpl* impl, uint32_t address, uint8_t value);
void    GenesisAresWriteVSRam(GenesisAresImpl* impl, uint32_t address, uint8_t value);

bool        GenesisAresSetProgramCounter(GenesisAresImpl* impl, uint32_t address);
uint32_t    GenesisAresGetInstructionSize(GenesisAresImpl* impl, uint32_t address);
const char* GenesisAresDisassembleInstruction(GenesisAresImpl* impl, uint32_t address);

// ---------------------------------------------------------------------------
// Save state (via Ares serializer)
// Returns serialized bytes; caller frees with GenesisAresFreeStateData.
// ---------------------------------------------------------------------------
uint8_t* GenesisAresSaveState(GenesisAresImpl* impl, uint32_t* size);
bool     GenesisAresLoadState(GenesisAresImpl* impl, const uint8_t* data, uint32_t size);
void     GenesisAresFreeStateData(uint8_t* data);
