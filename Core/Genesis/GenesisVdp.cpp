#include "pch.h"
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <filesystem>
#include "Genesis/GenesisVdp.h"
#include "Genesis/GenesisNativeBackend.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Utilities/HexUtilities.h"

// ---------------------------------------------------------------------------
// Serialization helpers (local)
// ---------------------------------------------------------------------------
namespace {
	template<typename T>
	void AppV(vector<uint8_t>& out, const T& v)
	{
		size_t old = out.size();
		out.resize(old + sizeof(T));
		memcpy(out.data() + old, &v, sizeof(T));
	}
	template<typename T>
	bool RdV(const vector<uint8_t>& d, size_t& o, T& v)
	{
		if(o + sizeof(T) > d.size()) return false;
		memcpy(&v, d.data() + o, sizeof(T));
		o += sizeof(T);
		return true;
	}

	struct GenesisLineSprite
	{
		uint16_t Tile = 0;
		uint16_t RawX = 0;
		int16_t X = 0;
		uint8_t Palette = 0;
		uint8_t VertCells = 1;
		uint8_t HorizCells = 1;
		uint8_t CellRow = 0;
		uint8_t PixRow = 0;
		bool Priority = false;
		bool HFlip = false;
		bool VFlip = false;
	};

	struct GenesisLineSpriteCell
	{
		uint16_t Tile = 0;
		uint16_t RawX = 0;
		int16_t X = 0;
		uint8_t Palette = 0;
		uint8_t VertCells = 1;
		uint8_t ScreenCellCol = 0;
		uint8_t PatternCellOffsetX = 0;
		uint8_t PatternCellOffsetY = 0;
		uint8_t PixRow = 0;
		bool Priority = false;
		bool HFlip = false;
		bool VFlip = false;
	};

	static constexpr const char* kTraceDirectory = "reference";
	static constexpr const char* kDmaTracePath = "reference/dma_trace.log";
	static constexpr const char* kSpriteTracePath = "reference/sprite_trace.log";
	static constexpr const char* kComposeTracePath = "reference/compose_trace.log";
	static constexpr const char* kScrollTracePath = "reference/scroll_trace.log";
	static constexpr const char* kHScrollDmaTracePath = "reference/hscroll_dma_trace.log";
	static vector<string> sDmaTraceBuffer;
	static vector<string> sSpriteTraceBuffer;
	static vector<string> sComposeTraceBuffer;
	static vector<string> sScrollTraceBuffer;
	static vector<string> sHScrollDmaTraceBuffer;

	static void AppendTraceBufferLine(vector<string>& buffer, const char* line)
	{
		buffer.emplace_back(line ? line : "");
	}

	// Sprite-tile diagnostic trace
	static FILE* sSpriteTraceFile = nullptr;
	static uint32_t sSpriteTraceLines = 0;
	static constexpr uint32_t kSpriteTraceFrameStartDefault = 0u;
	static constexpr uint32_t kSpriteTraceFrameEndDefault = 50u;
	static constexpr uint16_t kSpriteTraceLineStartDefault = 0u;
	static constexpr uint16_t kSpriteTraceLineEndDefault = 50u;
	static constexpr uint32_t kSpriteTraceMaxLinesDefault = 200000u;
	static uint32_t kSpriteTraceFrameStart = kSpriteTraceFrameStartDefault;
	static uint32_t kSpriteTraceFrameEnd = kSpriteTraceFrameEndDefault;
	static uint16_t kSpriteTraceLineStart = kSpriteTraceLineStartDefault;
	static uint16_t kSpriteTraceLineEnd = kSpriteTraceLineEndDefault;
	static uint32_t kSpriteTraceMaxLines = kSpriteTraceMaxLinesDefault;

	static bool SpriteTraceEnabled(uint32_t frame, uint16_t line)
	{
		return frame >= kSpriteTraceFrameStart
			&& frame <= kSpriteTraceFrameEnd
			&& line >= kSpriteTraceLineStart
			&& line <= kSpriteTraceLineEnd
			&& sSpriteTraceLines < kSpriteTraceMaxLines;
	}

	static void SpriteTraceLog(uint32_t frame, uint16_t line, const char* fmt, ...)
	{
		if(!SpriteTraceEnabled(frame, line)) return;
		va_list args;
		va_start(args, fmt);
		va_list argsCopy;
		va_copy(argsCopy, args);
		char msg[1024] = {};
		vsnprintf(msg, sizeof(msg), fmt, argsCopy);
		va_end(argsCopy);
		if(sSpriteTraceFile) {
			fprintf(sSpriteTraceFile, "F%04u L%03u ", frame, line);
			vfprintf(sSpriteTraceFile, fmt, args);
			fputc('\n', sSpriteTraceFile);
		}
		va_end(args);
		char lineBuf[1088] = {};
		snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u %s", frame, line, msg);
		AppendTraceBufferLine(sSpriteTraceBuffer, lineBuf);
		sSpriteTraceLines++;
		if(sSpriteTraceFile && (sSpriteTraceLines & 0x3FFu) == 0u) {
			fflush(sSpriteTraceFile);
		}
	}

	// Final compositor diagnostic trace
	static FILE* sComposeTraceFile = nullptr;
	static uint32_t sComposeTraceLines = 0;
	static constexpr uint32_t kComposeTraceFrameStartDefault = 0u;
	static constexpr uint32_t kComposeTraceFrameEndDefault = 50u;
	static constexpr uint16_t kComposeTraceLineStartDefault = 0u;
	static constexpr uint16_t kComposeTraceLineEndDefault = 50u;
	static constexpr uint16_t kComposeTraceXStartDefault = 0u;
	static constexpr uint16_t kComposeTraceXEndDefault = 50u;
	static constexpr uint32_t kComposeTraceMaxLinesDefault = 250000u;
	static uint32_t kComposeTraceFrameStart = kComposeTraceFrameStartDefault;
	static uint32_t kComposeTraceFrameEnd = kComposeTraceFrameEndDefault;
	static uint16_t kComposeTraceLineStart = kComposeTraceLineStartDefault;
	static uint16_t kComposeTraceLineEnd = kComposeTraceLineEndDefault;
	static uint16_t kComposeTraceXStart = kComposeTraceXStartDefault;
	static uint16_t kComposeTraceXEnd = kComposeTraceXEndDefault;
	static uint32_t kComposeTraceMaxLines = kComposeTraceMaxLinesDefault;

	// Horizontal scroll diagnostic trace
	static FILE* sScrollTraceFile = nullptr;
	static uint32_t sScrollTraceLines = 0;
	static constexpr uint32_t kScrollTraceFrameStartDefault = 0u;
	static constexpr uint32_t kScrollTraceFrameEndDefault = 50u;
	static constexpr uint16_t kScrollTraceLineStartDefault = 0u;
	static constexpr uint16_t kScrollTraceLineEndDefault = 50u;
	static constexpr uint16_t kScrollTraceColumnStartDefault = 0u;
	static constexpr uint16_t kScrollTraceColumnEndDefault = 50u;
	static constexpr uint32_t kScrollTraceMaxLinesDefault = 1000000u;
	static uint32_t kScrollTraceFrameStart = kScrollTraceFrameStartDefault;
	static uint32_t kScrollTraceFrameEnd = kScrollTraceFrameEndDefault;
	static uint16_t kScrollTraceLineStart = kScrollTraceLineStartDefault;
	static uint16_t kScrollTraceLineEnd = kScrollTraceLineEndDefault;
	static uint16_t kScrollTraceColumnStart = kScrollTraceColumnStartDefault;
	static uint16_t kScrollTraceColumnEnd = kScrollTraceColumnEndDefault;
	static uint32_t kScrollTraceMaxLines = kScrollTraceMaxLinesDefault;

	// DMA trace focused on H-scroll table writes (VRAM FC00-FDFF by default)
	static FILE* sHScrollDmaTraceFile = nullptr;
	static uint32_t sHScrollDmaTraceLines = 0;
	static constexpr uint32_t kHScrollDmaTraceFrameStartDefault = 0u;
	static constexpr uint32_t kHScrollDmaTraceFrameEndDefault = 50u;
	static constexpr uint16_t kHScrollDmaTraceDstStartDefault = 0xFC00u;
	static constexpr uint16_t kHScrollDmaTraceDstEndDefault = 0xFDFFu;
	static constexpr uint32_t kHScrollDmaTraceMaxLinesDefault = 300000u;
	static uint32_t kHScrollDmaTraceFrameStart = kHScrollDmaTraceFrameStartDefault;
	static uint32_t kHScrollDmaTraceFrameEnd = kHScrollDmaTraceFrameEndDefault;
	static uint16_t kHScrollDmaTraceDstStart = kHScrollDmaTraceDstStartDefault;
	static uint16_t kHScrollDmaTraceDstEnd = kHScrollDmaTraceDstEndDefault;
	static uint32_t kHScrollDmaTraceMaxLines = kHScrollDmaTraceMaxLinesDefault;

	static bool TryParseEnvU32(const char* name, uint32_t minVal, uint32_t maxVal, uint32_t& outVal)
	{
		const char* raw = std::getenv(name);
		if(!raw || !*raw) return false;

		char* end = nullptr;
		unsigned long v = std::strtoul(raw, &end, 10);
		if(end == raw || *end != '\0') return false;
		if(v < minVal || v > maxVal) return false;

		outVal = (uint32_t)v;
		return true;
	}

	static void LoadTraceConfigFromEnv()
	{
		uint32_t v = 0;
		kSpriteTraceFrameStart = kSpriteTraceFrameStartDefault;
		kSpriteTraceFrameEnd = kSpriteTraceFrameEndDefault;
		kSpriteTraceLineStart = kSpriteTraceLineStartDefault;
		kSpriteTraceLineEnd = kSpriteTraceLineEndDefault;
		kSpriteTraceMaxLines = kSpriteTraceMaxLinesDefault;
		kComposeTraceFrameStart = kComposeTraceFrameStartDefault;
		kComposeTraceFrameEnd = kComposeTraceFrameEndDefault;
		kComposeTraceLineStart = kComposeTraceLineStartDefault;
		kComposeTraceLineEnd = kComposeTraceLineEndDefault;
		kComposeTraceXStart = kComposeTraceXStartDefault;
		kComposeTraceXEnd = kComposeTraceXEndDefault;
		kComposeTraceMaxLines = kComposeTraceMaxLinesDefault;
		kScrollTraceFrameStart = kScrollTraceFrameStartDefault;
		kScrollTraceFrameEnd = kScrollTraceFrameEndDefault;
		kScrollTraceLineStart = kScrollTraceLineStartDefault;
		kScrollTraceLineEnd = kScrollTraceLineEndDefault;
		kScrollTraceColumnStart = kScrollTraceColumnStartDefault;
		kScrollTraceColumnEnd = kScrollTraceColumnEndDefault;
		kScrollTraceMaxLines = kScrollTraceMaxLinesDefault;
		kHScrollDmaTraceFrameStart = kHScrollDmaTraceFrameStartDefault;
		kHScrollDmaTraceFrameEnd = kHScrollDmaTraceFrameEndDefault;
		kHScrollDmaTraceDstStart = kHScrollDmaTraceDstStartDefault;
		kHScrollDmaTraceDstEnd = kHScrollDmaTraceDstEndDefault;
		kHScrollDmaTraceMaxLines = kHScrollDmaTraceMaxLinesDefault;

		if(TryParseEnvU32("MESEN_SPR_FRAME_START", 0u, 0xFFFFFFFFu, v)) kSpriteTraceFrameStart = v;
		if(TryParseEnvU32("MESEN_SPR_FRAME_END",   0u, 0xFFFFFFFFu, v)) kSpriteTraceFrameEnd = v;
		if(TryParseEnvU32("MESEN_SPR_LINE_START",  0u, 0xFFFFu,     v)) kSpriteTraceLineStart = (uint16_t)v;
		if(TryParseEnvU32("MESEN_SPR_LINE_END",    0u, 0xFFFFu,     v)) kSpriteTraceLineEnd = (uint16_t)v;
		if(TryParseEnvU32("MESEN_SPR_MAX_LINES",   1u, 0xFFFFFFFFu, v)) kSpriteTraceMaxLines = v;

		if(TryParseEnvU32("MESEN_CMP_FRAME_START", 0u, 0xFFFFFFFFu, v)) kComposeTraceFrameStart = v;
		if(TryParseEnvU32("MESEN_CMP_FRAME_END",   0u, 0xFFFFFFFFu, v)) kComposeTraceFrameEnd = v;
		if(TryParseEnvU32("MESEN_CMP_LINE_START",  0u, 0xFFFFu,     v)) kComposeTraceLineStart = (uint16_t)v;
		if(TryParseEnvU32("MESEN_CMP_LINE_END",    0u, 0xFFFFu,     v)) kComposeTraceLineEnd = (uint16_t)v;
		if(TryParseEnvU32("MESEN_CMP_X_START",     0u, 0xFFFFu,     v)) kComposeTraceXStart = (uint16_t)v;
		if(TryParseEnvU32("MESEN_CMP_X_END",       0u, 0xFFFFu,     v)) kComposeTraceXEnd = (uint16_t)v;
		if(TryParseEnvU32("MESEN_CMP_MAX_LINES",   1u, 0xFFFFFFFFu, v)) kComposeTraceMaxLines = v;

		if(TryParseEnvU32("MESEN_SCR_FRAME_START", 0u, 0xFFFFFFFFu, v)) kScrollTraceFrameStart = v;
		if(TryParseEnvU32("MESEN_SCR_FRAME_END",   0u, 0xFFFFFFFFu, v)) kScrollTraceFrameEnd = v;
		if(TryParseEnvU32("MESEN_SCR_LINE_START",  0u, 0xFFFFu,     v)) kScrollTraceLineStart = (uint16_t)v;
		if(TryParseEnvU32("MESEN_SCR_LINE_END",    0u, 0xFFFFu,     v)) kScrollTraceLineEnd = (uint16_t)v;
		if(TryParseEnvU32("MESEN_SCR_COL_START",   0u, 0xFFFFu,     v)) kScrollTraceColumnStart = (uint16_t)v;
		if(TryParseEnvU32("MESEN_SCR_COL_END",     0u, 0xFFFFu,     v)) kScrollTraceColumnEnd = (uint16_t)v;
		if(TryParseEnvU32("MESEN_SCR_MAX_LINES",   1u, 0xFFFFFFFFu, v)) kScrollTraceMaxLines = v;
		if(TryParseEnvU32("MESEN_HSDMA_FRAME_START", 0u, 0xFFFFFFFFu, v)) kHScrollDmaTraceFrameStart = v;
		if(TryParseEnvU32("MESEN_HSDMA_FRAME_END",   0u, 0xFFFFFFFFu, v)) kHScrollDmaTraceFrameEnd = v;
		if(TryParseEnvU32("MESEN_HSDMA_DST_START",   0u, 0xFFFFu,     v)) kHScrollDmaTraceDstStart = (uint16_t)v;
		if(TryParseEnvU32("MESEN_HSDMA_DST_END",     0u, 0xFFFFu,     v)) kHScrollDmaTraceDstEnd = (uint16_t)v;
		if(TryParseEnvU32("MESEN_HSDMA_MAX_LINES",   1u, 0xFFFFFFFFu, v)) kHScrollDmaTraceMaxLines = v;

		if(kSpriteTraceFrameStart > kSpriteTraceFrameEnd) {
			std::swap(kSpriteTraceFrameStart, kSpriteTraceFrameEnd);
		}
		if(kSpriteTraceLineStart > kSpriteTraceLineEnd) {
			std::swap(kSpriteTraceLineStart, kSpriteTraceLineEnd);
		}
		if(kComposeTraceFrameStart > kComposeTraceFrameEnd) {
			std::swap(kComposeTraceFrameStart, kComposeTraceFrameEnd);
		}
		if(kComposeTraceLineStart > kComposeTraceLineEnd) {
			std::swap(kComposeTraceLineStart, kComposeTraceLineEnd);
		}
		if(kComposeTraceXStart > kComposeTraceXEnd) {
			std::swap(kComposeTraceXStart, kComposeTraceXEnd);
		}
		if(kScrollTraceFrameStart > kScrollTraceFrameEnd) {
			std::swap(kScrollTraceFrameStart, kScrollTraceFrameEnd);
		}
		if(kScrollTraceLineStart > kScrollTraceLineEnd) {
			std::swap(kScrollTraceLineStart, kScrollTraceLineEnd);
		}
		if(kScrollTraceColumnStart > kScrollTraceColumnEnd) {
			std::swap(kScrollTraceColumnStart, kScrollTraceColumnEnd);
		}
		if(kHScrollDmaTraceFrameStart > kHScrollDmaTraceFrameEnd) {
			std::swap(kHScrollDmaTraceFrameStart, kHScrollDmaTraceFrameEnd);
		}
		if(kHScrollDmaTraceDstStart > kHScrollDmaTraceDstEnd) {
			std::swap(kHScrollDmaTraceDstStart, kHScrollDmaTraceDstEnd);
		}
	}

	static bool ComposeTraceEnabled(uint32_t frame, uint16_t line, uint16_t x)
	{
		return frame >= kComposeTraceFrameStart
			&& frame <= kComposeTraceFrameEnd
			&& line >= kComposeTraceLineStart
			&& line <= kComposeTraceLineEnd
			&& x >= kComposeTraceXStart
			&& x <= kComposeTraceXEnd
			&& sComposeTraceLines < kComposeTraceMaxLines;
	}

	static void ComposeTraceLog(uint32_t frame, uint16_t line, uint16_t x, const char* fmt, ...)
	{
		if(!ComposeTraceEnabled(frame, line, x)) return;
		va_list args;
		va_start(args, fmt);
		va_list argsCopy;
		va_copy(argsCopy, args);
		char msg[1024] = {};
		vsnprintf(msg, sizeof(msg), fmt, argsCopy);
		va_end(argsCopy);
		if(sComposeTraceFile) {
			vfprintf(sComposeTraceFile, fmt, args);
			fputc('\n', sComposeTraceFile);
		}
		va_end(args);
		AppendTraceBufferLine(sComposeTraceBuffer, msg);
		sComposeTraceLines++;
		if(sComposeTraceFile && (sComposeTraceLines & 0x3FFu) == 0u) {
			fflush(sComposeTraceFile);
		}
	}

	static bool ScrollTraceEnabled(uint32_t frame, uint16_t line, int32_t column)
	{
		if(frame < kScrollTraceFrameStart || frame > kScrollTraceFrameEnd) return false;
		if(line < kScrollTraceLineStart || line > kScrollTraceLineEnd) return false;
		if(sScrollTraceLines >= kScrollTraceMaxLines) return false;
		if(column >= 0) {
			uint32_t col = (uint32_t)column;
			if(col < kScrollTraceColumnStart || col > kScrollTraceColumnEnd) return false;
		}
		return true;
	}

	static void ScrollTraceLog(uint32_t frame, uint16_t line, int32_t column, const char* fmt, ...)
	{
		if(!ScrollTraceEnabled(frame, line, column)) return;
		va_list args;
		va_start(args, fmt);
		va_list argsCopy;
		va_copy(argsCopy, args);
		char msg[1024] = {};
		vsnprintf(msg, sizeof(msg), fmt, argsCopy);
		va_end(argsCopy);
		if(sScrollTraceFile) {
			fprintf(sScrollTraceFile, "F%04u L%03u ", frame, line);
			if(column >= 0) {
				fprintf(sScrollTraceFile, "C%02d ", (int)column);
			}
			vfprintf(sScrollTraceFile, fmt, args);
			fputc('\n', sScrollTraceFile);
		}
		va_end(args);
		char lineBuf[1088] = {};
		if(column >= 0) {
			snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u C%02d %s", frame, line, (int)column, msg);
		} else {
			snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u %s", frame, line, msg);
		}
		AppendTraceBufferLine(sScrollTraceBuffer, lineBuf);
		sScrollTraceLines++;
		if(sScrollTraceFile && (sScrollTraceLines & 0x3FFu) == 0u) {
			fflush(sScrollTraceFile);
		}
	}

	static bool HScrollDmaTraceEnabled(uint32_t frame, uint16_t dstAddr)
	{
		if(frame < kHScrollDmaTraceFrameStart || frame > kHScrollDmaTraceFrameEnd) return false;
		if(dstAddr < kHScrollDmaTraceDstStart || dstAddr > kHScrollDmaTraceDstEnd) return false;
		if(sHScrollDmaTraceLines >= kHScrollDmaTraceMaxLines) return false;
		return true;
	}

	static void HScrollDmaTraceLog(uint32_t frame, uint16_t line, const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		va_list argsCopy;
		va_copy(argsCopy, args);
		char msg[1024] = {};
		vsnprintf(msg, sizeof(msg), fmt, argsCopy);
		va_end(argsCopy);
		if(sHScrollDmaTraceFile) {
			fprintf(sHScrollDmaTraceFile, "F%04u L%03u ", frame, line);
			vfprintf(sHScrollDmaTraceFile, fmt, args);
			fputc('\n', sHScrollDmaTraceFile);
		}
		va_end(args);
		char lineBuf[1088] = {};
		snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u %s", frame, line, msg);
		AppendTraceBufferLine(sHScrollDmaTraceBuffer, lineBuf);
		sHScrollDmaTraceLines++;
		if(sHScrollDmaTraceFile && (sHScrollDmaTraceLines & 0x3FFu) == 0u) {
			fflush(sHScrollDmaTraceFile);
		}
	}
}

// ===========================================================================
// Slot tables
// ===========================================================================
// Macros to compactly define column render blocks.
// CRB  = COLUMN_RENDER_BLOCK       (normal: external slot at +1)
// CRBR = COLUMN_RENDER_BLOCK_REFRESH (refresh slot at +1, no DMA src advance)
#define S_OP(name) GenesisVdp::SlotOp::name
#define CRB(col, s) \
	{(uint8_t)(s),   S_OP(ReadMapScrollA), (col)}, \
	{(uint8_t)(s+1), S_OP(ExternalSlot),   -1},    \
	{(uint8_t)(s+2), S_OP(RenderMap1),     -1},    \
	{(uint8_t)(s+3), S_OP(RenderMap2),     -1},    \
	{(uint8_t)(s+4), S_OP(ReadMapScrollB), (col)}, \
	{(uint8_t)(s+5), S_OP(ReadSpriteX),   -1},    \
	{(uint8_t)(s+6), S_OP(RenderMap3),     -1},    \
	{(uint8_t)(s+7), S_OP(RenderMapOutput),(col)}
#define CRBR(col, s) \
	{(uint8_t)(s),   S_OP(ReadMapScrollA), (col)}, \
	{(uint8_t)(s+1), S_OP(Refresh),        -1},    \
	{(uint8_t)(s+2), S_OP(RenderMap1),     -1},    \
	{(uint8_t)(s+3), S_OP(RenderMap2),     -1},    \
	{(uint8_t)(s+4), S_OP(ReadMapScrollB), (col)}, \
	{(uint8_t)(s+5), S_OP(ReadSpriteX),   -1},    \
	{(uint8_t)(s+6), S_OP(RenderMap3),     -1},    \
	{(uint8_t)(s+7), S_OP(RenderMapOutput),(col)}
#define SPR(s)  {(uint8_t)(s), S_OP(SpriteRender),  -1}
#define EXT(s)  {(uint8_t)(s), S_OP(ExternalSlot),  -1}
#define NOP_(s) {(uint8_t)(s), S_OP(Nop),           -1}

// H40 mode: 210 slots per line, 16 mclk/slot = 3360 mclk (remaining 60 mclk is HSYNC overhead).
// Slot ordering follows BlastEM's vdp_h40 with column prefetch starting at hslot 249.
// Columns 0-19 at 8 slots each = 160 column slots, plus sprite/external/hsync/border slots.
const GenesisVdp::SlotDescriptor GenesisVdp::kSlotTableH40[SLOT_COUNT_H40] = {
	// --- Column 0 prefetch (hslots 249-255, slot indices 0-6) ---
	{249, S_OP(ReadMapScrollA), 0},
	SPR(250),
	{251, S_OP(RenderMap1),     -1},
	{252, S_OP(RenderMap2),     -1},
	{253, S_OP(ReadMapScrollB), 0},
	SPR(254),
	{255, S_OP(RenderMap3),     -1},
	// --- Column 0 output + columns 2-19 (hslots 0-159, slot indices 7-166) ---
	{0, S_OP(RenderMapOutput),  0},
	CRB (2,  1),     // col 2  @ hslots 1-8
	CRBR(4,  9),     // col 4  @ hslots 9-16   (refresh at +1)
	CRB (6,  17),    // col 6  @ hslots 17-24
	CRB (8,  25),    // col 8  @ hslots 25-32
	CRBR(10, 33),    // col 10 @ hslots 33-40  (refresh)
	CRB (12, 41),    // col 12 @ hslots 41-48
	CRB (14, 49),    // col 14 @ hslots 49-56
	CRBR(16, 57),    // col 16 @ hslots 57-64  (refresh)
	CRB (18, 65),    // col 18 @ hslots 65-72
	CRB (20, 73),    // col 20 @ hslots 73-80
	CRBR(22, 81),    // col 22 @ hslots 81-88  (refresh)
	CRB (24, 89),    // col 24 @ hslots 89-96
	CRB (26, 97),    // col 26 @ hslots 97-104
	CRBR(28, 105),   // col 28 @ hslots 105-112 (refresh)
	CRB (30, 113),   // col 30 @ hslots 113-120
	CRB (32, 121),   // col 32 @ hslots 121-128
	CRBR(34, 129),   // col 34 @ hslots 129-136 (refresh)
	CRB (36, 137),   // col 36 @ hslots 137-144
	CRB (38, 145),   // col 38 @ hslots 145-152
	// --- End of active display, remaining slots 167-210 ---
	EXT(153), EXT(154), EXT(155), EXT(156),   // external slots
	{157, S_OP(HScrollLoad), -1},             // load hscroll for next line
	EXT(158),
	{159, S_OP(ClearLinebuf), -1},
	// Sprite scan / render / border (hslots 160-182, then HSYNC jump to 229)
	SPR(160), SPR(161), SPR(162), SPR(163),
	SPR(164), SPR(165), SPR(166), SPR(167),
	SPR(168), SPR(169), SPR(170), SPR(171),
	SPR(172), SPR(173), SPR(174), SPR(175),
	SPR(176), SPR(177), SPR(178), SPR(179),
	SPR(180), SPR(181), SPR(182),
	// After HSYNC jump: hslots 229-248
	EXT(229), EXT(230), EXT(231), EXT(232),
	SPR(233), SPR(234), SPR(235), SPR(236),
	SPR(237), SPR(238), SPR(239), SPR(240),
	SPR(241), SPR(242), SPR(243), EXT(244),
	EXT(245), EXT(246), EXT(247), EXT(248),
};

// H32 mode: 171 slots per line, 20 mclk/slot = 3420 mclk.
const GenesisVdp::SlotDescriptor GenesisVdp::kSlotTableH32[SLOT_COUNT_H32] = {
	// --- Column 0 prefetch (hslots 244-249, slot indices 0-6) ---
	{244, S_OP(ReadMapScrollA), 0},
	SPR(245),
	{246, S_OP(RenderMap1),     -1},
	{247, S_OP(RenderMap2),     -1},
	{248, S_OP(ReadMapScrollB), 0},
	SPR(249),
	{250, S_OP(RenderMap3),     -1},
	// --- Column 0 output + columns 2-15 (hslots 0-127, slot indices 7-134) ---
	{0, S_OP(RenderMapOutput),  0},
	CRB (2,  1),     // col 2
	CRBR(4,  9),     // col 4 (refresh)
	CRB (6,  17),    // col 6
	CRB (8,  25),    // col 8
	CRBR(10, 33),    // col 10 (refresh)
	CRB (12, 41),    // col 12
	CRB (14, 49),    // col 14
	CRBR(16, 57),    // col 16 (refresh)
	CRB (18, 65),    // col 18
	CRB (20, 73),    // col 20
	CRBR(22, 81),    // col 22 (refresh)
	CRB (24, 89),    // col 24
	CRB (26, 97),    // col 26
	CRBR(28, 105),   // col 28 (refresh)
	CRB (30, 113),   // col 30
	// --- End of active display (slot indices 135-170) ---
	EXT(121), EXT(122), EXT(123), EXT(124),
	{125, S_OP(HScrollLoad), -1},
	EXT(126),
	{127, S_OP(ClearLinebuf), -1},
	// Sprite scan / render (hslots 128-147, then HSYNC jump to 233)
	SPR(128), SPR(129), SPR(130), SPR(131),
	SPR(132), SPR(133), SPR(134), SPR(135),
	SPR(136), SPR(137), SPR(138), SPR(139),
	SPR(140), SPR(141), SPR(142), SPR(143),
	SPR(144), SPR(145), SPR(146), SPR(147),
	// After HSYNC jump: hslots 233-246
	EXT(233), EXT(234), EXT(235), EXT(236),
	SPR(237), SPR(238), SPR(239), SPR(240),
	SPR(241), SPR(242), SPR(243), EXT(244),
	EXT(245), EXT(246), EXT(247), EXT(248),
};

#undef S_OP
#undef CRB
#undef CRBR
#undef SPR
#undef EXT
#undef NOP_

// ===========================================================================
// H counter lookup tables (mclk-in-line → 8-bit H counter value)
//
// The H counter (hslot) is NOT a linear function of master clock position.
// In H40 mode, hslot 182 is followed by 229 (HSYNC jump), and slots 230-246
// have variable timing (19/20/18 mclk instead of the usual 16).
// In H32 mode, hslot 147 is followed by 233 (HSYNC jump), all slots 20 mclk.
//
// Our line starts at mclk offset 0 = the line-change boundary, which is
// hslot 165 (H40) or hslot 133 (H32) in BlastEM terminology.
// ===========================================================================

static const uint32_t h40_hsync_cycles[] = {
	19, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 19
};

static uint8_t sHCounterH40[GenesisVdp::MCLKS_PER_LINE];
static uint8_t sHCounterH32[GenesisVdp::MCLKS_PER_LINE];
static bool sHCounterTablesInitialized = false;

static void BuildHCounterTables()
{
	if(sHCounterTablesInitialized) return;
	sHCounterTablesInitialized = true;

	// --- H40 ---
	{
		uint32_t mclk = 0;
		uint8_t hslot = 165;
		while(mclk < 3420) {
			uint32_t cyc;
			if(hslot >= 230 && hslot <= 246) {
				cyc = h40_hsync_cycles[hslot - 230];
			} else {
				cyc = 16;
			}
			for(uint32_t c = 0; c < cyc && mclk + c < 3420; c++) {
				sHCounterH40[mclk + c] = hslot;
			}
			mclk += cyc;
			if(hslot == 182) {
				hslot = 229;
			} else if(hslot == 255) {
				hslot = 0;
			} else {
				hslot++;
			}
		}
	}

	// --- H32 ---
	{
		uint32_t mclk = 0;
		uint8_t hslot = 133;
		while(mclk < 3420) {
			uint32_t cyc = 20;
			for(uint32_t c = 0; c < cyc && mclk + c < 3420; c++) {
				sHCounterH32[mclk + c] = hslot;
			}
			mclk += cyc;
			if(hslot == 147) {
				hslot = 233;
			} else if(hslot == 255) {
				hslot = 0;
			} else {
				hslot++;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// V counter value for a given scanline (handles non-linear wrap)
// ---------------------------------------------------------------------------
uint8_t GenesisVdp::VCounterValue(uint32_t scanline) const
{
	// NTSC V28 (224 lines): 0x00-0xEA, then jumps 0xEB→0x1E5
	// PAL  V28 (224 lines): 0x00-0x102, then jumps 0x103→0x1CA
	// PAL  V30 (240 lines): 0x00-0x10A, then jumps 0x10B→0x1D2
	uint16_t vc = (uint16_t)scanline;

	if(!_isPal) {
		// NTSC: 262 lines. V counter jumps from 0xEA (234) to 0x1E5 (485).
		if(scanline > 234u) {
			vc = (uint16_t)(0x1E5u + (scanline - 235u));
		}
	} else {
		if(IsV30()) {
			// PAL V30: jumps from 0x10A (266) to 0x1D2 (466)
			if(scanline > 266u) {
				vc = (uint16_t)(0x1D2u + (scanline - 267u));
			}
		} else {
			// PAL V28: jumps from 0x102 (258) to 0x1CA (458)
			if(scanline > 258u) {
				vc = (uint16_t)(0x1CAu + (scanline - 259u));
			}
		}
	}

	return (uint8_t)(vc & 0xFFu);
}

// ---------------------------------------------------------------------------
// ReadHVCounter — returns the 16-bit HV counter value
// ---------------------------------------------------------------------------
uint16_t GenesisVdp::ReadHVCounter() const
{
	// If HV latch is enabled (R0 bit 1), return latched value
	if(_reg[0] & 0x02u) {
		return _hvLatch;
	}

	uint32_t mclkInLine = _mclkPos % MCLKS_PER_LINE;
	uint32_t scanline   = _mclkPos / MCLKS_PER_LINE;

	uint8_t hc = IsH40() ? sHCounterH40[mclkInLine] : sHCounterH32[mclkInLine];
	uint8_t vc = VCounterValue(scanline);

	// Interlace: V counter is doubled, with field bit in LSB
	if(IsInterlace()) {
		uint16_t vc16 = (uint16_t)vc;
		if(IsInterlace2()) {
			vc16 <<= 1;
		} else {
			vc16 &= 0x1FEu;
		}
		if(vc16 & 0x100u) {
			vc16 |= 1u;
		}
		vc = (uint8_t)(vc16 & 0xFFu);
	}

	return ((uint16_t)vc << 8) | hc;
}

// ===========================================================================
// Slot operation implementations
// ===========================================================================

void GenesisVdp::DispatchSlot(const SlotDescriptor& slot)
{
	switch(slot.op) {
		case SlotOp::ReadMapScrollA:  SlotReadMapScrollA(slot.column); break;
		case SlotOp::ExternalSlot:    SlotExternalSlot(); break;
		case SlotOp::RenderMap1:      SlotRenderMap1(); break;
		case SlotOp::RenderMap2:      SlotRenderMap2(); break;
		case SlotOp::ReadMapScrollB:  SlotReadMapScrollB(slot.column); break;
		case SlotOp::ReadSpriteX:     SlotReadSpriteX(); break;
		case SlotOp::RenderMap3:      SlotRenderMap3(); break;
		case SlotOp::RenderMapOutput: SlotRenderMapOutput(slot.column); break;
		case SlotOp::SpriteRender:    SlotSpriteRender(); break;
		case SlotOp::HScrollLoad:     SlotHScrollLoad(); break;
		case SlotOp::Refresh:         break; // no-op (DRAM refresh)
		case SlotOp::ClearLinebuf:    SlotClearLinebuf(); break;
		case SlotOp::Nop:             break;
	}
}

// --- Render a tile row (8 pixels) into dst[0..7] from a nametable entry ---
// Render 8 pixels from a nametable entry into a circular buffer at offset `off`.
// bufMask should be SCROLL_BUF_MASK (31) for a 32-byte circular buffer.
static void RenderTileRowCirc(const uint8_t* vram, uint16_t nameEntry, uint8_t vOffset,
                              bool interlace2, uint8_t* buf, uint8_t off, uint8_t bufMask)
{
	bool     pri   = (nameEntry >> 15) & 1;
	uint8_t  pal   = (uint8_t)((nameEntry >> 13) & 3u);
	bool     vflip = (nameEntry >> 12) & 1;
	bool     hflip = (nameEntry >> 11) & 1;
	uint16_t tile  = nameEntry & 0x7FFu;

	uint16_t tilePixH = interlace2 ? 16u : 8u;
	uint8_t  row = vflip ? (uint8_t)(tilePixH - 1u - vOffset) : vOffset;
	uint16_t tileBase = interlace2 ? (uint16_t)(tile * 64u) : (uint16_t)(tile * 32u);
	uint16_t byteAddr = (tileBase + (uint16_t)row * 4u) & 0xFFFFu;

	uint8_t priPal = (uint8_t)((pri ? 0x80u : 0x00u) | (pal << 4));

	for(uint8_t px = 0; px < 8; px++) {
		uint8_t col = hflip ? (uint8_t)(7u - px) : px;
		uint8_t b = vram[(byteAddr + (col >> 1)) & 0xFFFFu];
		uint8_t nib = (col & 1u) ? (b & 0x0Fu) : (b >> 4);
		buf[(off + px) & bufMask] = (uint8_t)(priPal | nib);
	}
}

void GenesisVdp::SlotReadMapScrollA(int16_t column)
{
	uint16_t line = _scanline;
	bool int2 = IsInterlace2();
	uint16_t tilePixH = int2 ? 16u : 8u;

	// Determine if this column is window
	uint16_t colPixX = (uint16_t)(column < 0 ? 0 : column) * 8u;
	_windowActive = IsWindowPixel(line, colPixX);

	if(_windowActive) {
		// Window nametable
		uint16_t nameBase = WindowBase();
		uint16_t cellW = IsH40() ? 64u : 32u;
		uint32_t effLine = int2 ? ((uint32_t)line * 2u + (_interlaceField ? 1u : 0u)) : (uint32_t)line;
		uint16_t winLine = (uint16_t)(effLine / tilePixH);
		uint16_t pixRow = (uint16_t)(effLine % tilePixH);

		uint16_t tc1 = (uint16_t)column;
		uint16_t tc2 = (uint16_t)(column + 1u);
		uint16_t addr1 = (uint16_t)(nameBase + (winLine * cellW + tc1) * 2u) & 0xFFFFu;
		uint16_t addr2 = (uint16_t)(nameBase + (winLine * cellW + tc2) * 2u) & 0xFFFFu;

		_col1 = ((uint16_t)_vram[addr1] << 8) | _vram[(addr1 + 1u) & 0xFFFFu];
		_col2 = ((uint16_t)_vram[addr2] << 8) | _vram[(addr2 + 1u) & 0xFFFFu];
		_vOffsetA = (uint8_t)pixRow;
		ScrollTraceLog(_frameCount, _scanline, column,
			"HS_A_WIN mode=%u effLine=%u winLine=%u rowPix=%u tc1=%u tc2=%u addr1=%04X addr2=%04X col1=%04X col2=%04X hA=%03X fineA=%u",
			(unsigned)(_reg[11] & 0x03u), (unsigned)effLine, (unsigned)winLine, (unsigned)_vOffsetA,
			(unsigned)tc1, (unsigned)tc2, (unsigned)addr1, (unsigned)addr2,
			(unsigned)_col1, (unsigned)_col2, (unsigned)_hscrollA, (unsigned)_hscrollAFine);
	} else {
		// Scroll A nametable
		uint16_t planeW = PlaneWidthTiles();
		uint16_t planeH = PlaneHeightTiles();
		uint16_t nameBase = ScrollABase();
		uint32_t planePxH = (uint32_t)planeH * tilePixH;

		// Latch vscroll for this column pair
		uint16_t tileCol2 = (uint16_t)((uint16_t)column >> 1);
		uint16_t vscroll = GetVScroll(tileCol2, true);
		_vscrollLatch[0] = vscroll;

		// In interlace mode 2, tiles are 16 rows tall and the effective
		// line includes the field offset: effLine = line * 2 + field.
		uint32_t effLine = int2 ? ((uint32_t)line * 2u + (_interlaceField ? 1u : 0u)) : (uint32_t)line;
		uint32_t effY = (effLine + vscroll) % planePxH;
		uint16_t tileRow = (uint16_t)(effY / tilePixH) & (planeH - 1u);
		_vOffsetA = (uint8_t)(effY % tilePixH);

		// Fetch indexing:
		//   hscroll = (column - 2 + i - ((hscroll_val/8) & 0xFFFE)) & mask
		// where mask depends on scroll width mode (R16 low 2 bits).
		static constexpr uint16_t hscrollMasks[4] = { 0x1Fu, 0x3Fu, 0x1Fu, 0x7Fu };
		uint8_t mapMode = (uint8_t)(_reg[16] & 0x03u);
		uint16_t hscrollMask = hscrollMasks[mapMode];
		uint16_t hbase = (uint16_t)(((_hscrollA / 8u) & 0xFFFEu));
		uint16_t tc1 = (uint16_t)(((int32_t)column - 2 - (int32_t)hbase) & (int32_t)hscrollMask);
		uint16_t tc2 = (uint16_t)(((int32_t)column - 1 - (int32_t)hbase) & (int32_t)hscrollMask);
		int32_t debugPx1 = ((int32_t)column - 2) * 8 - (int32_t)(hbase * 8u);
		int32_t debugPx2 = debugPx1 + 8;

		uint16_t addr1 = (uint16_t)(nameBase + (tileRow * planeW + tc1) * 2u) & 0xFFFFu;
		uint16_t addr2 = (uint16_t)(nameBase + (tileRow * planeW + tc2) * 2u) & 0xFFFFu;
		_col1 = ((uint16_t)_vram[addr1] << 8) | _vram[(addr1 + 1u) & 0xFFFFu];
		_col2 = ((uint16_t)_vram[addr2] << 8) | _vram[(addr2 + 1u) & 0xFFFFu];
		ScrollTraceLog(_frameCount, _scanline, column,
			"HS_A_MAP mode=%u effLine=%u vA=%03X tileRow=%u rowPix=%u hA=%04X fineA=%u spx1=%d spx2=%d tc1=%u tc2=%u addr1=%04X addr2=%04X col1=%04X col2=%04X",
			(unsigned)mapMode, (unsigned)effLine, (unsigned)vscroll, (unsigned)tileRow, (unsigned)_vOffsetA,
			(unsigned)_hscrollA, (unsigned)_hscrollAFine,
			(int)debugPx1, (int)debugPx2, (unsigned)tc1, (unsigned)tc2,
			(unsigned)addr1, (unsigned)addr2, (unsigned)_col1, (unsigned)_col2);
	}
}

void GenesisVdp::SlotReadMapScrollB(int16_t column)
{
	uint16_t line = _scanline;
	bool int2 = IsInterlace2();
	uint16_t tilePixH = int2 ? 16u : 8u;
	uint16_t planeW = PlaneWidthTiles();
	uint16_t planeH = PlaneHeightTiles();
	uint16_t nameBase = ScrollBBase();
	uint32_t planePxH = (uint32_t)planeH * tilePixH;

	uint16_t tileCol2 = (uint16_t)((uint16_t)column >> 1);
	uint16_t vscroll = GetVScroll(tileCol2, false);
	_vscrollLatch[1] = vscroll;

	uint32_t effLine = int2 ? ((uint32_t)line * 2u + (_interlaceField ? 1u : 0u)) : (uint32_t)line;
	uint32_t effY = (effLine + vscroll) % planePxH;
	uint16_t tileRow = (uint16_t)(effY / tilePixH) & (planeH - 1u);
	_vOffsetB = (uint8_t)(effY % tilePixH);

	static constexpr uint16_t hscrollMasks[4] = { 0x1Fu, 0x3Fu, 0x1Fu, 0x7Fu };
	uint8_t mapMode = (uint8_t)(_reg[16] & 0x03u);
	uint16_t hscrollMask = hscrollMasks[mapMode];
	uint16_t hbase = (uint16_t)(((_hscrollB / 8u) & 0xFFFEu));
	uint16_t tc1 = (uint16_t)(((int32_t)column - 2 - (int32_t)hbase) & (int32_t)hscrollMask);
	uint16_t tc2 = (uint16_t)(((int32_t)column - 1 - (int32_t)hbase) & (int32_t)hscrollMask);
	int32_t debugPx1 = ((int32_t)column - 2) * 8 - (int32_t)(hbase * 8u);
	int32_t debugPx2 = debugPx1 + 8;

	uint16_t addr1 = (uint16_t)(nameBase + (tileRow * planeW + tc1) * 2u) & 0xFFFFu;
	uint16_t addr2 = (uint16_t)(nameBase + (tileRow * planeW + tc2) * 2u) & 0xFFFFu;
	_colB1 = ((uint16_t)_vram[addr1] << 8) | _vram[(addr1 + 1u) & 0xFFFFu];
	_colB2 = ((uint16_t)_vram[addr2] << 8) | _vram[(addr2 + 1u) & 0xFFFFu];
	ScrollTraceLog(_frameCount, _scanline, column,
		"HS_B_MAP mode=%u effLine=%u vB=%03X tileRow=%u rowPix=%u hB=%04X fineB=%u spx1=%d spx2=%d tc1=%u tc2=%u addr1=%04X addr2=%04X col1=%04X col2=%04X",
		(unsigned)mapMode, (unsigned)effLine, (unsigned)vscroll, (unsigned)tileRow, (unsigned)_vOffsetB,
		(unsigned)_hscrollB, (unsigned)_hscrollBFine,
		(int)debugPx1, (int)debugPx2, (unsigned)tc1, (unsigned)tc2,
		(unsigned)addr1, (unsigned)addr2, (unsigned)_colB1, (unsigned)_colB2);
}

void GenesisVdp::SlotRenderMap1()
{
	bool int2 = IsInterlace2();
	RenderTileRowCirc(_vram, _col1, _vOffsetA, int2, _tmpBufA, _bufAOff, SCROLL_BUF_MASK);
}

void GenesisVdp::SlotRenderMap2()
{
	bool int2 = IsInterlace2();
	RenderTileRowCirc(_vram, _col2, _vOffsetA, int2, _tmpBufA, (uint8_t)(_bufAOff + 8u), SCROLL_BUF_MASK);
}

void GenesisVdp::SlotRenderMap3()
{
	bool int2 = IsInterlace2();
	RenderTileRowCirc(_vram, _colB1, _vOffsetB, int2, _tmpBufB, _bufBOff, SCROLL_BUF_MASK);
}

void GenesisVdp::SlotRenderMapOutput(int16_t column)
{
	// Render col_2 of plane B, then composite 16 pixels
	bool int2 = IsInterlace2();
	RenderTileRowCirc(_vram, _colB2, _vOffsetB, int2, _tmpBufB, (uint8_t)(_bufBOff + 8u), SCROLL_BUF_MASK);

	// Pipeline phase:
	// column 0 is a prefetch/border phase (no active-area output in this simplified path),
	// but buffer offsets still advance at the end of the slot.
	if(column <= 0) {
		_bufAOff = (_bufAOff + 16u) & SCROLL_BUF_MASK;
		_bufBOff = (_bufBOff + 16u) & SCROLL_BUF_MASK;
		return;
	}

	// Composite 16 pixels: sprite (linebuf), plane A (tmpBufA), plane B (tmpBufB)
	uint16_t outX = (uint16_t)(column - 2) * 8u;
	int16_t aSampleOff = _windowActive ? (int16_t)_bufAOff : (int16_t)_bufAOff - (int16_t)_hscrollAFine;
	int16_t bSampleOff = (int16_t)_bufBOff - (int16_t)_hscrollBFine;
	ScrollTraceLog(_frameCount, _scanline, column,
		"HS_OUT outX=%u hA=%03X hB=%03X fineA=%u fineB=%u bufAOff=%d bufBOff=%d winCol=%u",
		(unsigned)outX, (unsigned)_hscrollA, (unsigned)_hscrollB,
		(unsigned)_hscrollAFine, (unsigned)_hscrollBFine,
		(int)aSampleOff, (int)bSampleOff, (unsigned)(_windowActive ? 1u : 0u));
	uint16_t width = ActiveWidth();
	bool shMode = ShadowHlEnabled();
	uint8_t bgIdx = _reg[7] & 0x3Fu;

	enum : uint8_t { Shade_Shadow = 0, Shade_Normal = 1, Shade_Highlight = 2 };

	for(uint8_t i = 0; i < 16; i++) {
		uint16_t px = outX + i;
		if(px >= width) break;

		uint8_t pA = _tmpBufA[(uint8_t)(aSampleOff + i) & SCROLL_BUF_MASK];
		uint8_t pB = _tmpBufB[(uint8_t)(bSampleOff + i) & SCROLL_BUF_MASK];
		uint8_t pS = (px < LINEBUF_SIZE) ? _linebuf[px] : 0u;

		// Window source: if this column was flagged as window, pA has window flag
		bool winSrc = _windowActive;
		bool winVis = winSrc && ((pA & 0x0Fu) != 0);
		bool winHi  = winSrc && ((pA & 0x80u) != 0);
		bool sprHi  = (pS & 0x80u) != 0;
		bool sprVis = (pS & 0x0Fu) != 0;
		bool pAHi   = !winSrc && ((pA & 0x80u) != 0);
		bool pAVis  = !winSrc && ((pA & 0x0Fu) != 0);
		bool pBHi   = (pB & 0x80u) != 0;
		bool pBVis  = (pB & 0x0Fu) != 0;

		// Shadow/highlight shade
		uint8_t shade = Shade_Normal;
		bool sprIsOp = false;
		if(shMode) {
			if((winHi && winVis) || (pAHi && pAVis) || (pBHi && pBVis))
				shade = Shade_Normal;
			if(sprVis) {
				uint8_t sprPal = (pS >> 4) & 3u;
				uint8_t sprColor = pS & 0x0Fu;
				if(!sprHi && sprPal == 3u) {
					if(sprColor == 14u) { shade = Shade_Shadow; sprIsOp = true; }
					else if(sprColor == 15u) { shade = (shade == Shade_Shadow) ? Shade_Normal : Shade_Highlight; sprIsOp = true; }
				} else if(sprHi) {
					shade = Shade_Normal;
				} else if(sprColor == 15u) {
					shade = Shade_Normal;
				}
			}
		}

		// Priority chain
		uint8_t cramIdx = bgIdx;
		char src = 'G';
		if      (winHi && winVis)               { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); src = 'W'; }
		else if (sprHi && sprVis && !sprIsOp)   { cramIdx = (uint8_t)(((pS >> 4) & 3u) * 16u + (pS & 0x0Fu)); src = 'S'; }
		else if (pAHi  && pAVis)                { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); src = 'A'; }
		else if (pBHi  && pBVis)                { cramIdx = (uint8_t)(((pB >> 4) & 3u) * 16u + (pB & 0x0Fu)); src = 'B'; }
		else if (!winHi && winVis)              { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); src = 'W'; }
		else if (!sprHi && sprVis && !sprIsOp)  { cramIdx = (uint8_t)(((pS >> 4) & 3u) * 16u + (pS & 0x0Fu)); src = 'S'; }
		else if (!pAHi  && pAVis)               { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); src = 'A'; }
		else if (!pBHi  && pBVis)               { cramIdx = (uint8_t)(((pB >> 4) & 3u) * 16u + (pB & 0x0Fu)); src = 'B'; }

		cramIdx &= 0x3Fu;

		// Encode shade into compositebuf: bits 7:6 = shade, bits 5:0 = cramIdx
		uint8_t enc = (uint8_t)((shade << 6) | cramIdx);
		_compositebuf[px] = enc;

		bool winPix = IsWindowPixel(_scanline, px);
		ComposeTraceLog(_frameCount, _scanline, px,
			"CMP f=%04u l=%03u x=%03u pA=%02X pB=%02X pS=%02X winCol=%u winPix=%u src=%c shade=%u idx=%02X enc=%02X",
			(unsigned)_frameCount, (unsigned)_scanline, (unsigned)px,
			(unsigned)pA, (unsigned)pB, (unsigned)pS,
			(unsigned)(_windowActive ? 1u : 0u), (unsigned)(winPix ? 1u : 0u),
			src, (unsigned)shade, (unsigned)cramIdx, (unsigned)enc);
	}

	// Advance circular buffers
	_bufAOff = (_bufAOff + 16u) & SCROLL_BUF_MASK;
	_bufBOff = (_bufBOff + 16u) & SCROLL_BUF_MASK;
}

void GenesisVdp::SlotScanSpriteTable()
{
	if(_sprScanDone) return;

	bool int2 = IsInterlace2();
	uint16_t maxSprites = IsH40() ? 80u : 64u;
	uint16_t cellPixH = int2 ? 16u : 8u;
	uint32_t line = (uint32_t)_scanline;

	uint8_t idx = _sprScanLink;
	// Read Y/link/size from the SAT cache (snapshot from previous EndLine),
	// not live VRAM, so that mid-line 68K SAT rewrites don't shift the scan.
	uint16_t cacheOff = (uint16_t)idx * 8u;
	uint16_t w0 = ((uint16_t)_satCache[cacheOff + 0u] << 8) | _satCache[cacheOff + 1u];
	uint16_t w1 = ((uint16_t)_satCache[cacheOff + 2u] << 8) | _satCache[cacheOff + 3u];

	int16_t sprY = (int16_t)(w0 & 0x01FFu) - 128;
	uint8_t vertCells = (uint8_t)(((w1 >> 8) & 0x03u) + 1u);
	uint8_t horizCells = (uint8_t)(((w1 >> 10) & 0x03u) + 1u);
	uint8_t link = (uint8_t)(w1 & 0x7Fu);
	uint16_t sprH = (uint16_t)vertCells * cellPixH;

	if((int32_t)line >= sprY && (int32_t)line < (int32_t)(sprY + sprH)) {
		if(_sprInfoCount >= _maxSpritesLine) {
			_status |= 0x0040u;
			_sprScanDone = true;
			return;
		}
		SpriteInfo& info = _spriteInfoList[_sprInfoCount++];
		info.index = idx;
		info.y = sprY;
		// Keep SAT size nibble in raw form (HHVV where each field is size-1),
		// then decode with +1 when building draw entries.
		info.size = (uint8_t)((w1 >> 8) & 0x0Fu);
	}

	if(link == 0 || link >= maxSprites) {
		_sprScanDone = true;
		return;
	}
	_sprScanLink = link;
}

void GenesisVdp::SlotReadSpriteX()
{
	// Circular wrap: BlastEM starts at _sprCurSlot = slot_counter (from scan),
	// wraps at _maxSpritesLine, then processes 0..slot_counter-1.
	if(_sprCurSlot == (int8_t)_maxSpritesLine) {
		_sprCurSlot = 0;
	}

	if(_sprCurSlot < (int8_t)_sprInfoCount) {
		bool int2 = IsInterlace2();
		uint16_t cellPixH = int2 ? 16u : 8u;
		const SpriteInfo& info = _spriteInfoList[_sprCurSlot];

		// Read tile/X from SAT cache (same snapshot as the Y/link scan).
		uint16_t cacheOff = (uint16_t)(info.index * 8u);
		uint16_t w2 = ((uint16_t)_satCache[cacheOff + 4u] << 8) | _satCache[cacheOff + 5u];
		uint16_t w3 = ((uint16_t)_satCache[cacheOff + 6u] << 8) | _satCache[cacheOff + 7u];

		uint16_t xPos = w3 & 0x01FFu;
		uint8_t vertCells = (uint8_t)((info.size & 0x03u) + 1u);
		uint8_t horizCells = (uint8_t)(((info.size >> 2) & 0x03u) + 1u);
		bool vflip = (w2 & 0x1000u) != 0;
		uint16_t tile = w2 & 0x07FFu;

		uint16_t sprRow = (uint16_t)((int16_t)_scanline - info.y);
		uint8_t cellRow = (uint8_t)(sprRow / cellPixH);
		uint8_t pixRow = (uint8_t)(sprRow % cellPixH);
		if(vflip) cellRow = (uint8_t)(vertCells - 1u - cellRow);
		if(int2) pixRow = (uint8_t)(pixRow * 2u + (_interlaceField ? 1u : 0u));
		if(vflip && !int2) pixRow = (uint8_t)(7u - pixRow);
		else if(vflip && int2) pixRow = (uint8_t)(15u - pixRow);

		if(_sprDraws > 0) {
			_sprDraws--;
			SpriteDraw& draw = _spriteDrawList[_sprDraws];
			draw.xPos = (int16_t)xPos - 128;
			draw.width = horizCells;
			draw.height = vertCells;
			draw.hFlip = (w2 & 0x0800u) ? 1u : 0u;
			draw.palPri = (uint8_t)(((w2 & 0x8000u) ? 0x80u : 0x00u) | (((w2 >> 13) & 0x03u) << 4));
			draw.baseTile = tile;
			draw.cellRow = cellRow;
			draw.pixRow = pixRow;
			draw.satIndex = (uint8_t)info.index;
			{
				uint16_t firstCol = draw.hFlip ? (uint16_t)(horizCells - 1u) : 0u;
				uint16_t tileIdx = tile + firstCol * vertCells + cellRow;
				uint16_t patBase = int2 ? (uint16_t)(tileIdx * 64u) : (uint16_t)(tileIdx * 32u);
				draw.address = (uint16_t)(patBase + pixRow * 4u);
				SpriteTraceLog(_frameCount, _scanline,
					"SPR_SETUP sat=%u xRaw=%03X x=%d size=%ux%u tile=%03X rowCell=%u rowPix=%u hflip=%u vflip=%u palPri=%02X firstCol=%u firstTile=%03X pat=%04X",
					(unsigned)info.index, (unsigned)xPos, (int)draw.xPos, (unsigned)horizCells, (unsigned)vertCells,
					(unsigned)tile, (unsigned)cellRow, (unsigned)pixRow,
					(unsigned)draw.hFlip, (unsigned)(vflip ? 1u : 0u), (unsigned)draw.palPri,
					(unsigned)firstCol, (unsigned)tileIdx, (unsigned)draw.address);
			}
		}
	}
	_sprCurSlot++;
}

void GenesisVdp::SlotSpriteRender()
{
	// First, advance sprite table scan
	SlotScanSpriteTable();

	// Then render one sprite cell into _linebuf
	uint8_t drawCount = (uint8_t)(_maxSpritesLine - _sprDraws);
	if(_sprRenderIdx >= drawCount) {
		// (x_pos=0) continue to be processed by render_sprite_cells, and the first
		// phantom slot consumes FLAG_CAN_MASK.
		if(drawCount < _maxSpritesLine) {
			_sprCanMask = false;
		}
		return;
	}
	if(_sprCellBudget == 0) return;

	uint8_t drawIndex = (uint8_t)(_maxSpritesLine - 1u - _sprRenderIdx);
	const SpriteDraw& draw = _spriteDrawList[drawIndex];
	bool int2 = IsInterlace2();
	uint8_t vertCells = draw.height;

	uint8_t cellCol = _sprRenderCell;
	uint8_t actualCol = draw.hFlip ? (uint8_t)(draw.width - 1u - cellCol) : cellCol;

	// For cells beyond the first, advance the VRAM address by vertCells tiles
	uint16_t cellBytes = int2 ? 64u : 32u;
	uint16_t patAddr;
	if(cellCol == 0) {
		patAddr = draw.address;
	} else {
		// First cell's col was already accounted for in draw.address.
		// For subsequent cols, we need to move by vertCells * cellBytes per column step.
		// But draw.address already points to the first column's row.
		// For column-major tile layout: next column = +vertCells tiles from base.
		// Since draw.address = (baseTile + firstCol*vertCells + cellRow) * cellBytes + pixRow*4,
		// stepping one column means ±vertCells * cellBytes.
		int16_t colDelta = draw.hFlip ? -(int16_t)(vertCells * cellBytes)
		                              :  (int16_t)(vertCells * cellBytes);
		patAddr = (uint16_t)(draw.address + colDelta * cellCol);
	}
	uint16_t expectedTile = (uint16_t)(draw.baseTile + actualCol * vertCells + draw.cellRow);
	uint16_t expectedPatAddr = (uint16_t)((int2 ? expectedTile * 64u : expectedTile * 32u) + draw.pixRow * 4u);
	bool patMismatch = patAddr != expectedPatAddr;

	int16_t screenX = draw.xPos + (int16_t)(cellCol * 8u);
	uint8_t priPal = draw.palPri; // bit 7 = priority, bits 5:4 = palette

	// Mask handling:
	// - X=0 sprite acts as a mask trigger only (never renders pixels).
	// - Non-zero X sprite enables mask eligibility for a later X=0 sprite.
	bool isMaskSprite = (draw.xPos == -128);
	if(isMaskSprite) {
		if(_sprCanMask) {
			_sprMasked = true;
			_sprCanMask = false;
		}
	} else {
		_sprCanMask = true;
	}

	if(SpriteTraceEnabled(_frameCount, _scanline)) {
		uint8_t b0 = _vram[(patAddr + 0u) & 0xFFFFu];
		uint8_t b1 = _vram[(patAddr + 1u) & 0xFFFFu];
		uint8_t b2 = _vram[(patAddr + 2u) & 0xFFFFu];
		uint8_t b3 = _vram[(patAddr + 3u) & 0xFFFFu];
		static constexpr char HexNib[] = "0123456789ABCDEF";
		char pixSeq[9] = {};
		for(uint8_t px = 0; px < 8; px++) {
			uint8_t col = draw.hFlip ? (uint8_t)(7u - px) : px;
			uint8_t b = _vram[(patAddr + (col >> 1)) & 0xFFFFu];
			uint8_t nib = (col & 1u) ? (b & 0x0Fu) : (b >> 4);
			pixSeq[px] = HexNib[nib & 0x0Fu];
		}
		SpriteTraceLog(_frameCount, _scanline,
			"SPR_CELL sat=%u draw=%u cell=%u/%u x=%d col=%u actCol=%u rowCell=%u rowPix=%u pat=%04X expPat=%04X mismatch=%u bytes=%02X%02X%02X%02X pix=%s mask=%u prevDotOvf=%u",
			(unsigned)draw.satIndex, (unsigned)drawIndex, (unsigned)cellCol, (unsigned)draw.width,
			(int)screenX, (unsigned)cellCol, (unsigned)actualCol, (unsigned)draw.cellRow, (unsigned)draw.pixRow,
			(unsigned)patAddr, (unsigned)expectedPatAddr, (unsigned)(patMismatch ? 1u : 0u),
			(unsigned)b0, (unsigned)b1, (unsigned)b2, (unsigned)b3, pixSeq,
			(unsigned)(_sprMasked ? 1u : 0u), (unsigned)(_prevLineDotOverflow ? 1u : 0u));
	}

	if(!isMaskSprite && !_sprMasked) {
		for(uint8_t px = 0; px < 8; px++) {
			int16_t sx = screenX + px;
			if(sx < 0 || sx >= (int16_t)ActiveWidth()) continue;

			uint8_t col = draw.hFlip ? (uint8_t)(7u - px) : px;
			uint8_t b = _vram[(patAddr + (col >> 1)) & 0xFFFFu];
			uint8_t nib = (col & 1u) ? (b & 0x0Fu) : (b >> 4);
			if(nib == 0) continue;

			if(_linebuf[sx] != 0) {
				_status |= 0x0020u; // collision
				continue;
			}
			_linebuf[sx] = (uint8_t)(priPal | nib);
		}
	}

	_sprRenderCell++;
	if(_sprRenderCell >= draw.width) {
		_sprRenderCell = 0;
		_sprRenderIdx++;
	}
	// One rendered sprite cell consumes one dot-budget slice (8 dots),
	// regardless of transparency, matching BlastEm's per-cell pacing.
	_sprCellBudget--;
}

void GenesisVdp::FifoDrainOne()
{
	if(_fifoCount == 0) return;

	const FifoEntry& e = _fifo[_fifoRead];
	uint8_t cd = e.code & 0x0Fu;
	switch(cd) {
		case 0x01: // VRAM write
			_vram[e.addr & 0xFFFFu]          = (uint8_t)(e.data >> 8);
			_vram[(e.addr + 1u) & 0xFFFFu]   = (uint8_t)e.data;
			break;
		case 0x03: // CRAM write
			CramWrite((e.addr >> 1) & 0x3Fu, e.data);
			break;
		case 0x05: // VSRAM write
			VsramWrite((e.addr >> 1) & 0x27u, e.data);
			break;
		default:
			break;
	}
	_fifoRead = (_fifoRead + 1u) & 3u;
	_fifoCount--;
	FifoUpdateStatus();
}

void GenesisVdp::FifoUpdateStatus()
{
	if(_fifoCount == 0) {
		_status |=  0x0200u;  // FIFO empty
		_status &= ~0x0100u;  // FIFO not full
	} else if(_fifoCount >= 4) {
		_status &= ~0x0200u;  // FIFO not empty
		_status |=  0x0100u;  // FIFO full
	} else {
		_status &= ~0x0200u;  // FIFO not empty
		_status &= ~0x0100u;  // FIFO not full
	}
}

void GenesisVdp::RunDmaSrc()
{
	if(_dmaType != DmaType::Bus68k || _dmaLen == 0 || !_backend) return;
	if(_fifoCount >= 4) return;  // FIFO full — can't accept another word

	// Read one word from 68K bus (same logic as ExecDmaBus68k, single word)
	uint32_t srcBase   = _dmaSrc & ~0x1FFFFu;
	uint32_t srcOffset = _dmaSrc &  0x1FFFFu;
	uint8_t  hi  = _backend->CpuBusRead8(srcBase |  srcOffset);
	uint8_t  lo  = _backend->CpuBusRead8(srcBase | ((srcOffset + 1u) & 0x1FFFFu));
	uint16_t word = ((uint16_t)hi << 8) | lo;
	srcOffset = (srcOffset + 2u) & 0x1FFFFu;

	// Enqueue into FIFO
	uint8_t cd = _dmaCode & 0x0Fu;
	_fifo[_fifoWrite].data = word;
	_fifo[_fifoWrite].addr = _dmaAddr;
	_fifo[_fifoWrite].code = cd;
	_fifoWrite = (_fifoWrite + 1u) & 3u;
	_fifoCount++;
	FifoUpdateStatus();

	// Advance DMA source and destination
	_dmaSrc = srcBase | srcOffset;
	uint32_t srcWords = srcOffset >> 1;
	_reg[21] = (uint8_t)(srcWords & 0xFFu);
	_reg[22] = (uint8_t)((srcWords >> 8) & 0xFFu);
	AdvanceDmaAddr();
	_dmaLen--;

	if(_dmaLen == 0) {
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaBusStartDelayMclk = 0;
		_dmaBusMclkRemainder = 0;
	}
}

void GenesisVdp::SlotExternalSlot()
{
	// Priority: (1) drain FIFO, (2) DMA fill, (3) DMA copy.
	// Bus68k DMA is paced centrally by Consume68kBusDma() to avoid
	// split-path behavior between scheduler and per-slot execution.
	if(_fifoCount > 0) {
		FifoDrainOne();
	} else if(_dmaType == DmaType::VramFill && _dmaFillPend && _dmaLen > 0) {
		ExecDmaFill(1);
	} else if(_dmaType == DmaType::VramCopy && _dmaLen > 0) {
		ExecDmaCopy(1);
	}
}

void GenesisVdp::SlotClearLinebuf()
{
	// Only clear sprite linebuf — NOT _compositebuf.
	// _compositebuf is cleared once per line in BeginLine(); the slot-table
	// ClearLinebuf fires mid-line (after tile compositing, before FlushCompositeBuf)
	// so wiping it here would erase the just-rendered pixels.
	memset(_linebuf, 0, sizeof(_linebuf));
	_sprRenderIdx = 0;
	_sprRenderCell = 0;
	// _sprCanMask intentionally NOT reset here.  BlastEm's FLAG_CAN_MASK carries
	// across lines but is consumed by "phantom" empty draw slots (x_pos=0) after
	// all real sprites are rendered — see SlotSpriteRender.  Only FLAG_MASKED
	// (= _sprMasked) is cleared per line.
	_sprMasked = false;
}

void GenesisVdp::SlotHScrollLoad()
{
	uint16_t nextLine = (_scanline + 1u < (uint16_t)TotalScanlines()) ? (_scanline + 1u) : 0u;
	_hscrollA = GetHScrollRaw(nextLine, true);
	_hscrollB = GetHScrollRaw(nextLine, false);
	_hscrollAFine = _hscrollA & 0x0Fu;
	_hscrollBFine = _hscrollB & 0x0Fu;
	ScrollTraceLog(_frameCount, _scanline, -1,
		"HS_LOAD nextLine=%u mode=%u base=%04X hA=%04X hB=%04X fineA=%u fineB=%u",
		(unsigned)nextLine, (unsigned)(_reg[11] & 0x03u), (unsigned)HScrollBase(),
		(unsigned)_hscrollA, (unsigned)_hscrollB, (unsigned)_hscrollAFine, (unsigned)_hscrollBFine);
}

void GenesisVdp::FlushCompositeBuf(uint16_t line)
{
	if(!_fb || line >= _fbH) return;

	uint32_t* outLine = _fb + (uint32_t)line * _fbW;
	uint16_t width = ActiveWidth();
	bool shMode = ShadowHlEnabled();

	for(uint16_t x = 0; x < width && x < _fbW; x++) {
		uint8_t encoded = _compositebuf[x];
		uint8_t shade = encoded >> 6;
		uint8_t cramIdx = encoded & 0x3Fu;

		if(shMode) {
			switch(shade) {
				case 0:  outLine[x] = _shadowPalette[cramIdx]; break;
				case 2:  outLine[x] = _highlightPalette[cramIdx]; break;
				default: outLine[x] = _palette[cramIdx]; break;
			}
		} else {
			outLine[x] = _palette[cramIdx];
		}
	}

	// Fill beyond active width with black
	for(uint32_t x = width; x < _fbW; x++) {
		outLine[x] = 0xFF000000u;
	}
}

// RunSlotsForLine is no longer used — slot dispatch is now cycle-interleaved
// in AdvanceToMclk(). Kept as a comment for reference during transition.

// ===========================================================================
// Init / Reset
// ===========================================================================

void GenesisVdp::Init(Emulator* emu, GenesisNativeBackend* backend, bool isPal)
{
	_emu     = emu;
	_backend = backend;
	LoadTraceConfigFromEnv();
	std::error_code fsErr;
	std::filesystem::create_directories(kTraceDirectory, fsErr);
	BuildHCounterTables();
	Reset(isPal);
	if(!_dmaTraceFile) {
		_dmaTraceFile = fopen(kDmaTracePath, "w");
		if(_dmaTraceFile) fprintf(_dmaTraceFile, "# DMA trace log\n");
	}
	if(!sSpriteTraceFile) {
		sSpriteTraceFile = fopen(kSpriteTracePath, "w");
		if(sSpriteTraceFile) {
			fprintf(sSpriteTraceFile, "# Sprite tile diagnostic trace\n");
			fprintf(sSpriteTraceFile, "# frameRange=%u-%u lineRange=%u-%u maxLines=%u\n",
				kSpriteTraceFrameStart, kSpriteTraceFrameEnd,
				kSpriteTraceLineStart, kSpriteTraceLineEnd, kSpriteTraceMaxLines);
			fflush(sSpriteTraceFile);
		}
	}
	if(!sComposeTraceFile) {
		sComposeTraceFile = fopen(kComposeTracePath, "w");
		if(sComposeTraceFile) {
			fprintf(sComposeTraceFile, "# Compositor diagnostic trace\n");
			fprintf(sComposeTraceFile, "# frameRange=%u-%u lineRange=%u-%u xRange=%u-%u maxLines=%u\n",
				kComposeTraceFrameStart, kComposeTraceFrameEnd,
				kComposeTraceLineStart, kComposeTraceLineEnd,
				kComposeTraceXStart, kComposeTraceXEnd, kComposeTraceMaxLines);
			fflush(sComposeTraceFile);
		}
	}
	if(!sScrollTraceFile) {
		sScrollTraceFile = fopen(kScrollTracePath, "w");
		if(sScrollTraceFile) {
			fprintf(sScrollTraceFile, "# Horizontal scroll diagnostic trace\n");
			fprintf(sScrollTraceFile, "# frameRange=%u-%u lineRange=%u-%u colRange=%u-%u maxLines=%u\n",
				kScrollTraceFrameStart, kScrollTraceFrameEnd,
				kScrollTraceLineStart, kScrollTraceLineEnd,
				kScrollTraceColumnStart, kScrollTraceColumnEnd, kScrollTraceMaxLines);
			fflush(sScrollTraceFile);
		}
	}
	if(!sHScrollDmaTraceFile) {
		sHScrollDmaTraceFile = fopen(kHScrollDmaTracePath, "w");
		if(sHScrollDmaTraceFile) {
			fprintf(sHScrollDmaTraceFile, "# H-scroll DMA write trace (VRAM destination filter)\n");
			fprintf(sHScrollDmaTraceFile, "# frameRange=%u-%u dstRange=%04X-%04X maxLines=%u\n",
				kHScrollDmaTraceFrameStart, kHScrollDmaTraceFrameEnd,
				(unsigned)kHScrollDmaTraceDstStart, (unsigned)kHScrollDmaTraceDstEnd, kHScrollDmaTraceMaxLines);
			fflush(sHScrollDmaTraceFile);
		}
	}
}

void GenesisVdp::Reset(bool isPal)
{
	_isPal = isPal;
	memset(_vram,  0, sizeof(_vram));
	memset(_cram,  0, sizeof(_cram));
	memset(_vsram, 0, sizeof(_vsram));
	memset(_reg,   0, sizeof(_reg));
	// Initialise palette from CRAM (all zeros → opaque black via CramWordToArgb)
	for(uint8_t i = 0; i < 64; i++) {
		RefreshPalette(i);
	}

	_ctrlPend    = false;
	_ctrlFirst   = 0;
	_addrReg     = 0;
	_codeReg     = 0;
	_readBuf     = 0;
	_writeHi     = 0;
	_writeHiData = false;
	_writeHiCtrl = false;

	// Default status: FIFO empty set, PAL bit set appropriately
	_status = 0x3600 | (_isPal ? 0x0001u : 0x0000u);  // bit 9 = FIFO empty, always 1 at reset
	_debugReg = 0;

	_vintPending = false;
	_vintNew     = false;
	_hintPending = false;
	_hintCounter = 0;
	_statusReadLatch = 0;
	_statusReadLatchValid = false;

	_dmaType     = DmaType::None;
	_dmaSrc      = 0;
	_dmaLen      = 0;
	_dmaAddr     = 0;
	_dmaCode     = 0;
	_dmaFillVal  = 0;
	_dmaFillPend = false;
	_dmaBusStartDelayMclk = 0;
	_dmaBusMclkRemainder = 0;
	_dmaVdpMclkRemainder = 0;

	memset(_fifo, 0, sizeof(_fifo));
	_fifoRead  = 0;
	_fifoWrite = 0;
	_fifoCount = 0;

	_hvLatch = 0;

	_interlaceField = false;

	_scanline    = 0;
	_frameCount  = 0;
	_prevLineDotOverflow = false;
	_fb          = nullptr;
	_fbW         = 320;
	_fbH         = 224;

	_mclkPos        = 0;
	_lineBegun      = false;
	_vintFiredFrame = false;
	_frameFb        = nullptr;
	_frameFbW       = 320;
	_frameFbH       = 224;
	memset(_lineBackdropMask, 0, sizeof(_lineBackdropMask));

	// Sensible power-on register defaults
	_reg[1]  = 0x04;   // display disable at reset
	_reg[12] = 0x81;   // H40 mode, no shadow/highlight
	_reg[15] = 0x02;   // auto-increment = 2
}

// ===========================================================================
// VRAM / CRAM / VSRAM word access (all big-endian pairs in byte arrays)
// ===========================================================================

uint16_t GenesisVdp::VramRead(uint16_t wordAddr) const
{
	uint32_t b = (uint32_t)(wordAddr & 0x7FFFu) * 2u;
	return ((uint16_t)_vram[b] << 8) | _vram[b + 1];
}

void GenesisVdp::VramWrite(uint16_t wordAddr, uint16_t value)
{
	uint32_t b = (uint32_t)(wordAddr & 0x7FFFu) * 2u;
	_vram[b]     = (uint8_t)(value >> 8);
	_vram[b + 1] = (uint8_t)(value);
}

uint16_t GenesisVdp::CramRead(uint8_t idx) const
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	return ((uint16_t)_cram[b] << 8) | _cram[b + 1];
}

void GenesisVdp::CramWrite(uint8_t idx, uint16_t value)
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	_cram[b]     = (uint8_t)(value >> 8);
	_cram[b + 1] = (uint8_t)(value);
	RefreshPalette(idx & 0x3Fu);
}

uint16_t GenesisVdp::VsramRead(uint8_t idx) const
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	if(b + 1 >= sizeof(_vsram)) return 0;
	return ((uint16_t)_vsram[b] << 8) | _vsram[b + 1];
}

void GenesisVdp::VsramWrite(uint8_t idx, uint16_t value)
{
	uint32_t b = (uint32_t)(idx & 0x3Fu) * 2u;
	if(b + 1 >= sizeof(_vsram)) return;
	_vsram[b]     = (uint8_t)(value >> 8);
	_vsram[b + 1] = (uint8_t)(value);
}

// ===========================================================================
// Palette
// ===========================================================================

// VDP DAC levels (vdp.c: levels[])
static constexpr uint8_t kLevels[15] = {
	0, 27, 49, 71, 87, 103, 119, 130, 146, 157, 174, 190, 206, 228, 255
};

// CRAM word format: 0000 BBB0 GGG0 RRR0
// B = bits 11:9, G = bits 7:5, R = bits 3:1
uint32_t GenesisVdp::CramWordToArgb(uint16_t w)
{
	// Default (mode 5 normal): levels[(channel3 << 1)].
	uint8_t r8 = kLevels[(((w >> 1) & 7u) << 1)];
	uint8_t g8 = kLevels[(((w >> 5) & 7u) << 1)];
	uint8_t b8 = kLevels[(((w >> 9) & 7u) << 1)];
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

uint32_t GenesisVdp::CramWordToArgbShadow(uint16_t w)
{
	// FBUF_SHADOW: levels[channel3].
	uint8_t r8 = kLevels[(w >> 1) & 7u];
	uint8_t g8 = kLevels[(w >> 5) & 7u];
	uint8_t b8 = kLevels[(w >> 9) & 7u];
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

uint32_t GenesisVdp::CramWordToArgbHighlight(uint16_t w)
{
	// FBUF_HILIGHT: levels[channel3 + 7].
	uint8_t r8 = kLevels[((w >> 1) & 7u) + 7u];
	uint8_t g8 = kLevels[((w >> 5) & 7u) + 7u];
	uint8_t b8 = kLevels[((w >> 9) & 7u) + 7u];
	return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
}

void GenesisVdp::RefreshPalette(uint8_t idx)
{
	uint16_t w         = CramRead(idx);
	_palette[idx]          = CramWordToArgb         (w);
	_shadowPalette[idx]    = CramWordToArgbShadow   (w);
	_highlightPalette[idx] = CramWordToArgbHighlight(w);
}

// ===========================================================================
// Register write
// ===========================================================================

void GenesisVdp::WriteReg(uint8_t r, uint8_t val)
{
	if(r >= 24) return;
	uint8_t oldVal = _reg[r];
	_reg[r] = val;

	switch(r) {
		case 0:
			// HV counter latch: when bit 1 transitions 0→1, latch current HV value
			if((val & 0x02u) && !(oldVal & 0x02u)) {
				// Need to read HV with the NEW register value (bit 1 now set),
				// but ReadHVCounter checks bit 1 and would return the latch.
				// Temporarily clear the flag to get the live value.
				_reg[0] = (uint8_t)(val & ~0x02u);
				_hvLatch = ReadHVCounter();
				_reg[0] = val;
			}
			break;
		case 1:
			// Display enable / V-int enable / DMA enable — update status PAL bit
			_status = (_status & ~0x0001u) | (_isPal ? 0x0001u : 0x0000u);
#ifdef _DEBUG
			{
				bool oldDisp = (oldVal & 0x40u) != 0;
				bool newDisp = (val    & 0x40u) != 0;
				if(oldDisp != newDisp) {
					LogDebug(string("[MD Native][VDP] Display ") + (newDisp ? "enabled" : "disabled")
						+ " (R1=$" + HexUtilities::ToHex(val)
						+ ", frame=" + std::to_string(_frameCount)
						+ ", line=" + std::to_string(_scanline) + ")");
				}
			}
#endif
			break;
		case 10:
			// H-int counter reload — takes effect next frame reload
			break;
		case 12:
			// Mode register 4: RS0/RS1 (H32/H40), shadow/highlight, interlace
			break;
		case 15:
			// Auto-increment: 0 is legal (no advance)
			break;
		default:
			break;
	}
}

// ===========================================================================
// Control port state machine
// ===========================================================================

void GenesisVdp::AdvanceAddr()
{
	uint8_t inc = AutoInc();
	if(inc == 0) return;
	_addrReg = (uint16_t)(_addrReg + inc);
}

void GenesisVdp::AdvanceDmaAddr()
{
	uint8_t inc = AutoInc();
	if(inc != 0) {
		_dmaAddr = (uint16_t)(_dmaAddr + inc);
	}
	// DMA writes advance the hardware address register as well.
	// Keep the live register in sync for post-DMA command behavior.
	_addrReg = _dmaAddr;
}

void GenesisVdp::PrimeBuf()
{
	// Fill the read-ahead buffer from the current address/code target
	uint8_t cd = _codeReg & 0x0F;  // lower 4 bits of code select source

	switch(cd) {
		case 0x00: // VRAM read
			_readBuf = ((uint16_t)_vram[_addrReg & 0xFFFFu] << 8)
			         | _vram[(_addrReg + 1u) & 0xFFFFu];
			AdvanceAddr();
			break;
		case 0x08: // CRAM read
			_readBuf = CramRead((_addrReg >> 1) & 0x3F);
			AdvanceAddr();
			break;
		case 0x04: // VSRAM read
			_readBuf = VsramRead((_addrReg >> 1) & 0x27u);  // 40 entries, max idx 0x27
			AdvanceAddr();
			break;
		default:
			break;
	}
}

void GenesisVdp::BeginOperation()
{
	// Check if this is a DMA start
	if((_codeReg & 0x20) && DmaEnabled()) {
		_dmaAddr = _addrReg;
		_dmaCode = _codeReg;
		uint8_t dmaMode = (_reg[23] >> 6) & 3;
		if(dmaMode == 2) {
			// VRAM fill — starts on next data-port write.
			_dmaType     = DmaType::VramFill;
			uint32_t rawLen = (uint32_t)_reg[19] | ((uint32_t)_reg[20] << 8);
			_dmaLen      = (rawLen == 0u) ? 0x10000u : rawLen;
			_dmaFillPend = false;
			_dmaBusStartDelayMclk = 0;
			_status |= 0x0002u; // DMA busy
			if(_dmaTraceFile) {
				fprintf(_dmaTraceFile, "F%04u L%03u DMA_FILL cd=%02X dst=%04X len=%u\n",
					_frameCount, _scanline, (unsigned)(_dmaCode & 0x0Fu), _dmaAddr, _dmaLen);
				fflush(_dmaTraceFile);
			}
			{
				char lineBuf[160] = {};
				snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u DMA_FILL cd=%02X dst=%04X len=%u",
					_frameCount, _scanline, (unsigned)(_dmaCode & 0x0Fu), _dmaAddr, _dmaLen);
				AppendTraceBufferLine(sDmaTraceBuffer, lineBuf);
			}
		} else if(dmaMode == 3) {
			// VRAM copy
			_dmaType = DmaType::VramCopy;
			_dmaLen  = (uint32_t)_reg[19] | ((uint32_t)_reg[20] << 8);
			if(_dmaLen == 0) _dmaLen = 0x10000u;
			_dmaSrc  = (uint32_t)_reg[21]
			         | ((uint32_t)_reg[22] << 8);   // source VRAM word address
			_dmaBusStartDelayMclk = 0;
			_dmaVdpMclkRemainder = 0;
			_status |= 0x0002u;
		} else {
			// Bus 68K to VDP (DMA mode 0 or 1)
			// Startup latency: 12*mclk * (H40 ? 4 : 5).
			uint8_t dma68kStartDelayMclk = IsH40() ? 48u : 60u;
			_dmaType = DmaType::Bus68k;
			_dmaLen  = (uint32_t)_reg[19] | ((uint32_t)_reg[20] << 8);
			if(_dmaLen == 0) _dmaLen = 0x10000u;
			_dmaSrc  = ((uint32_t)_reg[21]       )
			         | ((uint32_t)_reg[22] << 8  )
			         | ((uint32_t)(_reg[23] & 0x7Fu) << 16);
			_dmaSrc <<= 1;  // source is in words, convert to bytes
			_dmaBusStartDelayMclk = dma68kStartDelayMclk;
			_dmaBusMclkRemainder = 0;
			_status |= 0x0002u;
			if(_dmaTraceFile) {
				fprintf(_dmaTraceFile, "F%04u L%03u DMA_BUS68K cd=%02X src=%06X dst=%04X len=%u\n",
					_frameCount, _scanline, (unsigned)(_dmaCode & 0x0Fu), _dmaSrc, _dmaAddr, _dmaLen);
				fflush(_dmaTraceFile);
			}
			{
				char lineBuf[192] = {};
				snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u DMA_BUS68K cd=%02X src=%06X dst=%04X len=%u",
					_frameCount, _scanline, (unsigned)(_dmaCode & 0x0Fu), _dmaSrc, _dmaAddr, _dmaLen);
				AppendTraceBufferLine(sDmaTraceBuffer, lineBuf);
			}
			// 68K bus DMA is paced from the backend scheduler via Consume68kBusDma().
		}
	} else {
		// Read operation: prime buffer
		PrimeBuf();
	}
}

void GenesisVdp::HandleControlWrite(uint16_t word)
{
	// Register write: bit15=1, bit14=0
	if((word & 0xC000u) == 0x8000u) {
		uint8_t reg = (uint8_t)((word >> 8) & 0x1Fu);
		uint8_t val = (uint8_t)(word & 0xFFu);
		WriteReg(reg, val);
		_ctrlPend = false;  // reset pair state
		return;
	}

	if(!_ctrlPend) {
		// First word of address command
		_ctrlFirst = word;
		_ctrlPend  = true;
	} else {
		// Second word — assemble address and code
		_ctrlPend = false;
		_addrReg  = (uint16_t)((_ctrlFirst & 0x3FFFu) | ((word & 0x0003u) << 14));
		_codeReg  = (uint8_t)((_ctrlFirst >> 14) | ((word >> 2) & 0x3Cu));
		BeginOperation();
	}
}

// ===========================================================================
// Bus interface — called by GenesisNativeBackend ReadCartBus / WriteCartBus
// addr is the full 24-bit bus address in range $C00000-$C0001F
// ===========================================================================

uint8_t GenesisVdp::ReadByte(uint32_t addr)
{
	uint32_t reg = addr & 0x1Fu;
	bool isStatusOddRead = (reg >= 0x04u && reg <= 0x07u && (reg & 1u));
	if(_statusReadLatchValid && !isStatusOddRead) {
		// Latch is only valid for the immediate low-byte status read.
		_statusReadLatchValid = false;
	}

	switch(reg) {
		// Data port: $C00000-$C00003 (all map to data read)
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03: {
			// Even byte = high byte of read buffer, odd = low byte
			uint8_t shift = (addr & 1u) ? 0 : 8;
			uint8_t result = (uint8_t)(_readBuf >> shift);

			if((addr & 1u) == 1u) {
				// Low byte — advance and refill buffer
				PrimeBuf();
			}
			return result;
		}

		// Control / status port: $C00004-$C00007
			case 0x04:
			case 0x05:
			case 0x06:
			case 0x07: {
				auto latchStatusRead = [&]() -> uint16_t {
					// Reading the status port clears the pending-control latch.
					_ctrlPend = false;
					_writeHiCtrl = false;

					// Keep DMA busy bit coherent with pending DMA state.
					bool dmaBusy = (_dmaType != DmaType::None && _dmaLen > 0);
					if(dmaBusy) _status |= 0x0002u;
					else        _status &= ~0x0002u;

					// FIFO bits (bits 9:8) — reflect actual FIFO state.
					// During Bus68k DMA the FIFO is also considered non-empty.
					{
						bool busTransfer = (_dmaType == DmaType::Bus68k && _dmaLen > 0);
						bool fifoEmpty = (_fifoCount == 0 && !busTransfer);
						bool fifoFull  = (_fifoCount >= 4 || busTransfer);
						if(fifoEmpty) _status |=  0x0200u; else _status &= ~0x0200u;
						if(fifoFull)  _status |=  0x0100u; else _status &= ~0x0100u;
					}

					uint16_t s = _status;
					// Bit 7: V-interrupt pending — set at VBlank, cleared on status read.
					if(_vintPending) {
						s |= 0x0080u;
						_vintPending = false;
					}
					// Hardware clears sprite-overflow (bit 6) and sprite-collision (bit 5) on read.
					_status &= ~0x0060u;
					return s;
				};

				if((reg & 1u) == 0u) {
					// Even byte read (high byte): latch the full status so the
					// subsequent odd-byte read returns the matching low byte.
					_statusReadLatch = latchStatusRead();
					_statusReadLatchValid = true;
					return (uint8_t)(_statusReadLatch >> 8);
				}

				if(_statusReadLatchValid) {
					_statusReadLatchValid = false;
					return (uint8_t)_statusReadLatch;
				}

				uint16_t s = latchStatusRead();
				return (uint8_t)s;
			}

			// H/V counter: $C00008-$C00009, mirrored through $C0000A-$C0000F
			case 0x08:
			case 0x09:
			case 0x0A:
			case 0x0B:
			case 0x0C:
			case 0x0D:
			case 0x0E:
			case 0x0F: {
				uint16_t hv = ReadHVCounter();
				if((reg & 1u) == 0u) {
					return (uint8_t)(hv >> 8);   // V counter
				}
				return (uint8_t)(hv & 0xFFu);    // H counter
			}

			// Debug register: $C0001C-$C0001F (mirrors)
			case 0x1C:
			case 0x1E:
				return (uint8_t)(_debugReg >> 8);
			case 0x1D:
			case 0x1F:
				return (uint8_t)_debugReg;

		default:
			return 0xFFu;
	}
}

void GenesisVdp::WriteByte(uint32_t addr, uint8_t val)
{
	_statusReadLatchValid = false;
	uint32_t reg = addr & 0x1Fu;

	switch(reg) {
		// Data port: $C00000-$C00003
		case 0x00:
		case 0x01:
		case 0x02:
		case 0x03: {
			if((addr & 1u) == 0u) {
				// High byte — stash in dedicated write buffer (does NOT touch _readBuf)
				_writeHi     = val;
				_writeHiData = true;
			} else {
				// Low byte — assemble word and commit
				uint16_t word = _writeHiData
				              ? (uint16_t)(((uint16_t)_writeHi << 8) | val)
				              : (uint16_t)(((uint16_t)0x00u    << 8) | val);
				_writeHiData = false;

				uint8_t cd = _codeReg & 0x0Fu;
#ifdef _DEBUG
				{
					static uint32_t sDataPortLogCount = 0;
					if(sDataPortLogCount < 128) {
						LogDebug("[MD Native][VDP] DATA write cd=$" + HexUtilities::ToHex(cd) +
							" addr=$" + HexUtilities::ToHex(_addrReg) +
							" word=$" + HexUtilities::ToHex(word));
						sDataPortLogCount++;
					}
				}
#endif

				if(_dmaType == DmaType::VramFill && !_dmaFillPend) {
					TriggerDmaFill(word);
					return;
				}

				// --- FIFO enqueue ---
				// During V-blank or display-disabled, VRAM bus is free so writes
				// commit immediately (no queuing needed).
				uint32_t curLine = _mclkPos / MCLKS_PER_LINE;
				bool inVblank = !DispEnabled() || curLine >= (uint32_t)ActiveHeight();
				if(inVblank) {
					// Direct write — bypass FIFO
					switch(cd) {
						case 0x01:
							_vram[_addrReg & 0xFFFFu]          = (uint8_t)(word >> 8);
							_vram[(_addrReg + 1u) & 0xFFFFu]   = (uint8_t)word;
							break;
						case 0x03: CramWrite((_addrReg >> 1) & 0x3Fu, word); break;
						case 0x05: VsramWrite((_addrReg >> 1) & 0x27u, word); break;
						default: break;
					}
				} else {
					// Active display — enqueue into FIFO
					// If FIFO is full, force-drain the oldest entry (stall 68K
					// would be more accurate, but this keeps the backend simple).
					if(_fifoCount >= 4) {
						FifoDrainOne();
					}
					_fifo[_fifoWrite].data = word;
					_fifo[_fifoWrite].addr = _addrReg;
					_fifo[_fifoWrite].code = cd;
					_fifoWrite = (_fifoWrite + 1u) & 3u;
					_fifoCount++;
					FifoUpdateStatus();
				}
				AdvanceAddr();
			}
			break;
		}

		// Control port: $C00004-$C00007
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07: {
			if((addr & 1u) == 0u) {
				// High byte — stash in dedicated control write buffer
				_writeHi     = val;
				_writeHiCtrl = true;
			} else {
				// Low byte — assemble word and dispatch
				uint16_t word = _writeHiCtrl
				              ? (uint16_t)(((uint16_t)_writeHi << 8) | val)
				              : (uint16_t)(((uint16_t)0x00u    << 8) | val);
				_writeHiCtrl = false;
				HandleControlWrite(word);
			}
			break;
		}

		// Debug register: $C0001C-$C0001F (mirrors)
		case 0x1C:
		case 0x1E:
			_debugReg = (uint16_t)((_debugReg & 0x00FFu) | ((uint16_t)val << 8));
			break;
		case 0x1D:
		case 0x1F:
			_debugReg = (uint16_t)((_debugReg & 0xFF00u) | val);
			break;

		default:
			break;
	}
}

// ===========================================================================
// DMA
// ===========================================================================

void GenesisVdp::TriggerDmaFill(uint16_t data)
{
	_dmaFillVal  = data;
	_dmaFillPend = true;
	// Actual fill executes through ConsumeInternalDma() in the backend scheduler.
}

void GenesisVdp::ExecDmaBus68k(uint32_t maxWords)
{
	if(!_backend || _dmaType != DmaType::Bus68k || _dmaLen == 0 || maxWords == 0) return;
	uint8_t  cd  = _dmaCode & 0x0Fu;
	uint32_t len = (_dmaLen < maxWords) ? _dmaLen : maxWords;
	uint32_t src = _dmaSrc;

	// 68K source DMA increments only within the current 128KB source window.
	// When offset overflows, it wraps within that window (hardware quirk).
	uint32_t srcBase   = src & ~0x1FFFFu;
	uint32_t srcOffset = src &  0x1FFFFu;

	while(len > 0) {
		uint32_t srcByteAddr = srcBase | srcOffset;
		uint8_t  hi  = _backend->CpuBusRead8(srcByteAddr);
		uint8_t  lo  = _backend->CpuBusRead8(srcBase | ((srcOffset + 1u) & 0x1FFFFu));
		uint16_t word = ((uint16_t)hi << 8) | lo;
		srcOffset = (srcOffset + 2u) & 0x1FFFFu;
		uint16_t dstAddr = _dmaAddr;

			switch(cd) {
				case 0x01:
					_vram[_dmaAddr & 0xFFFFu]        = (uint8_t)(word >> 8);
					_vram[(_dmaAddr + 1u) & 0xFFFFu] = (uint8_t)word;
					break;
				case 0x03: CramWrite ((_dmaAddr >> 1) & 0x3Fu, word); break;
				case 0x05: VsramWrite((_dmaAddr >> 1) & 0x27u, word); break;
				default: break;
			}
		bool dstHit = HScrollDmaTraceEnabled(_frameCount, dstAddr)
			|| HScrollDmaTraceEnabled(_frameCount, (uint16_t)(dstAddr + 1u));
		if(cd == 0x01 && dstHit) {
			HScrollDmaTraceLog(_frameCount, _scanline,
				"HSDMA_WR src=%06X dst=%04X data=%04X lenRem=%u",
				(unsigned)srcByteAddr, (unsigned)dstAddr, (unsigned)word, (unsigned)_dmaLen);
		}
		AdvanceDmaAddr();
		len--;
		_dmaLen--;
	}

	src = srcBase | srcOffset;
	_dmaSrc  = src;
	// Update DMA source registers for the next transfer chunk.
	// Hardware keeps the high source bits in R23[6:0] fixed while a transfer runs;
	// only R21/R22 advance (wrapping within the current 128KB source window).
	uint32_t srcWords = srcOffset >> 1;
	_reg[21] = (uint8_t)(srcWords & 0xFFu);
	_reg[22] = (uint8_t)((srcWords >> 8) & 0xFFu);

	if(_dmaLen == 0) {
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaBusStartDelayMclk = 0;
		_dmaBusMclkRemainder = 0;
		if(_dmaTraceFile) {
			fprintf(_dmaTraceFile, "F%04u L%03u DMA_BUS68K_DONE src=%06X\n",
				_frameCount, _scanline, _dmaSrc);
			fflush(_dmaTraceFile);
		}
		{
			char lineBuf[160] = {};
			snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u DMA_BUS68K_DONE src=%06X",
				_frameCount, _scanline, _dmaSrc);
			AppendTraceBufferLine(sDmaTraceBuffer, lineBuf);
		}
	}
}

void GenesisVdp::ExecDmaFill(uint32_t maxBytes)
{
	if(_dmaType != DmaType::VramFill || _dmaLen == 0 || maxBytes == 0) {
		return;
	}

	uint8_t cd = _dmaCode & 0x0Fu;
	uint32_t len = (_dmaLen < maxBytes) ? _dmaLen : maxBytes;

	switch(cd) {
		case 0x01: {
			// VRAM fill is byte-oriented. The source comes from the upper byte of
			// the data-port word, matching the standard #$yy00 fill command form.
			uint8_t fillByte = (uint8_t)(_dmaFillVal >> 8);
			while(len > 0) {
				_vram[_dmaAddr & 0xFFFFu] = fillByte;
				AdvanceDmaAddr();
				len--;
				_dmaLen--;
			}
			break;
		}

		case 0x03:
			while(len > 0) {
				CramWrite((_dmaAddr >> 1) & 0x3Fu, _dmaFillVal);
				AdvanceDmaAddr();
				len--;
				_dmaLen--;
			}
			break;

		case 0x05:
			while(len > 0) {
				VsramWrite((_dmaAddr >> 1) & 0x27u, _dmaFillVal);
				AdvanceDmaAddr();
				len--;
				_dmaLen--;
			}
			break;

		default:
			// Invalid DMA-fill destinations do not write, but the VDP still
			// consumes the programmed length and advances the address.
			while(len > 0) {
				AdvanceDmaAddr();
				len--;
				_dmaLen--;
			}
			break;
	}

	if(_dmaLen == 0) {
		_dmaFillPend = false;
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaVdpMclkRemainder = 0;
		if(_dmaTraceFile) {
			fprintf(_dmaTraceFile, "F%04u L%03u DMA_FILL_DONE\n", _frameCount, _scanline);
			fflush(_dmaTraceFile);
		}
		{
			char lineBuf[128] = {};
			snprintf(lineBuf, sizeof(lineBuf), "F%04u L%03u DMA_FILL_DONE", _frameCount, _scanline);
			AppendTraceBufferLine(sDmaTraceBuffer, lineBuf);
		}
	}
}

void GenesisVdp::ExecDmaCopy(uint32_t maxWords)
{
	if(_dmaType != DmaType::VramCopy || _dmaLen == 0 || maxWords == 0) {
		return;
	}

	// Copy from VRAM source to VRAM destination, one byte at a time
	uint32_t src = _dmaSrc;   // byte address in VRAM
	uint32_t len = (_dmaLen < maxWords) ? _dmaLen : maxWords;

	while(len > 0) {
		uint8_t val = _vram[src & 0xFFFFu];
		_vram[_dmaAddr & 0xFFFFu] = val;
		src++;
		AdvanceDmaAddr();
		len--;
		_dmaLen--;
	}
	_dmaSrc = src;

	if(_dmaLen == 0) {
		_dmaType = DmaType::None;
		_status &= ~0x0002u;
		_dmaVdpMclkRemainder = 0;
	}
}

void GenesisVdp::ExecPendingDma()
{
	// Force-complete pending DMA (used by legacy call sites or debugger paths).
	if(_dmaType == DmaType::Bus68k) {
		ExecDmaBus68k(_dmaLen);
	} else if(_dmaType == DmaType::VramFill && _dmaFillPend) {
		ExecDmaFill(_dmaLen);
	} else if(_dmaType == DmaType::VramCopy) {
		ExecDmaCopy(_dmaLen);
	}
}

uint32_t GenesisVdp::ConsumeInternalDma(uint32_t masterClocks)
{
	if(masterClocks == 0u) {
		return 0u;
	}

	// During active display, SlotExternalSlot() inside RunSlotsForLine() handles
	// fill/copy at per-slot granularity. Skip here to avoid double-processing.
	if(DispEnabled()) {
		uint32_t currentLine = _mclkPos / MCLKS_PER_LINE;
		if(currentLine < (uint32_t)ActiveHeight()) {
			return 0u;
		}
	}

	// Approximation for compatibility-first pacing:
	// VDP-internal DMA step budget. Fill is byte-oriented, copy is word-oriented.
	constexpr uint32_t DmaInternalStepMclk = 8u;

	if(_dmaType == DmaType::VramFill) {
		if(!_dmaFillPend || _dmaLen == 0) {
			return 0u;
		}

		uint32_t budget = masterClocks + _dmaVdpMclkRemainder;
		uint32_t bytes  = budget / DmaInternalStepMclk;
		_dmaVdpMclkRemainder = (uint8_t)(budget % DmaInternalStepMclk);
		if(bytes == 0u) {
			return 0u;
		}

		uint32_t before = _dmaLen;
		ExecDmaFill(bytes);
		uint32_t transferred = before - _dmaLen;
		return transferred * DmaInternalStepMclk;
	}

	if(_dmaType == DmaType::VramCopy) {
		if(_dmaLen == 0) {
			return 0u;
		}

		uint32_t budget = masterClocks + _dmaVdpMclkRemainder;
		uint32_t words  = budget / DmaInternalStepMclk;
		_dmaVdpMclkRemainder = (uint8_t)(budget % DmaInternalStepMclk);
		if(words == 0u) {
			return 0u;
		}

		uint32_t before = _dmaLen;
		ExecDmaCopy(words);
		uint32_t transferred = before - _dmaLen;
		return transferred * DmaInternalStepMclk;
	}

	return 0u;
}

uint32_t GenesisVdp::Consume68kBusDma(uint32_t masterClocks, uint32_t sliceStartMclk)
{
	if(!Is68kBusDmaActive() || masterClocks == 0u) {
		return 0u;
	}

	auto getWordPeriodMclk = [&](bool blanking) -> uint32_t {
		if(blanking) {
			// V-blank / display disabled — measured from BlastEm reference:
			// H40: ~107 words/line → 3420/107 ≈ 32 mclk/word
			// H32: ~85  words/line → 3420/85  ≈ 40 mclk/word
			return IsH40() ? 32u : 40u;
		}

		// Active display: limited to external access slots only.
		// H40/H32 slot tables have 14 external slots per line.
		// Each Bus68K DMA word uses one external slot → ~14 words/line.
		// 3420 mclk / 14 ≈ 244 mclk/word.
		return 244u;
	};

	uint32_t budget = masterClocks + _dmaBusMclkRemainder;
	uint32_t consumed = 0u;
	_dmaBusMclkRemainder = 0u;

	// Bus68k DMA acquires the bus before the first transfer.
	if(_dmaBusStartDelayMclk > 0u) {
		uint32_t delay = (_dmaBusStartDelayMclk < budget) ? _dmaBusStartDelayMclk : budget;
		_dmaBusStartDelayMclk = (uint8_t)(_dmaBusStartDelayMclk - delay);
		budget -= delay;
		consumed += delay;
		if(_dmaBusStartDelayMclk > 0u) {
			return consumed;
		}
	}

	uint32_t framePos = sliceStartMclk + consumed;
	uint32_t sliceEnd = sliceStartMclk + masterClocks;
	uint32_t phaseMclk = _dmaBusMclkRemainder;

	while(framePos < sliceEnd && _dmaLen > 0u) {
		uint32_t line = framePos / MCLKS_PER_LINE;
		uint32_t segEnd = std::min(sliceEnd, (line + 1u) * MCLKS_PER_LINE);
		bool blanking = !DispEnabled() || line >= ActiveHeight();
		uint32_t periodMclk = getWordPeriodMclk(blanking);
		uint32_t clocksRemaining = segEnd - framePos;

		while(clocksRemaining > 0u && _dmaLen > 0u) {
			uint32_t clocksToWord = (phaseMclk < periodMclk) ? (periodMclk - phaseMclk) : periodMclk;
			if(clocksToWord > clocksRemaining) {
				phaseMclk += clocksRemaining;
				framePos += clocksRemaining;
				clocksRemaining = 0u;
				break;
			}

			uint32_t nextPos = framePos + clocksToWord;
			// Transfer one word at the paced rate. For CRAM writes, the native
			// renderer is scanline-based so per-word backdrop updates are skipped
			// (the updated CRAM takes effect on subsequent lines).
			ExecDmaBus68k(1u);
			framePos = nextPos;
			clocksRemaining -= clocksToWord;
			phaseMclk = 0u;

			if(_dmaLen == 0u) {
				_dmaBusMclkRemainder = 0u;
				return framePos - sliceStartMclk;
			}
		}
	}

	_dmaBusMclkRemainder = (uint8_t)phaseMclk;
	return masterClocks;
}

// ===========================================================================
// Rendering helpers
// ===========================================================================

uint16_t GenesisVdp::PlaneWidthTiles() const
{
	uint8_t sz = _reg[16] & 0x03u;
	switch(sz) {
		case 0x00: return 32;
		case 0x01: return 64;
		case 0x03: return 128;
		default:   return 32; // reserved value
	}
}

uint16_t GenesisVdp::PlaneHeightTiles() const
{
	uint8_t sz = (_reg[16] >> 4) & 0x03u;
	switch(sz) {
		case 0x00: return 32;
		case 0x01: return 64;
		case 0x03: return 128;
		default:   return 32; // reserved value
	}
}

uint16_t GenesisVdp::WindowBase() const
{
	// R3: Window name table base.
	// H40: aligned to 0x1000; H32: aligned to 0x800.
	if(IsH40()) {
		return (uint16_t)(_reg[3] & 0x3Cu) << 10;
	} else {
		return (uint16_t)(_reg[3] & 0x3Eu) << 10;
	}
}

uint16_t GenesisVdp::SpriteBase() const
{
	// R5: Sprite attribute table base.
	// H40: bits 6:1 × 0x200; H32: bits 5:0 × 0x200
	if(IsH40()) {
		return (uint16_t)(_reg[5] & 0x7Eu) << 9;
	} else {
		return (uint16_t)(_reg[5] & 0x7Fu) << 9;
	}
}

uint16_t GenesisVdp::GetHScrollRaw(uint16_t line, bool planeA) const
{
	uint16_t tableBase = HScrollBase();
	uint16_t lineMask = 0;
	if((_reg[11] & 0x02u) != 0) lineMask |= 0xF8u;
	if((_reg[11] & 0x01u) != 0) lineMask |= 0x07u;

	uint16_t byteAddr = (uint16_t)((tableBase + ((line & lineMask) * 4u)) & 0xFFFFu);
	uint16_t scrollA = ((uint16_t)_vram[byteAddr] << 8) | _vram[(byteAddr + 1u) & 0xFFFFu];
	uint16_t scrollB = ((uint16_t)_vram[(byteAddr + 2u) & 0xFFFFu] << 8) | _vram[(byteAddr + 3u) & 0xFFFFu];
	return planeA ? scrollA : scrollB;
}

// H-scroll for non-slot render helpers: use 10-bit effective scroll.
uint16_t GenesisVdp::GetHScroll(uint16_t line, bool planeA) const
{
	return GetHScrollRaw(line, planeA) & 0x03FFu;
}

// V-scroll: tileCol2 is the pair-of-columns index (0 = columns 0-1, 1 = columns 2-3, ...)
uint16_t GenesisVdp::GetVScroll(uint16_t tileCol2, bool planeA) const
{
	bool twoColumn = (_reg[11] & 0x04u) != 0;
	uint8_t vsramIdx;
	if(twoColumn) {
		// One entry pair per 2 columns: idx = tileCol2 * 2 (A), tileCol2 * 2 + 1 (B)
		uint8_t base = (uint8_t)((tileCol2 & 0x1Fu) * 2u);
		vsramIdx = planeA ? base : (base + 1u);
	} else {
		// Full scroll: VSRAM[0] = A, VSRAM[1] = B
		vsramIdx = planeA ? 0u : 1u;
	}
	return VsramRead(vsramIdx) & 0x3FFu;
}

// Returns true if screen pixel (x, line) is covered by the window plane.
//
// Per Genesis VDP hardware behaviour:
//   R17 (WHP): bit7 = right-side; bits 4:0 = cell column boundary (each unit = 2 tiles = 16px in H40)
//   R18 (WVP): bit7 = down-side;  bits 4:0 = cell row boundary
//
// The window covers two independent strips, OR-ed together:
//   Y strip — ALL columns on lines where the Y-condition is met.
//   X strip — ALL lines in columns where the X-condition is met.
// Together they produce an L-shaped (or full-screen) region, not a rectangle.
bool GenesisVdp::IsWindowPixel(uint16_t line, uint16_t x) const
{
	uint8_t  wx          = _reg[17];
	uint8_t  wy          = _reg[18];
	bool     windowRight = (wx & 0x80u) != 0;
	bool     windowDown  = (wy & 0x80u) != 0;
	// R17 HP4-HP0: horizontal boundary in units of 16 pixels.
	// Using 8-pixel units incorrectly exposes a right-side window slice on
	// patterns like 240p Test Suite Bleed1.
	uint16_t wx_pix      = (uint16_t)(wx & 0x1Fu) * 16u;
	uint16_t wy_cell     = (uint16_t)(wy & 0x1Fu);
	uint16_t line_cell   = line >> 3;

	bool covY = windowDown  ? (line_cell >= wy_cell) : (line_cell < wy_cell);
	bool covX = windowRight ? (x         >= wx_pix)  : (x         <  wx_pix);

	return covX || covY;
}

uint8_t GenesisVdp::FetchTilePixel(uint16_t tileBase, uint8_t row, uint8_t col) const
{
	uint16_t byteAddr = tileBase + (uint16_t)row * 4u + (col >> 1);
	uint8_t  byte     = _vram[byteAddr & 0xFFFFu];
	return (col & 1u) ? (byte & 0x0Fu) : (byte >> 4);
}

// ===========================================================================
// Rendering — per-plane
// Pixel encoding: bit7=priority, bits5:4=palette, bits3:0=color (0=transparent)
// ===========================================================================

void GenesisVdp::RenderPlaneB(uint16_t line, uint8_t* dst, uint16_t pixels) const
{
	bool     int2     = IsInterlace2();
	uint16_t planeW   = PlaneWidthTiles();
	uint16_t planeH   = PlaneHeightTiles();
	uint16_t hscroll  = GetHScroll(line, false);  // plane B

	// In interlace mode 2 each tile is 16 rows tall; name-table rows advance
	// every 16 display lines.  Outside interlace: normal 8-row tiles.
	uint16_t tilePixH = int2 ? 16u : 8u;
	uint16_t tileRow  = line / tilePixH;
	uint16_t pixRow   = line % tilePixH;
	// In interlace mode 2 the actual row within the 16-pixel tile depends
	// on which field we're in:  even pixels for field 0, odd for field 1.
	uint16_t intPixRow = int2 ? (uint16_t)(pixRow * 2u + (_interlaceField ? 1u : 0u)) : pixRow;

	uint16_t nameBase = ScrollBBase();
	uint32_t planePxH = (uint32_t)planeH * tilePixH;
	uint32_t planePxW = (uint32_t)planeW * 8u;

	for(uint16_t x = 0; x < pixels; x++) {
		uint16_t px = (uint16_t)(x - hscroll) & (uint16_t)(planePxW - 1u);

		uint16_t vscroll = GetVScroll(x >> 4, false);
		uint32_t py = ((uint32_t)tileRow * tilePixH + intPixRow + vscroll) % planePxH;

		uint16_t tc  = (px >> 3) & (planeW - 1u);
		uint16_t tr  = (uint16_t)(py / tilePixH) & (planeH - 1u);
		uint16_t tpx = px & 7u;
		uint16_t tpy = (uint16_t)(py % tilePixH);

		uint16_t nameAddr = nameBase + (uint16_t)((tr * planeW + tc) * 2u);
		uint16_t entry    = ((uint16_t)_vram[nameAddr & 0xFFFFu] << 8)
		                  | _vram[(nameAddr + 1u) & 0xFFFFu];

		bool     pri   = (entry >> 15) & 1;
		uint8_t  pal   = (uint8_t)((entry >> 13) & 3u);
		bool     vflip = (entry >> 12) & 1;
		bool     hflip = (entry >> 11) & 1;
		uint16_t tile  = entry & 0x7FFu;

		uint8_t col = hflip ? (7u - tpx) : (uint8_t)tpx;
		uint8_t row = vflip ? ((uint8_t)(tilePixH - 1u) - (uint8_t)tpy) : (uint8_t)tpy;

		// Interlace mode 2: each tile is 64 bytes (16 rows × 4 bytes/row)
		uint16_t tileBase = int2 ? (tile * 64u) : (tile * 32u);
		uint8_t  pix = FetchTilePixel(tileBase, row, col);

		dst[x] = (uint8_t)((pri ? 0x80u : 0x00u) | (pal << 4) | pix);
	}
}

void GenesisVdp::RenderPlaneA(uint16_t line, uint8_t* dst, uint16_t pixels) const
{
	bool     int2     = IsInterlace2();
	uint16_t planeW   = PlaneWidthTiles();
	uint16_t planeH   = PlaneHeightTiles();
	uint16_t hscroll  = GetHScroll(line, true);   // plane A

	uint16_t tilePixH  = int2 ? 16u : 8u;
	uint16_t tileRow   = line / tilePixH;
	uint16_t pixRow    = line % tilePixH;
	uint16_t intPixRow = int2 ? (uint16_t)(pixRow * 2u + (_interlaceField ? 1u : 0u)) : pixRow;

	uint16_t nameBase = ScrollABase();
	uint32_t planePxH = (uint32_t)planeH * tilePixH;
	uint32_t planePxW = (uint32_t)planeW * 8u;

	for(uint16_t x = 0; x < pixels; x++) {
		uint16_t px = (uint16_t)(x - hscroll) & (uint16_t)(planePxW - 1u);

		uint16_t vscroll = GetVScroll(x >> 4, true);
		uint32_t py = ((uint32_t)tileRow * tilePixH + intPixRow + vscroll) % planePxH;

		uint16_t tc  = (px >> 3) & (planeW - 1u);
		uint16_t tr  = (uint16_t)(py / tilePixH) & (planeH - 1u);
		uint16_t tpx = px & 7u;
		uint16_t tpy = (uint16_t)(py % tilePixH);

		uint16_t nameAddr = nameBase + (uint16_t)((tr * planeW + tc) * 2u);
		uint16_t entry    = ((uint16_t)_vram[nameAddr & 0xFFFFu] << 8)
		                  | _vram[(nameAddr + 1u) & 0xFFFFu];

		bool     pri   = (entry >> 15) & 1;
		uint8_t  pal   = (uint8_t)((entry >> 13) & 3u);
		bool     vflip = (entry >> 12) & 1;
		bool     hflip = (entry >> 11) & 1;
		uint16_t tile  = entry & 0x7FFu;

		uint8_t col = hflip ? (7u - tpx) : (uint8_t)tpx;
		uint8_t row = vflip ? ((uint8_t)(tilePixH - 1u) - (uint8_t)tpy) : (uint8_t)tpy;

		uint16_t tileBase = int2 ? (tile * 64u) : (tile * 32u);
		uint8_t  pix = FetchTilePixel(tileBase, row, col);

		dst[x] = (uint8_t)((pri ? 0x80u : 0x00u) | (pal << 4) | pix);
	}
}

void GenesisVdp::RenderWindow(uint16_t line, uint8_t* dst, uint16_t pixels) const
{
	bool     int2     = IsInterlace2();
	uint16_t nameBase = WindowBase();
	uint16_t cellW    = IsH40() ? 64u : 32u;   // window table width in cells
	uint16_t tilePixH = int2 ? 16u : 8u;
	uint16_t winLine  = line / tilePixH;
	uint16_t pixRow   = line % tilePixH;
	uint16_t intPixRow = int2 ? (uint16_t)(pixRow * 2u + (_interlaceField ? 1u : 0u)) : pixRow;

	for(uint16_t x = 0; x < pixels; x++) {
		if(!IsWindowPixel(line, x)) {
			dst[x] = 0;  // not window — transparent (keep plane A result)
			continue;
		}

		uint16_t tc  = x >> 3;
		uint16_t tpx = x & 7u;

		uint16_t nameAddr = nameBase + (uint16_t)((winLine * cellW + tc) * 2u);
		uint16_t entry    = ((uint16_t)_vram[nameAddr & 0xFFFFu] << 8)
		                  | _vram[(nameAddr + 1u) & 0xFFFFu];

		bool     pri   = (entry >> 15) & 1;
		uint8_t  pal   = (uint8_t)((entry >> 13) & 3u);
		bool     vflip = (entry >> 12) & 1;
		bool     hflip = (entry >> 11) & 1;
		uint16_t tile  = entry & 0x7FFu;

		uint8_t col = hflip ? (7u - tpx) : (uint8_t)tpx;
		uint8_t row = vflip ? ((uint8_t)(tilePixH - 1u) - (uint8_t)intPixRow) : (uint8_t)intPixRow;

		uint16_t tileBase = int2 ? (tile * 64u) : (tile * 32u);
		uint8_t  pix = FetchTilePixel(tileBase, row, col);

		// Mark window pixels with a special flag (bit 6) so compositor knows
		dst[x] = (uint8_t)((pri ? 0x80u : 0x00u) | 0x40u | (pal << 4) | pix);
	}
}

void GenesisVdp::RenderSprites(uint16_t line, uint8_t* dst, uint16_t pixels)
{
	bool     int2       = IsInterlace2();
	uint16_t sprBase    = SpriteBase();
	uint16_t maxSprites = IsH40() ? 80u : 64u;
	uint16_t maxPerLine = IsH40() ? 20u : 16u;
	uint16_t maxCells   = IsH40() ? 40u : 32u;
	uint16_t cellPixH   = int2 ? 16u : 8u;
	uint16_t effLine    = int2 ? (uint16_t)(line * 2u + (_interlaceField ? 1u : 0u)) : line;

	GenesisLineSprite spriteList[80] = {};
	GenesisLineSpriteCell cellList[40] = {};
	uint16_t spriteCount = 0;
	uint16_t cellCount = 0;
	bool lineDotOverflow = false;
	bool prevLineDotOverflow = _prevLineDotOverflow;

	// Build the list of sprites active on this line by traversing the SAT link chain.
	uint8_t idx = 0;
	for(uint16_t s = 0; s < maxSprites; s++) {
		uint16_t entryBase = (uint16_t)(sprBase + (uint16_t)idx * 8u);
		uint16_t w0 = ((uint16_t)_vram[(entryBase + 0u) & 0xFFFFu] << 8) | _vram[(entryBase + 1u) & 0xFFFFu];
		uint16_t w1 = ((uint16_t)_vram[(entryBase + 2u) & 0xFFFFu] << 8) | _vram[(entryBase + 3u) & 0xFFFFu];
		uint16_t w2 = ((uint16_t)_vram[(entryBase + 4u) & 0xFFFFu] << 8) | _vram[(entryBase + 5u) & 0xFFFFu];
		uint16_t w3 = ((uint16_t)_vram[(entryBase + 6u) & 0xFFFFu] << 8) | _vram[(entryBase + 7u) & 0xFFFFu];

		int16_t sprY = (int16_t)(w0 & 0x01FFu) - 128;
		uint8_t vertCells = (uint8_t)(((w1 >> 8) & 0x03u) + 1u);
		uint8_t horizCells = (uint8_t)(((w1 >> 10) & 0x03u) + 1u);
		uint8_t link = (uint8_t)(w1 & 0x7Fu);
		uint16_t sprH = (uint16_t)vertCells * cellPixH;

		if((int16_t)effLine >= sprY && (int16_t)effLine < (int16_t)(sprY + sprH)) {
			if(spriteCount >= maxPerLine) {
				_status |= 0x0040u;
				break;
			}

			bool vflip = (w2 & 0x1000u) != 0;
			uint16_t sprRow = (uint16_t)((int16_t)effLine - sprY);
			uint8_t cellRow = (uint8_t)(sprRow / cellPixH);
			uint8_t pixRow = (uint8_t)(sprRow % cellPixH);

			GenesisLineSprite& sprite = spriteList[spriteCount++];
			sprite.Tile = w2 & 0x07FFu;
			sprite.RawX = w3 & 0x01FFu;
			sprite.X = (int16_t)sprite.RawX - 128;
			sprite.Palette = (uint8_t)((w2 >> 13) & 0x03u);
			sprite.VertCells = vertCells;
			sprite.HorizCells = horizCells;
			sprite.CellRow = vflip ? (uint8_t)(vertCells - 1u - cellRow) : cellRow;
			sprite.PixRow = pixRow;
			sprite.Priority = (w2 & 0x8000u) != 0;
			sprite.HFlip = (w2 & 0x0800u) != 0;
			sprite.VFlip = vflip;
		}

		if(link == 0 || link >= maxSprites) {
			break;
		}
		idx = link;
	}

	// Expand the selected sprites into individual 8x8/8x16 cells.
	// Dot-overflow accounting is based on parsed sprite cells, independent of
	// later masking decisions.
	for(uint16_t i = 0; i < spriteCount; i++) {
		const GenesisLineSprite& sprite = spriteList[i];

		for(uint8_t screenCellCol = 0; screenCellCol < sprite.HorizCells; screenCellCol++) {
			if(cellCount >= maxCells) {
				_status |= 0x0040u;
				lineDotOverflow = true;
				break;
			}

			GenesisLineSpriteCell& cell = cellList[cellCount++];
			cell.Tile = sprite.Tile;
			cell.RawX = sprite.RawX;
			cell.X = sprite.X;
			cell.Palette = sprite.Palette;
			cell.VertCells = sprite.VertCells;
			cell.ScreenCellCol = screenCellCol;
			cell.PatternCellOffsetX = sprite.HFlip ? (uint8_t)(sprite.HorizCells - 1u - screenCellCol) : screenCellCol;
			cell.PatternCellOffsetY = sprite.CellRow;
			cell.PixRow = sprite.PixRow;
			cell.Priority = sprite.Priority;
			cell.HFlip = sprite.HFlip;
			cell.VFlip = sprite.VFlip;
		}

		if(lineDotOverflow) {
			break;
		}
	}

	// Rasterize prepared cells in SAT order so the first visible sprite pixel wins.
	bool maskActive = false;
	bool nonMaskCellEncountered = false;
	for(uint16_t i = 0; i < cellCount; i++) {
		const GenesisLineSpriteCell& cell = cellList[i];
		if(cell.RawX == 0) {
			if(nonMaskCellEncountered || prevLineDotOverflow) {
				maskActive = true;
			}
			continue;
		}

		nonMaskCellEncountered = true;
		if(maskActive) {
			continue;
		}

		uint16_t tileIdx = cell.Tile + (uint16_t)(cell.PatternCellOffsetX * cell.VertCells) + cell.PatternCellOffsetY;
		uint16_t tileBase = int2 ? (uint16_t)(tileIdx * 64u) : (uint16_t)(tileIdx * 32u);

		for(uint8_t px = 0; px < 8u; px++) {
			int16_t screenX = cell.X + (int16_t)(cell.ScreenCellCol * 8u + px);
			if(screenX < 0 || screenX >= (int16_t)pixels) {
				continue;
			}

			uint8_t col = cell.HFlip ? (uint8_t)(7u - px) : px;
			uint8_t row = cell.VFlip ? (uint8_t)(cellPixH - 1u - cell.PixRow) : cell.PixRow;
			uint8_t pix = FetchTilePixel(tileBase, row, col);
			if(pix == 0) {
				continue;
			}

			if(dst[screenX] != 0) {
				_status |= 0x0020u;
				continue;
			}

			dst[screenX] = (uint8_t)((cell.Priority ? 0x80u : 0x00u) | (cell.Palette << 4) | pix);
		}
	}

	_prevLineDotOverflow = lineDotOverflow;
}

void GenesisVdp::Composite(uint16_t line,
	const uint8_t* planeB, const uint8_t* planeA,
	const uint8_t* spr,    uint16_t pixels)
{
	if(!_fb) return;
	uint32_t* outLine = _fb + (uint32_t)line * _fbW;

	// Priority order (front to back):
	//   1. hi-pri window
	//   2. hi-pri sprite
	//   3. hi-pri plane A
	//   4. hi-pri plane B
	//   5. lo-pri window
	//   6. lo-pri sprite
	//   7. lo-pri plane A
	//   8. lo-pri plane B
	//   9. backdrop

	uint8_t bgIdx = _reg[7] & 0x3Fu;
	bool    shMode = ShadowHlEnabled();

	// Shadow/highlight shade codes
	enum : uint8_t { Shade_Shadow = 0, Shade_Normal = 1, Shade_Highlight = 2 };

	for(uint16_t x = 0; x < pixels; x++) {
		uint8_t pB = planeB[x];
		uint8_t pA = planeA[x];  // may have bit6 set = window
		uint8_t pS = spr[x];

		// Decode layer flags
		bool  winSrc = (pA & 0x40u) != 0;
		bool  winHi  = winSrc && ((pA & 0x80u) != 0);
		bool  winVis = winSrc && ((pA & 0x0Fu) != 0);
		bool  sprHi  = (pS & 0x80u) != 0;
		bool  sprVis = (pS & 0x0Fu) != 0;
		bool  pAHi   = !winSrc && ((pA & 0x80u) != 0);
		bool  pAVis  = !winSrc && ((pA & 0x0Fu) != 0);
		bool  pBHi   = (pB & 0x80u) != 0;
		bool  pBVis  = (pB & 0x0Fu) != 0;

		// Determine shade in shadow/highlight mode
		uint8_t shade = Shade_Normal;
		bool    sprIsOp = false;  // sprite is a shadow/highlight operator

		if(shMode) {
			// Step 1: any hi-pri visible plane or window → normal
			if((winHi && winVis) || (pAHi && pAVis) || (pBHi && pBVis)) {
				shade = Shade_Normal;
			}
			// Step 2: sprite determines shade (operators are transparent)
			if(sprVis) {
				uint8_t sprPal   = (pS >> 4) & 3u;
				uint8_t sprColor = pS & 0x0Fu;
				if(!sprHi && sprPal == 3u) {
					if(sprColor == 14u) {
						// Shadow operator: force shadow, sprite is transparent
						shade    = Shade_Shadow;
						sprIsOp  = true;
					} else if(sprColor == 15u) {
						// Highlight operator: lift one brightness level, sprite is transparent.
						// Doc: "If the pixel is shadowed by the first rule, it will appear normal."
						// Shadow→Normal, Normal→Highlight.
						shade   = (shade == Shade_Shadow) ? Shade_Normal : Shade_Highlight;
						sprIsOp = true;
					}
					// else: lo-pri pal-3 regular sprite — shade stays as determined by planes
				} else if(sprHi) {
					// High-priority non-operator sprite → normal brightness
					shade = Shade_Normal;
				} else if(sprColor == 15u) {
					// Lo-pri sprite with color 15 in non-operator palette → always Normal.
					// Doc: "Pixels in sprites using colour 15 of palette lines 1, 2 or 3
					//        will always appear normal."
					shade = Shade_Normal;
				}
			}
		}

		// Select cramIdx using priority chain.
		// In S/H mode, operator sprites are transparent (skip them in color selection).
		uint8_t cramIdx = bgIdx;  // backdrop fallback
		bool    backdrop = true;

		if      (winHi && winVis)                    { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (sprHi && sprVis && !sprIsOp)       { cramIdx = (uint8_t)(((pS >> 4) & 3u) * 16u + (pS & 0x0Fu)); backdrop = false; }
		else if (pAHi  && pAVis)                    { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (pBHi  && pBVis)                    { cramIdx = (uint8_t)(((pB >> 4) & 3u) * 16u + (pB & 0x0Fu)); backdrop = false; }
		else if (!winHi && winVis)                  { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (!sprHi && sprVis && !sprIsOp)      { cramIdx = (uint8_t)(((pS >> 4) & 3u) * 16u + (pS & 0x0Fu)); backdrop = false; }
		else if (!pAHi  && pAVis)                   { cramIdx = (uint8_t)(((pA >> 4) & 3u) * 16u + (pA & 0x0Fu)); backdrop = false; }
		else if (!pBHi  && pBVis)                   { cramIdx = (uint8_t)(((pB >> 4) & 3u) * 16u + (pB & 0x0Fu)); backdrop = false; }

		cramIdx &= 0x3Fu;
		_lineBackdropMask[x] = backdrop ? 1u : 0u;

		if(shMode) {
			switch(shade) {
				case Shade_Shadow:    outLine[x] = _shadowPalette   [cramIdx]; break;
				case Shade_Highlight: outLine[x] = _highlightPalette[cramIdx]; break;
				default:              outLine[x] = _palette         [cramIdx]; break;
			}
		} else {
			outLine[x] = _palette[cramIdx];
		}
	}

	// Fill any framebuffer pixels beyond active display with black
	uint32_t black = 0xFF000000u;
	for(uint32_t x = pixels; x < _fbW; x++) {
		outLine[x] = black;
	}
	for(uint32_t x = pixels; x < std::min<uint32_t>(_fbW, 320u); x++) {
		_lineBackdropMask[x] = 0u;
	}
}

// ===========================================================================
// BeginLine / EndLine / RunLine
// ===========================================================================

void GenesisVdp::BeginLine(uint16_t line, uint32_t* fb, uint32_t fbW, uint32_t fbH)
{
	_fb   = fb;
	_fbW  = fbW;
	_fbH  = fbH;
	_scanline = line;

	uint16_t activeH = ActiveHeight();

	if(line == 0) {
		// Start of frame: reset V-blank, reload H-int counter
		_status &= ~0x0008u;  // clear V-blank
		_hintCounter = (int)HIntReload();
		_frameCount++;
		_prevLineDotOverflow = false;

		// Interlace field: toggle every frame; update status bit 4 (odd frame)
		if(IsInterlace()) {
			_interlaceField = !_interlaceField;
		} else {
			_interlaceField = false;
		}
		if(_interlaceField) _status |= 0x0010u;
		else                _status &= ~0x0010u;
	}

	// Clear H-blank at line start.
	_status &= ~0x0004u;

	if(line < activeH) {
		// Active display.
		_status &= ~0x0008u;

		if(DispEnabled()) {
			// Initialize per-line slot state
			_slotCycles = IsH40() ? 16u : 20u;
			_maxSpritesLine = IsH40() ? MAX_SPRITES_LINE_H40 : MAX_SPRITES_LINE_H32;
			_maxDrawsLine = IsH40() ? MAX_DRAWS_H40 : MAX_DRAWS_H32;
			_slotIndex = 0;
			_bufAOff = 0;
			_bufBOff = 0;
			memset(_tmpBufA, 0, sizeof(_tmpBufA));
			memset(_tmpBufB, 0, sizeof(_tmpBufB));

			// Clear sprite linebuf + composite buffer for this line
			memset(_compositebuf, 0, sizeof(_compositebuf));
			SlotClearLinebuf();

			// Load hscroll for this line
			_hscrollA = GetHScrollRaw(line, true);
			_hscrollB = GetHScrollRaw(line, false);
			_hscrollAFine = _hscrollA & 0x0Fu;
			_hscrollBFine = _hscrollB & 0x0Fu;
			ScrollTraceLog(_frameCount, _scanline, -1,
				"HS_BEGIN mode=%u base=%04X hA=%04X hB=%04X fineA=%u fineB=%u",
				(unsigned)(_reg[11] & 0x03u), (unsigned)HScrollBase(),
				(unsigned)_hscrollA, (unsigned)_hscrollB,
				(unsigned)_hscrollAFine, (unsigned)_hscrollBFine);

			// Batch sprite processing: scan + build draw list + render into linebuf.
			// Sprites are processed atomically so that column slots can read from
			// a fully-populated _linebuf. Per-slot sprite timing is a future step.
			_sprInfoCount = 0;
			memset(_spriteDrawList, 0, sizeof(_spriteDrawList));
			_sprDraws = _maxSpritesLine;
			_sprCurSlot = 0;
			_sprRenderIdx = 0;
			_sprRenderCell = 0;
			_sprScanLink = 0;
			_sprScanDone = false;
			_sprCellBudget = (uint8_t)(ActiveWidth() / 8u); // H40=40, H32=32

			for(uint16_t s = 0; s < (IsH40() ? 80u : 64u) && !_sprScanDone; s++) {
				SlotScanSpriteTable();
			}
			for(int8_t s = 0; s < (int8_t)_sprInfoCount; s++) {
				_sprCurSlot = s;
				SlotReadSpriteX();
			}
			_sprRenderIdx = 0;
			_sprRenderCell = 0;
			uint8_t drawCount = (uint8_t)(_maxSpritesLine - _sprDraws);
			while(_sprRenderIdx < drawCount && _sprCellBudget > 0) {
				SlotSpriteRender();
			}
			// Phantom mask sprites: clear _sprCanMask if unused draw slots remain
			// (see SlotSpriteRender for the full explanation).
			if(_sprRenderIdx >= drawCount && drawCount < _maxSpritesLine) {
				_sprCanMask = false;
			}
			// BlastEm does not feed sprite overflow into next-line mask activation.
			_prevLineDotOverflow = false;
		}
	} else if(line == activeH) {
		// First V-blank line.
		_status |= 0x0008u;
		if(VIntEnabled()) { _vintPending = true; _vintNew = true; }
		_vintFiredFrame = true;  // V-blank start processed; suppress further NextVIntMclk events
	} else {
		// Remaining V-blank lines.
		_status |= 0x0008u;
	}
}

void GenesisVdp::CacheSpriteTable()
{
	uint16_t base = SpriteBase();
	for(uint16_t i = 0; i < SAT_CACHE_SIZE; i++) {
		_satCache[i] = _vram[(base + i) & 0xFFFFu];
	}
}

void GenesisVdp::EndLine()
{
	// Snapshot the SAT for the next line's sprite scan. This must happen
	// *before* the 68K runs the next line's cycles so that the scan sees the
	// VRAM state at the end of the current line — matching the hardware sprite
	// scan timing which reads the SAT during H-blank.
	CacheSpriteTable();

	// Enter H-blank at end of scanline.
	_status |= 0x0004u;

	// H-interrupt counter behaviour (per hardware reference):
	// Active display (line < ActiveHeight): decrement; fire and reload at 0.
	// V-blank (line >= ActiveHeight): reload every line; do NOT fire interrupt.
	if(_scanline < ActiveHeight()) {
		if(_hintCounter <= 0) {
			if(HIntEnabled()) _hintPending = true;
			_hintCounter = (int)HIntReload();
		} else {
			_hintCounter--;
		}
	} else {
		// V-blank — reset counter for the next active-display period.
		_hintCounter = (int)HIntReload();
	}
}

void GenesisVdp::RunLine(uint16_t line, uint32_t* fb, uint32_t fbW, uint32_t fbH)
{
	BeginLine(line, fb, fbW, fbH);
	EndLine();
}

bool GenesisVdp::ConsumeVInt()
{
	if(_vintNew) {
		_vintNew = false;
		return true;
	}
	return false;
}

bool GenesisVdp::ConsumeHInt()
{
	if(_hintPending) {
		_hintPending = false;
		return true;
	}
	return false;
}

// ===========================================================================
// Event-driven master-clock scheduler interface
// ===========================================================================

void GenesisVdp::BeginFrame(uint32_t* fb, uint32_t fbW, uint32_t fbH)
{
	sDmaTraceBuffer.clear();
	sSpriteTraceBuffer.clear();
	sComposeTraceBuffer.clear();
	sScrollTraceBuffer.clear();
	sHScrollDmaTraceBuffer.clear();

	_frameFb  = fb;
	_frameFbW = fbW;
	_frameFbH = fbH;
	_mclkPos        = 0;
	_lineBegun      = false;
	_vintFiredFrame = false;
	if(_dmaTraceFile && _frameCount <= 160) {
		fprintf(_dmaTraceFile, "--- FRAME %u dmaType=%d dmaLen=%u status=%04X ---\n",
			_frameCount, (int)_dmaType, _dmaLen, _status);
		fflush(_dmaTraceFile);
	}
	if(_frameCount <= 160) {
		char lineBuf[160] = {};
		snprintf(lineBuf, sizeof(lineBuf), "--- FRAME %u dmaType=%d dmaLen=%u status=%04X ---",
			_frameCount, (int)_dmaType, _dmaLen, _status);
		AppendTraceBufferLine(sDmaTraceBuffer, lineBuf);
	}
	// Pre-load H-int counter so NextHIntMclk() is valid before AdvanceToMclk
	// calls BeginLine(0). BeginLine(0) will set this again (idempotent).
	_hintCounter = (int)HIntReload();

	// Seed the SAT cache so line 0's sprite scan uses the VRAM state from the
	// end of the previous frame (before the 68K runs any cycles this frame).
	CacheSpriteTable();
}

void GenesisVdp::GetDebugState(GenesisVdpDebugState& state) const
{
	memset(&state, 0, sizeof(state));

	state.FrameCount = _frameCount;
	state.Scanline = _scanline;
	state.HClock = GetHClock();
	state.VClock = GetVClock();
	state.HvCounter = GetHVCounter();
	state.Status = _status;
	state.ActiveWidth = ActiveWidth();
	state.ActiveHeight = ActiveHeight();
	memcpy(state.Regs, _reg, sizeof(_reg));
	state.IsH40 = IsH40();
	state.Interlace2 = IsInterlace2();
	state.DisplayEnabled = DispEnabled();
	state.ShadowHighlightEnabled = ShadowHlEnabled();

	state.SlotIndex = _slotIndex;
	state.SlotCycles = _slotCycles;

	memcpy(state.TmpBufA, _tmpBufA, sizeof(_tmpBufA));
	memcpy(state.TmpBufB, _tmpBufB, sizeof(_tmpBufB));
	state.BufAOff = _bufAOff;
	state.BufBOff = _bufBOff;

	state.Col1 = _col1;
	state.Col2 = _col2;
	state.ColB1 = _colB1;
	state.ColB2 = _colB2;
	state.VOffsetA = _vOffsetA;
	state.VOffsetB = _vOffsetB;
	state.WindowActive = _windowActive;
	state.VscrollLatch[0] = _vscrollLatch[0];
	state.VscrollLatch[1] = _vscrollLatch[1];
	state.HscrollA = _hscrollA;
	state.HscrollAFine = _hscrollAFine;
	state.HscrollB = _hscrollB;
	state.HscrollBFine = _hscrollBFine;

	memcpy(state.Linebuf, _linebuf, sizeof(_linebuf));
	memcpy(state.Compositebuf, _compositebuf, sizeof(_compositebuf));

	state.SprInfoCount = _sprInfoCount;
	state.SprDraws = _sprDraws;
	state.SprCurSlot = _sprCurSlot;
	state.SprRenderIdx = _sprRenderIdx;
	state.SprRenderCell = _sprRenderCell;
	state.SprScanLink = _sprScanLink;
	state.SprScanDone = _sprScanDone;
	state.SprCanMask = _sprCanMask;
	state.SprMasked = _sprMasked;
	state.MaxSpritesLine = _maxSpritesLine;
	state.MaxDrawsLine = _maxDrawsLine;
	state.SprCellBudget = _sprCellBudget;
	state.PrevLineDotOverflow = _prevLineDotOverflow;

	for(uint32_t i = 0; i < GenesisDebugMaxSpritesLine; i++) {
		state.SpriteInfos[i].Index = _spriteInfoList[i].index;
		state.SpriteInfos[i].Y = _spriteInfoList[i].y;
		state.SpriteInfos[i].Size = _spriteInfoList[i].size;
	}

	for(uint32_t i = 0; i < GenesisDebugMaxSpriteDraws; i++) {
		state.SpriteDrawList[i].XPos = _spriteDrawList[i].xPos;
		state.SpriteDrawList[i].Address = _spriteDrawList[i].address;
		state.SpriteDrawList[i].PalPri = _spriteDrawList[i].palPri;
		state.SpriteDrawList[i].HFlip = _spriteDrawList[i].hFlip;
		state.SpriteDrawList[i].Width = _spriteDrawList[i].width;
		state.SpriteDrawList[i].Height = _spriteDrawList[i].height;
		state.SpriteDrawList[i].BaseTile = _spriteDrawList[i].baseTile;
		state.SpriteDrawList[i].CellRow = _spriteDrawList[i].cellRow;
		state.SpriteDrawList[i].PixRow = _spriteDrawList[i].pixRow;
		state.SpriteDrawList[i].SatIndex = _spriteDrawList[i].satIndex;
	}
}

bool GenesisVdp::GetDebugTraceLines(GenesisTraceBufferKind kind, vector<string>& lines) const
{
	switch(kind) {
		case GenesisTraceBufferKind::Dma:
			lines = sDmaTraceBuffer;
			return true;
		case GenesisTraceBufferKind::Sprite:
			lines = sSpriteTraceBuffer;
			return true;
		case GenesisTraceBufferKind::Compose:
			lines = sComposeTraceBuffer;
			return true;
		case GenesisTraceBufferKind::Scroll:
			lines = sScrollTraceBuffer;
			return true;
		case GenesisTraceBufferKind::HScrollDma:
			lines = sHScrollDmaTraceBuffer;
			return true;
	}

	lines.clear();
	return false;
}

void GenesisVdp::AdvanceToMclk(uint32_t targetMclk)
{
	while(_mclkPos < targetMclk) {
		uint32_t currentLine = _mclkPos / MCLKS_PER_LINE;
		uint32_t lineEnd     = (currentLine + 1u) * MCLKS_PER_LINE;

		if(!_lineBegun) {
			BeginLine((uint16_t)currentLine, _frameFb, _frameFbW, _frameFbH);
			_lineBegun = true;
		}

		bool isActiveLine = currentLine < (uint32_t)ActiveHeight() && DispEnabled();
		if(isActiveLine) {
			// --- Per-slot dispatch for active display lines ---
			const SlotDescriptor* table = IsH40() ? kSlotTableH40 : kSlotTableH32;
			uint16_t slotCount = IsH40() ? SLOT_COUNT_H40 : SLOT_COUNT_H32;

			while(_mclkPos < targetMclk && _slotIndex < slotCount) {
				const SlotDescriptor& slot = table[_slotIndex];

				// Dispatch all slot types except sprite ops (already batched in BeginLine)
				switch(slot.op) {
					case SlotOp::ReadMapScrollA:
					case SlotOp::ReadMapScrollB:
					case SlotOp::RenderMap1:
					case SlotOp::RenderMap2:
					case SlotOp::RenderMap3:
					case SlotOp::RenderMapOutput:
					case SlotOp::HScrollLoad:
						DispatchSlot(slot);
						break;
					case SlotOp::ExternalSlot:
						SlotExternalSlot();
						break;
					case SlotOp::SpriteRender:
					case SlotOp::ReadSpriteX:
					case SlotOp::ClearLinebuf:
						// Sprites already processed in BeginLine batch
						// and the sprite line buffer must remain valid through the
						// tail column composite at the end of the active area.
						break;
					case SlotOp::Refresh:
					case SlotOp::Nop:
						break;
				}

				_mclkPos += _slotCycles;
				_slotIndex++;
			}

			// Tail column render (C40 in H40, C32 in H32) to complete the visible width.
			// The static slot table stops at C38/C30, so emit one final map/composite step
			// once per line before flushing.
			if(_slotIndex == slotCount) {
				int16_t tailCol = IsH40() ? 40 : 32;
				SlotReadMapScrollA(tailCol);
				SlotRenderMap1();
				SlotRenderMap2();
				SlotReadMapScrollB(tailCol);
				SlotRenderMap3();
				SlotRenderMapOutput(tailCol);
				_slotIndex++;
			}

			// All slots dispatched — snap to line boundary (HSYNC dead time)
			if(_slotIndex >= slotCount && _mclkPos < lineEnd) {
				if(lineEnd <= targetMclk) {
					_mclkPos = lineEnd;
				} else {
					// Target is in HSYNC dead zone — advance to target
					_mclkPos = targetMclk;
				}
			}

			// Line complete?
			if(_mclkPos >= lineEnd) {
				FlushCompositeBuf((uint16_t)currentLine);
				EndLine();
				_lineBegun = false;
				_mclkPos = lineEnd;
			}
		} else {
			// --- V-blank or display-disabled: skip to line end ---
			if(lineEnd <= targetMclk) {
				// Display disabled during active region — fill with backdrop
				if(currentLine < (uint32_t)ActiveHeight() && !DispEnabled()) {
					if(_fb && currentLine < _fbH) {
						uint32_t bgColor = _palette[_reg[7] & 0x3Fu];
						uint32_t* outLine = _fb + currentLine * _fbW;
						for(uint32_t x = 0; x < _fbW; x++) {
							outLine[x] = bgColor;
						}
					}
				}
				EndLine();
				_lineBegun = false;
				_mclkPos = lineEnd;
			} else {
				_mclkPos = targetMclk;
				break;
			}
		}
	}

	// If we stopped exactly at a line boundary without having begun the line,
	// call BeginLine now so that start-of-line events (e.g. V-int at VBlank)
	// are fired before the caller delivers interrupts.
	if(_mclkPos == targetMclk && !_lineBegun && (_mclkPos % MCLKS_PER_LINE == 0u)) {
		uint32_t line = _mclkPos / MCLKS_PER_LINE;
		if(line < (uint32_t)TotalScanlines()) {
			BeginLine((uint16_t)line, _frameFb, _frameFbW, _frameFbH);
			_lineBegun = true;
		}
	}
}

uint32_t GenesisVdp::NextVIntMclk() const
{
	if(!VIntEnabled() || _vintFiredFrame) return UINT32_MAX;
	uint32_t vblankMclk = (uint32_t)ActiveHeight() * MCLKS_PER_LINE;
	return (_mclkPos <= vblankMclk) ? vblankMclk : UINT32_MAX;
}

uint32_t GenesisVdp::NextHIntMclk() const
{
	if(!HIntEnabled()) return UINT32_MAX;
	uint32_t currentLine = _mclkPos / MCLKS_PER_LINE;

	// H-int only fires during active display lines (0 .. ActiveHeight()-1).
	// During V-blank the counter is reloaded each line but no interrupt is raised.
	if(currentLine >= (uint32_t)ActiveHeight()) return UINT32_MAX;

	uint32_t eventMclk  = (currentLine + (uint32_t)_hintCounter + 1u) * MCLKS_PER_LINE;
	uint32_t vblankMclk = (uint32_t)ActiveHeight() * MCLKS_PER_LINE;

	// If the counter roll-over would land in or past V-blank, the interrupt will
	// not fire this frame (it fires at REG_HINT lines from the next frame start).
	if(eventMclk > vblankMclk) return UINT32_MAX;

	return eventMclk;
}

// ===========================================================================
// Save / load state
// ===========================================================================

void GenesisVdp::SaveState(vector<uint8_t>& out) const
{
	AppV(out, _vram);
	AppV(out, _cram);
	AppV(out, _vsram);
	AppV(out, _reg);
	AppV(out, _ctrlPend);
	AppV(out, _ctrlFirst);
	AppV(out, _addrReg);
	AppV(out, _codeReg);
	AppV(out, _readBuf);
	AppV(out, _writeHi);
	AppV(out, _writeHiData);
	AppV(out, _writeHiCtrl);
	AppV(out, _status);
	AppV(out, _statusReadLatch);
	AppV(out, _statusReadLatchValid);
	AppV(out, _vintPending);
	AppV(out, _vintNew);
	AppV(out, _hintPending);
	AppV(out, _hintCounter);
	uint8_t dm = (uint8_t)_dmaType;
	AppV(out, dm);
	AppV(out, _dmaSrc);
	AppV(out, _dmaLen);
	AppV(out, _dmaFillVal);
	AppV(out, _dmaFillPend);
	AppV(out, _dmaBusStartDelayMclk);
	AppV(out, _dmaBusMclkRemainder);
	AppV(out, _dmaVdpMclkRemainder);
	AppV(out, _interlaceField);
	AppV(out, _scanline);
	AppV(out, _frameCount);
	AppV(out, _prevLineDotOverflow);
	AppV(out, _mclkPos);
	AppV(out, _lineBegun);
	AppV(out, _vintFiredFrame);
	AppV(out, _dmaAddr);
	AppV(out, _dmaCode);
	// Sprite mask carry-over (added in version 33)
	AppV(out, _sprCanMask);
	// FIFO state (added in version 32)
	AppV(out, _fifoRead);
	AppV(out, _fifoWrite);
	AppV(out, _fifoCount);
	for(int i = 0; i < 4; i++) {
		AppV(out, _fifo[i].data);
		AppV(out, _fifo[i].addr);
		AppV(out, _fifo[i].code);
		AppV(out, _fifo[i].pad);
	}
	// HV latch (added in version 32)
	AppV(out, _hvLatch);
}

bool GenesisVdp::LoadState(const vector<uint8_t>& data, size_t& offset)
{
	if(!RdV(data, offset, _vram))        return false;
	if(!RdV(data, offset, _cram))        return false;
	if(!RdV(data, offset, _vsram))       return false;
	if(!RdV(data, offset, _reg))         return false;
	if(!RdV(data, offset, _ctrlPend))    return false;
	if(!RdV(data, offset, _ctrlFirst))   return false;
	if(!RdV(data, offset, _addrReg))     return false;
	if(!RdV(data, offset, _codeReg))     return false;
	if(!RdV(data, offset, _readBuf))     return false;
	if(!RdV(data, offset, _writeHi))     return false;
	if(!RdV(data, offset, _writeHiData)) return false;
	if(!RdV(data, offset, _writeHiCtrl)) return false;
	if(!RdV(data, offset, _status))      return false;
	if(!RdV(data, offset, _statusReadLatch)) return false;
	if(!RdV(data, offset, _statusReadLatchValid)) return false;
	if(!RdV(data, offset, _vintPending)) return false;
	if(!RdV(data, offset, _vintNew))     return false;
	if(!RdV(data, offset, _hintPending)) return false;
	if(!RdV(data, offset, _hintCounter)) return false;
	uint8_t dm = 0;
	if(!RdV(data, offset, dm))           return false;
	_dmaType = (DmaType)dm;
	if(!RdV(data, offset, _dmaSrc))      return false;
	if(!RdV(data, offset, _dmaLen))      return false;
	if(!RdV(data, offset, _dmaFillVal))  return false;
	if(!RdV(data, offset, _dmaFillPend)) return false;
	if(!RdV(data, offset, _dmaBusStartDelayMclk)) return false;
	if(!RdV(data, offset, _dmaBusMclkRemainder)) return false;
	if(!RdV(data, offset, _dmaVdpMclkRemainder)) return false;
	if(!RdV(data, offset, _interlaceField)) return false;
	if(!RdV(data, offset, _scanline))      return false;
	if(!RdV(data, offset, _frameCount))   return false;
	if(offset < data.size()) {
		if(!RdV(data, offset, _prevLineDotOverflow)) return false;
	} else {
		_prevLineDotOverflow = false;
	}
	if(!RdV(data, offset, _mclkPos))      return false;
	if(!RdV(data, offset, _lineBegun))    return false;
	if(!RdV(data, offset, _vintFiredFrame)) return false;
	if(offset + sizeof(_dmaAddr) + sizeof(_dmaCode) <= data.size()) {
		if(!RdV(data, offset, _dmaAddr)) return false;
		if(!RdV(data, offset, _dmaCode)) return false;
	} else {
		_dmaAddr = _addrReg;
		_dmaCode = _codeReg;
	}

	// Sprite mask carry-over (version 33+)
	if(offset + sizeof(_sprCanMask) <= data.size()) {
		if(!RdV(data, offset, _sprCanMask)) return false;
	} else {
		_sprCanMask = false;
	}

	// FIFO state (version 32+)
	if(offset + 3 + 4 * 6 <= data.size()) {
		if(!RdV(data, offset, _fifoRead))  return false;
		if(!RdV(data, offset, _fifoWrite)) return false;
		if(!RdV(data, offset, _fifoCount)) return false;
		for(int i = 0; i < 4; i++) {
			if(!RdV(data, offset, _fifo[i].data)) return false;
			if(!RdV(data, offset, _fifo[i].addr)) return false;
			if(!RdV(data, offset, _fifo[i].code)) return false;
			if(!RdV(data, offset, _fifo[i].pad))  return false;
		}
	} else {
		// Older save state — initialize FIFO as empty
		memset(_fifo, 0, sizeof(_fifo));
		_fifoRead = _fifoWrite = _fifoCount = 0;
	}
	// HV latch (version 32+)
	if(offset + sizeof(_hvLatch) <= data.size()) {
		if(!RdV(data, offset, _hvLatch)) return false;
	} else {
		_hvLatch = 0;
	}

	// Debug register is not serialized; clear to power-on default on load.
	_debugReg = 0;

	// Rebuild expanded palette
	for(uint8_t i = 0; i < 64; i++) RefreshPalette(i);

	return true;
}
