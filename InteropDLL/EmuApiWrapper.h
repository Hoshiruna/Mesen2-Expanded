#pragma once

#include "Common.h"
#include "Core/Shared/CheatManager.h"
#include "Core/Shared/Interfaces/INotificationListener.h"

extern "C" {
	DllExport bool __stdcall TestDll();
	DllExport void __stdcall SetExclusiveFullscreenMode(bool fullscreen, void* windowHandle);
	DllExport void __stdcall TakeScreenshot();
	DllExport void __stdcall Stop();
	DllExport void __stdcall UnregisterNotificationCallback(INotificationListener* listener);
	DllExport void __stdcall SetRendererSize(uint32_t width, uint32_t height);
	DllExport void __stdcall SetCheats(CheatCode codes[], uint32_t length);
	DllExport void __stdcall WriteLogEntry(char* message);
	DllExport void __stdcall SaveStateFile(char* filepath);
}
