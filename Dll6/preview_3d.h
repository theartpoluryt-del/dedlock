#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

struct Preview3DPoint {
    float x = 0.0f;
    float y = 0.0f;
    bool visible = false;
};

struct Preview3DFrame {
    ID3D11Texture2D* texture = nullptr;
    std::array<Preview3DPoint, 18> skeleton{};
    float left = 0.0f;
    float top = 0.0f;
    float right = 1.0f;
    float bottom = 1.0f;
};

bool RenderPreview3D(ID3D11Device* device, ID3D11DeviceContext* context,
                     float elapsedSeconds, bool glowEnabled,
                     const float* glowColor, Preview3DFrame& frame);
bool ReadPreview3DPixels(ID3D11DeviceContext* context,
                         std::vector<uint8_t>& pixels,
                         uint32_t& width, uint32_t& height,
                         uint32_t& stride);
void ShutdownPreview3D();
