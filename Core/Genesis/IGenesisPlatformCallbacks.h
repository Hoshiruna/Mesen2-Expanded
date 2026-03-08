#pragma once
#include <cstdint>

class IGenesisPlatformCallbacks
{
public:
	virtual ~IGenesisPlatformCallbacks() = default;

	virtual void OnVideoFrame(const uint32_t* pixels, uint32_t pitch,
	                          uint32_t width, uint32_t height) = 0;
	virtual void OnAudioSamples(const int16_t* samples, uint32_t pairCount, uint32_t sourceRate) = 0;
	virtual uint32_t GetAudioSampleRate() = 0;
	virtual uint32_t GetControllerButtons(int port) = 0;
	virtual bool IsControllerConnected(int port) = 0;
};
