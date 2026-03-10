#include "pch.h"
#include "Genesis/Debugger/GenesisVdpTools.h"
#include "Genesis/GenesisConsole.h"
#include "Genesis/GenesisTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"

namespace {
	uint16_t GetSpriteBaseAddress(uint8_t reg5, bool h40)
	{
		return h40 ? (uint16_t)(reg5 & 0x7Eu) << 9 : (uint16_t)(reg5 & 0x7Fu) << 9;
	}

	struct DebugGenesisLineSprite
	{
		uint16_t tile = 0;
		uint16_t rawX = 0;
		int16_t x = 0;
		uint8_t palette = 0;
		uint8_t vertCells = 1;
		uint8_t horizCells = 1;
		uint8_t cellRow = 0;
		uint8_t pixRow = 0;
		bool hflip = false;
		bool vflip = false;
	};

	struct DebugGenesisLineSpriteCell
	{
		uint16_t tile = 0;
		int16_t x = 0;
		uint8_t palette = 0;
		uint8_t vertCells = 1;
		uint8_t screenCellCol = 0;
		uint8_t patternCellOffsetX = 0;
		uint8_t patternCellOffsetY = 0;
		uint8_t pixRow = 0;
		bool hflip = false;
		bool vflip = false;
	};
}

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
		case 0x01:
			entryOffset = 0u;
			break;
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
	(void)ppuToolsState;

	GenesisVdpState& vdpState = (GenesisVdpState&)state;

	DebugSpritePreviewInfo info = {};
	info.Width = 512;
	info.Height = 512;
	info.SpriteCount = vdpState.Width >= 320 ? 80 : 64;
	info.CoordOffsetX = 128;
	info.CoordOffsetY = 128;
	info.VisibleX = 128;
	info.VisibleY = 128;
	info.VisibleWidth = vdpState.Width;
	info.VisibleHeight = vdpState.Height;
	info.WrapBottomToTop = false;
	info.WrapRightToLeft = false;
	return info;
}

void GenesisVdpTools::GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview)
{
	(void)ppuToolsState;
	(void)oamRam;

	GenesisVdpState& vdpState = (GenesisVdpState&)baseState;

	bool h40 = (_console->GetVdpRegister(12) & 0x01u) != 0;
	bool int2 = (_console->GetVdpRegister(12) & 0x06u) == 0x06u;
	uint16_t spriteBase = GetSpriteBaseAddress(_console->GetVdpRegister(5), h40);
	uint16_t spriteCount = h40 ? 80u : 64u;
	uint16_t cellPixH = int2 ? 16u : 8u;

	DebugSpritePreviewInfo previewInfo = GetSpritePreviewInfo(options, baseState, ppuToolsState);
	uint32_t bgColor = GetSpriteBackgroundColor(options.Background, palette, false);
	uint32_t offscreenBgColor = GetSpriteBackgroundColor(options.Background, palette, true);

	std::fill(screenPreview, screenPreview + previewInfo.Width * previewInfo.Height, offscreenBgColor);
	for(uint32_t y = 0; y < previewInfo.VisibleHeight; y++) {
		uint32_t* row = screenPreview + (previewInfo.VisibleY + y) * previewInfo.Width + previewInfo.VisibleX;
		std::fill(row, row + previewInfo.VisibleWidth, bgColor);
	}

	for(uint16_t i = 0; i < spriteCount; i++) {
		DebugSpriteInfo& sprite = outBuffer[i];
		uint32_t* spritePreview = spritePreviews + (i * _spritePreviewSize);
		sprite.Init();
		std::fill(spritePreview, spritePreview + _spritePreviewSize, 0u);

		uint16_t entryBase = (uint16_t)(spriteBase + i * 8u);
		uint16_t w0 = ((uint16_t)vram[(entryBase + 0u) & 0xFFFFu] << 8) | vram[(entryBase + 1u) & 0xFFFFu];
		uint16_t w1 = ((uint16_t)vram[(entryBase + 2u) & 0xFFFFu] << 8) | vram[(entryBase + 3u) & 0xFFFFu];
		uint16_t w2 = ((uint16_t)vram[(entryBase + 4u) & 0xFFFFu] << 8) | vram[(entryBase + 5u) & 0xFFFFu];
		uint16_t w3 = ((uint16_t)vram[(entryBase + 6u) & 0xFFFFu] << 8) | vram[(entryBase + 7u) & 0xFFFFu];

		uint8_t vertCells = (uint8_t)(((w1 >> 8) & 0x03u) + 1u);
		uint8_t horizCells = (uint8_t)(((w1 >> 10) & 0x03u) + 1u);
		bool pri = (w2 & 0x8000u) != 0;
		uint8_t pal = (uint8_t)((w2 >> 13) & 0x03u);
		bool vflip = (w2 & 0x1000u) != 0;
		bool hflip = (w2 & 0x0800u) != 0;
		uint16_t tile = w2 & 0x07FFu;
		uint16_t rawY = w0 & 0x01FFu;
		uint16_t rawX = w3 & 0x01FFu;
		int16_t x = (int16_t)rawX - 128;
		int16_t y = (int16_t)rawY - 128;
		uint16_t width = (uint16_t)horizCells * 8u;
		uint16_t height = (uint16_t)vertCells * cellPixH;
		uint16_t tileBytes = int2 ? 64u : 32u;

		sprite.Format = TileFormat::GbaBpp4;
		sprite.Bpp = 4;
		sprite.SpriteIndex = i;
		sprite.X = x;
		sprite.Y = y;
		sprite.RawX = (int16_t)rawX;
		sprite.RawY = (int16_t)rawY;
		sprite.Width = width;
		sprite.Height = height;
		sprite.TileIndex = tile;
		sprite.TileAddress = tile * tileBytes;
		sprite.Palette = pal;
		sprite.PaletteAddress = pal * 32;
		sprite.Priority = pri ? DebugSpritePriority::Foreground : DebugSpritePriority::Background;
		sprite.Mode = DebugSpriteMode::Normal;
		sprite.HorizontalMirror = hflip ? NullableBoolean::True : NullableBoolean::False;
		sprite.VerticalMirror = vflip ? NullableBoolean::True : NullableBoolean::False;
		sprite.UseExtendedVram = false;
		sprite.UseSecondTable = NullableBoolean::Undefined;

		bool visible = x < (int16_t)vdpState.Width && y < (int16_t)vdpState.Height &&
			(x + (int16_t)width) > 0 && (y + (int16_t)height) > 0;
		sprite.Visibility = visible ? SpriteVisibility::Visible : SpriteVisibility::Offscreen;

		uint32_t tileCount = 0;
		for(uint8_t cx = 0; cx < horizCells && tileCount < 64u; cx++) {
			for(uint8_t cy = 0; cy < vertCells && tileCount < 64u; cy++) {
				uint8_t patternCellX = hflip ? (uint8_t)(horizCells - 1u - cx) : cx;
				uint8_t patternCellY = vflip ? (uint8_t)(vertCells - 1u - cy) : cy;
				uint16_t tileIdx = tile + (uint16_t)(patternCellX * vertCells) + patternCellY;
				sprite.TileAddresses[tileCount++] = tileIdx * tileBytes;
			}
		}
		sprite.TileCount = tileCount;

		for(uint16_t py = 0; py < height; py++) {
			uint8_t cellRow = (uint8_t)(py / cellPixH);
			uint8_t pixRow = (uint8_t)(py % cellPixH);
			uint8_t patternCellY = vflip ? (uint8_t)(vertCells - 1u - cellRow) : cellRow;
			uint8_t row = vflip ? (uint8_t)(cellPixH - 1u - pixRow) : pixRow;

			for(uint16_t px = 0; px < width; px++) {
				uint8_t screenCellCol = (uint8_t)(px >> 3);
				uint8_t patternCellX = hflip ? (uint8_t)(horizCells - 1u - screenCellCol) : screenCellCol;
				uint16_t tileIdx = tile + (uint16_t)(patternCellX * vertCells) + patternCellY;
				uint32_t tileBase = (uint32_t)tileIdx * tileBytes;
				uint8_t col = hflip ? (uint8_t)(7u - (px & 7u)) : (uint8_t)(px & 7u);
				uint8_t color = GetTilePixel(vram, tileBase, row, col);
				if(color != 0) {
					spritePreview[py * width + px] = palette[pal * 16u + color];
				}
			}
		}

	}

	bool prevLineDotOverflow = false;
	uint16_t maxPerLine = h40 ? 20u : 16u;
	uint16_t maxCells = h40 ? 40u : 32u;
	for(uint16_t line = 0; line < vdpState.Height; line++) {
		DebugGenesisLineSprite lineSprites[80] = {};
		DebugGenesisLineSpriteCell lineCells[40] = {};
		uint16_t lineSpriteCount = 0;
		uint16_t lineCellCount = 0;
		bool lineDotOverflow = false;
		uint8_t idx = 0;

		for(uint16_t s = 0; s < spriteCount; s++) {
			uint16_t entryBase = (uint16_t)(spriteBase + (uint16_t)idx * 8u);
			uint16_t w0 = ((uint16_t)vram[(entryBase + 0u) & 0xFFFFu] << 8) | vram[(entryBase + 1u) & 0xFFFFu];
			uint16_t w1 = ((uint16_t)vram[(entryBase + 2u) & 0xFFFFu] << 8) | vram[(entryBase + 3u) & 0xFFFFu];
			uint16_t w2 = ((uint16_t)vram[(entryBase + 4u) & 0xFFFFu] << 8) | vram[(entryBase + 5u) & 0xFFFFu];
			uint16_t w3 = ((uint16_t)vram[(entryBase + 6u) & 0xFFFFu] << 8) | vram[(entryBase + 7u) & 0xFFFFu];
			uint8_t link = (uint8_t)(w1 & 0x7Fu);
			int16_t sprY = (int16_t)(w0 & 0x01FFu) - 128;
			uint8_t vertCells = (uint8_t)(((w1 >> 8) & 0x03u) + 1u);
			uint8_t horizCells = (uint8_t)(((w1 >> 10) & 0x03u) + 1u);
			uint16_t sprH = (uint16_t)vertCells * cellPixH;

			if((int16_t)line >= sprY && (int16_t)line < (int16_t)(sprY + sprH)) {
				if(lineSpriteCount >= maxPerLine) {
					break;
				}

				bool spriteVflip = (w2 & 0x1000u) != 0;
				uint16_t sprRow = (uint16_t)((int16_t)line - sprY);
				uint8_t cellRow = (uint8_t)(sprRow / cellPixH);
				uint8_t pixRow = (uint8_t)(sprRow % cellPixH);

				DebugGenesisLineSprite& sprite = lineSprites[lineSpriteCount++];
				sprite.tile = w2 & 0x07FFu;
				sprite.rawX = w3 & 0x01FFu;
				sprite.x = (int16_t)sprite.rawX - 128;
				sprite.palette = (uint8_t)((w2 >> 13) & 0x03u);
				sprite.vertCells = vertCells;
				sprite.horizCells = horizCells;
				sprite.cellRow = spriteVflip ? (uint8_t)(vertCells - 1u - cellRow) : cellRow;
				sprite.pixRow = pixRow;
				sprite.hflip = (w2 & 0x0800u) != 0;
				sprite.vflip = spriteVflip;
			}

			if(link == 0 || link >= spriteCount) {
				break;
			}
			idx = link;
		}

		bool maskActive = false;
		bool nonMaskCellEncountered = false;
		for(uint16_t i = 0; i < lineSpriteCount; i++) {
			const DebugGenesisLineSprite& sprite = lineSprites[i];
			if(sprite.rawX == 0) {
				if(nonMaskCellEncountered || prevLineDotOverflow) {
					maskActive = true;
				}
				continue;
			}

			nonMaskCellEncountered = true;
			if(maskActive) {
				continue;
			}

			for(uint8_t screenCellCol = 0; screenCellCol < sprite.horizCells; screenCellCol++) {
				if(lineCellCount >= maxCells) {
					lineDotOverflow = true;
					break;
				}

				DebugGenesisLineSpriteCell& cell = lineCells[lineCellCount++];
				cell.tile = sprite.tile;
				cell.x = sprite.x;
				cell.palette = sprite.palette;
				cell.vertCells = sprite.vertCells;
				cell.screenCellCol = screenCellCol;
				cell.patternCellOffsetX = sprite.hflip ? (uint8_t)(sprite.horizCells - 1u - screenCellCol) : screenCellCol;
				cell.patternCellOffsetY = sprite.cellRow;
				cell.pixRow = sprite.pixRow;
				cell.hflip = sprite.hflip;
				cell.vflip = sprite.vflip;
			}

			if(lineDotOverflow) {
				break;
			}
		}

		prevLineDotOverflow = lineDotOverflow;
		uint32_t* outLine = screenPreview + (previewInfo.VisibleY + line) * previewInfo.Width + previewInfo.VisibleX;
		for(uint16_t i = 0; i < lineCellCount; i++) {
			const DebugGenesisLineSpriteCell& cell = lineCells[i];
			uint16_t tileIdx = cell.tile + (uint16_t)(cell.patternCellOffsetX * cell.vertCells) + cell.patternCellOffsetY;
			uint32_t tileBase = (uint32_t)tileIdx * (int2 ? 64u : 32u);
			for(uint8_t px = 0; px < 8u; px++) {
				int16_t screenX = cell.x + (int16_t)(cell.screenCellCol * 8u + px);
				if(screenX < 0 || screenX >= (int16_t)vdpState.Width) {
					continue;
				}

				uint8_t col = cell.hflip ? (uint8_t)(7u - px) : px;
				uint8_t row = cell.vflip ? (uint8_t)(cellPixH - 1u - cell.pixRow) : cell.pixRow;
				uint8_t color = GetTilePixel(vram, tileBase, row, col);
				if(color != 0 && outLine[screenX] == bgColor) {
					outLine[screenX] = palette[cell.palette * 16u + color];
				}
			}
		}
	}

	for(uint16_t i = 0; i < spriteCount; i++) {
		DebugSpriteInfo& sprite = outBuffer[i];
		uint32_t* spritePreview = spritePreviews + (i * _spritePreviewSize);
		for(uint32_t p = 0; p < (uint32_t)sprite.Width * sprite.Height; p++) {
			if(spritePreview[p] == 0) {
				spritePreview[p] = bgColor;
			}
		}
	}
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
