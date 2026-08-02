#include "shared.h"
#include "menu_d2d.h"
#include "preview_3d.h"
#include "resource.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kDesignWidth = 1448.0f;
constexpr float kDesignHeight = 840.0f;

struct Renderer {
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1Bitmap> logoBitmap;
    ComPtr<ID2D1Bitmap> previewHeroBitmap;
    ComPtr<ID2D1Bitmap> preview3dBitmap;
    ComPtr<ID3D11Texture2D> preview3dTexture;
    Preview3DFrame preview3dFrame{};
    std::vector<uint8_t> preview3dPixels;
    bool preview3dShared = false;
    bool preview3dActive = false;
    ComPtr<ID2D1Bitmap> tabIcons[4];
    ComPtr<ID2D1Bitmap> sceneBitmap;
    ComPtr<ID2D1BitmapRenderTarget> blurTarget;
    ComPtr<ID2D1Bitmap> blurBitmap;
    ComPtr<ID2D1Layer> menuLayer;
    D2D1_SIZE_U blurSourceSize{};
    ComPtr<IWICBitmap> softwareBitmap;
    ComPtr<ID3D11Texture2D> softwareTexture;
    ComPtr<ID3D11ShaderResourceView> softwareSrv;
    UINT softwareWidth = 0;
    UINT softwareHeight = 0;
    bool softwareTarget = false;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> regular;
    ComPtr<IDWriteTextFormat> medium;
    ComPtr<IDWriteTextFormat> semibold;
    ComPtr<IDWriteTextFormat> title;
    ComPtr<IDWriteTextFormat> centered;
    bool ready = false;
    int tab = 0;
    int visualTeam = 0;
    int aimSubtab = 0;
    int openCombo = 0;
    float pageAlpha = 1.0f;
    float pageShift = 0.0f;
    float menuAlpha = 0.0f;
    float tabHighlightY = 88.0f;
    float windowX = 0.0f;
    float windowY = 0.0f;
    float dragGrabX = 0.0f;
    float dragGrabY = 0.0f;
    bool positionInitialized = false;
    bool draggingWindow = false;
    bool wasOpen = false;
    float* colorPopup = nullptr;
    D2D1_RECT_F colorPopupAnchor{};
    D2D1_RECT_F comboPopupRect{};
    float leftColumnScroll = 0.0f;
    float rightColumnScroll = 0.0f;
    float leftContentBottom = 0.0f;
    float rightContentBottom = 0.0f;
    int activeScrollColumn = 0;
    float scrollGrabOffset = 0.0f;
    void* activeSlider = nullptr;
    std::unordered_map<const void*, float> toggleAnimation;
    std::unordered_map<const void*, float> sliderAnimation;
} g;

struct Layout {
    float scale = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    D2D1_POINT_2F mouse{};
    bool clicked = false;
    bool down = false;
};

struct Popup {
    int id = 0;
    int* value = nullptr;
    const wchar_t* const* items = nullptr;
    int count = 0;
    D2D1_RECT_F rect{};
};

Popup pendingPopup{};
int popupSelectionId = 0;
int popupSelectionValue = -1;
static const wchar_t* const kFarmModes[] = {L"Normal", L"pSilent", L"Mixed"};
static const wchar_t* const kFarmActivationModes[] = {L"Hold", L"Toggle"};

D2D1_COLOR_F Color(float r, float gg, float b, float a = 1.0f) {
    return D2D1::ColorF(r, gg, b, a);
}

D2D1_COLOR_F White(float a = 1.0f) { return Color(0.94f, 0.945f, 0.965f, a); }
D2D1_COLOR_F Muted(float a = 1.0f) { return Color(0.58f, 0.59f, 0.66f, a); }
D2D1_COLOR_F Red(float a = 1.0f) { return Color(0.94f, 0.025f, 0.12f, a); }
D2D1_COLOR_F Border(float a = 1.0f) { return Color(0.145f, 0.16f, 0.205f, a); }

D2D1_RECT_F Rect(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

bool Contains(const D2D1_RECT_F& r, const D2D1_POINT_2F& p) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

float ColumnScroll(float x) {
    return x < 870.0f ? g.leftColumnScroll : g.rightColumnScroll;
}

float ScrolledY(float x, float y) {
    return y + ColumnScroll(x);
}

void NoteContent(float x, float bottom) {
    float& contentBottom = x < 870.0f ? g.leftContentBottom : g.rightContentBottom;
    contentBottom = (std::max)(contentBottom, bottom);
}

float ColumnMaxScroll(float contentBottom) {
    return (std::max)(0.0f, contentBottom - 818.0f + 8.0f);
}

float ColumnViewportTop() {
    return g.tab == 2 ? 174.0f : 234.0f;
}

bool ColumnVisible(float x, float y, float height) {
    return y + height >= ColumnViewportTop() && y <= 818.0f;
}

D2D1_RECT_F ActiveColorPopupRect() {
    if (!g.colorPopup) return Rect(0, 0, 0, 0);
    return Rect(g.colorPopupAnchor.left - 132,
                g.colorPopupAnchor.bottom + 8,
                g.colorPopupAnchor.right + 8,
                g.colorPopupAnchor.bottom + 104);
}

void SetBrush(const D2D1_COLOR_F& color) {
    g.brush->SetColor(color);
}

void FillRect(const D2D1_RECT_F& r, const D2D1_COLOR_F& color) {
    SetBrush(color);
    g.target->FillRectangle(r, g.brush.Get());
}

void FillRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color) {
    SetBrush(color);
    g.target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g.brush.Get());
}

void StrokeRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color,
                   float width = 1.0f) {
    SetBrush(color);
    g.target->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g.brush.Get(), width);
}

void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, const D2D1_COLOR_F& color,
          float width = 1.0f) {
    SetBrush(color);
    g.target->DrawLine(a, b, g.brush.Get(), width);
}

void Text(const wchar_t* value, const D2D1_RECT_F& r, IDWriteTextFormat* format,
          const D2D1_COLOR_F& color) {
    if (!value || !format) return;
    SetBrush(color);
    g.target->DrawText(value, static_cast<UINT32>(std::wcslen(value)), format, r,
                       g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void GradientRounded(const D2D1_RECT_F& r, float radius,
                     const D2D1_COLOR_F& start, const D2D1_COLOR_F& end,
                     bool vertical = false) {
    D2D1_GRADIENT_STOP stops[2]{
        {0.0f, start},
        {1.0f, end},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    if (FAILED(g.target->CreateGradientStopCollection(
            stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &collection))) {
        FillRounded(r, radius, start);
        return;
    }
    const D2D1_POINT_2F from = D2D1::Point2F(r.left, r.top);
    const D2D1_POINT_2F to = vertical
        ? D2D1::Point2F(r.left, r.bottom)
        : D2D1::Point2F(r.right, r.top);
    ComPtr<ID2D1LinearGradientBrush> gradient;
    if (FAILED(g.target->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(from, to), collection.Get(), &gradient))) {
        FillRounded(r, radius, start);
        return;
    }
    g.target->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gradient.Get());
}

void GlowRounded(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& color,
                 int layers = 6, float spread = 2.0f) {
    for (int i = layers; i >= 1; --i) {
        const float d = i * spread;
        const float proximity = 1.0f - static_cast<float>(i - 1) / layers;
        const float alpha = color.a * (0.025f + proximity * 0.070f);
        FillRounded(Rect(r.left - d, r.top - d, r.right + d, r.bottom + d),
                    radius + d, Color(color.r, color.g, color.b, alpha));
    }
}

void InnerGlow(const D2D1_RECT_F& r, float radius) {
    FillRounded(Rect(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3),
                (std::max)(2.0f, radius - 3), Red(0.060f));
}

std::wstring KeyName(int key) {
    switch (key) {
        case VK_LBUTTON: return L"LMB";
        case VK_RBUTTON: return L"RMB";
        case VK_MBUTTON: return L"MMB";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        case VK_SHIFT: return L"Shift";
        case VK_CONTROL: return L"Ctrl";
        case VK_MENU: return L"Alt";
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        default:
            if (key >= 'A' && key <= 'Z') return std::wstring(1, static_cast<wchar_t>(key));
            wchar_t buffer[20]{};
            std::swprintf(buffer, 20, L"VK %02X", key & 0xFF);
            return buffer;
    }
}

bool Clicked(const Layout& l, const D2D1_RECT_F& r) {
    // The color picker is modal: controls underneath must not receive input.
    if (g.colorPopup || g.openCombo) return false;
    return l.clicked && Contains(r, l.mouse);
}

void DrawLogo() {
    if (g.logoBitmap) {
        // Keep the supplied mark entirely inside the 62px header.
        g.target->DrawBitmap(g.logoBitmap.Get(), Rect(29, 4, 88, 58), 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }

    SetBrush(Red());
    const auto logoPoint = [](float x, float y) {
        return D2D1::Point2F(31.0f + (x - 29.0f) * 0.86f,
                             13.0f + (y - 12.0f) * 0.89f);
    };
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (SUCCEEDED(g.factory->CreatePathGeometry(&geometry)) &&
        SUCCEEDED(geometry->Open(&sink))) {
        sink->BeginFigure(logoPoint(57, 12), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(logoPoint(31, 56));
        sink->AddLine(logoPoint(43, 50));
        sink->AddLine(logoPoint(57, 27));
        sink->AddLine(logoPoint(60, 39));
        sink->AddLine(logoPoint(68, 36));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->BeginFigure(logoPoint(60, 42), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(logoPoint(75, 38));
        sink->AddLine(logoPoint(64, 47));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->BeginFigure(logoPoint(68, 39), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(logoPoint(93, 65));
        sink->AddLine(logoPoint(69, 59));
        sink->AddLine(logoPoint(79, 56));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->BeginFigure(logoPoint(29, 65), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(logoPoint(47, 54));
        sink->AddLine(logoPoint(61, 49));
        sink->AddLine(logoPoint(61, 54));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        g.target->FillGeometry(geometry.Get(), g.brush.Get());
    }
}

void DrawEye(float x, float y, const D2D1_COLOR_F& color) {
    SetBrush(color);
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g.factory->CreatePathGeometry(&geometry)) ||
        FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(x - 16, y), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x - 8, y - 11),
                                        D2D1::Point2F(x + 8, y - 11),
                                        D2D1::Point2F(x + 16, y)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x + 8, y + 11),
                                        D2D1::Point2F(x - 8, y + 11),
                                        D2D1::Point2F(x - 16, y)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    g.target->DrawGeometry(geometry.Get(), g.brush.Get(), 1.7f);
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 4, 4), g.brush.Get());
}

void DrawTabIcon(int index, float x, float y, bool selected) {
    // The embedded settings raster has uneven tooth spacing; keep the gear
    // vector-symmetric like the other navigation icons.
    if (index >= 0 && index < 4 && index != 2 && index != 3 && g.tabIcons[index]) {
        g.target->DrawBitmap(g.tabIcons[index].Get(), Rect(x - 16, y - 16, x + 16, y + 16),
                             selected ? 1.0f : 0.52f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    const D2D1_COLOR_F color = selected ? White() : Muted();
    if (index == 0) {
        DrawEye(x, y, color);
        return;
    }
    SetBrush(color);
    if (index == 1) {
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 10, 10), g.brush.Get(), 1.7f);
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 3, 3), g.brush.Get(), 1.4f);
        Line(D2D1::Point2F(x - 16, y), D2D1::Point2F(x - 8, y), color, 1.6f);
        Line(D2D1::Point2F(x + 8, y), D2D1::Point2F(x + 16, y), color, 1.6f);
        Line(D2D1::Point2F(x, y - 16), D2D1::Point2F(x, y - 8), color, 1.6f);
        Line(D2D1::Point2F(x, y + 8), D2D1::Point2F(x, y + 16), color, 1.6f);
    } else {
        // Misc uses a centered, symmetric gear; the old sprout icon belonged
        // to the removed Farm sidebar item.
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 11, 11), g.brush.Get(), 1.8f);
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 4, 4), g.brush.Get(), 1.6f);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.78539816f;
            Line(D2D1::Point2F(x + std::cos(a) * 12, y + std::sin(a) * 12),
                 D2D1::Point2F(x + std::cos(a) * 16, y + std::sin(a) * 16),
                 color, 2.0f);
        }
    }
}

void DrawToggle(const Layout& l, float x, float y, float width,
                const wchar_t* label, const wchar_t* description,
                bool* value, const float* colorValue = nullptr) {
    const float baseY = y;
    NoteContent(x, baseY + 66.0f);
    y = ScrolledY(x, y);
    const D2D1_RECT_F hit = Rect(x, y, x + width, y + 66);
    const D2D1_RECT_F colorRect = colorValue
        ? Rect(x + width - 22, y + 13, x + width, y + 37)
        : Rect(0, 0, 0, 0);
    const bool clickedColor = colorValue && Clicked(l, colorRect);
    if (ColumnVisible(x, y, 66.0f) && Clicked(l, hit) && !clickedColor)
        *value = !*value;
    if (!g.colorPopup && Contains(hit, l.mouse)) {
        FillRounded(Rect(x + 4, y + 3, x + width - 4, y + 62), 5,
                    Color(0.26f, 0.30f, 0.38f, 0.075f));
        StrokeRounded(Rect(x + 4, y + 3, x + width - 4, y + 62), 5,
                      Color(0.42f, 0.48f, 0.60f, 0.16f), 0.8f);
    }
    const float targetAnimation = *value ? 1.0f : 0.0f;
    auto [toggleIt, inserted] = g.toggleAnimation.emplace(value, targetAnimation);
    float& animation = toggleIt->second;
    animation += ((*value ? 1.0f : 0.0f) - animation) * 0.18f;

    Text(label, Rect(x, y + 5, x + width - 110, y + 30), g.medium.Get(), White());
    if (description)
        Text(description, Rect(x, y + 29, x + width - 110, y + 54),
             g.regular.Get(), Muted());

    const float colorOffset = colorValue ? 34.0f : 0.0f;
    const D2D1_RECT_F track = Rect(x + width - 62 - colorOffset, y + 14,
                                   x + width - 20 - colorOffset, y + 36);
    const D2D1_COLOR_F off = Color(0.16f, 0.17f, 0.20f);
    GradientRounded(track, 11,
                    Color(off.r + (1.0f - off.r) * animation,
                          off.g + (0.12f - off.g) * animation,
                          off.b + (0.19f - off.b) * animation),
                    Color(off.r + (0.78f - off.r) * animation,
                          off.g + (0.01f - off.g) * animation,
                          off.b + (0.08f - off.b) * animation), true);
    if (animation > 0.05f) {
        GlowRounded(track, 11, Red(animation * 0.42f), 3, 1.2f);
        FillRounded(Rect(track.left + 3, track.top + 3, track.right - 3,
                         track.top + 8), 4,
                    Color(1.0f, 0.42f, 0.50f, animation * 0.10f));
    }
    const float knob = track.left + 11 + animation * 20.0f;
    SetBrush(White());
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, track.top + 11), 8, 8),
                          g.brush.Get());

    if (colorValue) {
        FillRect(colorRect,
                 Color(colorValue[0], colorValue[1], colorValue[2]));
        SetBrush(Border());
        g.target->DrawRectangle(colorRect,
                                g.brush.Get(), 1.0f);
        if (clickedColor) {
            g.colorPopup = g.colorPopup == colorValue ? nullptr : const_cast<float*>(colorValue);
            g.colorPopupAnchor = colorRect;
            g.openCombo = 0;
        }
    }
    Line(D2D1::Point2F(x, y + 65), D2D1::Point2F(x + width, y + 65),
         Color(0.16f, 0.17f, 0.20f, 0.75f));
}

void DrawSlider(const Layout& l, float x, float y, float width,
                const wchar_t* label, float* value, float minimum, float maximum,
                const wchar_t* format) {
    const float baseY = y;
    NoteContent(x, baseY + 48.0f);
    y = ScrolledY(x, y);
    const D2D1_RECT_F row = Rect(x, y, x + width, y + 48);
    FillRounded(row, 6, Color(0.047f, 0.050f, 0.061f, 0.78f));
    StrokeRounded(row, 6, Border(0.8f));
    Text(label, Rect(x + 17, y + 12, x + 150, y + 38), g.regular.Get(), White());
    const float trackStart = x + 150;
    const float trackEnd = x + width - 108;
    const D2D1_RECT_F sliderHit = Rect(trackStart - 8, y, trackEnd + 8, y + 48);
    if (ColumnVisible(x, y, 48.0f) && l.clicked && Contains(sliderHit, l.mouse))
        g.activeSlider = value;
    if (!l.down && g.activeSlider == value) g.activeSlider = nullptr;
    if (l.down && g.activeSlider == value) {
        const float f = std::clamp((l.mouse.x - trackStart) / (trackEnd - trackStart), 0.0f, 1.0f);
        *value = minimum + (maximum - minimum) * f;
    }
    const float fraction = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    auto [sliderIt, inserted] = g.sliderAnimation.emplace(value, fraction);
    float& animated = sliderIt->second;
    animated += (fraction - animated) * 0.20f;
    FillRounded(Rect(trackStart, y + 22, trackEnd, y + 26), 2, Color(0.17f, 0.18f, 0.22f));
    FillRounded(Rect(trackStart, y + 22, trackStart + (trackEnd - trackStart) * animated, y + 26),
                2, Red());
    const float knob = trackStart + (trackEnd - trackStart) * animated;
    SetBrush(Color(0.035f, 0.037f, 0.045f));
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y + 24), 8, 8), g.brush.Get());
    SetBrush(Red());
    g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y + 24), 8, 8), g.brush.Get(), 1.8f);
    SetBrush(White());
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y + 24), 3, 3), g.brush.Get());

    wchar_t output[48]{};
    std::swprintf(output, 48, format, *value);
    const D2D1_RECT_F valueRect = Rect(x + width - 92, y, x + width, y + 48);
    StrokeRounded(valueRect, 6, Border());
    Text(output, valueRect, g.centered.Get(), Muted());
}

void DrawCombo(const Layout& l, int id, float x, float y, float width,
               const wchar_t* label, int* value,
               const wchar_t* const* items, int count) {
    const float baseY = y;
    NoteContent(x, baseY + 42.0f);
    y = ScrolledY(x, y);
    Text(label, Rect(x, y + 8, x + width - 150, y + 38), g.regular.Get(), White());
    const D2D1_RECT_F button = Rect(x + width - 150, y, x + width, y + 42);
    GradientRounded(button, 6, Color(0.075f, 0.078f, 0.094f),
                    Color(0.050f, 0.052f, 0.064f), true);
    StrokeRounded(button, 6, Border());
    Text(items[*value], Rect(button.left + 12, button.top, button.right - 28, button.bottom),
         g.regular.Get(), White());
    Line(D2D1::Point2F(button.right - 19, button.top + 17),
         D2D1::Point2F(button.right - 13, button.top + 23), Muted(), 1.4f);
    Line(D2D1::Point2F(button.right - 13, button.top + 23),
         D2D1::Point2F(button.right - 7, button.top + 17), Muted(), 1.4f);
    if (ColumnVisible(x, y, 42.0f) && Clicked(l, button))
        g.openCombo = g.openCombo == id ? 0 : id;
    if (g.openCombo == id) {
        pendingPopup = {id, value, items, count,
                        Rect(button.left, button.bottom + 5, button.right,
                             button.bottom + 5 + count * 38.0f)};
        g.comboPopupRect = pendingPopup.rect;
    }
}

void DrawEspChip(const Layout& l, float x, float y, float width,
                 const wchar_t* label, bool* value,
                 const float* colorValue = nullptr) {
    const D2D1_RECT_F hit = Rect(x, y, x + width, y + 42);
    const D2D1_RECT_F swatch = colorValue
        ? Rect(x + width - 34, y + 10, x + width - 14, y + 32)
        : Rect(0, 0, 0, 0);
    const bool clickedSwatch = colorValue && Clicked(l, swatch);
    if (Clicked(l, hit) && !clickedSwatch)
        *value = !*value;
    const bool hovered = Contains(hit, l.mouse);
    if (*value) {
        GlowRounded(hit, 6, Red(0.26f), 3, 1.0f);
        GradientRounded(hit, 6, Color(0.42f, 0.025f, 0.075f, 0.92f),
                        Color(0.17f, 0.030f, 0.055f, 0.96f), true);
        StrokeRounded(hit, 6, Red(), 1.0f);
    } else {
        GradientRounded(hit, 6,
                        hovered ? Color(0.105f, 0.115f, 0.14f)
                                : Color(0.070f, 0.076f, 0.092f),
                        Color(0.047f, 0.051f, 0.063f), true);
        StrokeRounded(hit, 6, hovered ? Color(0.34f, 0.37f, 0.45f) : Border());
    }
    Text(label, Rect(x + 13, y, x + width - 48, y + 42),
         g.regular.Get(), *value ? White() : Muted());
    if (colorValue) {
        FillRounded(swatch, 4,
                    Color(colorValue[0], colorValue[1], colorValue[2]));
        StrokeRounded(swatch, 4, Border(), 0.8f);
        if (clickedSwatch) {
            g.colorPopup = g.colorPopup == colorValue
                ? nullptr : const_cast<float*>(colorValue);
            g.colorPopupAnchor = swatch;
            g.openCombo = 0;
        }
    } else {
        Text(*value ? L"ON" : L"OFF", Rect(x + width - 52, y, x + width - 10, y + 42),
             g.centered.Get(), *value ? Red() : Muted());
    }
}

void DrawHeroEspPreview(float x, float y, float width, float height,
                        const wchar_t* presetLabel, bool enabled,
                        bool boxes, bool cornerBoxes,
                        bool skeleton, bool health, bool healthValue,
                        bool heroName, bool playerName, bool distance,
                        bool snaplines, const float* boxColor,
                        const float* skeletonColor, const float* healthColor,
                        const float* nameColor, const float* playerColor,
                        const float* healthValueColor) {
    GlowRounded(Rect(x, y, x + width, y + height), 16,
                Color(0, 0, 0, 0.65f), 5, 2.0f);
    FillRounded(Rect(x, y, x + width, y + height), 16,
                Color(0.026f, 0.030f, 0.039f, 0.97f));
    StrokeRounded(Rect(x, y, x + width, y + height), 16,
                  Color(0.24f, 0.27f, 0.34f, 0.92f), 1.1f);
    Text(L"ESP Preview", Rect(x + 16, y + 10, x + width - 16, y + 42),
         g.semibold.Get(), White());
    Text(enabled ? presetLabel : L"ESP disabled",
         Rect(x + 16, y + 40, x + width - 16, y + 65),
         g.regular.Get(), enabled ? Muted() : Red());

    const D2D1_RECT_F stage = Rect(x + 15, y + 74, x + width - 15, y + height - 24);
    FillRounded(stage, 12, Color(0.008f, 0.010f, 0.014f, 1.0f));
    StrokeRounded(stage, 12, Color(0.20f, 0.23f, 0.29f, 0.92f), 1.0f);
    // Keep the character aspect ratio from the source sheet. The previous
    // crop used coordinates for a 2048px image, while the embedded reference
    // is 2515px wide; that selected half of two poses and pushed the hero out
    // of the preview window.
    // Reserve visible padding for labels above the head, distance below the
    // feet, and the box/health bar on both sides. The preview itself is scaled
    // down with the menu, so small design-space margins were disappearing at
    // common 1080p resolutions.
    const float previewCenterX = x + width * 0.5f;
    D2D1_RECT_F modelRect{};
    const D2D1_RECT_F renderRect = Rect(stage.left + 2.0f, stage.top + 2.0f,
                                        stage.right - 2.0f, stage.bottom - 2.0f);
    if (g.preview3dActive && g.preview3dBitmap) {
        g.target->DrawBitmap(g.preview3dBitmap.Get(), renderRect, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        const float renderWidth = renderRect.right - renderRect.left;
        const float renderHeight = renderRect.bottom - renderRect.top;
        modelRect = Rect(
            renderRect.left + g.preview3dFrame.left * renderWidth,
            renderRect.top + g.preview3dFrame.top * renderHeight,
            renderRect.left + g.preview3dFrame.right * renderWidth,
            renderRect.top + g.preview3dFrame.bottom * renderHeight);
    } else {
        const float modelTop = y + 158.0f;
        const float modelBottom = y + height - 72.0f;
        const float modelHeight = modelBottom - modelTop;
        constexpr float sourceAspect = 515.0f / 1140.0f;
        const float modelHalfWidth = modelHeight * sourceAspect * 0.5f;
        modelRect = Rect(previewCenterX - modelHalfWidth, modelTop,
                         previewCenterX + modelHalfWidth, modelBottom);
    }
    if (!g.preview3dActive && g.previewHeroBitmap) {
        // Front view from the in-game Infernus model reference sheet.
        g.target->DrawBitmap(g.previewHeroBitmap.Get(), modelRect, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
            Rect(620.0f, 170.0f, 1135.0f, 1310.0f));
    }
    if (!enabled) return;

    const float left = modelRect.left + 7.0f;
    const float right = modelRect.right - 7.0f;
    const float top = modelRect.top + 8.0f;
    const float bottom = modelRect.bottom - 8.0f;
    const float cx = (left + right) * 0.5f;
    const D2D1_COLOR_F box = Color(boxColor[0], boxColor[1], boxColor[2]);
    const D2D1_COLOR_F bones = Color(skeletonColor[0], skeletonColor[1], skeletonColor[2]);
    const D2D1_COLOR_F hp = Color(healthColor[0], healthColor[1], healthColor[2]);
    if (boxes) {
        if (cornerBoxes) {
            constexpr float c = 25.0f;
            Line(D2D1::Point2F(left, top), D2D1::Point2F(left + c, top), box, 1.6f);
            Line(D2D1::Point2F(left, top), D2D1::Point2F(left, top + c), box, 1.6f);
            Line(D2D1::Point2F(right - c, top), D2D1::Point2F(right, top), box, 1.6f);
            Line(D2D1::Point2F(right, top), D2D1::Point2F(right, top + c), box, 1.6f);
            Line(D2D1::Point2F(left, bottom - c), D2D1::Point2F(left, bottom), box, 1.6f);
            Line(D2D1::Point2F(left, bottom), D2D1::Point2F(left + c, bottom), box, 1.6f);
            Line(D2D1::Point2F(right - c, bottom), D2D1::Point2F(right, bottom), box, 1.6f);
            Line(D2D1::Point2F(right, bottom - c), D2D1::Point2F(right, bottom), box, 1.6f);
        } else {
            SetBrush(box);
            g.target->DrawRectangle(Rect(left, top, right, bottom), g.brush.Get(), 1.6f);
        }
    }
    if (health) {
        FillRounded(Rect(left - 10, top, left - 5, bottom), 2, Color(0.10f, 0.11f, 0.14f));
        FillRounded(Rect(left - 10, top + 54, left - 5, bottom), 2, hp);
    }
    if (skeleton && g.preview3dActive) {
        const float renderWidth = renderRect.right - renderRect.left;
        const float renderHeight = renderRect.bottom - renderRect.top;
        std::array<D2D1_POINT_2F, 18> points{};
        for (size_t i = 0; i < points.size(); ++i) {
            points[i] = D2D1::Point2F(
                renderRect.left + g.preview3dFrame.skeleton[i].x * renderWidth,
                renderRect.top + g.preview3dFrame.skeleton[i].y * renderHeight);
        }
        static constexpr int segments[][2]{
            {0, 1}, {1, 2}, {2, 11},
            {2, 3}, {3, 4}, {4, 5}, {5, 6},
            {2, 7}, {7, 8}, {8, 9}, {9, 10},
            {11, 12}, {12, 13}, {13, 14},
            {11, 15}, {15, 16}, {16, 17},
        };
        for (const auto& segment : segments) {
            if (g.preview3dFrame.skeleton[segment[0]].visible &&
                g.preview3dFrame.skeleton[segment[1]].visible) {
                Line(points[segment[0]], points[segment[1]], bones, 1.4f);
            }
        }
        SetBrush(bones);
        g.target->DrawEllipse(D2D1::Ellipse(points[0], 8.0f, 10.0f),
                              g.brush.Get(), 1.3f);
    } else if (skeleton) {
        const float bodyHeight = bottom - top;
        const float head = top + bodyHeight * 0.055f;
        const float neck = top + bodyHeight * 0.12f;
        const float shoulders = top + bodyHeight * 0.21f;
        const float hips = top + bodyHeight * 0.52f;
        const float knees = top + bodyHeight * 0.76f;
        SetBrush(bones);
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, head), 10, 13),
                              g.brush.Get(), 1.3f);
        Line(D2D1::Point2F(cx, head + 13), D2D1::Point2F(cx, neck), bones, 1.4f);
        Line(D2D1::Point2F(cx, neck), D2D1::Point2F(cx, hips), bones, 1.4f);
        Line(D2D1::Point2F(cx, shoulders),
             D2D1::Point2F(left + 10, top + bodyHeight * 0.42f), bones, 1.4f);
        Line(D2D1::Point2F(cx, shoulders),
             D2D1::Point2F(right - 10, top + bodyHeight * 0.42f), bones, 1.4f);
        const float legSpread = (right - left) * 0.23f;
        Line(D2D1::Point2F(cx, hips), D2D1::Point2F(cx - legSpread, knees), bones, 1.4f);
        Line(D2D1::Point2F(cx - legSpread, knees),
             D2D1::Point2F(cx - legSpread, bottom), bones, 1.4f);
        Line(D2D1::Point2F(cx, hips), D2D1::Point2F(cx + legSpread, knees), bones, 1.4f);
        Line(D2D1::Point2F(cx + legSpread, knees),
             D2D1::Point2F(cx + legSpread, bottom), bones, 1.4f);
    }
    float labelY = y + 86.0f;
    if (heroName) {
        Text(L"Infernus", Rect(left - 20, labelY, right + 20, labelY + 24),
             g.centered.Get(), Color(nameColor[0], nameColor[1], nameColor[2]));
        labelY += 23.0f;
    }
    if (playerName)
        Text(L"Player", Rect(left - 20, labelY, right + 20, labelY + 24),
             g.centered.Get(), Color(playerColor[0], playerColor[1], playerColor[2]));
    if (healthValue)
        Text(L"658 / 830", Rect(cx - 55, y + 134, cx + 55, y + 157),
             g.centered.Get(), Color(healthValueColor[0], healthValueColor[1], healthValueColor[2]));
    if (distance)
        Text(L"12m", Rect(cx - 35, bottom + 2, cx + 35, bottom + 26),
             g.centered.Get(), Muted());
    if (snaplines)
        Line(D2D1::Point2F(x + width * 0.5f, y + height - 18),
             D2D1::Point2F(cx, bottom), Color(1, 1, 1, 0.70f), 1.0f);
}

void DrawKeyBind(const Layout& l, float x, float y, float width,
                 bool capture, int key, bool* captureState) {
    const float baseY = y;
    NoteContent(x, baseY + 42.0f);
    y = ScrolledY(x, y);
    const D2D1_RECT_F button = Rect(x, y, x + width, y + 42);
    GradientRounded(button, 6, Color(0.075f, 0.078f, 0.094f),
                    Color(0.050f, 0.052f, 0.064f), true);
    StrokeRounded(button, 6, Border());
    const std::wstring value = capture ? L"Press any key..." : KeyName(key);
    Text(value.c_str(), button, g.centered.Get(), capture ? Red() : White());
    if (ColumnVisible(x, y, 42.0f) && Clicked(l, button)) *captureState = true;
}

void DrawPopup(const Layout& l) {
    if (pendingPopup.id && g.openCombo == pendingPopup.id) {
        const D2D1_RECT_F r = pendingPopup.rect;
        GlowRounded(r, 7, Color(0, 0, 0, 0.55f), 4, 2.0f);
        FillRounded(r, 7, Color(0.045f, 0.047f, 0.058f, 0.995f));
        StrokeRounded(r, 7, Border());
        for (int i = 0; i < pendingPopup.count; ++i) {
            const D2D1_RECT_F item = Rect(r.left + 4, r.top + 4 + i * 38.0f,
                                         r.right - 4, r.top + 4 + (i + 1) * 38.0f - 4);
            if (i == *pendingPopup.value)
                FillRounded(item, 5, Color(0.94f, 0.025f, 0.12f, 0.16f));
            else if (Contains(item, l.mouse))
                FillRounded(item, 5, Color(1, 1, 1, 0.04f));
            Text(pendingPopup.items[i], Rect(item.left + 10, item.top, item.right, item.bottom),
                 g.regular.Get(), i == *pendingPopup.value ? Red() : White());
        if (l.clicked && Contains(item, l.mouse)) {
            *pendingPopup.value = i;
            popupSelectionId = pendingPopup.id;
            popupSelectionValue = i;
            g.openCombo = 0;
        }
        }
    }

    if (!g.colorPopup) return;
    const D2D1_RECT_F r = ActiveColorPopupRect();
    GlowRounded(r, 7, Color(0, 0, 0, 0.55f), 4, 2.0f);
    FillRounded(r, 7, Color(0.045f, 0.047f, 0.058f, 0.99f));
    StrokeRounded(r, 7, Border());
    static const float palette[][3] = {
        {1.0f, 0.08f, 0.12f}, {1.0f, 0.82f, 0.04f}, {0.20f, 1.0f, 0.18f},
        {0.05f, 0.75f, 1.0f}, {0.35f, 0.25f, 1.0f}, {1.0f, 0.15f, 0.75f},
        {1.0f, 1.0f, 1.0f}, {0.55f, 0.58f, 0.66f}
    };
    for (int i = 0; i < 8; ++i) {
        const float px = r.left + 12 + (i % 4) * 28.0f;
        const float py = r.top + 12 + (i / 4) * 32.0f;
        const D2D1_RECT_F swatch = Rect(px, py, px + 20, py + 20);
        FillRect(swatch, Color(palette[i][0], palette[i][1], palette[i][2]));
        StrokeRounded(swatch, 3, Border());
            if (l.clicked && Contains(swatch, l.mouse)) {
                g.colorPopup[0] = palette[i][0];
            g.colorPopup[1] = palette[i][1];
            g.colorPopup[2] = palette[i][2];
            g.colorPopup[3] = 1.0f;
            g.colorPopup = nullptr;
        }
    }
}

void DrawColumnScrollbar(const Layout& l, float x, float viewportTop,
                         float contentBottom, float& scroll) {
    const float viewportBottom = 818.0f;
    const float viewportHeight = viewportBottom - viewportTop;
    const float maxScroll = ColumnMaxScroll(contentBottom);
    if (maxScroll <= 0.0f) {
        scroll = 0.0f;
        return;
    }
    const float trackTop = viewportTop + 4.0f;
    const float trackBottom = viewportBottom - 4.0f;
    const float thumbHeight = (std::max)(34.0f, viewportHeight * viewportHeight /
                                         (contentBottom - viewportTop));
    const float travel = (std::max)(1.0f, trackBottom - trackTop - thumbHeight);
    const float thumbTop = trackTop + (-scroll / maxScroll) * travel;
    const D2D1_RECT_F track = Rect(x, trackTop, x + 8.0f, trackBottom);
    const D2D1_RECT_F thumb = Rect(x, thumbTop, x + 8.0f, thumbTop + thumbHeight);
    FillRounded(track, 4, Color(0.10f, 0.11f, 0.14f, 0.55f));
    FillRounded(thumb, 4, Contains(thumb, l.mouse) || g.activeScrollColumn != 0
                             ? Color(0.82f, 0.10f, 0.18f, 0.90f)
                             : Color(0.50f, 0.52f, 0.58f, 0.75f));
    if (l.clicked && Contains(thumb, l.mouse)) {
        g.activeScrollColumn = x < 870.0f ? 1 : 2;
        g.scrollGrabOffset = l.mouse.y - thumbTop;
    }
    if (g.activeScrollColumn == (x < 870.0f ? 1 : 2) && l.down) {
        const float newTop = std::clamp(l.mouse.y - g.scrollGrabOffset,
                                        trackTop, trackBottom - thumbHeight);
        scroll = -((newTop - trackTop) / travel) * maxScroll;
    }
}

bool CreateTextFormat(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                      IDWriteTextFormat** output, bool centered = false) {
    if (FAILED(g.writeFactory->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", output))) return false;
    (*output)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    (*output)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (centered)
        (*output)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else
        (*output)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    return true;
}

bool EnsureFactories() {
    if (!g.factory && FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED, g.factory.GetAddressOf()))) return false;
    if (!g.writeFactory && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(g.writeFactory.GetAddressOf())))) return false;
    if (!g.wicFactory) {
        HRESULT wicResult = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(g.wicFactory.GetAddressOf()));
        if (wicResult == CO_E_NOTINITIALIZED) {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            wicResult = CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(g.wicFactory.GetAddressOf()));
        }
        if (FAILED(wicResult)) return false;
    }
    if (!g.regular &&
        (!CreateTextFormat(L"Segoe UI", 19.0f, DWRITE_FONT_WEIGHT_REGULAR,
                           g.regular.GetAddressOf()) ||
         !CreateTextFormat(L"Segoe UI", 20.0f, DWRITE_FONT_WEIGHT_MEDIUM,
                           g.medium.GetAddressOf()) ||
         !CreateTextFormat(L"Segoe UI", 21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                           g.semibold.GetAddressOf()) ||
         !CreateTextFormat(L"Segoe UI", 30.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                           g.title.GetAddressOf()) ||
         !CreateTextFormat(L"Segoe UI", 19.0f, DWRITE_FONT_WEIGHT_REGULAR,
                           g.centered.GetAddressOf(), true))) return false;
    return true;
}

bool LoadEmbeddedBitmap(UINT resourceId, ComPtr<ID2D1Bitmap>& output) {
    if (output) return true;
    if (!moduleHandle || !g.wicFactory || !g.target) return false;

    HRSRC resource = FindResourceW(moduleHandle, MAKEINTRESOURCEW(resourceId),
                                   MAKEINTRESOURCEW(10));
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(moduleHandle, resource);
    if (!loaded) return false;
    void* bytes = LockResource(loaded);
    const DWORD size = SizeofResource(moduleHandle, resource);
    if (!bytes || !size) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(g.wicFactory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(bytes), size)) ||
        FAILED(g.wicFactory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) ||
        FAILED(g.wicFactory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(g.target->CreateBitmapFromWicBitmap(
            converter.Get(), nullptr, output.GetAddressOf()))) {
        output.Reset();
        return false;
    }
    return true;
}

void LoadEmbeddedAssets() {
    LoadEmbeddedBitmap(IDR_DEADLOCK_LOGO, g.logoBitmap);
    LoadEmbeddedBitmap(IDR_ESP_PREVIEW_HERO, g.previewHeroBitmap);
    const UINT iconIds[4]{IDR_ICON_EYE, IDR_ICON_CROSSHAIR,
                          IDR_ICON_SPROUT, IDR_ICON_SETTINGS};
    for (int i = 0; i < 4; ++i)
        LoadEmbeddedBitmap(iconIds[i], g.tabIcons[i]);
}

bool BindPreview3DFrame(const Preview3DFrame& frame,
                        ID3D11DeviceContext* context) {
    if (!frame.texture || !g.target || g.softwareTarget) return false;
    if (g.preview3dTexture.Get() != frame.texture || !g.preview3dBitmap) {
        g.preview3dBitmap.Reset();
        g.preview3dTexture.Reset();
        g.preview3dShared = false;
        ComPtr<IDXGISurface> surface;
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (SUCCEEDED(frame.texture->QueryInterface(
                IID_PPV_ARGS(surface.GetAddressOf()))) &&
            SUCCEEDED(g.target->CreateSharedBitmap(
                __uuidof(IDXGISurface), surface.Get(), &properties,
                g.preview3dBitmap.GetAddressOf()))) {
            g.preview3dShared = true;
        } else {
            uint32_t width = 0, height = 0, stride = 0;
            if (!ReadPreview3DPixels(context, g.preview3dPixels,
                                     width, height, stride) ||
                FAILED(g.target->CreateBitmap(
                    D2D1::SizeU(width, height), g.preview3dPixels.data(),
                    stride, properties, g.preview3dBitmap.GetAddressOf()))) {
                g.preview3dBitmap.Reset();
                return false;
            }
        }
        g.preview3dTexture = frame.texture;
    } else if (!g.preview3dShared) {
        uint32_t width = 0, height = 0, stride = 0;
        if (!ReadPreview3DPixels(context, g.preview3dPixels,
                                 width, height, stride) ||
            FAILED(g.preview3dBitmap->CopyFromMemory(
                nullptr, g.preview3dPixels.data(), stride))) {
            return false;
        }
    }
    g.preview3dFrame = frame;
    return true;
}

bool PrepareBackgroundBlur(UINT width, UINT height) {
    if (!width || !height || !g.target) return false;
    if (!g.sceneBitmap || g.blurSourceSize.width != width ||
        g.blurSourceSize.height != height) {
        g.sceneBitmap.Reset();
        g.blurBitmap.Reset();
        g.blurTarget.Reset();
        g.blurSourceSize = {};

        const D2D1_PIXEL_FORMAT pixelFormat = g.target->GetPixelFormat();
        if (FAILED(g.target->CreateBitmap(
                D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties(pixelFormat, 96.0f, 96.0f),
                g.sceneBitmap.GetAddressOf()))) {
            return false;
        }
        const UINT blurWidth = (std::max)(1u, width / 4u);
        const UINT blurHeight = (std::max)(1u, height / 4u);
        if (FAILED(g.target->CreateCompatibleRenderTarget(
                D2D1::SizeF(static_cast<float>(blurWidth),
                            static_cast<float>(blurHeight)),
                D2D1::SizeU(blurWidth, blurHeight), pixelFormat,
                D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
                g.blurTarget.GetAddressOf())) ||
            FAILED(g.blurTarget->GetBitmap(g.blurBitmap.GetAddressOf()))) {
            g.sceneBitmap.Reset();
            g.blurTarget.Reset();
            g.blurBitmap.Reset();
            return false;
        }
        g.blurSourceSize = D2D1::SizeU(width, height);
    }

    if (FAILED(g.sceneBitmap->CopyFromRenderTarget(nullptr, g.target.Get(), nullptr)))
        return false;
    g.blurTarget->BeginDraw();
    g.blurTarget->Clear(Color(0, 0, 0, 0));
    const D2D1_SIZE_F blurSize = g.blurTarget->GetSize();
    g.blurTarget->DrawBitmap(
        g.sceneBitmap.Get(), Rect(0, 0, blurSize.width, blurSize.height), 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return SUCCEEDED(g.blurTarget->EndDraw());
}

void ResetTarget() {
    g.menuLayer.Reset();
    g.logoBitmap.Reset();
    g.previewHeroBitmap.Reset();
    g.preview3dBitmap.Reset();
    g.preview3dTexture.Reset();
    g.preview3dFrame = {};
    g.preview3dPixels.clear();
    g.preview3dShared = false;
    g.preview3dActive = false;
    for (auto& icon : g.tabIcons) icon.Reset();
    g.blurBitmap.Reset();
    g.blurTarget.Reset();
    g.sceneBitmap.Reset();
    g.blurSourceSize = {};
    g.softwareSrv.Reset();
    g.softwareTexture.Reset();
    g.softwareBitmap.Reset();
    g.softwareWidth = 0;
    g.softwareHeight = 0;
    g.softwareTarget = false;
    g.brush.Reset();
    g.target.Reset();
    g.surface.Reset();
    g.ready = false;
}

bool CreateSoftwareMenuTarget(IDXGISwapChain* swapChain, UINT width, UINT height) {
    if (!swapChain || !width || !height || !g.wicFactory || !g.factory)
        return false;
    ResetTarget();

    if (FAILED(g.wicFactory->CreateBitmap(
            width, height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, g.softwareBitmap.GetAddressOf()))) {
        return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    if (FAILED(g.factory->CreateWicBitmapRenderTarget(
            g.softwareBitmap.Get(), properties, g.target.GetAddressOf())) ||
        FAILED(g.target->CreateSolidColorBrush(
            White(), g.brush.GetAddressOf()))) {
        ResetTarget();
        return false;
    }

    ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))) {
        ResetTarget();
        return false;
    }
    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = width;
    textureDescription.Height = height;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDescription.SampleDesc.Count = 1;
    textureDescription.Usage = D3D11_USAGE_DEFAULT;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(
            &textureDescription, nullptr, g.softwareTexture.GetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(
            g.softwareTexture.Get(), nullptr, g.softwareSrv.GetAddressOf()))) {
        ResetTarget();
        return false;
    }

    g.softwareWidth = width;
    g.softwareHeight = height;
    g.softwareTarget = true;
    g.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    LoadEmbeddedAssets();
    g.ready = true;
    return true;
}

bool UploadSoftwareMenuTexture() {
    if (!g.softwareTarget || !g.softwareBitmap || !g.softwareTexture ||
        !g.softwareSrv || !g.softwareWidth || !g.softwareHeight) {
        return false;
    }
    WICRect lockRect{0, 0, static_cast<INT>(g.softwareWidth),
                     static_cast<INT>(g.softwareHeight)};
    ComPtr<IWICBitmapLock> bitmapLock;
    if (FAILED(g.softwareBitmap->Lock(
            &lockRect, WICBitmapLockRead, bitmapLock.GetAddressOf()))) {
        return false;
    }
    UINT stride = 0;
    UINT dataSize = 0;
    BYTE* data = nullptr;
    if (FAILED(bitmapLock->GetStride(&stride)) ||
        FAILED(bitmapLock->GetDataPointer(&dataSize, &data)) || !data) {
        return false;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    g.softwareTexture->GetDevice(device.GetAddressOf());
    if (!device) return false;
    device->GetImmediateContext(context.GetAddressOf());
    if (!context) return false;
    context->UpdateSubresource(g.softwareTexture.Get(), 0, nullptr, data, stride, 0);

    const ImTextureID textureId = static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(g.softwareSrv.Get()));
    ImGui::GetForegroundDrawList()->AddImage(
        ImTextureRef(textureId), ImVec2(0, 0),
        ImVec2(static_cast<float>(g.softwareWidth),
               static_cast<float>(g.softwareHeight)));
    return true;
}

} // namespace

bool PrepareD2DMenu(IDXGISwapChain* swapChain) {
    if (!swapChain || !EnsureFactories()) return false;
    const ImGuiIO& io = ImGui::GetIO();
    const UINT displayWidth = (std::max)(1u, static_cast<UINT>(io.DisplaySize.x));
    const UINT displayHeight = (std::max)(1u, static_cast<UINT>(io.DisplaySize.y));
    if (g.softwareTarget && g.ready &&
        g.softwareWidth == displayWidth && g.softwareHeight == displayHeight) {
        return true;
    }
    char forceSoftwareValue[2]{};
    if (GetEnvironmentVariableA("DLL6_FORCE_SOFTWARE_MENU",
                                forceSoftwareValue,
                                static_cast<DWORD>(sizeof(forceSoftwareValue))) > 0) {
        return CreateSoftwareMenuTarget(swapChain, displayWidth, displayHeight);
    }

    ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&surface))))
        return CreateSoftwareMenuTarget(swapChain, displayWidth, displayHeight);
    if (g.target && g.surface.Get() == surface.Get()) return true;

    ResetTarget();
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_HARDWARE,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(g.factory->CreateDxgiSurfaceRenderTarget(
            surface.Get(), &properties, g.target.GetAddressOf()))) {
        return CreateSoftwareMenuTarget(swapChain, displayWidth, displayHeight);
    }
    if (FAILED(g.target->CreateSolidColorBrush(White(), g.brush.GetAddressOf()))) {
        ResetTarget();
        return CreateSoftwareMenuTarget(swapChain, displayWidth, displayHeight);
    }
    g.surface = surface;
    g.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    LoadEmbeddedAssets();
    g.ready = true;
    return true;
}

bool UsesSoftwareD2DMenu() {
    return g.ready && g.softwareTarget;
}

void RenderD2DMenu(std::size_t playerCount) {
    if (!g.ready || !g.target) {
        return;
    }

    // Keep rendering briefly after the toggle is released so closing is animated.
    const float targetMenuAlpha = menuOpen ? 1.0f : 0.0f;
    g.menuAlpha += (targetMenuAlpha - g.menuAlpha) * 0.16f;
    if (!menuOpen && g.menuAlpha < 0.01f) {
        g.menuAlpha = 0.0f;
        if (g.wasOpen) SaveConfig();
        g.wasOpen = false;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const float scale = (std::min)(io.DisplaySize.x * 0.925f / kDesignWidth,
                                    io.DisplaySize.y * 0.958f / kDesignHeight) * 0.54f;
    const float safeScale = (std::max)(0.32f, scale);
    const float windowWidth = kDesignWidth * safeScale;
    const float windowHeight = kDesignHeight * safeScale;
    if (!g.positionInitialized) {
        g.windowX = (io.DisplaySize.x - windowWidth) * 0.5f;
        g.windowY = (io.DisplaySize.y - windowHeight) * 0.5f;
        g.positionInitialized = true;
    }

    const bool mouseInHeader =
        io.MousePos.x >= g.windowX &&
        io.MousePos.x <= g.windowX + windowWidth - 54.0f * safeScale &&
        io.MousePos.y >= g.windowY &&
        io.MousePos.y <= g.windowY + 62.0f * safeScale;
    if (io.MouseClicked[0] && mouseInHeader) {
        g.draggingWindow = true;
        g.dragGrabX = io.MousePos.x - g.windowX;
        g.dragGrabY = io.MousePos.y - g.windowY;
    }
    if (!io.MouseDown[0])
        g.draggingWindow = false;
    if (g.draggingWindow) {
        g.windowX = io.MousePos.x - g.dragGrabX;
        g.windowY = io.MousePos.y - g.dragGrabY;
    }
    const float visibleEdge = 70.0f * safeScale;
    g.windowX = std::clamp(g.windowX, -windowWidth + visibleEdge,
                           io.DisplaySize.x - visibleEdge);
    g.windowY = std::clamp(g.windowY, 0.0f,
                           (std::max)(0.0f, io.DisplaySize.y - visibleEdge));

    Layout l{};
    l.scale = safeScale;
    l.x = g.windowX;
    l.y = g.windowY;
    l.mouse = D2D1::Point2F((io.MousePos.x - l.x) / safeScale,
                            (io.MousePos.y - l.y) / safeScale);
    l.clicked = menuOpen && io.MouseClicked[0];
    l.down = menuOpen && io.MouseDown[0];
    popupSelectionId = 0;
    popupSelectionValue = -1;
    if (g.colorPopup && l.clicked && !Contains(ActiveColorPopupRect(), l.mouse)) {
        // Close the picker, but consume this click so the underlying row does
        // not toggle on the same frame.
        g.colorPopup = nullptr;
        l.clicked = false;
    }
    if (g.openCombo && l.clicked && !Contains(g.comboPopupRect, l.mouse)) {
        g.openCombo = 0;
        l.clicked = false;
    }
    pendingPopup = {};

    g.pageAlpha += (1.0f - g.pageAlpha) * 0.18f;
    g.pageShift += (0.0f - g.pageShift) * 0.18f;

    const bool blurReady = !g.softwareTarget && PrepareBackgroundBlur(
        static_cast<UINT>(io.DisplaySize.x),
        static_cast<UINT>(io.DisplaySize.y));
    g.preview3dActive = false;
    if (!g.softwareTarget && g.tab == 0 && pDevice && pContext) {
        static const ULONGLONG previewStart = GetTickCount64();
        Preview3DFrame previewFrame{};
        const float previewTime = static_cast<float>(
            GetTickCount64() - previewStart) * 0.001f;
        const bool previewGlowEnabled =
            g.visualTeam == 0 ? enemyEspEnabled && enemyGlowEnabled :
            g.visualTeam == 1 ? allyEspEnabled && allyGlowEnabled : false;
        const float* previewGlowColor =
            g.visualTeam == 0 ? enemyGlowColor : teammateGlowColor;
        if (RenderPreview3D(pDevice, pContext, previewTime,
                            previewGlowEnabled, previewGlowColor,
                            previewFrame))
            g.preview3dActive = BindPreview3DFrame(previewFrame, pContext);
    }
    g.target->BeginDraw();
    g.target->SetTransform(D2D1::Matrix3x2F::Identity());
    if (g.softwareTarget)
        g.target->Clear(Color(0, 0, 0, 0));

    bool pushedMenuLayer = false;
    if (!g.menuLayer)
        g.target->CreateLayer(nullptr, g.menuLayer.GetAddressOf());
    if (g.menuLayer) {
        D2D1_LAYER_PARAMETERS layerParams{};
        layerParams.contentBounds = D2D1::InfiniteRect();
        layerParams.opacity = g.menuAlpha;
        g.target->PushLayer(layerParams, g.menuLayer.Get());
        pushedMenuLayer = true;
    }
    if (blurReady && g.blurBitmap) {
        g.target->PushAxisAlignedClip(
            Rect(l.x, l.y, l.x + kDesignWidth * safeScale,
                 l.y + kDesignHeight * safeScale),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g.target->DrawBitmap(
            g.blurBitmap.Get(), Rect(0, 0, io.DisplaySize.x, io.DisplaySize.y),
            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.target->PopAxisAlignedClip();
    }
    FillRect(Rect(0, 0, io.DisplaySize.x, io.DisplaySize.y),
             Color(0.008f, 0.010f, 0.016f, 0.18f));
    g.target->SetTransform(D2D1::Matrix3x2F(safeScale, 0, 0, safeScale, l.x, l.y));

    const D2D1_RECT_F window = Rect(0, 0, kDesignWidth, kDesignHeight);
    GlowRounded(window, 14, Color(0, 0, 0, 0.78f), 8, 3.0f);
    GradientRounded(window, 14, Color(0.068f, 0.074f, 0.105f, 0.56f),
                    Color(0.063f, 0.069f, 0.100f, 0.59f), true);
    // A subtle cool sheen keeps the panel translucent without turning it black.
    GradientRounded(Rect(1, 1, kDesignWidth - 1, 170), 13,
                    Color(0.20f, 0.24f, 0.31f, 0.055f),
                    Color(0.05f, 0.07f, 0.11f, 0.0f), true);
    StrokeRounded(window, 14, Color(0.39f, 0.13f, 0.20f, 0.58f), 1.0f);

    GradientRounded(Rect(0, 0, kDesignWidth, 62), 14,
                    Color(0.050f, 0.068f, 0.094f, 0.52f),
                    Color(0.092f, 0.052f, 0.076f, 0.52f));
    FillRect(Rect(0, 52, kDesignWidth, 62), Color(0.060f, 0.063f, 0.086f, 0.52f));
    Line(D2D1::Point2F(0, 62), D2D1::Point2F(kDesignWidth, 62), Border());
    Line(D2D1::Point2F(313, 62), D2D1::Point2F(313, kDesignHeight), Border());

    DrawLogo();
    Text(L"Axiom", Rect(104, 6, 300, 56), g.title.Get(), White());
    Line(D2D1::Point2F(1402, 21), D2D1::Point2F(1418, 37), Muted(), 1.6f);
    Line(D2D1::Point2F(1418, 21), D2D1::Point2F(1402, 37), Muted(), 1.6f);
    if (Clicked(l, Rect(1388, 8, 1433, 53))) {
        SaveConfig();
        SetMenuOpen(false);
    }

    FillRect(Rect(0, 62, 313, kDesignHeight), Color(0.065f, 0.080f, 0.115f, 0.56f));
    const wchar_t* tabs[] = {L"Visuals", L"Aim assist", L"Misc"};
    for (int i = 0; i < 3; ++i) {
        const float top = 88.0f + i * 74.0f;
        const D2D1_RECT_F tab = Rect(18, top, 292, top + 68);
        const bool selected = g.tab == i;
        if (Clicked(l, tab)) {
            g.tab = i;
            g.pageAlpha = 0.0f;
            g.pageShift = 14.0f;
            g.openCombo = 0;
        }
        if (selected) {
            g.tabHighlightY += (top - g.tabHighlightY) * 0.20f;
            const D2D1_RECT_F highlight = Rect(18, g.tabHighlightY, 292,
                                               g.tabHighlightY + 68);
            GlowRounded(highlight, 9, Red(0.40f), 7, 1.8f);
            GradientRounded(highlight, 9, Color(0.205f, 0.085f, 0.110f, 0.80f),
                            Color(0.125f, 0.100f, 0.125f, 0.91f));
            InnerGlow(highlight, 9);
            StrokeRounded(highlight, 9, Red(0.92f), 1.2f);
            SetBrush(Red());
            g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(270, top + 34), 5, 5),
                                  g.brush.Get());
        } else if (Contains(tab, l.mouse)) {
            FillRounded(tab, 9, Color(1, 1, 1, 0.035f));
        }
        DrawTabIcon(i, 46, top + 34, selected);
        Text(tabs[i], Rect(78, top + 20, 245, top + 52),
             g.medium.Get(), selected ? Red() : Muted());
    }

    const float contentX = 349.0f + g.pageShift;
    Text(tabs[g.tab], Rect(contentX, 73, 700, 113), g.title.Get(),
         Color(White().r, White().g, White().b, g.pageAlpha));

    float cardTop = 130.0f;
    if (g.tab == 0) {
        const D2D1_RECT_F enemy = Rect(contentX, 124, contentX + 140, 164);
        const D2D1_RECT_F ally = Rect(contentX + 146, 124, contentX + 286, 164);
        const D2D1_RECT_F creep = Rect(contentX + 292, 124, contentX + 432, 164);
        auto segment = [&](const D2D1_RECT_F& r, int value, const wchar_t* label) {
            if (Clicked(l, r)) g.visualTeam = value;
            const bool selected = g.visualTeam == value;
            if (selected) {
                GlowRounded(r, 7, Red(0.40f), 5, 1.4f);
                GradientRounded(r, 7, Color(0.45f, 0.035f, 0.085f),
                                Color(0.19f, 0.030f, 0.060f), true);
                InnerGlow(r, 7);
                StrokeRounded(r, 7, Red(), 1.0f);
            } else {
                GradientRounded(r, 7, Color(0.085f, 0.092f, 0.11f),
                                Color(0.060f, 0.063f, 0.075f), true);
                StrokeRounded(r, 7, Border());
            }
            Text(label, r, g.centered.Get(), selected ? White() : Muted());
        };
        segment(enemy, 0, L"Enemy");
        segment(ally, 1, L"Ally");
        segment(creep, 2, L"Creep");
        cardTop = 190.0f;
    }
    if (g.tab == 1) {
        const D2D1_RECT_F human = Rect(contentX, 124, contentX + 180, 164);
        const D2D1_RECT_F farm = Rect(contentX + 186, 124, contentX + 366, 164);
        auto aimSegment = [&](const D2D1_RECT_F& r, int value, const wchar_t* label) {
            if (Clicked(l, r)) g.aimSubtab = value;
            const bool selected = g.aimSubtab == value;
            if (selected) {
                GlowRounded(r, 7, Red(0.40f), 5, 1.4f);
                GradientRounded(r, 7, Color(0.45f, 0.035f, 0.085f),
                                Color(0.19f, 0.030f, 0.060f), true);
                InnerGlow(r, 7);
                StrokeRounded(r, 7, Red(), 1.0f);
            } else {
                GradientRounded(r, 7, Color(0.085f, 0.092f, 0.11f),
                                Color(0.060f, 0.063f, 0.075f), true);
                StrokeRounded(r, 7, Border());
            }
            Text(label, r, g.centered.Get(), selected ? White() : Muted());
        };
        aimSegment(human, 0, L"Human");
        aimSegment(farm, 1, L"Farm");
        cardTop = 190.0f;
    }

    const bool visualEditor = g.tab == 0;
    // All tabs share one bottom edge. Their top can differ (Aim has subtabs),
    // but the content cards must terminate at the same design-space Y.
    constexpr float contentPanelBottom = 818.0f;
    const float visualPanelBottom = contentPanelBottom;
    const float visualPanelHeight = visualPanelBottom - cardTop;
    const D2D1_RECT_F cardRect = Rect(334, cardTop,
                                      visualEditor ? 990.0f : 1414.0f,
                                      contentPanelBottom);
    GlowRounded(cardRect, 10, Color(0, 0, 0, 0.64f), 5, 2.0f);
    GradientRounded(cardRect, 10, Color(0.098f, 0.112f, 0.148f, 0.68f),
                    Color(0.105f, 0.116f, 0.154f, 0.71f), true);
    StrokeRounded(cardRect, 10, Border(), 1.0f);

    const wchar_t* cardTitles[] = {L"Overlay", L"Aim configuration",
                                    L"Farm configuration", L"Miscellaneous"};
    StrokeRounded(Rect(348, cardTop + 10, 380, cardTop + 42), 6, Red(), 1.4f);
    Line(D2D1::Point2F(356, cardTop + 18), D2D1::Point2F(362, cardTop + 18), Red(), 1.4f);
    Line(D2D1::Point2F(356, cardTop + 18), D2D1::Point2F(356, cardTop + 24), Red(), 1.4f);
    Line(D2D1::Point2F(372, cardTop + 34), D2D1::Point2F(366, cardTop + 34), Red(), 1.4f);
    Line(D2D1::Point2F(372, cardTop + 34), D2D1::Point2F(372, cardTop + 28), Red(), 1.4f);
    Text(cardTitles[g.tab], Rect(391, cardTop + 3, 700, cardTop + 43),
         g.semibold.Get(), White());
    if (!visualEditor)
        Line(D2D1::Point2F(875, cardTop + 58), D2D1::Point2F(875, 772), Border());

    const float leftX = visualEditor ? 350.0f : 392.0f;
    const float rightX = visualEditor ? 660.0f : 908.0f;
    const float leftColumnWidth = visualEditor ? 288.0f : 449.0f;
    const float rightColumnWidth = visualEditor ? 308.0f : 462.0f;
    const float leftColorWidth = leftColumnWidth;
    const float rightColorWidth = rightColumnWidth;
    const float columnWidth = leftColumnWidth;
    const float firstY = cardTop + 44;

    const float viewportTop = cardTop + 44.0f;
    const bool mouseInColumnViewport =
        l.mouse.y >= viewportTop && l.mouse.y <= 818.0f;
    const float previousLeftMax = ColumnMaxScroll(g.leftContentBottom);
    const float previousRightMax = ColumnMaxScroll(g.rightContentBottom);
    if (mouseInColumnViewport && io.MouseWheel != 0.0f) {
        const float scrollStep = 72.0f * io.MouseWheel;
        if (l.mouse.x >= leftX && l.mouse.x < rightX)
            g.leftColumnScroll = std::clamp(g.leftColumnScroll + scrollStep,
                                            -previousLeftMax, 0.0f);
        else if (l.mouse.x >= rightX && l.mouse.x <= 1414.0f)
            g.rightColumnScroll = std::clamp(g.rightColumnScroll + scrollStep,
                                             -previousRightMax, 0.0f);
    }
    if (!l.down) g.activeScrollColumn = 0;
    g.leftContentBottom = viewportTop;
    g.rightContentBottom = viewportTop;

    g.target->PushAxisAlignedClip(
        Rect(334.0f, visualEditor ? cardTop : viewportTop, 1414.0f,
             visualEditor ? kDesignHeight : 818.0f),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (g.tab == 0) {
        bool* teamEsp = g.visualTeam == 0 ? &enemyEspEnabled :
                        g.visualTeam == 1 ? &allyEspEnabled : &creepEspEnabled;
        bool* teamBoxes = g.visualTeam == 0 ? &enemyBoxesEnabled :
                          g.visualTeam == 1 ? &allyBoxesEnabled : &creepBoxesEnabled;
        bool* teamCornerBoxes = g.visualTeam == 0 ? &enemyCornerBoxesEnabled :
                                 g.visualTeam == 1 ? &allyCornerBoxesEnabled : &creepCornerBoxesEnabled;
        bool* teamHealth = g.visualTeam == 0 ? &enemyHealthEnabled :
                           g.visualTeam == 1 ? &allyHealthEnabled : &creepHealthEnabled;
        bool* teamHealthValues = g.visualTeam == 0 ? &enemyHealthValuesEnabled :
                                 g.visualTeam == 1 ? &allyHealthValuesEnabled : &creepHealthValuesEnabled;
        bool* teamNames = g.visualTeam == 0 ? &enemyNamesEnabled : &allyNamesEnabled;
        bool* teamPlayerNames = g.visualTeam == 0 ? &enemyPlayerNamesEnabled : &allyPlayerNamesEnabled;
        bool* teamDistance = g.visualTeam == 0 ? &enemyDistanceEnabled :
                             g.visualTeam == 1 ? &allyDistanceEnabled : &creepDistanceEnabled;
        bool* teamSnaplines = g.visualTeam == 0 ? &enemySnaplinesEnabled : &allySnaplinesEnabled;
        bool* teamBones = g.visualTeam == 0 ? &enemyBonesEnabled : &allyBonesEnabled;
        float* teamBoxColor = g.visualTeam == 0 ? enemyBoxColor :
                              g.visualTeam == 1 ? teammateBoxColor : creepBoxColor;
        float* teamHealthColor = g.visualTeam == 0 ? enemyHealthBarColor :
                                 g.visualTeam == 1 ? teammateHealthBarColor : creepHealthColor;
        float* teamNameColor = g.visualTeam == 0 ? enemyNameColor : teammateNameColor;
        float* teamSkeletonColor = g.visualTeam == 0 ? enemySkeletonColor : teammateSkeletonColor;
        float* teamPlayerColor = g.visualTeam == 0 ? enemyPlayerNameColor : teammatePlayerNameColor;
        float* teamHealthValueColor = g.visualTeam == 0 ? enemyHealthValueColor : teammateHealthValueColor;
        float* teamGlowColor = g.visualTeam == 0 ? enemyGlowColor : teammateGlowColor;
        bool* teamGlowEnabled = g.visualTeam == 0 ? &enemyGlowEnabled : &allyGlowEnabled;
        DrawHeroEspPreview(1008.0f, cardTop, 406.0f, visualPanelHeight,
                           g.visualTeam == 0 ? L"Enemy preset" :
                           g.visualTeam == 1 ? L"Ally preset" : L"Creep preset",
                           *teamEsp,
                           *teamBoxes, *teamCornerBoxes, *teamBones,
                           *teamHealth, *teamHealthValues, *teamNames,
                           *teamPlayerNames, *teamDistance, *teamSnaplines,
                           teamBoxColor, teamSkeletonColor, teamHealthColor,
                           teamNameColor, teamPlayerColor, teamHealthValueColor);

        Text(g.visualTeam == 0 ? L"Enemy ESP settings" :
             g.visualTeam == 1 ? L"Ally ESP settings" : L"Creep ESP settings",
             Rect(leftX, firstY + 4, rightX + rightColumnWidth, firstY + 34),
             g.semibold.Get(), White());

        if (g.visualTeam == 2) {
            DrawEspChip(l, leftX, firstY + 48, leftColumnWidth, L"Creep ESP", teamEsp);
            DrawEspChip(l, rightX, firstY + 48, rightColumnWidth, L"Ally creep ESP",
                        &allyCreepEspEnabled);
            DrawEspChip(l, leftX, firstY + 100, leftColumnWidth, L"Box",
                        teamBoxes, teamBoxColor);
            DrawEspChip(l, rightX, firstY + 100, rightColumnWidth, L"Ally box",
                        &allyCreepBoxesEnabled, allyCreepBoxColor);
            DrawEspChip(l, leftX, firstY + 152, leftColumnWidth, L"Corner box",
                        teamCornerBoxes);
            DrawEspChip(l, rightX, firstY + 152, rightColumnWidth, L"Ally corner box",
                        &allyCreepCornerBoxesEnabled);
            DrawEspChip(l, leftX, firstY + 204, leftColumnWidth, L"Health bar",
                        teamHealth, teamHealthColor);
            DrawEspChip(l, rightX, firstY + 204, rightColumnWidth, L"Ally health bar",
                        &allyCreepHealthEnabled, allyCreepHealthColor);
            DrawEspChip(l, leftX, firstY + 256, leftColumnWidth, L"Health value",
                        teamHealthValues, creepHealthValueColor);
            DrawEspChip(l, rightX, firstY + 256, rightColumnWidth, L"Ally health value",
                        &allyCreepHealthValuesEnabled, allyCreepHealthValueColor);
            DrawEspChip(l, leftX, firstY + 308, leftColumnWidth, L"Distance",
                        teamDistance);
            DrawEspChip(l, rightX, firstY + 308, rightColumnWidth, L"Ally distance",
                        &allyCreepDistanceEnabled);
            DrawEspChip(l, leftX, firstY + 360, leftColumnWidth, L"Soul orbs",
                        &drawOrbEsp);
        } else {
            DrawEspChip(l, leftX, firstY + 48,
                        rightX + rightColumnWidth - leftX, L"Enable ESP", teamEsp);
            DrawEspChip(l, leftX, firstY + 100, leftColumnWidth, L"Box",
                        teamBoxes, teamBoxColor);
            DrawEspChip(l, rightX, firstY + 100, rightColumnWidth, L"Corner box",
                        teamCornerBoxes);
            DrawEspChip(l, leftX, firstY + 152, leftColumnWidth, L"Health bar",
                        teamHealth, teamHealthColor);
            DrawEspChip(l, rightX, firstY + 152, rightColumnWidth, L"Health value",
                        teamHealthValues, teamHealthValueColor);
            DrawEspChip(l, leftX, firstY + 204, leftColumnWidth, L"Skeleton",
                        teamBones, teamSkeletonColor);
            DrawEspChip(l, rightX, firstY + 204, rightColumnWidth, L"Hero name",
                        teamNames, teamNameColor);
            DrawEspChip(l, leftX, firstY + 256, leftColumnWidth, L"Player name",
                        teamPlayerNames, teamPlayerColor);
            DrawEspChip(l, rightX, firstY + 256, rightColumnWidth, L"Distance",
                        teamDistance);
            DrawEspChip(l, leftX, firstY + 308, leftColumnWidth, L"Snapline",
                        teamSnaplines);
            DrawEspChip(l, rightX, firstY + 308, rightColumnWidth, L"Model glow",
                        teamGlowEnabled, teamGlowColor);
            DrawSlider(l, leftX, firstY + 366,
                       rightX + rightColumnWidth - leftX, L"Box thickness",
                       &boxThickness, 0.5f, 4.0f, L"%.2f px");
            DrawSlider(l, leftX, firstY + 424,
                       rightX + rightColumnWidth - leftX, L"Corner length",
                       &cornerBoxLength, 0.10f, 0.50f, L"%.2f");
            const wchar_t* glowModes[] = {L"HP-based fill", L"Normal fill"};
            int* teamGlowMode = g.visualTeam == 0 ? &enemyGlowMode : &allyGlowMode;
            DrawCombo(l, 401, leftX, firstY + 486,
                      rightX + rightColumnWidth - leftX, L"Glow mode",
                      teamGlowMode, glowModes, 2);
        }
    } else if (g.tab == 1) {
        if (g.aimSubtab == 0) {
        DrawToggle(l, leftX, firstY, 280, L"Aim assist",
                   L"Enable player targeting", &aimAssist);
        if (aimAssist) {
        DrawKeyBind(l, leftX + 290, firstY, 159,
                    aimKeyCapture, aimAssistKey, &aimKeyCapture);
        DrawToggle(l, leftX, firstY + 72, columnWidth, L"Visibility check",
                   L"Ignore occluded targets", &aimVisibilityCheck);
        int aimMode = aimMixedMode ? 2 : (aimSilentMode ? 1 : 0);
        const wchar_t* aimModes[] = {L"Normal", L"pSilent", L"Mixed"};
        DrawCombo(l, 101, leftX, firstY + 154, columnWidth, L"Aim mode",
                  &aimMode, aimModes, 3);
        aimSilentMode = aimMode == 1;
        aimMixedMode = aimMode == 2;
        int bindMode = aimToggleMode ? 1 : 0;
        const wchar_t* bindModes[] = {L"Hold", L"Toggle"};
        DrawCombo(l, 102, leftX, firstY + 214, columnWidth, L"Activation",
                  &bindMode, bindModes, 2);
        aimToggleMode = bindMode == 1;
        int targetMode = static_cast<int>(aimTargetMode);
        const wchar_t* targets[] = {L"Head", L"Body", L"Closest"};
        DrawCombo(l, 103, rightX, firstY, columnWidth, L"Target point",
                  &targetMode, targets, 3);
        aimTargetMode = static_cast<AimTargetMode>(std::clamp(targetMode, 0, 2));
        int selectionMode = static_cast<int>(aimSelectionMode);
        const wchar_t* selections[] = {L"Crosshair", L"Distance", L"Health"};
        DrawCombo(l, 104, rightX, firstY + 60, columnWidth, L"Target selection",
                  &selectionMode, selections, 3);
        aimSelectionMode = static_cast<AimSelectionMode>(std::clamp(selectionMode, 0, 2));
        const float previousAimFov = aimFov;
        const float previousPitchSmooth = aimPitchSmooth;
        const float previousYawSmooth = aimYawSmooth;
        DrawSlider(l, rightX, firstY + 130, columnWidth, L"Aim FOV",
                   &aimFov, 40.0f, 600.0f, L"%.0f px");
        DrawSlider(l, rightX, firstY + 192, columnWidth, L"Pitch smoothing",
                   &aimPitchSmooth, 1.0f, 20.0f, L"%.1f");
        DrawSlider(l, rightX, firstY + 254, columnWidth, L"Yaw smoothing",
                   &aimYawSmooth, 1.0f, 20.0f, L"%.1f");
        static bool aimSliderConfigDirty = false;
        if (aimFov != previousAimFov ||
            aimPitchSmooth != previousPitchSmooth ||
            aimYawSmooth != previousYawSmooth)
            aimSliderConfigDirty = true;
        if (aimSliderConfigDirty && !l.down) {
            SaveConfig();
            aimSliderConfigDirty = false;
        }
        DrawToggle(l, rightX, firstY + 316, columnWidth, L"Only Yaw",
                   L"Adjust horizontal aim only", &aimOnlyYaw);
        DrawToggle(l, rightX, firstY + 388, columnWidth, L"Lock Target",
                   L"Keep the current target while valid", &aimLockTarget);
        DrawSlider(l, leftX, firstY + 280, columnWidth, L"Hitchance",
                   &aimHitchance, 0.0f, 100.0f, L"%.0f%%");
        DrawToggle(l, leftX, firstY + 342, columnWidth, L"Backtrack",
                   L"Aim at a recent target position", &aimBacktrack);
        if (aimBacktrack)
            DrawSlider(l, leftX, firstY + 404, columnWidth, L"Backtrack time",
                       &aimBacktrackMs, 1.0f, 1000.0f, L"%.0f ms");
        DrawToggle(l, rightX, firstY + 460, columnWidth, L"Draw FOV circle",
                   L"Show active target radius", &drawFovCircle);
        DrawSlider(l, rightX, firstY + 532, columnWidth, L"FOV opacity",
                   &fovCircleAlpha, 0.0f, 255.0f, L"%.0f");
        }
        } else if (g.aimSubtab == 99) {
            DrawToggle(l, leftX, firstY, columnWidth, L"Creep aim",
                       L"Enable creep targeting", &farmAssist);
            if (farmAssist) {
            int farmMode = farmMixedMode ? 2 : (farmSilentMode ? 1 : 0);
            DrawCombo(l, 301, leftX, firstY + 72, columnWidth, L"Farm mode",
                      &farmMode, kFarmModes, 3);
            farmSilentMode = farmMode == 1;
            farmMixedMode = farmMode == 2;
            int farmBind = farmToggleMode ? 1 : 0;
            DrawCombo(l, 302, leftX, firstY + 132, 280, L"Activation",
                      &farmBind, kFarmActivationModes, 2);
            farmToggleMode = farmBind == 1;
            DrawKeyBind(l, leftX + 290, firstY + 132, 159,
                        farmKeyCapture, farmAssistKey, &farmKeyCapture);
             if (!farmSilentMode || farmMixedMode) {
                 DrawSlider(l, leftX, firstY + 194, columnWidth, L"Farm FOV",
                            &farmFov, 40.0f, 600.0f, L"%.0f px");
                 DrawSlider(l, leftX, firstY + 256, columnWidth, L"Smoothing",
                            &farmAimSmooth, 1.0f, 20.0f, L"%.1f");
             }
             const float farmCircleY = (!farmSilentMode || farmMixedMode)
                 ? firstY + 328.0f : firstY + 194.0f;
             DrawToggle(l, leftX, farmCircleY, columnWidth, L"Farm FOV circle",
                        L"Show the creep aim radius", &drawFarmFovCircle);
             if (drawFarmFovCircle)
                 DrawSlider(l, leftX, farmCircleY + 72, columnWidth, L"FOV opacity",
                            &farmFovAlpha, 0.0f, 255.0f, L"%.0f");
            }
        } else {
            DrawToggle(l, leftX, firstY, columnWidth, L"Creep aim",
                       L"Enable creep targeting", &farmAssist);
            if (farmAssist) {
                int farmMode = farmMixedMode ? 2 : (farmSilentMode ? 1 : 0);
                DrawCombo(l, 301, leftX, firstY + 72, columnWidth, L"Farm mode",
                          &farmMode, kFarmModes, 3);
                farmSilentMode = farmMode == 1;
                farmMixedMode = farmMode == 2;
                int farmBind = farmToggleMode ? 1 : 0;
                DrawCombo(l, 302, leftX, firstY + 132, 280, L"Activation",
                          &farmBind, kFarmActivationModes, 2);
                farmToggleMode = farmBind == 1;
                DrawKeyBind(l, leftX + 290, firstY + 132, 159,
                            farmKeyCapture, farmAssistKey, &farmKeyCapture);
                 if (!farmSilentMode || farmMixedMode) {
                     DrawSlider(l, leftX, firstY + 194, columnWidth, L"Farm FOV",
                                &farmFov, 40.0f, 600.0f, L"%.0f px");
                     DrawSlider(l, leftX, firstY + 256, columnWidth, L"Smoothing",
                                &farmAimSmooth, 1.0f, 20.0f, L"%.1f");
                 }
                 const float farmCircleY = (!farmSilentMode || farmMixedMode)
                     ? firstY + 328.0f : firstY + 194.0f;
                 DrawToggle(l, leftX, farmCircleY, columnWidth, L"Farm FOV circle",
                            L"Show the creep aim radius", &drawFarmFovCircle);
                 if (drawFarmFovCircle)
                     DrawSlider(l, leftX, farmCircleY + 72, columnWidth, L"FOV opacity",
                                &farmFovAlpha, 0.0f, 255.0f, L"%.0f");
             }

            DrawToggle(l, rightX, firstY, columnWidth, L"Orb aim",
                       L"Aim at valid soul orbs", &autoLastHitOrbs);
            if (autoLastHitOrbs) {
                int fireMode = autoLastHitOrbsAutoFire ? 0 : 1;
                const wchar_t* fireModes[] = {L"Auto fire", L"Player fire"};
                DrawCombo(l, 203, rightX, firstY + 72, columnWidth, L"Fire mode",
                          &fireMode, fireModes, 2);
                autoLastHitOrbsAutoFire = fireMode == 0;
                int orbBind = autoLastHitOrbsToggleMode ? 1 : 0;
                const wchar_t* binds[] = {L"Hold", L"Toggle"};
                DrawCombo(l, 204, rightX, firstY + 132, 280, L"Activation",
                          &orbBind, binds, 2);
                autoLastHitOrbsToggleMode = orbBind == 1;
                DrawKeyBind(l, rightX + 290, firstY + 132, 159,
                            autoLastHitOrbsKeyCapture, autoLastHitOrbsKey,
                            &autoLastHitOrbsKeyCapture);
            }
        }
    } else if (g.tab == 99) {
        DrawToggle(l, leftX, firstY, columnWidth, L"Creep aim",
                   L"Enable creep targeting", &farmAssist);
        DrawToggle(l, leftX, firstY + 72, columnWidth, L"Creep ESP",
                   L"Highlight valid creeps", &drawCreepEsp);
        int farmMode = farmMixedMode ? 2 : (farmSilentMode ? 1 : 0);
        DrawCombo(l, 301, leftX, firstY + 154, columnWidth, L"Farm mode",
                  &farmMode, kFarmModes, 3);
        farmSilentMode = farmMode == 1;
        farmMixedMode = farmMode == 2;
        int farmBind = farmToggleMode ? 1 : 0;
        DrawCombo(l, 302, leftX, firstY + 214, columnWidth, L"Activation",
                  &farmBind, kFarmActivationModes, 2);
        farmToggleMode = farmBind == 1;
        DrawSlider(l, leftX, firstY + 280, columnWidth, L"Farm FOV",
                   &farmFov, 40.0f, 600.0f, L"%.0f px");
        DrawSlider(l, leftX, firstY + 342, columnWidth, L"Smoothing",
                   &farmAimSmooth, 1.0f, 20.0f, L"%.1f");
        Text(L"Farm key", Rect(leftX, firstY + 408, leftX + columnWidth, firstY + 436),
             g.regular.Get(), Muted());
        DrawKeyBind(l, leftX, firstY + 442, columnWidth,
                    farmKeyCapture, farmAssistKey, &farmKeyCapture);

        DrawToggle(l, rightX, firstY, columnWidth, L"Orb ESP",
                   L"Highlight active soul orbs", &drawOrbEsp);
        DrawToggle(l, rightX, firstY + 72, columnWidth, L"Orb aim",
                   L"Aim at valid soul orbs", &autoLastHitOrbs);
        DrawToggle(l, rightX, firstY + 144, columnWidth, L"Visibility check",
                   L"Ignore occluded orbs", &orbAimVisibilityCheck);
        int fireMode = autoLastHitOrbsAutoFire ? 0 : 1;
        const wchar_t* fireModes[] = {L"Auto fire", L"Player fire"};
        DrawCombo(l, 203, rightX, firstY + 226, columnWidth, L"Fire mode",
                  &fireMode, fireModes, 2);
        autoLastHitOrbsAutoFire = fireMode == 0;
        int orbBind = autoLastHitOrbsToggleMode ? 1 : 0;
        DrawCombo(l, 204, rightX, firstY + 286, columnWidth, L"Activation",
                  &orbBind, kFarmActivationModes, 2);
        autoLastHitOrbsToggleMode = orbBind == 1;
        Text(L"Orb key", Rect(rightX, firstY + 350, rightX + columnWidth, firstY + 378),
             g.regular.Get(), Muted());
        DrawKeyBind(l, rightX, firstY + 384, columnWidth,
                    autoLastHitOrbsKeyCapture, autoLastHitOrbsKey,
                    &autoLastHitOrbsKeyCapture);
    } else {
        DrawToggle(l, leftX, firstY, columnWidth, L"Auto parry",
                   L"Automatically use parry", &autoParry);
        DrawToggle(l, leftX, firstY + 72, columnWidth, L"Spectator list",
                   L"Show current observers", &drawSpectatorList);
        DrawToggle(l, leftX, firstY + 144, 280, L"Free camera",
                   L"Detach camera from player", &freeCam);
        if (freeCam) {
            DrawKeyBind(l, leftX + 290, firstY + 144, 159,
                        freeCamKeyCapture, freeCamKey, &freeCamKeyCapture);
            DrawSlider(l, leftX, firstY + 226, columnWidth, L"Freecam speed",
                       &freeCamSpeed, 50.0f, 5000.0f, L"%.0f u/s");
        }

        Text(L"Session", Rect(rightX, firstY, rightX + columnWidth, firstY + 38),
             g.semibold.Get(), White());
        wchar_t status[80]{};
        std::swprintf(status, 80, L"%.0f FPS    %zu players", io.Framerate, playerCount);
        Text(status, Rect(rightX, firstY + 45, rightX + columnWidth, firstY + 80),
             g.regular.Get(), Muted());
        Text(L"Unload the module and restore all hooks safely.",
             Rect(rightX, firstY + 112, rightX + columnWidth, firstY + 152),
             g.regular.Get(), Muted());
        const D2D1_RECT_F unload = Rect(rightX, firstY + 170,
                                       rightX + columnWidth, firstY + 214);
        GradientRounded(unload, 7, Color(1.0f, 0.10f, 0.19f), Color(0.60f, 0.01f, 0.06f), true);
        InnerGlow(unload, 7);
        Text(L"Unload DLL", unload, g.centered.Get(), White());
        if (Clicked(l, unload)) RequestUnload();
    }

    const float leftMax = ColumnMaxScroll(g.leftContentBottom);
    const float rightMax = ColumnMaxScroll(g.rightContentBottom);
    g.leftColumnScroll = std::clamp(g.leftColumnScroll, -leftMax, 0.0f);
    g.rightColumnScroll = std::clamp(g.rightColumnScroll, -rightMax, 0.0f);
    DrawColumnScrollbar(l, 866.0f, viewportTop, g.leftContentBottom,
                        g.leftColumnScroll);
    DrawColumnScrollbar(l, 1402.0f, viewportTop, g.rightContentBottom,
                        g.rightColumnScroll);
    g.target->PopAxisAlignedClip();
    DrawPopup(l);
    if (popupSelectionId == 101) {
        aimSilentMode = popupSelectionValue == 1;
        aimMixedMode = popupSelectionValue == 2;
    } else if (popupSelectionId == 102) {
        aimToggleMode = popupSelectionValue == 1;
    } else if (popupSelectionId == 103) {
        aimTargetMode = static_cast<AimTargetMode>(std::clamp(popupSelectionValue, 0, 2));
    } else if (popupSelectionId == 104) {
        aimSelectionMode = static_cast<AimSelectionMode>(std::clamp(popupSelectionValue, 0, 2));
    } else if (popupSelectionId == 301) {
        farmSilentMode = popupSelectionValue == 1;
        farmMixedMode = popupSelectionValue == 2;
    } else if (popupSelectionId == 302) {
        farmToggleMode = popupSelectionValue == 1;
    } else if (popupSelectionId == 203) {
        autoLastHitOrbsAutoFire = popupSelectionValue == 0;
    } else if (popupSelectionId == 204) {
        autoLastHitOrbsToggleMode = popupSelectionValue == 1;
    }
    if (pushedMenuLayer)
        g.target->PopLayer();
    g.target->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT result = g.target->EndDraw();
    if (SUCCEEDED(result) && g.softwareTarget)
        UploadSoftwareMenuTexture();
    else if (result == D2DERR_RECREATE_TARGET)
        ResetTarget();
    g.wasOpen = menuOpen;
}

void ShutdownD2DMenu() {
    ShutdownPreview3D();
    ResetTarget();
    g.regular.Reset();
    g.medium.Reset();
    g.semibold.Reset();
    g.title.Reset();
    g.centered.Reset();
    g.writeFactory.Reset();
    g.wicFactory.Reset();
    g.factory.Reset();
}
