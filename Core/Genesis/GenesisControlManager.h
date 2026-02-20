#pragma once
#include "pch.h"
#include "Shared/BaseControlManager.h"
#include "Shared/SettingTypes.h"

class GenesisConsole;

class GenesisControlManager : public BaseControlManager
{
private:
	GenesisConsole* _console = nullptr;
	GenesisConfig   _prevConfig = {};

public:
	GenesisControlManager(Emulator* emu, GenesisConsole* console);

	shared_ptr<BaseControlDevice> CreateControllerDevice(ControllerType type, uint8_t port) override;
	void UpdateControlDevices() override;

	// Returns the GenesisButton bitmask for a given port (called by GenesisConsole::GetControllerButtons)
	uint32_t GetButtonsForAres(int port);

	void Serialize(Serializer& s) override;
};
