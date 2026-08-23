#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include "preview_3d.h"

struct IDXGISwapChain;
struct ID3D11DeviceContext;
struct ID3D11Device;
struct ID3D11Texture2D;

bool InitializePanoramaPreview();
void UpdatePanoramaPreview(IDXGISwapChain* swapChain,
                           ID3D11DeviceContext* context,
                           float left, float top, float right, float bottom,
                           bool visible);
ID3D11Texture2D* GetPanoramaPreviewTexture();
ID3D11Texture2D* GetPanoramaPreviewFrozenTexture();
std::wstring PanoramaFallbackPath(int heroId);
bool PersistPanoramaFallbackFrame(ID3D11Device*, ID3D11DeviceContext*, int heroId,
                                  const Preview3DFrame& frame);
bool LoadPanoramaFallbackFrame(int heroId, Preview3DFrame& frame);
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
void SetPanoramaRadarEnabled(bool enabled);
void ShutdownPanoramaPreview();
