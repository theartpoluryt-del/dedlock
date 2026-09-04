#pragma once

#include <cstddef>
#include <dxgi.h>

bool PrepareD2DMenu(IDXGISwapChain* swapChain);
bool UsesSoftwareD2DMenu();
void RenderD2DMenu(std::size_t playerCount);
bool HandleD2DMenuTextInput(UINT message, WPARAM wParam);
bool GetD2DPreviewCaptureRect(float& left, float& top, float& right, float& bottom);
void ShutdownD2DMenu();
bool GetD2DPreviewCaptureRect(float& left, float& top, float& right, float& bottom);
