#include "pch.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/Input/GenesisController.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/KeyManager.h"

namespace
{
	bool HasAnyKeyMapping(KeyMappingSet& keys)
	{
		return keys.Mapping1.HasKeySet()
			|| keys.Mapping2.HasKeySet()
			|| keys.Mapping3.HasKeySet()
			|| keys.Mapping4.HasKeySet();
	}

	void ApplyDefaultGenesisMapping(KeyMappingSet& keys, uint8_t port, bool sixButton)
	{
		if(HasAnyKeyMapping(keys)) {
			return;
		}

		KeyMapping map = {};
		if(port == 0) {
			// Player 1: arrows + ZXCV row + ASDF row.
			map.Up    = KeyManager::GetKeyCode("Up Arrow");
			map.Down  = KeyManager::GetKeyCode("Down Arrow");
			map.Left  = KeyManager::GetKeyCode("Left Arrow");
			map.Right = KeyManager::GetKeyCode("Right Arrow");
			map.A     = KeyManager::GetKeyCode("Z");
			map.B     = KeyManager::GetKeyCode("X");
			map.TurboA = KeyManager::GetKeyCode("C");      // Genesis C button
			map.Start = KeyManager::GetKeyCode("Enter");
			if(sixButton) {
				map.X      = KeyManager::GetKeyCode("A");
				map.Y      = KeyManager::GetKeyCode("S");
				map.TurboB = KeyManager::GetKeyCode("D");      // Genesis Z button
				map.Select = KeyManager::GetKeyCode("Right Shift"); // Genesis Mode button
			}
		} else {
			// Player 2 fallback.
			map.Up    = KeyManager::GetKeyCode("W");
			map.Down  = KeyManager::GetKeyCode("S");
			map.Left  = KeyManager::GetKeyCode("A");
			map.Right = KeyManager::GetKeyCode("D");
			map.A     = KeyManager::GetKeyCode("F");
			map.B     = KeyManager::GetKeyCode("G");
			map.TurboA = KeyManager::GetKeyCode("H");      // Genesis C button
			map.Start = KeyManager::GetKeyCode("Tab");
			if(sixButton) {
				map.X      = KeyManager::GetKeyCode("R");
				map.Y      = KeyManager::GetKeyCode("T");
				map.TurboB = KeyManager::GetKeyCode("Y");      // Genesis Z button
				map.Select = KeyManager::GetKeyCode("Left Shift"); // Genesis Mode button
			}
		}

		keys.Mapping1 = map;
		keys.TurboSpeed = 2;
	}
}

GenesisControlManager::GenesisControlManager(Emulator* emu, GenesisConsole* console)
	: BaseControlManager(emu, CpuType::GenesisMain)
{
	_console = console;
	UpdateControlDevices();
}

shared_ptr<BaseControlDevice> GenesisControlManager::CreateControllerDevice(ControllerType type, uint8_t port)
{
	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();

	KeyMappingSet keys;
	switch(port) {
		default:
		case 0: keys = cfg.Port1.Keys; break;
		case 1: keys = cfg.Port2.Keys; break;
	}

	switch(type) {
		default:
		case ControllerType::None: return nullptr;
		case ControllerType::GenesisController:
			ApplyDefaultGenesisMapping(keys, port, true);
			return shared_ptr<BaseControlDevice>(new GenesisController(_emu, ControllerType::GenesisController, port, keys, true));
		case ControllerType::GenesisController3Buttons:
			ApplyDefaultGenesisMapping(keys, port, false);
			return shared_ptr<BaseControlDevice>(new GenesisController(_emu, ControllerType::GenesisController3Buttons, port, keys, false));
	}
}

void GenesisControlManager::UpdateControlDevices()
{
	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();
	bool hasPortDevices = false;
	for(shared_ptr<BaseControlDevice>& dev : _controlDevices) {
		uint8_t port = dev->GetPort();
		if(port == 0 || port == 1) {
			hasPortDevices = true;
			break;
		}
	}

	// Only skip rebuild when config is unchanged and gameplay ports already exist.
	// BaseControlManager constructor only adds system devices, so this guards
	// against an initial false "already configured" state.
	if(_emu->GetSettings()->IsEqual(_prevConfig, cfg) && hasPortDevices) {
		return;
	}

	auto lock = _deviceLock.AcquireSafe();
	ClearDevices();

	for(int i = 0; i < 2; i++) {
		ControllerType type = (i == 0) ? cfg.Port1.Type : cfg.Port2.Type;
		shared_ptr<BaseControlDevice> device = CreateControllerDevice(type, i);
		if(device) {
			RegisterControlDevice(device);
		}
	}

	_prevConfig = cfg;
}

uint32_t GenesisControlManager::GetButtonsForPort(int port)
{
	auto lock = _deviceLock.AcquireSafe();
	for(shared_ptr<BaseControlDevice>& dev : _controlDevices) {
		if(dev->GetPort() == (uint8_t)port && (
			dev->HasControllerType(ControllerType::GenesisController) ||
			dev->HasControllerType(ControllerType::GenesisController3Buttons)
		)) {
			return static_cast<GenesisController*>(dev.get())->GetButtonMask();
		}
	}
	return 0;
}

bool GenesisControlManager::IsPortConnected(int port)
{
	auto lock = _deviceLock.AcquireSafe();
	for(shared_ptr<BaseControlDevice>& dev : _controlDevices) {
		if(dev->GetPort() == (uint8_t)port) {
			return dev->IsConnected();
		}
	}
	return false;
}

void GenesisControlManager::Serialize(Serializer& s)
{
	BaseControlManager::Serialize(s);

	if(!s.IsSaving()) {
		UpdateControlDevices();
	}

	for(uint8_t i = 0; i < _controlDevices.size(); i++) {
		SVI(_controlDevices[i]);
	}
}
