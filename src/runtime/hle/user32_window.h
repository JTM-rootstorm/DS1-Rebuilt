#pragma once
// Window state shared between user32_window.cpp and other HLE modules
// (ddraw needs the SDL window behind an HWND).

#include <SDL3/SDL.h>

#include <cstdint>

namespace ds1 {

// The SDL window behind a top-level HWND, or nullptr.
SDL_Window* SdlWindowFor(uint32_t hwnd);

// Ensures SDL video is initialised (main thread only).
void EnsureSdlVideo();

// Pumps SDL events into the guest message queues. Main thread only;
// called from PeekMessage/GetMessage and long waits.
void PumpHostEvents();

}  // namespace ds1
