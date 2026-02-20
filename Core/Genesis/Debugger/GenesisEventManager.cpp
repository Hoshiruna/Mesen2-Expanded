#include "pch.h"
#include "Genesis/Debugger/GenesisEventManager.h"
#include "Genesis/GenesisConsole.h"
#include "Shared/ColorUtilities.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/BaseEventManager.h"

GenesisEventManager::GenesisEventManager(Debugger* debugger, GenesisConsole* console)
{
	_debugger = debugger;
	_console  = console;

	_ppuBuffer = new uint32_t[320 * 240];
	memset(_ppuBuffer, 0, 320 * 240 * sizeof(uint32_t));
}

GenesisEventManager::~GenesisEventManager()
{
	delete[] _ppuBuffer;
}

void GenesisEventManager::AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId)
{
	DebugEventInfo evt = {};
	evt.Type        = type;
	evt.Flags       = (uint32_t)EventFlags::ReadWriteOp;
	evt.Operation   = operation;
	evt.Scanline    = 0;
	evt.Cycle       = 0;
	evt.BreakpointId = breakpointId;
	evt.DmaChannel  = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(CpuType::GenesisMain, true);
	_debugEvents.push_back(evt);
}

void GenesisEventManager::AddEvent(DebugEventType type)
{
	DebugEventInfo evt = {};
	evt.Type        = type;
	evt.Scanline    = 0;
	evt.Cycle       = 0;
	evt.BreakpointId = -1;
	evt.DmaChannel  = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(CpuType::GenesisMain, true);
	_debugEvents.push_back(evt);
}

EventViewerCategoryCfg GenesisEventManager::GetEventConfig(DebugEventInfo& evt)
{
	switch(evt.Type) {
		case DebugEventType::Breakpoint: return _config.MarkedBreakpoints;
		case DebugEventType::Irq:        return _config.Irq;
		default:                         return {};
	}
}

bool GenesisEventManager::ShowPreviousFrameEvents()
{
	return _config.ShowPreviousFrameEvents;
}

void GenesisEventManager::ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y)
{
	// x = master clock within scanline, y = scanline
	// Map to pixel coordinates in the event viewer canvas
	// Use a 640-wide canvas (320 pixels * 2 for visibility)
	x = (x * 640) / ScanlineWidth;
	y = y;
}

void GenesisEventManager::DrawScreen(uint32_t* buffer)
{
	// Draw the last rendered frame as backdrop in the event viewer
	PpuFrameInfo frame = _console->GetPpuFrame();
	if(frame.FrameBuffer && frame.Width > 0 && frame.Height > 0) {
		const uint32_t* src = (const uint32_t*)frame.FrameBuffer;
		uint32_t copyH = std::min(frame.Height, (uint32_t)ScreenHeight);
		for(uint32_t y = 0; y < copyH; y++) {
			for(uint32_t x = 0; x < 640u; x++) {
				uint32_t srcX = (x * frame.Width) / 640;
				buffer[y * 640 + x] = src[y * frame.Width + srcX] & 0x00FFFFFF;  // dim
			}
		}
	}
}

uint32_t GenesisEventManager::TakeEventSnapshot(bool forAutoRefresh)
{
	DebugBreakHelper breakHelper(_debugger);
	auto lock = _lock.AcquireSafe();

	// Capture the last rendered frame for the event display background
	PpuFrameInfo frame = _console->GetPpuFrame();
	if(frame.FrameBuffer) {
		uint32_t copyPixels = std::min((uint32_t)(320 * 240), frame.Width * frame.Height);
		memcpy(_ppuBuffer, frame.FrameBuffer, copyPixels * sizeof(uint32_t));
	}

	_scanlineCount = _console->GetPpuFrame().ScanlineCount;
	FilterEvents();
	return _scanlineCount;
}

DebugEventInfo GenesisEventManager::GetEvent(uint16_t y, uint16_t x)
{
	auto lock = _lock.AcquireSafe();
	for(DebugEventInfo& evt : _sentEvents) {
		if(std::abs((int)evt.Scanline - y) <= 1 && std::abs((int)evt.Cycle - x) <= 4) {
			return evt;
		}
	}
	return {};
}

FrameInfo GenesisEventManager::GetDisplayBufferSize()
{
	return { 640, (uint32_t)_scanlineCount };
}

void GenesisEventManager::SetConfiguration(BaseEventViewerConfig& config)
{
	_config = (GenesisEventViewerConfig&)config;
}
