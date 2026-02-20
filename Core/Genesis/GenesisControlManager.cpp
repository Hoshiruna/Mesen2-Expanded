#include "pch.h"
#include "Genesis/GenesisControlManager.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/Input/GenesisController.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"

GenesisControlManager::GenesisControlManager(Emulator* emu, GenesisConsole* console)
	: BaseControlManager(emu, CpuType::GenesisMain)
{
	_console = console;
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
			return shared_ptr<BaseControlDevice>(new GenesisController(_emu, port, keys));
	}
}

void GenesisControlManager::UpdateControlDevices()
{
	GenesisConfig& cfg = _emu->GetSettings()->GetGenesisConfig();
	if(_emu->GetSettings()->IsEqual(_prevConfig, cfg) && _controlDevices.size() > 0) {
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

uint32_t GenesisControlManager::GetButtonsForAres(int port)
{
	auto lock = _deviceLock.AcquireSafe();
	for(shared_ptr<BaseControlDevice>& dev : _controlDevices) {
		if(dev->GetPort() == (uint8_t)port) {
			GenesisController* ctrl = dynamic_cast<GenesisController*>(dev.get());
			if(ctrl) {
				return ctrl->GetButtonMask();
			}
		}
	}
	return 0;
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
