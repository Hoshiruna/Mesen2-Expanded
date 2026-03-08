#pragma once
#include "pch.h"

// ---------------------------------------------------------------------------
// GenesisI2cEeprom
//
// Bit-bang I2C EEPROM state machine for Mega Drive cartridges.
// Ported from a prior M24C implementation and adapted for GenesisNativeBackend.
// Supports X24C01 through M24C512 chip families.
// ---------------------------------------------------------------------------
class GenesisI2cEeprom
{
public:
	enum class ChipType : uint32_t {
		None,
		X24C01,   //    128 bytes  (8-bit address, Xicor proprietary)
		M24C01,   //    128 bytes  (8-bit device + 8-bit address)
		M24C02,   //    256 bytes
		M24C04,   //    512 bytes
		M24C08,   //   1024 bytes
		M24C16,   //   2048 bytes
		M24C32,   //   4096 bytes  (8-bit device + 8-bit bank + 8-bit address)
		M24C64,   //   8192 bytes
		M24C65,   //   8192 bytes
		M24C128,  //  16384 bytes
		M24C256,  //  32768 bytes
		M24C512,  //  65536 bytes
	};

	// Plain-old-data snapshot for save-state serialization.
	struct SavedState {
		uint32_t type;
		uint32_t mode;
		uint8_t  counter;
		uint8_t  device;
		uint8_t  bank;
		uint8_t  address;
		uint8_t  input;
		uint8_t  output;
		bool     response;
		bool     writable;
		bool     clockLatch;
		bool     clockValue;
		bool     dataLatch;
		bool     dataValue;
	};

private:
	enum class Mode : uint32_t { Standby, Device, Bank, Address, Read, Write };

	static constexpr bool Acknowledge = false;

	struct Line {
		bool latch = true;
		bool value = true;

		bool hi()   const { return  latch &&  value; }
		bool fall() const { return  latch && !value; }
		bool rise() const { return !latch &&  value; }
		bool get()  const { return  value; }

		Line& operator=(bool data) { latch = value; value = data; return *this; }
	};

	ChipType _type     = ChipType::None;
	Mode     _mode     = Mode::Standby;
	uint8_t  _counter  = 0;
	uint8_t  _device   = 0;
	uint8_t  _bank     = 0;
	uint8_t  _address  = 0;
	uint8_t  _input    = 0;
	uint8_t  _output   = 0;
	bool     _response = Acknowledge;
	bool     _writable = true;

	vector<uint8_t> _memory;

	Line _clock;   // SCL
	Line _data;    // SDA (write side)

	bool Select() const
	{
		switch(_device >> 4) {
			case 0b1010u: return Acknowledge;
			case 0b1011u: return (_type > ChipType::M24C16) ? Acknowledge : !Acknowledge;
			default:      return !Acknowledge;
		}
	}

	uint32_t Offset() const
	{
		if(_type == ChipType::X24C01)  return _address >> 1;
		if(_type <= ChipType::M24C16)  return ((_device >> 1) & 0x07u) << 8 | _address;
		return ((_device >> 1) & 0x07u) << 16 | (uint32_t)_bank << 8 | _address;
	}

	bool DoLoad()
	{
		_output = _memory[Offset() & (Size() - 1)];
		return Acknowledge;
	}

	bool DoStore()
	{
		if(!_writable) return !Acknowledge;
		_memory[Offset() & (Size() - 1)] = _input;
		return Acknowledge;
	}

public:
	explicit operator bool() const { return _type != ChipType::None; }

	uint32_t Size() const
	{
		switch(_type) {
			default:                  return     0;
			case ChipType::X24C01:    return   128;
			case ChipType::M24C01:    return   128;
			case ChipType::M24C02:    return   256;
			case ChipType::M24C04:    return   512;
			case ChipType::M24C08:    return  1024;
			case ChipType::M24C16:    return  2048;
			case ChipType::M24C32:    return  4096;
			case ChipType::M24C64:    return  8192;
			case ChipType::M24C65:    return  8192;
			case ChipType::M24C128:   return 16384;
			case ChipType::M24C256:   return 32768;
			case ChipType::M24C512:   return 65536;
		}
	}

	// Initialize chip type and load optional pre-existing save data.
	void Load(ChipType type, const uint8_t* saveData = nullptr, uint32_t saveSize = 0)
	{
		_type = type;
		uint32_t sz = Size();
		_memory.assign(sz, 0xFF);
		if(saveData && saveSize > 0) {
			uint32_t copyLen = (saveSize < sz) ? saveSize : sz;
			memcpy(_memory.data(), saveData, copyLen);
		}
	}

	// Reset runtime state (call after Load, or on power-on/reset).
	void Power()
	{
		_mode     = Mode::Standby;
		_clock    = {};
		_data     = {};
		_counter  = 0;
		_device   = 0b10100000u;   // Area::Memory << 4
		_bank     = 0;
		_address  = 0;
		_input    = 0;
		_output   = 0;
		_response = Acknowledge;
		_writable = true;
	}

	// Drive the SCL and SDA write lines, then clock the state machine.
	// scl / sda correspond to the values the CPU is currently writing.
	void Update(bool scl, bool sda)
	{
		_clock = scl;
		_data  = sda;
		Write();
	}

	// Returns the SDA bit the EEPROM drives back toward the CPU.
	bool ReadSda() const
	{
		if(_mode == Mode::Standby) return _data.get();
		return _response;
	}

	// Raw access to backing store (for debugger / battery save).
	const vector<uint8_t>& GetMemory() const { return _memory; }
	vector<uint8_t>&       GetMemory()       { return _memory; }

	// Save-state snapshot (scalar fields only; caller appends memory separately).
	SavedState CaptureState() const
	{
		SavedState s;
		s.type       = static_cast<uint32_t>(_type);
		s.mode       = static_cast<uint32_t>(_mode);
		s.counter    = _counter;
		s.device     = _device;
		s.bank       = _bank;
		s.address    = _address;
		s.input      = _input;
		s.output     = _output;
		s.response   = _response;
		s.writable   = _writable;
		s.clockLatch = _clock.latch;
		s.clockValue = _clock.value;
		s.dataLatch  = _data.latch;
		s.dataValue  = _data.value;
		return s;
	}

	// Restore scalar state; caller must also restore memory separately.
	void RestoreState(const SavedState& s)
	{
		_type     = static_cast<ChipType>(s.type);
		_mode     = static_cast<Mode>(s.mode);
		_counter  = s.counter;
		_device   = s.device;
		_bank     = s.bank;
		_address  = s.address;
		_input    = s.input;
		_output   = s.output;
		_response = s.response;
		_writable = s.writable;
		_clock.latch = s.clockLatch;
		_clock.value = s.clockValue;
		_data.latch  = s.dataLatch;
		_data.value  = s.dataValue;
	}

private:
	// Core I2C protocol step — called after updating clock/data lines.
	void Write()
	{
		Mode phase = _mode;

		if(_clock.hi()) {
			if(_data.fall()) {
				// START condition
				_counter = 0;
				_mode = (_type == ChipType::X24C01) ? Mode::Address : Mode::Device;
			} else if(_data.rise()) {
				// STOP condition
				_counter = 0;
				_mode = Mode::Standby;
			}
		}

		if(_clock.fall()) {
			if(++_counter > 8) _counter = 1;
		}

		if(_clock.rise()) {
			switch(phase) {
				case Mode::Device:
					if(_counter <= 8) {
						_device = static_cast<uint8_t>((_device << 1) | (_data.get() ? 1u : 0u));
					} else if(Select() != Acknowledge) {
						_mode = Mode::Standby;
					} else if(_device & 1u) {
						_mode = Mode::Read;
						_response = DoLoad();
					} else {
						_mode = (_type <= ChipType::M24C16) ? Mode::Address : Mode::Bank;
						_response = Acknowledge;
					}
					break;

				case Mode::Bank:
					if(_counter <= 8) {
						_bank = static_cast<uint8_t>((_bank << 1) | (_data.get() ? 1u : 0u));
					} else {
						_mode = Mode::Address;
						_response = Acknowledge;
					}
					break;

				case Mode::Address:
					if(_counter <= 8) {
						_address = static_cast<uint8_t>((_address << 1) | (_data.get() ? 1u : 0u));
					} else if(_type == ChipType::X24C01 && (_address & 1u)) {
						_mode = Mode::Read;
						_response = DoLoad();
					} else {
						_mode = Mode::Write;
						_response = Acknowledge;
					}
					break;

				case Mode::Read:
					if(_counter <= 8) {
						_response = static_cast<bool>((_output >> (8 - _counter)) & 1u);
					} else if(_data.get() == Acknowledge) {
						_address += (_type == ChipType::X24C01) ? 2u : 1u;
						if(!_address) _bank++;
						_response = DoLoad();
					} else {
						_mode = Mode::Standby;
					}
					break;

				case Mode::Write:
					if(_counter <= 8) {
						_input = static_cast<uint8_t>((_input << 1) | (_data.get() ? 1u : 0u));
					} else {
						_response = DoStore();
						_address += (_type == ChipType::X24C01) ? 2u : 1u;
						if(!_address) _bank++;
					}
					break;

				default:
					break;
			}
		}
	}
};
