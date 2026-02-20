#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/BaseEventManager.h"
#include "Utilities/SimpleLock.h"

enum class DebugEventType;
struct DebugEventInfo;
class GenesisConsole;
class Debugger;

struct GenesisEventViewerConfig : public BaseEventViewerConfig
{
	EventViewerCategoryCfg Irq;
	EventViewerCategoryCfg MarkedBreakpoints;
	bool ShowPreviousFrameEvents = true;
};

class GenesisEventManager final : public BaseEventManager
{
private:
	static constexpr int ScanlineWidth  = 3420;
	static constexpr int ScreenHeight   = 262;

	GenesisEventViewerConfig _config;
	GenesisConsole* _console;
	Debugger* _debugger;

	uint32_t _scanlineCount = 262;
	uint32_t* _ppuBuffer = nullptr;

protected:
	bool ShowPreviousFrameEvents() override;
	void ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y) override;
	void DrawScreen(uint32_t* buffer) override;

public:
	GenesisEventManager(Debugger* debugger, GenesisConsole* console);
	~GenesisEventManager();

	void AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId = -1) override;
	void AddEvent(DebugEventType type) override;

	EventViewerCategoryCfg GetEventConfig(DebugEventInfo& evt) override;

	uint32_t TakeEventSnapshot(bool forAutoRefresh) override;
	DebugEventInfo GetEvent(uint16_t y, uint16_t x) override;

	FrameInfo GetDisplayBufferSize() override;
	void SetConfiguration(BaseEventViewerConfig& config) override;
};
