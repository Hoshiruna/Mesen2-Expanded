#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/InputHud.h"
#include "Genesis/GenesisTypes.h"
#include "Utilities/Serializer.h"

class GenesisController : public BaseControlDevice
{
protected:
	bool _sixButton = true;

	string GetKeyNames() override
	{
		return _sixButton ? "UDLRABCStXYZMd" : "UDLRABCSt";
	}

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::Up,    keyMapping.Up);
			SetPressedState(Buttons::Down,  keyMapping.Down);
			SetPressedState(Buttons::Left,  keyMapping.Left);
			SetPressedState(Buttons::Right, keyMapping.Right);
			SetPressedState(Buttons::A,     keyMapping.A);
			SetPressedState(Buttons::B,     keyMapping.B);
			SetPressedState(Buttons::C,     keyMapping.TurboA); // C mapped to TurboA slot
			SetPressedState(Buttons::Start, keyMapping.Start);
			if(_sixButton) {
				SetPressedState(Buttons::X,     keyMapping.X);
				SetPressedState(Buttons::Y,     keyMapping.Y);
				SetPressedState(Buttons::Z,     keyMapping.TurboB); // Z mapped to TurboB slot
				SetPressedState(Buttons::Mode,  keyMapping.Select);
			}
		}
	}

	void RefreshStateBuffer() override {}

public:
	enum Buttons { Up = 0, Down, Left, Right, A, B, C, Start, X, Y, Z, Mode };

	GenesisController(Emulator* emu, ControllerType type, uint8_t port, KeyMappingSet keyMappings, bool sixButton)
		: BaseControlDevice(emu, type, port, keyMappings)
	{
		_sixButton = sixButton;
	}

	// Build a bitmask using GenesisButton flags.
	uint32_t GetButtonMask()
	{
		uint32_t mask = 0;
		if(IsPressed(Buttons::Up))    mask |= GenesisButton::Up;
		if(IsPressed(Buttons::Down))  mask |= GenesisButton::Down;
		if(IsPressed(Buttons::Left))  mask |= GenesisButton::Left;
		if(IsPressed(Buttons::Right)) mask |= GenesisButton::Right;
		if(IsPressed(Buttons::A))     mask |= GenesisButton::A;
		if(IsPressed(Buttons::B))     mask |= GenesisButton::B;
		if(IsPressed(Buttons::C))     mask |= GenesisButton::C;
		if(IsPressed(Buttons::Start)) mask |= GenesisButton::Start;
		if(_sixButton) {
			if(IsPressed(Buttons::X))     mask |= GenesisButton::X;
			if(IsPressed(Buttons::Y))     mask |= GenesisButton::Y;
			if(IsPressed(Buttons::Z))     mask |= GenesisButton::Z;
			if(IsPressed(Buttons::Mode))  mask |= GenesisButton::Mode;
		}
		return mask;
	}

	uint8_t ReadRam(uint16_t addr) override { return 0xFF; }
	void WriteRam(uint16_t addr, uint8_t value) override {}

	void InternalDrawController(InputHud& hud) override
	{
		hud.DrawOutline(39, 14);

		hud.DrawButton(4, 3, 3, 3, IsPressed(Buttons::Up));
		hud.DrawButton(4, 9, 3, 3, IsPressed(Buttons::Down));
		hud.DrawButton(1, 6, 3, 3, IsPressed(Buttons::Left));
		hud.DrawButton(7, 6, 3, 3, IsPressed(Buttons::Right));
		hud.DrawButton(4, 6, 3, 3, false);

		hud.DrawButton(15, 9, 5, 2, IsPressed(Buttons::Start));

		if(_sixButton) {
			hud.DrawButton(27, 5, 3, 3, IsPressed(Buttons::X));
			hud.DrawButton(31, 5, 3, 3, IsPressed(Buttons::Y));
			hud.DrawButton(35, 5, 3, 3, IsPressed(Buttons::Z));

			hud.DrawButton(27, 9, 3, 3, IsPressed(Buttons::A));
			hud.DrawButton(31, 9, 3, 3, IsPressed(Buttons::B));
			hud.DrawButton(35, 9, 3, 3, IsPressed(Buttons::C));
		} else {
			hud.DrawButton(27, 7, 3, 3, IsPressed(Buttons::A));
			hud.DrawButton(31, 7, 3, 3, IsPressed(Buttons::B));
			hud.DrawButton(35, 7, 3, 3, IsPressed(Buttons::C));
		}

		hud.DrawNumber(_port + 1, 19, 2);
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		vector<DeviceButtonName> names = {
			{ "up",    Buttons::Up },
			{ "down",  Buttons::Down },
			{ "left",  Buttons::Left },
			{ "right", Buttons::Right },
			{ "a",     Buttons::A },
			{ "b",     Buttons::B },
			{ "c",     Buttons::C },
			{ "start", Buttons::Start },
		};

		if(_sixButton) {
			names.push_back({ "x", Buttons::X });
			names.push_back({ "y", Buttons::Y });
			names.push_back({ "z", Buttons::Z });
			names.push_back({ "mode", Buttons::Mode });
		}

		return names;
	}
};
