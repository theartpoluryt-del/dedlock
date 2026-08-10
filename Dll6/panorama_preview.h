#pragma once

#include <cstdint>
#include <cstddef>

struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct Preview3DPoint;

bool InitializePanoramaPreview();
void UpdatePanoramaPreview(IDXGISwapChain* swapChain,
                           ID3D11DeviceContext* context,
                           float left, float top, float right, float bottom,
                           bool visible);
ID3D11Texture2D* GetPanoramaPreviewTexture();
uint64_t GetPanoramaPreviewCaptureSerial();
bool GetPanoramaPreviewSkeleton(Preview3DPoint* points, std::size_t count);
void SetPanoramaPreviewHero(int heroId);
int GetPanoramaPreviewHero();
void SetPanoramaPreviewRole(int role);
void SetPanoramaPreviewHeroForRole(int role, int heroId);
int GetPanoramaPreviewHeroForRole(int role);
bool IsPanoramaPreviewNativeGlowActive();
void ReportPanoramaPreviewGlowRegistration(bool success);
void ReportPanoramaPreviewBinding(bool success);
void ProcessPanoramaPreviewUiThread();
void ShutdownPanoramaPreview();
