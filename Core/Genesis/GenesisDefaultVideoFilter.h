#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/Emulator.h"

// ---------------------------------------------------------------------------
// GenesisDefaultVideoFilter
//
// The Genesis core renders into an ARGB8888 buffer which is already display-
// ready. We reinterpret the uint16_t* parameter (actually a uint8_t* payload
// cast by the generic video path) back to uint32_t* and copy directly.
// ---------------------------------------------------------------------------
class GenesisDefaultVideoFilter : public BaseVideoFilter
{
protected:
	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		uint32_t* out = GetOutputBuffer();
		FrameInfo frame = _frameInfo;

		// The framebuffer is ARGB8888, passed via GetPpuFrame().FrameBuffer.
		// VideoDecoder casts it to uint16_t*; cast it back to uint32_t* here.
		const uint32_t* src = reinterpret_cast<const uint32_t*>(ppuOutputBuffer);
		uint32_t pixelCount = frame.Width * frame.Height;
		memcpy(out, src, pixelCount * sizeof(uint32_t));
	}

public:
	GenesisDefaultVideoFilter(Emulator* emu) : BaseVideoFilter(emu) {}
};
