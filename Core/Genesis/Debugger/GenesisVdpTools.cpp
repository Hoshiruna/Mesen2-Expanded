#include "pch.h"
#include "Genesis/Debugger/GenesisVdpTools.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"

GenesisVdpTools::GenesisVdpTools(Debugger* debugger, Emulator* emu, GenesisConsole* console) : PpuTools(debugger, emu)
{
	_console = console;
}

uint8_t GenesisVdpTools::GetPlaneWidthTiles(uint8_t sizeReg)
{
	switch(sizeReg & 0x03u) {
		case 0x00: return 32;
		case 0x01: return 64;
		case 0x03: return 128;
		default: return 32;
	}
}

uint8_t GenesisVdpTools::GetPlaneHeightTiles(uint8_t sizeReg)
{
	switch((sizeReg >> 4) & 0x03u) {
		case 0x00: return 32;
		case 0x01: return 64;
		case 0x03: return 128;
		default: return 32;
	}
}

uint16_t GenesisVdpTools::GetPlaneBase(uint8_t regValue, bool planeA)
{
	return planeA ? (uint16_t)(regValue & 0x38u) << 10 : (uint16_t)(regValue & 0x07u) << 13;
}

uint16_t GenesisVdpTools::GetHScrollBase(uint8_t regValue)
{
	return (uint16_t)(regValue & 0x3Fu) << 10;
}

uint16_t GenesisVdpTools::GetHScroll(const uint8_t* vram, uint8_t reg11, uint16_t hScrollBase, uint16_t line, bool planeA)
{
	uint32_t entryOffset;
	switch(reg11 & 0x03u) {
		case 0x02:
			entryOffset = (line & ~7u) * 4u;
			break;
		case 0x03:
			entryOffset = (uint32_t)line * 4u;
			break;
		default:
			entryOffset = 0u;
			break;
	}

	uint16_t byteAddr = (uint16_t)((hScrollBase + entryOffset) & 0xFFFFu);
	uint16_t scrollA = ((uint16_t)vram[byteAddr] << 8) | vram[(byteAddr + 1u) & 0xFFFFu];
	uint16_t scrollB = ((uint16_t)vram[(byteAddr + 2u) & 0xFFFFu] << 8) | vram[(byteAddr + 3u) & 0xFFFFu];
	return planeA ? (scrollA & 0x03FFu) : (scrollB & 0x03FFu);
}

uint16_t GenesisVdpTools::GetVScroll(const uint8_t* vsram, uint8_t reg11, uint16_t tileCol2, bool planeA)
{
	uint8_t vsramIdx;
	if((reg11 & 0x04u) != 0) {
		uint8_t base = (uint8_t)((tileCol2 & 0x1Fu) * 2u);
		vsramIdx = planeA ? base : (base + 1u);
	} else {
		vsramIdx = planeA ? 0u : 1u;
	}

	uint32_t byteAddr = (uint32_t)(vsramIdx & 0x27u) * 2u;
	return ((((uint16_t)vsram[byteAddr] << 8) | vsram[byteAddr + 1u]) & 0x03FFu);
}

uint8_t GenesisVdpTools::GetTilePixel(const uint8_t* vram, uint32_t tileBase, uint8_t row, uint8_t col)
{
	uint32_t byteAddr = (tileBase + (uint32_t)row * 4u + (col >> 1)) & 0xFFFFu;
	uint8_t value = vram[byteAddr];
	return (col & 1u) != 0 ? (value & 0x0Fu) : (value >> 4);
}

uint32_t GenesisVdpTools::CramWordToArgb(uint16_t value)
{
	uint8_t r3 = (value >> 1) & 0x07u;
	uint8_t g3 = (value >> 5) & 0x07u;
	uint8_t b3 = (value >> 9) & 0x07u;
	uint8_t r8 = (uint8_t)((r3 << 5) | (r3 << 2) | (r3 >> 1));
	uint8_t g8 = (uint8_t)((g3 << 5) | (g3 << 2) | (g3 >> 1));
	uint8_t b8 = (uint8_t)((b3 << 5) | (b3 << 2) | (b3 >> 1));
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

FrameInfo GenesisVdpTools::GetTilemapSize(GetTilemapOptions options, BaseState& state)
{
	(void)state;

	uint8_t sizeReg = _console->GetVdpRegister(16);
	return {
		(uint32_t)GetPlaneWidthTiles(sizeReg) * 8u,
		(uint32_t)GetPlaneHeightTiles(sizeReg) * 8u
	};
}

DebugTilemapInfo GenesisVdpTools::GetTilemap(GetTilemapOptions options, BaseState& state, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer)
{
	(void)ppuToolsState;

	GenesisVdpState& vdpState = (GenesisVdpState&)state;
	bool planeA = options.Layer == 0;

	uint8_t reg2 = _console->GetVdpRegister(2);
	uint8_t reg4 = _console->GetVdpRegister(4);
	uint8_t reg11 = _console->GetVdpRegister(11);
	uint8_t reg13 = _console->GetVdpRegister(13);
	uint8_t reg16 = _console->GetVdpRegister(16);

	uint8_t planeWidth = GetPlaneWidthTiles(reg16);
	uint8_t planeHeight = GetPlaneHeightTiles(reg16);
	uint16_t planeBase = GetPlaneBase(planeA ? reg2 : reg4, planeA);
	uint16_t hScrollBase = GetHScrollBase(reg13);

	uint8_t* vsram = _debugger->GetMemoryDumper()->GetMemoryBuffer(MemoryType::GenesisVScrollRam);
	uint8_t emptyVsram[0x50] = {};
	if(vsram == nullptr) {
		vsram = emptyVsram;
	}

	uint32_t* displayPalette = palette;
	if(options.DisplayMode == TilemapDisplayMode::Grayscale) {
		displayPalette = (uint32_t*)_grayscaleColorsBpp4;
	}

	for(uint32_t row = 0; row < planeHeight; row++) {
		for(uint32_t column = 0; column < planeWidth; column++) {
			uint16_t entryAddr = (uint16_t)(planeBase + ((row * planeWidth + column) * 2u));
			uint16_t entry = ((uint16_t)vram[entryAddr & 0xFFFFu] << 8) | vram[(entryAddr + 1u) & 0xFFFFu];
			uint16_t tileIndex = entry & 0x07FFu;
			uint8_t paletteIndex = (uint8_t)((entry >> 13) & 0x03u);
			bool vFlip = (entry & 0x1000u) != 0;
			bool hFlip = (entry & 0x0800u) != 0;

			uint32_t tileBase = (uint32_t)tileIndex * 32u;
			for(uint32_t y = 0; y < 8; y++) {
				uint8_t tileRow = (uint8_t)(vFlip ? (7u - y) : y);
				for(uint32_t x = 0; x < 8; x++) {
					uint8_t tileColumn = (uint8_t)(hFlip ? (7u - x) : x);
					uint8_t colorIndex = GetTilePixel(vram, tileBase, tileRow, tileColumn);
					uint32_t outOffset = ((row * 8u + y) * planeWidth * 8u) + column * 8u + x;
					outBuffer[outOffset] = displayPalette[paletteIndex * 16u + colorIndex];
				}
			}
		}
	}

	uint16_t activeLine = vdpState.Height > 0 ? (uint16_t)std::min<uint32_t>(vdpState.VClock, vdpState.Height - 1u) : 0u;

	DebugTilemapInfo result = {};
	result.Bpp = 4;
	result.Format = TileFormat::GbaBpp4;
	result.TileWidth = 8;
	result.TileHeight = 8;
	result.ColumnCount = planeWidth;
	result.RowCount = planeHeight;
	result.TilemapAddress = planeBase;
	result.TilesetAddress = 0;
	result.ScrollX = GetHScroll(vram, reg11, hScrollBase, activeLine, planeA);
	result.ScrollY = GetVScroll(vsram, reg11, 0, planeA);
	result.ScrollWidth = vdpState.Width;
	result.ScrollHeight = vdpState.Height;
	return result;
}

DebugTilemapTileInfo GenesisVdpTools::GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	(void)ppuToolsState;

	DebugTilemapTileInfo result = {};
	FrameInfo size = GetTilemapSize(options, baseState);
	if(x >= size.Width || y >= size.Height) {
		return result;
	}

	bool planeA = options.Layer == 0;
	uint8_t reg2 = _console->GetVdpRegister(2);
	uint8_t reg4 = _console->GetVdpRegister(4);
	uint8_t reg16 = _console->GetVdpRegister(16);

	uint8_t planeWidth = GetPlaneWidthTiles(reg16);
	uint16_t planeBase = GetPlaneBase(planeA ? reg2 : reg4, planeA);
	uint32_t column = x / 8u;
	uint32_t row = y / 8u;
	uint16_t entryAddr = (uint16_t)(planeBase + ((row * planeWidth + column) * 2u));
	uint16_t entry = ((uint16_t)vram[entryAddr & 0xFFFFu] << 8) | vram[(entryAddr + 1u) & 0xFFFFu];

	result.Width = 8;
	result.Height = 8;
	result.Column = column;
	result.Row = row;
	result.TileMapAddress = entryAddr;
	result.TileIndex = entry & 0x07FFu;
	result.TileAddress = result.TileIndex * 32;
	result.PaletteIndex = (entry >> 13) & 0x03;
	result.PaletteAddress = result.PaletteIndex * 32;
	result.HighPriority = (entry & 0x8000u) ? NullableBoolean::True : NullableBoolean::False;
	result.VerticalMirroring = (entry & 0x1000u) ? NullableBoolean::True : NullableBoolean::False;
	result.HorizontalMirroring = (entry & 0x0800u) ? NullableBoolean::True : NullableBoolean::False;
	return result;
}

DebugSpritePreviewInfo GenesisVdpTools::GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& state, BaseState& ppuToolsState)
{
	(void)options;
	(void)state;
	(void)ppuToolsState;
	return {};
}

void GenesisVdpTools::GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview)
{
	(void)options;
	(void)baseState;
	(void)ppuToolsState;
	(void)vram;
	(void)oamRam;
	(void)palette;
	(void)outBuffer;
	(void)spritePreviews;
	(void)screenPreview;
}

DebugPaletteInfo GenesisVdpTools::GetPaletteInfo(GetPaletteInfoOptions options)
{
	(void)options;

	DebugPaletteInfo info = {};
	info.HasMemType = true;
	info.PaletteMemType = MemoryType::GenesisColorRam;
	info.PaletteMemOffset = 0;
	info.ColorCount = 64;
	info.BgColorCount = 64;
	info.SpriteColorCount = 64;
	info.SpritePaletteOffset = 0;
	info.ColorsPerPalette = 16;
	info.RawFormat = RawPaletteFormat::Rgb333;

	uint8_t* cram = _debugger->GetMemoryDumper()->GetMemoryBuffer(MemoryType::GenesisColorRam);
	if(cram == nullptr) {
		return info;
	}

	for(int i = 0; i < 64; i++) {
		uint16_t value = ((uint16_t)cram[i * 2] << 8) | cram[i * 2 + 1];
		uint32_t raw = ((value >> 1) & 0x07u) | (((value >> 5) & 0x07u) << 3) | (((value >> 9) & 0x07u) << 6);
		info.RawPalette[i] = raw;
		info.RgbPalette[i] = CramWordToArgb(value);
	}

	return info;
}

void GenesisVdpTools::SetPaletteColor(int32_t colorIndex, uint32_t color)
{
	if(colorIndex < 0 || colorIndex >= 64) {
		return;
	}

	uint16_t r = (uint16_t)((color >> 21) & 0x07u);
	uint16_t g = (uint16_t)((color >> 13) & 0x07u);
	uint16_t b = (uint16_t)((color >> 5) & 0x07u);
	uint16_t cramValue = (uint16_t)((r << 1) | (g << 5) | (b << 9));

	_debugger->GetMemoryDumper()->SetMemoryValue(MemoryType::GenesisColorRam, colorIndex * 2, (uint8_t)(cramValue >> 8));
	_debugger->GetMemoryDumper()->SetMemoryValue(MemoryType::GenesisColorRam, colorIndex * 2 + 1, (uint8_t)cramValue);
}
