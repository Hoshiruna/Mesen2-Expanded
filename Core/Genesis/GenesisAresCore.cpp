// =============================================================================
// GenesisAresCore.cpp
//
// Isolated translation unit that compiles the Ares Mega Drive emulation core.
// This file MUST NOT include any Mesen2 headers (pch.h etc.) because ares/nall
// define `using namespace nall` and `using namespace nall::primitives` at
// global scope, which would collide with Mesen2's std:: aliases.
//
// The bridge API declared in GenesisAresCore.h uses only plain C types so that
// GenesisConsole.cpp can call it without ever seeing nall.
// =============================================================================

// ---------------------------------------------------------------------------
// Include path setup note:
//   Additional include directories required for this TU (set in vcxproj):
//     $(ProjectDir)\Genesis\Ares             -- for <libco/libco.h>
//     $(ProjectDir)\Genesis\Ares\ares        -- for <ares/ares.hpp>, <md/md.hpp>
//     $(ProjectDir)\Genesis\Ares\nall        -- for <nall/platform.hpp> etc.
//     $(ProjectDir)\Genesis\Ares\thirdparty  -- for <sljit.h>
// ---------------------------------------------------------------------------

// Disable precompiled headers for this file (it cannot use Mesen2's pch.h).
// In MSVC this is set per-file in the .vcxproj.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Ares infrastructure
// ---------------------------------------------------------------------------
#include <ares/ares.hpp>

// Provide the ares global variables (replaces the generated ares.cpp)
namespace ares {
	Platform*         platform        = nullptr;
	std::atomic<bool> _runAhead       = false;
	const string      Name            = "Ares";
	const string      Version         = "135";
	const string      Copyright       = "ares authors";
	const string      License         = "ISC";
	const string      LicenseURI      = "https://opensource.org/licenses/ISC";
	const string      Website         = "ares-emu.net";
	const string      WebsiteURI      = "https://ares-emu.net/";
	const u32         SerializerSignature = 0x31545342; // "BST1" LE
}

// Node system implementation
#include <ares/node/node.cpp>

// Debug infrastructure
#include <ares/debug/debug.cpp>

// Fixed-allocator non-inline implementations + static buffer
#include <ares/memory/fixed-allocator.cpp>

// Note: scheduler (thread.cpp / scheduler.cpp) is already included inside
// namespace ares::MegaDrive via md/md.hpp -> ares/inline.hpp. Do NOT include
// it here at the top level to avoid ODR violations.

// Component processor/audio implementations are compiled as SEPARATE TUs
// in Core.vcxproj (see ClCompile entries for Genesis\Ares component cpps).
// They cannot be included here because:
//   - sh2.cpp uses '#define SP/PC/...' macros that pollute the preprocessor
//   - ssp1601.cpp similarly uses '#define AL/AH/...' macros
//   - Each component .cpp starts with '#include <ares/ares.hpp>' and should
//     be compiled in isolation for correctness.

// Enable QOI2 and QON implementations in this TU.
// md/md.hpp includes <qon/qon.h> and <qon/qoi2.h>; the macros must be
// defined BEFORE that include so the implementation is compiled here.
#define QOI2_IMPLEMENTATION
#define QON_IMPLEMENTATION

// ---------------------------------------------------------------------------
// Mega Drive core (unity build)
// ---------------------------------------------------------------------------
#include <md/md.hpp>
#include <md/md.cpp>

// ---------------------------------------------------------------------------
// Bridge implementation
// ---------------------------------------------------------------------------
#include "GenesisAresCore.h"

// Forward declarations for callbacks interface (no nall types exposed)
// The implementation must not include GenesisAresCore.h again from ares side.

// ============================================================
// Mesen2 Platform implementation
// ============================================================

// We need to collect audio samples and video frames.
// The platform is instantiated per-GenesisAresImpl.

struct MesenAresPlatform : ares::Platform
{
	IGenesisPlatformCallbacks* callbacks = nullptr;

	// Nodes we track for input
	// We store input buttons by (port, name) for controller lookup.
	// Since ares input is polled per-button, we keep a flat map.
	// Format: "Controller Port 1:Control Pad:Up" etc.
	struct InputEntry {
		ares::Node::Input::Button node;
		int    port;   // 0 or 1
		uint32_t mask; // GenesisButton bitmask
	};
	std::vector<InputEntry> _inputs;

	// Audio buffering
	// Keep at least one full frame even at high output rates to avoid
	// splitting a single emulated frame across multiple mixer frame commits.
	static constexpr int kAudioBufPairs = 8192;
	int16_t _audioBuf[kAudioBufPairs * 2] = {};
	int     _audioBufPos = 0;
	std::vector<ares::Node::Audio::Stream> _audioStreams;
	uint32_t _audioSampleRate = 48000;

	// Frame ready flag
	bool _frameReady = false;

	// --------------------------------------------------------
	// ares::Platform overrides
	// --------------------------------------------------------

	auto attach(ares::Node::Object node) -> void override
	{
		// When a button node is attached, register it if it's an input button
		if(auto btn = std::dynamic_pointer_cast<ares::Core::Input::Button>(node)) {
			registerInputButton(btn);
		}

		if(auto stream = node->cast<ares::Node::Audio::Stream>()) {
			(void)stream;
			rebuildAudioStreams(node);
			refreshAudioRate();
			for(auto& s : _audioStreams) {
				if(s) {
					s->setResamplerFrequency((double)_audioSampleRate);
				}
			}
		}
	}

	auto detach(ares::Node::Object node) -> void override
	{
		if(auto btn = std::dynamic_pointer_cast<ares::Core::Input::Button>(node)) {
			// Remove from _inputs
			_inputs.erase(
				std::remove_if(_inputs.begin(), _inputs.end(),
					[&](const InputEntry& e){ return e.node == btn; }),
				_inputs.end()
			);
		}

		if(auto stream = node->cast<ares::Node::Audio::Stream>()) {
			stream->setResamplerFrequency((double)_audioSampleRate);
			rebuildAudioStreams(node);
		}
	}

	auto pak(ares::Node::Object node) -> std::shared_ptr<nall::vfs::directory> override
	{
		// Return the cart pak for all nodes — the system node pak() is used to
		// locate TMSS ROM (which won't be found, disabling TMSS gracefully).
		// The cartridge peripheral pak() is used to get "program.rom".
		// Both are served from _cartPak (program ROM + optional save files).
		if(_cartPak) return _cartPak;
		return std::make_shared<nall::vfs::directory>();
	}

	auto event(ares::Event evt) -> void override
	{
		if(evt == ares::Event::Frame) {
			_frameReady = true;
		}
	}

	auto video(ares::Node::Video::Screen screen,
	           const uint32_t* data, uint32_t pitch,
	           uint32_t width, uint32_t height) -> void override
	{
		if(callbacks) {
			callbacks->OnVideoFrame(data, pitch, width, height);
		}
		_frameReady = true;
	}

	auto audio(ares::Node::Audio::Stream stream) -> void override
	{
		(void)stream;
		if(!callbacks) return;
		if(_audioStreams.empty()) {
			rebuildAudioStreams();
		}
		_audioStreams.erase(
			std::remove_if(_audioStreams.begin(), _audioStreams.end(),
				[](const ares::Node::Audio::Stream& s){ return !s; }),
			_audioStreams.end()
		);
		if(_audioStreams.empty()) return;
		refreshAudioRate();

		// Match upstream Ares frontend behavior:
		// only mix/output when every active stream has one pending frame.
		while(true) {
			for(auto& s : _audioStreams) {
				if(!s || !s->pending()) return;
			}

			double mixed[2] = { 0.0, 0.0 };
			for(auto& s : _audioStreams) {
				double frame[8] = {};
				uint32_t channels = s->read(frame);
				if(channels <= 1) {
					mixed[0] += frame[0];
					mixed[1] += frame[0];
				} else {
					mixed[0] += frame[0];
					mixed[1] += frame[1];
				}
			}

			if(mixed[0] > 1.0) mixed[0] = 1.0;
			if(mixed[0] < -1.0) mixed[0] = -1.0;
			if(mixed[1] > 1.0) mixed[1] = 1.0;
			if(mixed[1] < -1.0) mixed[1] = -1.0;

			if(_audioBufPos + 2 > kAudioBufPairs * 2) {
				flushAudio();
			}

			if(_audioBufPos + 2 <= kAudioBufPairs * 2) {
				_audioBuf[_audioBufPos++] = (int16_t)(mixed[0] * 32767.0);
				_audioBuf[_audioBufPos++] = (int16_t)(mixed[1] * 32767.0);
			}
		}
	}

	void flushAudio()
	{
		if(_audioBufPos > 0 && callbacks) {
			callbacks->OnAudioSamples(_audioBuf, _audioBufPos / 2, _audioSampleRate);
			_audioBufPos = 0;
		}
	}

	auto input(ares::Node::Input::Input node) -> void override
	{
		if(!callbacks) return;

		// Find this node in our input map.
		// entry.node is shared_ptr<Button>, node is shared_ptr<Input>.
		// Compare raw pointers (Button* implicitly converts to Input*).
		for(auto& entry : _inputs) {
			if(entry.node.get() == node.get()) {
				uint32_t buttons = callbacks->GetControllerButtons(entry.port);
				entry.node->setValue((buttons & entry.mask) != 0);
				return;
			}
		}
	}

	// Cart pak — set before load() so pak() callbacks can return it
	std::shared_ptr<nall::vfs::directory> _cartPak;
	std::shared_ptr<nall::vfs::memory> _saveRamFile;
	std::shared_ptr<nall::vfs::memory> _saveEepromFile;

private:
	void rebuildAudioStreams(ares::Node::Object node = {})
	{
		ares::Node::Object root = node;
		if(!root) {
			root = ares::MegaDrive::system.node;
		}
		while(root) {
			ares::Node::Object parent = root->parent().lock();
			if(!parent) {
				break;
			}
			root = parent;
		}

		if(root) {
			_audioStreams = root->find<ares::Node::Audio::Stream>();
		} else {
			_audioStreams.clear();
		}
	}

	void refreshAudioRate()
	{
		uint32_t targetRate = callbacks ? callbacks->GetAudioSampleRate() : 0;
		if(targetRate == 0) {
			targetRate = 48000;
		}

		if(_audioSampleRate != targetRate) {
			_audioSampleRate = targetRate;
			for(auto& s : _audioStreams) {
				if(s) {
					s->setResamplerFrequency((double)_audioSampleRate);
				}
			}
		}
	}

	// Figure out which port/button a newly attached button belongs to
	void registerInputButton(ares::Node::Input::Button btn)
	{
		// Walk the parent chain to identify port and button name.
		// Expected tree: System -> "Controller Port N" -> "Control Pad" -> "ButtonName"
		// The node name is the button name (Up, Down, A, B, etc.)

		string btnName = btn->name();

		// Find port by walking ancestors.
		// Object::parent() returns weak_ptr<Object>; must .lock() before use.
		int port = -1;
		ares::Node::Object parent = btn->parent().lock();
		while(parent) {
			string pname = parent->name();
			if(pname == "Controller Port 1") { port = 0; break; }
			if(pname == "Controller Port 2") { port = 1; break; }
			parent = parent->parent().lock();
		}
		if(port < 0) return;

		uint32_t mask = 0;
		if(btnName == "Up"   ) mask = 0x0001;
		else if(btnName == "Down" ) mask = 0x0002;
		else if(btnName == "Left" ) mask = 0x0004;
		else if(btnName == "Right") mask = 0x0008;
		else if(btnName == "A"    ) mask = 0x0010;
		else if(btnName == "B"    ) mask = 0x0020;
		else if(btnName == "C"    ) mask = 0x0040;
		else if(btnName == "Start") mask = 0x0080;
		else if(btnName == "X"    ) mask = 0x0100;
		else if(btnName == "Y"    ) mask = 0x0200;
		else if(btnName == "Z"    ) mask = 0x0400;
		else if(btnName == "Mode" ) mask = 0x0800;
		else return;  // unknown button

		_inputs.push_back({ btn, port, mask });
	}
};

// ============================================================
// GenesisAresImpl
// ============================================================

struct GenesisAresImpl
{
	MesenAresPlatform platform;

	ares::Node::System systemNode;

	// Keep a copy of ROM data for state-restore on power cycles
	std::vector<uint8_t> romData;

	// Current frame count (from VDP)
	uint32_t lastFrameCount = 0;

	// Track loaded state
	bool isLoaded = false;
	string regionName;  // "NTSC-J", "NTSC-U", "PAL"
	std::vector<uint8_t> vramView;
	std::vector<uint8_t> cramView;
	std::vector<uint8_t> vsramView;

	GenesisAresImpl(IGenesisPlatformCallbacks* cb)
	{
		platform.callbacks = cb;
	}
};

// ============================================================
// Helper: build the cart VFS pak from ROM + battery data
// ============================================================

struct SaveRamInfo
{
	uint32_t address = 0;
	uint32_t fileSize = 0;
	string mode;
};

struct CartPakBuildResult
{
	std::shared_ptr<nall::vfs::directory> pak;
	std::shared_ptr<nall::vfs::memory> saveRamFile;
	std::shared_ptr<nall::vfs::memory> saveEepromFile;
};

static uint32_t readBe32(const uint8_t* p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static bool detectSaveRam(const uint8_t* romData, uint32_t romSize, SaveRamInfo& info)
{
	// ROM header SRAM metadata:
	// 0x1B0..0x1B1 = "RA", 0x1B4..0x1B7 = start, 0x1B8..0x1BB = end.
	if(!romData || romSize < 0x1BC) {
		return false;
	}

	if(romData[0x1B0] != 'R' || romData[0x1B1] != 'A') {
		return false;
	}

	uint32_t start = readBe32(romData + 0x1B4);
	uint32_t end = readBe32(romData + 0x1B8);
	if(end < start) {
		return false;
	}

	uint32_t range = end - start + 1;
	if(range < 2 || range > 4 * 1024 * 1024) {
		return false;
	}

	info.address = start;
	info.fileSize = (range + 1) / 2;  // one byte stored for each 16-bit bus slot

	if((start & 1) && (end & 1)) {
		info.mode = "lower";
	} else if(!(start & 1) && !(end & 1)) {
		info.mode = "upper";
	} else {
		// Mixed parity mappings are uncommon; default to lower-byte behavior.
		info.mode = "lower";
	}

	return info.fileSize > 0;
}

static std::shared_ptr<nall::vfs::memory> createSaveFile(
	const uint8_t* data, uint32_t dataSize, uint32_t targetSize)
{
	uint32_t allocSize = targetSize ? targetSize : dataSize;
	if(allocSize == 0) {
		return {};
	}

	auto file = nall::vfs::memory::create(allocSize);
	if(data && dataSize > 0) {
		uint32_t copySize = std::min(dataSize, allocSize);
		memcpy(file->data(), data, copySize);
	}
	return file;
}

static CartPakBuildResult buildCartPak(
	const uint8_t* romData, uint32_t romSize,
	const string& region,
	const uint8_t* saveRamData, uint32_t saveRamSize,
	const uint8_t* saveEepromData, uint32_t saveEepromSize)
{
	CartPakBuildResult result = {};
	result.pak = std::make_shared<nall::vfs::directory>();

	// The board's load() function opens "program.rom" to get the ROM image.
	auto romFile = nall::vfs::memory::open({romData, romSize});
	result.pak->append("program.rom", romFile);

	// Set cart attributes
	result.pak->setAttribute("title", "Genesis Game");
	result.pak->setAttribute("region", region);
	result.pak->setAttribute("bootable", "true");

	SaveRamInfo saveInfo = {};
	bool hasSaveHeader = detectSaveRam(romData, romSize, saveInfo);
	if(hasSaveHeader || saveRamSize > 0) {
		uint32_t fileSize = hasSaveHeader ? saveInfo.fileSize : saveRamSize;
		auto saveRamFile = createSaveFile(saveRamData, saveRamSize, fileSize);
		if(saveRamFile) {
			if(hasSaveHeader) {
				saveRamFile->setAttribute("address", string{saveInfo.address});
				saveRamFile->setAttribute("mode", saveInfo.mode);
			} else {
				// Fallback for manual battery blobs without header metadata.
				saveRamFile->setAttribute("address", "2097153");  // 0x200001
				saveRamFile->setAttribute("mode", "lower");
			}
			saveRamFile->setAttribute("enable", "true");
			result.pak->append("save.ram", saveRamFile);
			result.saveRamFile = saveRamFile;
		}
	}

	// EEPROM mapper parameters vary per cart and normally come from manifests.
	// If we have prior EEPROM bytes, expose a conservative default mapping.
	if(saveEepromSize > 0) {
		auto saveEepromFile = createSaveFile(saveEepromData, saveEepromSize, saveEepromSize);
		if(saveEepromFile) {
			saveEepromFile->setAttribute("address", "2097152");  // 0x200000
			saveEepromFile->setAttribute("mode", "M24C02");
			saveEepromFile->setAttribute("rsda", "0");
			saveEepromFile->setAttribute("wsda", "0");
			saveEepromFile->setAttribute("wscl", "1");
			result.pak->append("save.eeprom", saveEepromFile);
			result.saveEepromFile = saveEepromFile;
		}
	}

	return result;
}

static ares::Node::Debugger::Memory getVramNode()
{
	return ares::MegaDrive::vdp.debugger.memory.vram;
}

static ares::Node::Debugger::Memory getCramNode()
{
	return ares::MegaDrive::vdp.debugger.memory.cram;
}

static ares::Node::Debugger::Memory getVsramNode()
{
	return ares::MegaDrive::vdp.debugger.memory.vsram;
}

static const uint8_t* snapshotDebuggerMemory(GenesisAresImpl* impl, ares::Node::Debugger::Memory node, std::vector<uint8_t>& outBuffer, uint32_t* size)
{
	if(!impl || !impl->isLoaded || !node) {
		if(size) *size = 0;
		return nullptr;
	}

	uint32_t memSize = node->size();
	if(memSize == 0) {
		if(size) *size = 0;
		return nullptr;
	}

	if(outBuffer.size() != memSize) {
		outBuffer.resize(memSize);
	}

	for(uint32_t i = 0; i < memSize; i++) {
		outBuffer[i] = node->read(i);
	}

	if(size) *size = memSize;
	return outBuffer.data();
}

static uint8_t readDebuggerMemoryByte(GenesisAresImpl* impl, ares::Node::Debugger::Memory node, uint32_t address)
{
	if(!impl || !impl->isLoaded || !node || address >= node->size()) {
		return 0;
	}
	return node->read(address);
}

static void writeDebuggerMemoryByte(GenesisAresImpl* impl, ares::Node::Debugger::Memory node, std::vector<uint8_t>* cache, uint32_t address, uint8_t value)
{
	if(!impl || !impl->isLoaded || !node || address >= node->size()) {
		return;
	}
	node->write(address, value);
	if(cache && address < cache->size()) {
		(*cache)[address] = value;
	}
}

static std::string sanitizeDisassemblyText(const string& disasm)
{
	std::string text = (const char*)disasm;
	size_t split = text.find("  ");
	if(split != std::string::npos) {
		text = text.substr(split + 2);
	}
	while(!text.empty() && std::isspace((unsigned char)text.back())) {
		text.pop_back();
	}
	return text;
}

// ============================================================
// Public API implementation
// ============================================================

GenesisAresImpl* GenesisAresCreate(IGenesisPlatformCallbacks* callbacks)
{
	auto* impl = new GenesisAresImpl(callbacks);
	return impl;
}

void GenesisAresDestroy(GenesisAresImpl* impl)
{
	if(!impl) return;
	GenesisAresUnload(impl);
	delete impl;
}

bool GenesisAresLoadRom(GenesisAresImpl* impl,
                         const uint8_t* romData, uint32_t romSize,
                         const char* region,
                         const uint8_t* saveRamData, uint32_t saveRamSize,
                         const uint8_t* saveEepromData, uint32_t saveEepromSize)
{
	if(!impl) return false;

	// Unload any existing session
	GenesisAresUnload(impl);

	// Save a copy of the ROM
	impl->romData.assign(romData, romData + romSize);

	// Set the global platform pointer
	ares::platform = &impl->platform;

	// Determine system name from region
	string sysName;
	if(std::string(region) == "PAL") {
		sysName = "[Sega] Mega Drive (PAL)";
	} else if(std::string(region) == "NTSC-J") {
		sysName = "[Sega] Mega Drive (NTSC-J)";
	} else {
		sysName = "[Sega] Mega Drive (NTSC-U)";
	}
	impl->regionName = sysName;

	// Build the cart pak and install it BEFORE calling load(),
	// because load() will call platform->pak() during component loading.
	auto cartPak = buildCartPak(
		romData, romSize, sysName,
		saveRamData, saveRamSize,
		saveEepromData, saveEepromSize
	);
	impl->platform._cartPak = cartPak.pak;
	impl->platform._saveRamFile = cartPak.saveRamFile;
	impl->platform._saveEepromFile = cartPak.saveEepromFile;

	// Load the system (pak() callbacks fire here for the system node)
	if(!ares::MegaDrive::load(impl->systemNode, sysName)) {
		impl->platform._cartPak.reset();
		impl->platform._saveRamFile.reset();
		impl->platform._saveEepromFile.reset();
		return false;
	}

	// Allocate and connect the cartridge peripheral.
	// cartridgeSlot.port is populated by system.load() → cartridgeSlot.load().
	// allocate() creates the Node::Peripheral; connect() calls cartridge.connect()
	// which calls platform->pak(node) to fetch the cart pak and loads "program.rom".
	if(ares::MegaDrive::cartridgeSlot.port) {
		ares::MegaDrive::cartridgeSlot.port->allocate("Mega Drive Cartridge");
		ares::MegaDrive::cartridgeSlot.port->connect();
	}

	// Power on
	impl->systemNode->power(false);
	impl->isLoaded = true;

	return true;
}

void GenesisAresUnload(GenesisAresImpl* impl)
{
	if(!impl || !impl->isLoaded) return;

	if(impl->systemNode) {
		impl->systemNode->unload();
		impl->systemNode.reset();
	}
	impl->isLoaded = false;
	impl->platform._inputs.clear();
	impl->platform._audioStreams.clear();
	impl->platform._audioBufPos = 0;
	impl->platform._frameReady = false;
	impl->platform._audioSampleRate = 48000;
	impl->platform._cartPak.reset();
	impl->platform._saveRamFile.reset();
	impl->platform._saveEepromFile.reset();
	impl->vramView.clear();
	impl->cramView.clear();
	impl->vsramView.clear();
}

void GenesisAresRunFrame(GenesisAresImpl* impl)
{
	if(!impl || !impl->isLoaded || !impl->systemNode) return;

	ares::platform = &impl->platform;

	impl->platform._frameReady = false;

	// Run until the VDP fires a frame event (video() callback sets _frameReady)
	while(!impl->platform._frameReady) {
		impl->systemNode->run();
	}

	impl->platform.flushAudio();
}

void GenesisAresPower(GenesisAresImpl* impl, bool reset)
{
	if(!impl || !impl->isLoaded || !impl->systemNode) return;
	ares::platform = &impl->platform;
	impl->systemNode->power(reset);
}

double GenesisAresGetFps(GenesisAresImpl* impl)
{
	if(!impl || !impl->systemNode) return 60.0;
	// PAL = ~50 Hz, NTSC = ~60 Hz
	return GenesisAresIsPAL(impl) ? 49.701460 : 59.922743;
}

bool GenesisAresIsPAL(GenesisAresImpl* impl)
{
	if(!impl) return false;
	return ares::MegaDrive::Region::PAL();
}

uint64_t GenesisAresGetMasterClock(GenesisAresImpl* impl)
{
	if(!impl) return 0;
	// Approximate elapsed master clocks from M68K cycles.
	// A 68000 cycle is 7 MD master clocks.
	uint32_t dummy_pc, dummy_sp, dummy_sr;
	uint64_t cycles;
	uint32_t d[8], a[8];
	uint16_t sr;
	GenesisAresGetCpuState(impl, &dummy_pc, &dummy_sp, d, a, &sr, &cycles);
	return cycles * 7;
}

uint32_t GenesisAresGetMasterClockRate(GenesisAresImpl* impl)
{
	if(!impl) return 53693175;
	return GenesisAresIsPAL(impl) ? 53203424 : 53693175;
}

void GenesisAresGetCpuState(GenesisAresImpl* impl,
                              uint32_t* pc, uint32_t* sp,
                              uint32_t d[8], uint32_t a[8],
                              uint16_t* sr, uint64_t* cycles)
{
	if(!impl) return;
	auto& r = ares::MegaDrive::cpu.r;
	if(pc) *pc = r.pc;
	if(sp) *sp = r.a[7];
	if(d) { for(int i=0;i<8;i++) d[i] = r.d[i]; }
	if(a) { for(int i=0;i<8;i++) a[i] = r.a[i]; }
	if(sr) *sr = (uint16_t)ares::MegaDrive::cpu.readSR();
	if(cycles) *cycles = ares::MegaDrive::cpu.clock();
}

uint32_t GenesisAresGetFrameCount(GenesisAresImpl* impl)
{
	if(!impl) return 0;
	return impl->lastFrameCount;
}

void GenesisAresGetFrameSize(GenesisAresImpl* impl,
                              uint32_t* width, uint32_t* height)
{
	if(!impl) return;
	if(width)  *width  = ares::MegaDrive::vdp.screenWidth();
	if(height) *height = ares::MegaDrive::vdp.screenHeight();
}

uint8_t GenesisAresReadMemory(GenesisAresImpl* impl, uint32_t address)
{
	if(!impl) return 0xFF;
	// Bus read (byte)
	uint16_t word = ares::MegaDrive::bus.read(1, 1, address & ~1);
	return (address & 1) ? (uint8_t)(word & 0xFF) : (uint8_t)(word >> 8);
}

void GenesisAresWriteMemory(GenesisAresImpl* impl, uint32_t address, uint8_t value)
{
	if(!impl) return;
	bool upper = !(address & 1);
	bool lower = (address & 1) != 0;
	uint16_t data = upper ? (value << 8) : value;
	ares::MegaDrive::bus.write(upper, lower, address & ~1, data);
}

const uint8_t* GenesisAresGetVRam(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl) { if(size) *size = 0; return nullptr; }
	return snapshotDebuggerMemory(impl, getVramNode(), impl->vramView, size);
}

const uint8_t* GenesisAresGetCRam(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl) { if(size) *size = 0; return nullptr; }
	return snapshotDebuggerMemory(impl, getCramNode(), impl->cramView, size);
}

const uint8_t* GenesisAresGetVSRam(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl) { if(size) *size = 0; return nullptr; }
	return snapshotDebuggerMemory(impl, getVsramNode(), impl->vsramView, size);
}

const uint8_t* GenesisAresGetWorkRam(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl) { if(size) *size = 0; return nullptr; }
	if(size) *size = 65536;  // 64 KB work RAM
	return (const uint8_t*)ares::MegaDrive::cpu.ram.data();
}

const uint8_t* GenesisAresGetRom(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl) { if(size) *size = 0; return nullptr; }
	if(size) *size = (uint32_t)impl->romData.size();
	return impl->romData.data();
}

const uint8_t* GenesisAresGetSaveRam(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl || !impl->platform._saveRamFile) {
		if(size) *size = 0;
		return nullptr;
	}
	if(size) *size = (uint32_t)impl->platform._saveRamFile->size();
	return impl->platform._saveRamFile->data();
}

const uint8_t* GenesisAresGetSaveEeprom(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl || !impl->platform._saveEepromFile) {
		if(size) *size = 0;
		return nullptr;
	}
	if(size) *size = (uint32_t)impl->platform._saveEepromFile->size();
	return impl->platform._saveEepromFile->data();
}

void GenesisAresSyncSaveData(GenesisAresImpl* impl)
{
	if(!impl || !impl->isLoaded || !impl->systemNode) {
		return;
	}
	ares::platform = &impl->platform;
	impl->systemNode->save();
}

uint8_t GenesisAresReadVRam(GenesisAresImpl* impl, uint32_t address)
{
	return readDebuggerMemoryByte(impl, getVramNode(), address);
}

uint8_t GenesisAresReadCRam(GenesisAresImpl* impl, uint32_t address)
{
	return readDebuggerMemoryByte(impl, getCramNode(), address);
}

uint8_t GenesisAresReadVSRam(GenesisAresImpl* impl, uint32_t address)
{
	return readDebuggerMemoryByte(impl, getVsramNode(), address);
}

void GenesisAresWriteVRam(GenesisAresImpl* impl, uint32_t address, uint8_t value)
{
	if(!impl) return;
	writeDebuggerMemoryByte(impl, getVramNode(), &impl->vramView, address, value);
}

void GenesisAresWriteCRam(GenesisAresImpl* impl, uint32_t address, uint8_t value)
{
	if(!impl) return;
	writeDebuggerMemoryByte(impl, getCramNode(), &impl->cramView, address, value);
}

void GenesisAresWriteVSRam(GenesisAresImpl* impl, uint32_t address, uint8_t value)
{
	if(!impl) return;
	writeDebuggerMemoryByte(impl, getVsramNode(), &impl->vsramView, address, value);
}

bool GenesisAresSetProgramCounter(GenesisAresImpl* impl, uint32_t address)
{
	if(!impl || !impl->isLoaded) {
		return false;
	}

	ares::platform = &impl->platform;

	uint32_t pc = (address & 0x00fffffe);
	ares::MegaDrive::cpu.r.pc = (pc + 2) & 0x00ffffff;
	ares::MegaDrive::cpu.r.irc = ares::MegaDrive::bus.read(1, 1, pc);
	ares::MegaDrive::cpu.r.ir = ares::MegaDrive::cpu.r.irc;
	ares::MegaDrive::cpu.r.ird = ares::MegaDrive::cpu.r.irc;
	ares::MegaDrive::cpu.r.stop = false;
	return true;
}

uint32_t GenesisAresGetInstructionSize(GenesisAresImpl* impl, uint32_t address)
{
	if(!impl || !impl->isLoaded) {
		return 2;
	}

	ares::platform = &impl->platform;

	uint32_t pc = (address & 0x00fffffe);
	uint32_t size = ares::MegaDrive::cpu.disassembleInstructionLength(pc);
	if(size < 2 || size > 10 || (size & 1)) {
		return 2;
	}
	return size;
}

const char* GenesisAresDisassembleInstruction(GenesisAresImpl* impl, uint32_t address)
{
	static thread_local std::string disassemblyText;

	if(!impl || !impl->isLoaded) {
		disassemblyText.clear();
		return disassemblyText.c_str();
	}

	ares::platform = &impl->platform;

	uint32_t pc = (address & 0x00fffffe);
	disassemblyText = sanitizeDisassemblyText(ares::MegaDrive::cpu.disassembleInstruction(pc));
	return disassemblyText.c_str();
}

uint8_t* GenesisAresSaveState(GenesisAresImpl* impl, uint32_t* size)
{
	if(!impl || !impl->isLoaded || !impl->systemNode) {
		if(size) *size = 0;
		return nullptr;
	}
	ares::platform = &impl->platform;
	auto s = impl->systemNode->serialize(false);
	uint32_t sz = (uint32_t)s.size();
	if(size) *size = sz;
	if(sz == 0) return nullptr;
	uint8_t* buf = (uint8_t*)malloc(sz);
	memcpy(buf, s.data(), sz);
	return buf;
}

bool GenesisAresLoadState(GenesisAresImpl* impl, const uint8_t* data, uint32_t size)
{
	if(!impl || !impl->isLoaded || !impl->systemNode) return false;
	ares::platform = &impl->platform;
	serializer s(data, size);
	return impl->systemNode->unserialize(s);
}

void GenesisAresFreeStateData(uint8_t* data)
{
	free(data);
}
