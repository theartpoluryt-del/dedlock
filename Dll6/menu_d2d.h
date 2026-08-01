#pragma once

#include <cstddef>
#include <dxgi.h>

bool PrepareD2DMenu(IDXGISwapChain* swapChain);
bool UsesSoftwareD2DMenu();
void RenderD2DMenu(std::size_t playerCount);
void ShutdownD2DMenu();
