#pragma once

#include <array>
#include <cstdint>

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
void ShutdownPreview3D();
