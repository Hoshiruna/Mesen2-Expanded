#pragma once
#include "pch.h"
#include "Debugger/PpuTools.h"

class Debugger;
class Emulator;
class GenesisConsole;

class GenesisVdpTools final : public PpuTools
{
private:
	GenesisConsole* _console = nullptr;

	static uint8_t GetPlaneWidthTiles(uint8_t sizeReg);
	static uint8_t GetPlaneHeightTiles(uint8_t sizeReg);
	static uint16_t GetPlaneBase(uint8_t regValue, bool planeA);
	static uint16_t GetHScrollBase(uint8_t regValue);
	static uint16_t GetHScroll(const uint8_t* vram, uint8_t reg11, uint16_t hScrollBase, uint16_t line, bool planeA);
	static uint16_t GetVScroll(const uint8_t* vsram, uint8_t reg11, uint16_t tileCol2, bool planeA);
	static uint8_t GetTilePixel(const uint8_t* vram, uint32_t tileBase, uint8_t row, uint8_t col);
	static uint32_t CramWordToArgb(uint16_t value);

public:
	GenesisVdpTools(Debugger* debugger, Emulator* emu, GenesisConsole* console);

	DebugTilemapInfo GetTilemap(GetTilemapOptions options, BaseState& state, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer) override;
	FrameInfo GetTilemapSize(GetTilemapOptions options, BaseState& state) override;
	DebugTilemapTileInfo GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState) override;

	DebugSpritePreviewInfo GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& state, BaseState& ppuToolsState) override;
	void GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview) override;

	DebugPaletteInfo GetPaletteInfo(GetPaletteInfoOptions options) override;
	void SetPaletteColor(int32_t colorIndex, uint32_t color) override;
};
