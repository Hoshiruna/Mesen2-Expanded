#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/Emulator.h"

// ---------------------------------------------------------------------------
// GenesisDefaultVideoFilter
//
// The Ares VDP renders into an ARGB8888 buffer which is already ready for
// display. We reinterpret the uint16_t* parameter (which is actually the
// ARGB32 framebuffer cast from uint8_t*) back to uint32_t* and do a direct
// copy to the output buffer.
// ---------------------------------------------------------------------------
class GenesisDefaultVideoFilter : public BaseVideoFilter
{
protected:
	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		uint32_t* out = GetOutputBuffer();
		FrameInfo frame = _frameInfo;

		// The Ares framebuffer is ARGB8888, passed via GetPpuFrame().FrameBuffer
		// which VideoDecoder casts to uint16_t*. We cast it back to uint32_t*.
		const uint32_t* src = reinterpret_cast<const uint32_t*>(ppuOutputBuffer);
		uint32_t pixelCount = frame.Width * frame.Height;
		memcpy(out, src, pixelCount * sizeof(uint32_t));
	}

public:
	GenesisDefaultVideoFilter(Emulator* emu) : BaseVideoFilter(emu) {}
};
