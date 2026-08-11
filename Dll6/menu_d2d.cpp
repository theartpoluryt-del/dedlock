#include "shared.h"
#include "menu_d2d.h"
#include "preview_3d.h"
#include "panorama_preview.h"
#include "resource.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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
constexpr float kMainWindowWidth = 996.0f;
constexpr float kContentPanelBottom = 840.0f;

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
    ComPtr<ID3D11Texture2D> previewReadbackTexture;
    DXGI_FORMAT previewReadbackFormat = DXGI_FORMAT_UNKNOWN;
    UINT previewReadbackWidth = 0;
    UINT previewReadbackHeight = 0;
    Preview3DFrame preview3dFrame{};
    std::vector<uint8_t> preview3dPixels;
    bool preview3dShared = false;
    bool preview3dActive = false;
    bool previewWasDragging = false;
    bool previewFreezeAfterDrag = false;
    uint64_t previewFreezeSerial = 0;
    ComPtr<ID2D1Bitmap> tabIcons[4];
    ComPtr<ID2D1Bitmap> previewAbilityIcons[4];
    ComPtr<ID2D1Bitmap> previewHeroPortraits[38];
    int previewAbilityHeroIndex = -1;
    int heroPopupFirst = 0;
    ComPtr<ID2D1Bitmap> sceneBitmap;
    ComPtr<ID2D1BitmapRenderTarget> blurTarget;
    ComPtr<ID2D1Bitmap> blurBitmap;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11DeviceContext> deviceContext;
    ComPtr<ID3D11Texture2D> sceneReadbackTexture;
    DXGI_FORMAT sceneReadbackFormat = DXGI_FORMAT_UNKNOWN;
    UINT sceneReadbackWidth = 0;
    UINT sceneReadbackHeight = 0;
    std::vector<uint8_t> scenePixels;
    std::vector<uint8_t> sceneBlurPixels;
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
    bool settingsOpen = false;
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
    const wchar_t* items[8]{};
    int count = 0;
    int selected = 0;
    bool heroes = false;
    D2D1_RECT_F rect{};
};

Popup pendingPopup{};
int popupSelectionId = 0;
int popupSelectionValue = -1;
float menuAccentColor[4] = {0.15f, 0.62f, 1.00f, 1.00f};
static const wchar_t* const kFarmModes[] = {L"Normal", L"pSilent", L"Mixed"};
static const wchar_t* const kFarmActivationModes[] = {L"Hold", L"Toggle"};
static const int kPreviewHeroIds[] = {
    1, 2, 3, 4, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 25, 27, 31, 35, 50, 52, 58, 60, 63, 64, 65, 66, 67, 69,
    72, 76, 77, 79, 80, 81};
static const wchar_t* const kPreviewHeroNames[] = {
    L"Infernus", L"Seven", L"Vindicta", L"Lady Geist", L"Abrams",
    L"Wraith", L"McGinnis", L"Paradox", L"Dynamo", L"Kelvin",
    L"Haze", L"Holliday", L"Bebop", L"Calico", L"Grey Talon",
    L"Mo & Krill", L"Shiv", L"Ivy", L"Warden", L"Yamato", L"Lash",
    L"Viscous", L"Pocket", L"Mirage", L"Vyper", L"Sinclair", L"Mina",
    L"Drifter", L"Venator", L"Victor", L"Paige", L"The Doorman",
    L"Billy", L"Graves", L"Apollo", L"Rem", L"Silver", L"Celeste"};

static_assert(std::size(kPreviewHeroIds) == std::size(kPreviewHeroNames));

int PreviewHeroIndex(int heroId) {
    for (int i = 0; i < static_cast<int>(std::size(kPreviewHeroIds)); ++i) {
        if (kPreviewHeroIds[i] == heroId) return i;
    }
    return 0;
}

D2D1_COLOR_F Color(float r, float gg, float b, float a = 1.0f) {
    return D2D1::ColorF(r, gg, b, a);
}

D2D1_COLOR_F White(float a = 1.0f) { return Color(0.88f, 0.89f, 0.92f, a); }
D2D1_COLOR_F Muted(float a = 1.0f) { return Color(0.47f, 0.49f, 0.54f, a); }
D2D1_COLOR_F Red(float a = 1.0f) {
    return Color(menuAccentColor[0], menuAccentColor[1], menuAccentColor[2], a);
}
D2D1_COLOR_F ThemeSurface(float base, float tint, float a = 1.0f) {
    return Color(base + menuAccentColor[0] * tint,
                 base + menuAccentColor[1] * tint,
                 base + menuAccentColor[2] * tint, a);
}
D2D1_COLOR_F Border(float a = 1.0f) { return ThemeSurface(0.075f, 0.105f, a); }

void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    const float m = v - c;
    if (h < 1.0f / 6.0f) { r = c; g = x; b = 0.0f; }
    else if (h < 2.0f / 6.0f) { r = x; g = c; b = 0.0f; }
    else if (h < 3.0f / 6.0f) { r = 0.0f; g = c; b = x; }
    else if (h < 4.0f / 6.0f) { r = 0.0f; g = x; b = c; }
    else if (h < 5.0f / 6.0f) { r = x; g = 0.0f; b = c; }
    else { r = c; g = 0.0f; b = x; }
    r += m; g += m; b += m;
}

void RGBtoHSV(float r, float g, float b, float& h, float& s, float& v) {
    const float maximum = (std::max)(r, (std::max)(g, b));
    const float minimum = (std::min)(r, (std::min)(g, b));
    const float delta = maximum - minimum;
    v = maximum;
    s = maximum <= 0.0001f ? 0.0f : delta / maximum;
    if (delta <= 0.0001f) { h = 0.0f; return; }
    if (maximum == r) h = std::fmod((g - b) / delta, 6.0f);
    else if (maximum == g) h = (b - r) / delta + 2.0f;
    else h = (r - g) / delta + 4.0f;
    h /= 6.0f;
    if (h < 0.0f) h += 1.0f;
}

D2D1_RECT_F Rect(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

bool Contains(const D2D1_RECT_F& r, const D2D1_POINT_2F& p) {
    return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom;
}

float ColumnScroll(float x) {
    return x < 655.0f ? g.leftColumnScroll : g.rightColumnScroll;
}

float ScrolledY(float x, float y) {
    return y + ColumnScroll(x);
}

void NoteContent(float x, float bottom) {
    float& contentBottom = x < 655.0f ? g.leftContentBottom : g.rightContentBottom;
    contentBottom = (std::max)(contentBottom, bottom);
}

float ColumnMaxScroll(float contentBottom) {
    return (std::max)(0.0f, contentBottom - kContentPanelBottom);
}

float ColumnViewportTop() {
    return g.tab == 2 ? 174.0f : 234.0f;
}

bool ColumnVisible(float x, float y, float height) {
    return y + height >= ColumnViewportTop() && y <= kContentPanelBottom;
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
    const D2D1_RECT_F hit = Rect(x, y, x + width, y + 44);
    const D2D1_RECT_F colorRect = colorValue
        ? Rect(x + width - 22, y + 10, x + width, y + 32)
        : Rect(0, 0, 0, 0);
    const bool clickedColor = colorValue && Clicked(l, colorRect);
    if (ColumnVisible(x, y, 66.0f) && Clicked(l, hit) && !clickedColor)
        *value = !*value;
    if (!g.colorPopup && Contains(hit, l.mouse)) {
        FillRounded(Rect(x + 3, y + 2, x + width - 3, y + 42), 3,
                    Color(1, 1, 1, 0.025f));
    }
    const float targetAnimation = *value ? 1.0f : 0.0f;
    auto [toggleIt, inserted] = g.toggleAnimation.emplace(value, targetAnimation);
    float& animation = toggleIt->second;
    animation += ((*value ? 1.0f : 0.0f) - animation) * 0.18f;

    Text(label, Rect(x + 10, y + 8, x + width - 110, y + 34), g.regular.Get(), White());

    const float colorOffset = colorValue ? 34.0f : 0.0f;
    const D2D1_RECT_F track = Rect(x + width - 48 - colorOffset, y + 8,
                                   x + width - 2 - colorOffset, y + 32);
    const D2D1_COLOR_F off = Color(0.17f, 0.19f, 0.22f);
    FillRounded(track, 12, animation > 0.02f ? Red(0.90f) : off);
    const float knob = track.left + 12 + animation * 22.0f;
    SetBrush(White());
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, track.top + 12), 10, 10),
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
}

void DrawSlider(const Layout& l, float x, float y, float width,
                const wchar_t* label, float* value, float minimum, float maximum,
                const wchar_t* format) {
    const float baseY = y;
    NoteContent(x, baseY + 66.0f);
    y = ScrolledY(x, y);
    Text(label, Rect(x + 10, y + 2, x + width - 82, y + 27), g.regular.Get(), White());
    const float trackStart = x + 2;
    const float trackEnd = x + width - 2;
    const D2D1_RECT_F sliderHit = Rect(trackStart - 4, y + 31, trackEnd + 4, y + 64);
    if (ColumnVisible(x, y, 66.0f) && l.clicked && Contains(sliderHit, l.mouse))
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
    FillRounded(Rect(trackStart, y + 46, trackEnd, y + 48), 1, Color(0.16f, 0.17f, 0.20f));
    FillRounded(Rect(trackStart, y + 46, trackStart + (trackEnd - trackStart) * animated, y + 48),
                1, Red());
    const float knob = trackStart + (trackEnd - trackStart) * animated;
    SetBrush(Red());
    g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob, y + 47), 8, 8), g.brush.Get());

    wchar_t output[48]{};
    std::swprintf(output, 48, format, *value);
    const D2D1_RECT_F valueRect = Rect(x + width - 74, y, x + width, y + 27);
    Text(output, valueRect, g.centered.Get(), Muted());
}

void DrawCombo(const Layout& l, int id, float x, float y, float width,
               const wchar_t* label, int* value,
               const wchar_t* const* items, int count) {
    if (!value || !items || count <= 0 || count > 8) {
        if (g.openCombo == id) g.openCombo = 0;
        return;
    }
    *value = std::clamp(*value, 0, count - 1);
    const float baseY = y;
    const bool compact = width <= 320.0f;
    NoteContent(x, baseY + (compact ? 64.0f : 42.0f));
    y = ScrolledY(x, y);
    Text(label, Rect(x + 10, y + 2, compact ? x + width : x + width - 150, y + 24),
         g.regular.Get(), White());
    const D2D1_RECT_F button = compact ? Rect(x, y + 31, x + width, y + 61)
                                      : Rect(x + width - 150, y, x + width, y + 42);
    GradientRounded(button, 6, Color(0.075f, 0.078f, 0.094f),
                    Color(0.050f, 0.052f, 0.064f), true);
    StrokeRounded(button, 6, Border());
    Text(items[*value], Rect(button.left + 12, button.top, button.right - 28, button.bottom),
         g.regular.Get(), White());
    Line(D2D1::Point2F(button.right - 19, button.top + 17),
         D2D1::Point2F(button.right - 13, button.top + 23), Muted(), 1.4f);
    Line(D2D1::Point2F(button.right - 13, button.top + 23),
         D2D1::Point2F(button.right - 7, button.top + 17), Muted(), 1.4f);
    if (ColumnVisible(x, y, compact ? 64.0f : 42.0f) && Clicked(l, button))
        g.openCombo = g.openCombo == id ? 0 : id;
    if (g.openCombo == id) {
        pendingPopup = {};
        pendingPopup.id = id;
        pendingPopup.count = count;
        pendingPopup.selected = *value;
        for (int i = 0; i < count; ++i)
            pendingPopup.items[i] = items[i];
        // Keep a visible lower inset after the last item instead of letting
        // its background touch the popup border.
        const float popupHeight = count * 38.0f + 3.0f;
        const float popupTop = button.bottom + 5.0f + popupHeight > kContentPanelBottom
            ? button.top - 5.0f - popupHeight
            : button.bottom + 5.0f;
        pendingPopup.rect = Rect(button.left, popupTop, button.right,
                                 popupTop + popupHeight);
        g.comboPopupRect = pendingPopup.rect;
    }
}

void DrawHeroCombo(const Layout& l, int id, float x, float y, float width,
                   const wchar_t* label, int* value) {
    constexpr int heroCount = static_cast<int>(std::size(kPreviewHeroNames));
    constexpr int visibleRows = 8;
    constexpr float rowHeight = 44.0f;
    if (!value) return;
    *value = std::clamp(*value, 0, heroCount - 1);
    const float baseY = y;
    NoteContent(x, baseY + 42.0f);
    y = ScrolledY(x, y);
    Text(label, Rect(x + 10, y + 2, x + width - 190, y + 40),
         g.regular.Get(), White());
    const D2D1_RECT_F button = Rect(x + width - 190, y, x + width, y + 42);
    GradientRounded(button, 6, Color(0.075f, 0.078f, 0.094f),
                    Color(0.050f, 0.052f, 0.064f), true);
    StrokeRounded(button, 6, Border());
    if (g.previewHeroPortraits[*value]) {
        g.target->DrawBitmap(g.previewHeroPortraits[*value].Get(),
                             Rect(button.left + 5, button.top + 5,
                                  button.left + 37, button.bottom - 5),
                             1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    Text(kPreviewHeroNames[*value],
         Rect(button.left + 43, button.top, button.right - 27, button.bottom),
         g.regular.Get(), White());
    Line(D2D1::Point2F(button.right - 19, button.top + 17),
         D2D1::Point2F(button.right - 13, button.top + 23), Muted(), 1.4f);
    Line(D2D1::Point2F(button.right - 13, button.top + 23),
         D2D1::Point2F(button.right - 7, button.top + 17), Muted(), 1.4f);
    if (ColumnVisible(x, y, 42.0f) && Clicked(l, button)) {
        const bool opening = g.openCombo != id;
        g.openCombo = opening ? id : 0;
        if (opening) {
            g.heroPopupFirst = std::clamp(*value - visibleRows / 2, 0,
                                          heroCount - visibleRows);
        }
    }
    if (g.openCombo == id) {
        pendingPopup = {};
        pendingPopup.id = id;
        pendingPopup.count = heroCount;
        pendingPopup.selected = *value;
        pendingPopup.heroes = true;
        const float popupHeight = visibleRows * rowHeight + 8.0f;
        const float popupTop = button.bottom + 5.0f + popupHeight > kDesignHeight
            ? button.top - 5.0f - popupHeight
            : button.bottom + 5.0f;
        pendingPopup.rect = Rect(button.left, popupTop, button.right,
                                 popupTop + popupHeight);
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
    if (hovered) FillRounded(Rect(x + 2, y + 1, x + width - 2, y + 40), 3,
                             Color(1, 1, 1, 0.025f));
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
        const D2D1_RECT_F track = Rect(x + width - 40, y + 13, x + width - 10, y + 29);
        FillRounded(track, 8, *value ? Red(0.84f) : Color(0.17f, 0.19f, 0.22f));
        SetBrush(White());
        g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(*value ? track.right - 8 : track.left + 8,
                                                           track.top + 8), 6, 6), g.brush.Get());
    }
}

void DrawNavigationIcon(int variant, float x, float y, bool selected) {
    const D2D1_COLOR_F c = selected ? White() : Muted();
    if (variant == 0) { DrawEye(x, y, c); return; }                 // Enemy
    SetBrush(c);
    if (variant == 1) {                                             // Ally
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x - 5, y - 3), 5, 5), g.brush.Get(), 1.4f);
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 5, y - 3), 5, 5), g.brush.Get(), 1.4f);
        Line(D2D1::Point2F(x - 12, y + 10), D2D1::Point2F(x + 12, y + 10), c, 1.6f); return;
    }
    if (variant == 2) {                                             // Creep
        g.target->DrawRectangle(Rect(x - 8, y - 8, x + 8, y + 8), g.brush.Get(), 1.5f);
        Line(D2D1::Point2F(x - 12, y), D2D1::Point2F(x - 8, y), c, 1.4f);
        Line(D2D1::Point2F(x + 8, y), D2D1::Point2F(x + 12, y), c, 1.4f); return;
    }
    if (variant == 3) {                                             // Player aim
        g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 8, 8), g.brush.Get(), 1.5f);
        Line(D2D1::Point2F(x - 13, y), D2D1::Point2F(x + 13, y), c, 1.4f);
        Line(D2D1::Point2F(x, y - 13), D2D1::Point2F(x, y + 13), c, 1.4f); return;
    }
    if (variant == 4) {                                             // Creep aim
        Line(D2D1::Point2F(x - 9, y + 8), D2D1::Point2F(x, y - 9), c, 1.6f);
        Line(D2D1::Point2F(x, y - 9), D2D1::Point2F(x + 9, y + 8), c, 1.6f);
        Line(D2D1::Point2F(x - 6, y + 2), D2D1::Point2F(x + 6, y + 2), c, 1.4f); return;
    }
    DrawTabIcon(2, x, y, selected);                                 // Misc
}

void DrawSectionHeading(float x, float y, float width, const wchar_t* title) {
    Text(title, Rect(x, y, x + width, y + 27), g.semibold.Get(), White());
    Line(D2D1::Point2F(x, y + 31), D2D1::Point2F(x + width, y + 31), Border(0.92f));
}

float PreviewTileWidth(const wchar_t* label) {
    constexpr float kHorizontalPadding = 9.0f;
    if (!label || !g.writeFactory || !g.regular)
        return 18.0f;

    ComPtr<IDWriteTextLayout> layout;
    const UINT32 length = static_cast<UINT32>(std::wcslen(label));
    if (FAILED(g.writeFactory->CreateTextLayout(label, length, g.regular.Get(),
                                                512.0f, 36.0f, &layout))) {
        return 18.0f + length * 8.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
        return 18.0f + length * 8.0f;
    return std::ceil(metrics.widthIncludingTrailingWhitespace) + kHorizontalPadding * 2.0f;
}

float DrawPreviewTile(const Layout& l, float x, float y,
                      const wchar_t* label, bool* value) {
    const float width = PreviewTileWidth(label);
    const D2D1_RECT_F r = Rect(x, y, x + width, y + 36);
    if (Clicked(l, r)) *value = !*value;
    const bool hover = Contains(r, l.mouse);
    // Off state intentionally mirrors the reference: compact matte-grey tags
    // with no outline, indicator dot, or coloured glow.
    FillRounded(r, 4, *value ? Color(Red().r, Red().g, Red().b, 0.22f)
                             : Color(0.18f, 0.19f, 0.21f, hover ? 1.0f : 0.92f));
    if (*value) StrokeRounded(r, 4, Red(0.70f), 0.8f);
    Text(label, Rect(x + 9, y, x + width - 9, y + 36), g.regular.Get(),
         *value ? White() : Muted());
    return width;
}

void DrawHeroEspPreview(float x, float y, float width, float height,
                        const wchar_t* presetLabel,
                        const wchar_t* selectedHeroName, bool enabled,
                        bool boxes, bool cornerBoxes,
                        bool skeleton, bool health, bool healthValue,
                        bool heroName, bool playerName, bool distance,
                        bool snaplines, bool abilities, const float* boxColor,
                        const float* skeletonColor, const float* healthColor,
                        const float* nameColor, const float* playerColor,
                        const float* healthValueColor, float previewBoxThickness,
                        float previewCornerLength, const Layout& l,
                        bool* enabledToggle, bool* boxesToggle, bool* cornerToggle,
                        bool* skeletonToggle, bool* healthToggle, bool* healthValueToggle,
                        bool* heroNameToggle, bool* playerNameToggle, bool* distanceToggle,
                        bool* snaplineToggle, bool* abilitiesToggle) {
    GlowRounded(Rect(x, y, x + width, y + height), 16,
                Color(0, 0, 0, 0.65f), 5, 2.0f);
    FillRounded(Rect(x, y, x + width, y + height), 8,
                Color(0.0f, 0.0f, 0.0f, 1.0f));
    StrokeRounded(Rect(x, y, x + width, y + height), 16,
                  Color(0.24f, 0.27f, 0.34f, 0.92f), 1.1f);
    Text(L"ESP Preview", Rect(x + 16, y + 10, x + width - 16, y + 42),
         g.semibold.Get(), White());
    Text(enabled ? presetLabel : L"ESP disabled",
         Rect(x + 16, y + 40, x + width - 16, y + 65),
         g.regular.Get(), enabled ? Muted() : Red());

    // The tag flow occupies three 36px rows. Anchor it to the panel bottom
    // and donate the reclaimed space to the model preview above it.
    constexpr float kTileHeight = 36.0f;
    constexpr float kTileRows = 3.0f;
    constexpr float kTileGapY = 8.0f;
    constexpr float kPreviewBottomPadding = 16.0f;
    const float tileY = y + height -
        (kTileRows * kTileHeight + (kTileRows - 1.0f) * kTileGapY + kPreviewBottomPadding);
    const D2D1_RECT_F captureRect = Rect(
        x + 15, y + 74, x + width - 15, tileY - 16.0f);
    const D2D1_RECT_F stage = captureRect;
    // Single, uninterrupted preview surface — no nested grey viewport.
    FillRect(stage, Color(0.0f, 0.0f, 0.0f, 1.0f));
    // Compact left-aligned tag flow: exactly the visual rhythm of the
    // supplied preview rather than a two-column settings grid.
    constexpr float kTileGapX = 8.0f;
    const float tileLeft = x + 16.0f;
    const float tileRight = x + width - 16.0f;
    float tileCursorX = tileLeft;
    float tileCursorY = tileY;
    const auto drawFlowTile = [&](const wchar_t* label, bool* value) {
        const float tileWidth = PreviewTileWidth(label);
        // Wrap only when the next measured tile does not fit. This avoids
        // the large unused right edge caused by the old hand-made rows.
        if (tileCursorX > tileLeft && tileCursorX + tileWidth > tileRight) {
            tileCursorX = tileLeft;
            tileCursorY += 36.0f + kTileGapY;
        }
        DrawPreviewTile(l, tileCursorX, tileCursorY, label, value);
        tileCursorX += tileWidth + kTileGapX;
    };
    drawFlowTile(L"Skeleton", skeletonToggle);
    drawFlowTile(L"Box", boxesToggle);
    drawFlowTile(L"Distance", distanceToggle);
    drawFlowTile(L"Hero name", heroNameToggle);
    drawFlowTile(L"Player name", playerNameToggle);
    drawFlowTile(L"Corner box", cornerToggle);
    drawFlowTile(L"Health", healthToggle);
    drawFlowTile(L"Health bar", healthValueToggle);
    drawFlowTile(L"Snapline", snaplineToggle);
    drawFlowTile(L"Abilities", abilitiesToggle);
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
    const D2D1_RECT_F renderRect = captureRect;
    if (g.preview3dActive && g.preview3dBitmap) {
        // The capture texture is created in physical backbuffer pixels. Draw
        // it in the same coordinate space so the D2D menu scale does not add
        // a second blur-producing resample.
        D2D1_MATRIX_3X2_F menuTransform{};
        g.target->GetTransform(&menuTransform);
        const D2D1_RECT_F physicalRect = Rect(
            std::floor(renderRect.left * menuTransform._11 + menuTransform._31),
            std::floor(renderRect.top * menuTransform._22 + menuTransform._32),
            std::floor(renderRect.right * menuTransform._11 + menuTransform._31),
            std::floor(renderRect.bottom * menuTransform._22 + menuTransform._32));
        // Preserve the 3D target's aspect ratio while filling the preview.
        // A tiny centred crop is preferable to stretching the hero vertically.
        const D2D1_SIZE_F sourceSize = g.preview3dBitmap->GetSize();
        const float destinationAspect = (physicalRect.right - physicalRect.left) /
                                        (physicalRect.bottom - physicalRect.top);
        const float sourceAspect = sourceSize.width / sourceSize.height;
        D2D1_RECT_F sourceRect = Rect(0.0f, 0.0f, sourceSize.width, sourceSize.height);
        if (destinationAspect < sourceAspect) {
            const float croppedWidth = sourceSize.height * destinationAspect;
            sourceRect.left = (sourceSize.width - croppedWidth) * 0.5f;
            sourceRect.right = sourceRect.left + croppedWidth;
        } else if (destinationAspect > sourceAspect) {
            const float croppedHeight = sourceSize.width / destinationAspect;
            sourceRect.top = (sourceSize.height - croppedHeight) * 0.5f;
            sourceRect.bottom = sourceRect.top + croppedHeight;
        }
        g.target->SetTransform(D2D1::Matrix3x2F::Identity());
        g.target->DrawBitmap(g.preview3dBitmap.Get(), physicalRect, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, sourceRect);
        g.target->SetTransform(menuTransform);
        const float renderWidth = renderRect.right - renderRect.left;
        const float renderHeight = renderRect.bottom - renderRect.top;
        modelRect = Rect(
            renderRect.left + g.preview3dFrame.left * renderWidth,
            renderRect.top + g.preview3dFrame.top * renderHeight,
            renderRect.left + g.preview3dFrame.right * renderWidth,
            renderRect.top + g.preview3dFrame.bottom * renderHeight);
    } else {
        const float modelTop = renderRect.top;
        const float modelBottom = renderRect.bottom;
        const float modelHeight = modelBottom - modelTop;
        constexpr float sourceAspect = 515.0f / 1140.0f;
        const float modelHalfWidth = modelHeight * sourceAspect * 0.5f;
        modelRect = Rect(previewCenterX - modelHalfWidth, modelTop,
                         previewCenterX + modelHalfWidth, modelBottom);
    }
    if (!g.preview3dActive) {
        Text(L"Panorama preview unavailable",
             Rect(stage.left + 12, stage.top, stage.right - 12, stage.bottom),
             g.centered.Get(), Red(0.82f));
        return;
    }
    if (!enabled) return;

    const float left = (std::max)(stage.left + 2.0f, modelRect.left - 3.0f);
    const float right = (std::min)(stage.right - 2.0f, modelRect.right + 3.0f);
    const float top = (std::max)(stage.top + 2.0f, modelRect.top - 9.0f);
    const float bottom = (std::min)(stage.bottom - 2.0f, modelRect.bottom + 5.0f);
    const float cx = (left + right) * 0.5f;
    const D2D1_COLOR_F box = Color(boxColor[0], boxColor[1], boxColor[2]);
    const D2D1_COLOR_F bones = Color(skeletonColor[0], skeletonColor[1], skeletonColor[2]);
    const D2D1_COLOR_F hp = Color(healthColor[0], healthColor[1], healthColor[2]);
    if (boxes) {
        const float lineThickness = std::clamp(previewBoxThickness, 0.5f, 4.0f);
        if (cornerBoxes) {
            // Match the in-game player ESP exactly: the slider is a fraction
            // of the smaller box dimension, with no preview-only pixel clamp.
            const float c = std::clamp(previewCornerLength, 0.05f, 0.35f) *
                            (std::min)(right - left, bottom - top);
            Line(D2D1::Point2F(left, top), D2D1::Point2F(left + c, top), box, lineThickness);
            Line(D2D1::Point2F(left, top), D2D1::Point2F(left, top + c), box, lineThickness);
            Line(D2D1::Point2F(right - c, top), D2D1::Point2F(right, top), box, lineThickness);
            Line(D2D1::Point2F(right, top), D2D1::Point2F(right, top + c), box, lineThickness);
            Line(D2D1::Point2F(left, bottom - c), D2D1::Point2F(left, bottom), box, lineThickness);
            Line(D2D1::Point2F(left, bottom), D2D1::Point2F(left + c, bottom), box, lineThickness);
            Line(D2D1::Point2F(right - c, bottom), D2D1::Point2F(right, bottom), box, lineThickness);
            Line(D2D1::Point2F(right, bottom - c), D2D1::Point2F(right, bottom), box, lineThickness);
        } else {
            SetBrush(box);
            g.target->DrawRectangle(Rect(left, top, right, bottom), g.brush.Get(),
                                    lineThickness);
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
    } else if (skeleton) {
        const float bodyHeight = bottom - top;
        const float head = top + bodyHeight * 0.055f;
        const float neck = top + bodyHeight * 0.12f;
        const float shoulders = top + bodyHeight * 0.21f;
        const float hips = top + bodyHeight * 0.52f;
        const float knees = top + bodyHeight * 0.76f;
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
        Text(selectedHeroName ? selectedHeroName : L"Hero",
             Rect(left - 20, labelY, right + 20, labelY + 24),
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
    // Abilities are a regular D2D overlay, just like the box and labels above.
    // Do not resize captureRect/renderRect: Panorama must keep its original
    // destination and aspect ratio. DrawBitmap is intentionally used instead
    // of FillOpacityMask so a PBGRA WIC bitmap cannot invalidate the target.
    if (abilities) {
        constexpr float iconSize = 54.0f;
        constexpr float iconGap = 5.0f;
        constexpr float levelGap = 3.0f;
        constexpr float levelHeight = 4.0f;
        constexpr int maxAbilityLevel = 3;
        constexpr int previewLevels[4] = {3, 2, 1, 0};
        const float rowWidth = iconSize * 4.0f + iconGap * 3.0f;
        float iconX = stage.left + ((stage.right - stage.left) - rowWidth) * 0.5f;
        const float iconY = stage.bottom - iconSize - levelHeight - 10.0f;
        for (int slot = 0; slot < 4; ++slot) {
            const D2D1_RECT_F tile = Rect(
                iconX, iconY, iconX + iconSize, iconY + iconSize);
            FillRounded(tile, 2.0f,
                        Color(221.0f / 255.0f, 213.0f / 255.0f, 195.0f / 255.0f));
            if (g.previewAbilityIcons[slot]) {
                const D2D1_RECT_F image = Rect(
                    tile.left + 2.0f, tile.top + 2.0f,
                    tile.right - 2.0f, tile.bottom - 2.0f);
                g.target->DrawBitmap(
                    g.previewAbilityIcons[slot].Get(), image, 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            StrokeRounded(tile, 2.0f,
                          Color(62.0f / 255.0f, 54.0f / 255.0f, 43.0f / 255.0f), 1.0f);
            const float levelY = tile.bottom + 3.0f;
            FillRounded(Rect(tile.left + 1.0f, levelY - 1.0f,
                             tile.right - 1.0f, levelY + levelHeight + 1.0f),
                        1.0f,
                        Color(58.0f / 255.0f, 50.0f / 255.0f, 39.0f / 255.0f));
            const float levelWidth =
                (iconSize - 6.0f - (maxAbilityLevel - 1) * levelGap) /
                maxAbilityLevel;
            for (int level = 0; level < maxAbilityLevel; ++level) {
                const float levelX = tile.left + 3.0f +
                    level * (levelWidth + levelGap);
                const D2D1_COLOR_F levelColor = level < previewLevels[slot]
                    ? Color(74.0f / 255.0f, 210.0f / 255.0f, 112.0f / 255.0f)
                    : Color(210.0f / 255.0f, 76.0f / 255.0f, 65.0f / 255.0f);
                FillRounded(Rect(levelX, levelY,
                                 levelX + levelWidth, levelY + levelHeight),
                            0.5f, levelColor);
            }
            iconX += iconSize + iconGap;
        }
    }
    if (snaplines)
        Line(D2D1::Point2F(x + width * 0.5f, stage.bottom - 8),
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
        const int maximumCount = pendingPopup.heroes
            ? static_cast<int>(std::size(kPreviewHeroNames)) : 8;
        if (pendingPopup.count <= 0 || pendingPopup.count > maximumCount) {
            g.openCombo = 0;
            pendingPopup = {};
            return;
        }
        pendingPopup.selected = std::clamp(pendingPopup.selected, 0,
                                            pendingPopup.count - 1);
        const D2D1_RECT_F r = pendingPopup.rect;
        GlowRounded(r, 7, Color(0, 0, 0, 0.55f), 4, 2.0f);
        FillRounded(r, 7, Color(0.045f, 0.047f, 0.058f, 0.995f));
        StrokeRounded(r, 7, Border());
        constexpr int visibleHeroRows = 8;
        constexpr float heroRowHeight = 44.0f;
        if (pendingPopup.heroes && Contains(r, l.mouse)) {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                const int maximumFirst = pendingPopup.count - visibleHeroRows;
                g.heroPopupFirst = std::clamp(
                    g.heroPopupFirst - (wheel > 0.0f ? 1 : -1),
                    0, (std::max)(0, maximumFirst));
            }
        }
        const int first = pendingPopup.heroes ? g.heroPopupFirst : 0;
        const int visibleCount = pendingPopup.heroes
            ? (std::min)(visibleHeroRows, pendingPopup.count - first)
            : pendingPopup.count;
        const float rowHeight = pendingPopup.heroes ? heroRowHeight : 38.0f;
        for (int row = 0; row < visibleCount; ++row) {
            const int itemIndex = first + row;
            const D2D1_RECT_F item = Rect(
                r.left + 4, r.top + 4 + row * rowHeight,
                r.right - 4, r.top + 4 + (row + 1) * rowHeight - 4);
            if (itemIndex == pendingPopup.selected)
                FillRounded(item, 5, Color(0.94f, 0.025f, 0.12f, 0.16f));
            else if (Contains(item, l.mouse))
                FillRounded(item, 5, Color(1, 1, 1, 0.04f));
            const wchar_t* itemLabel = pendingPopup.heroes
                ? kPreviewHeroNames[itemIndex]
                : (pendingPopup.items[itemIndex]
                       ? pendingPopup.items[itemIndex] : L"");
            float textLeft = item.left + 10.0f;
            if (pendingPopup.heroes && g.previewHeroPortraits[itemIndex]) {
                g.target->DrawBitmap(
                    g.previewHeroPortraits[itemIndex].Get(),
                    Rect(item.left + 4, item.top + 2,
                         item.left + 40, item.bottom - 2),
                    1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                textLeft = item.left + 48.0f;
            }
            Text(itemLabel, Rect(textLeft, item.top, item.right, item.bottom),
                 g.regular.Get(),
                 itemIndex == pendingPopup.selected ? Red() : White());
            if (l.clicked && Contains(item, l.mouse)) {
                popupSelectionId = pendingPopup.id;
                popupSelectionValue = itemIndex;
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
    const float viewportBottom = kContentPanelBottom;
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
        g.activeScrollColumn = x < 655.0f ? 1 : 2;
        g.scrollGrabOffset = l.mouse.y - thumbTop;
    }
    if (g.activeScrollColumn == (x < 655.0f ? 1 : 2) && l.down) {
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

bool LoadEmbeddedBitmap(UINT resourceId, ComPtr<ID2D1Bitmap>& output,
                        bool tintBlack = false) {
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
                                     WICBitmapPaletteTypeCustom))) {
        output.Reset();
        return false;
    }
    if (!tintBlack) {
        if (FAILED(g.target->CreateBitmapFromWicBitmap(
                converter.Get(), nullptr, output.GetAddressOf()))) {
            output.Reset();
            return false;
        }
        return true;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height)
        return false;
    const UINT stride = width * 4;
    std::vector<BYTE> pixels(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride,
                                     static_cast<UINT>(pixels.size()),
                                     pixels.data()))) return false;
    // 32bppPBGRA: clear B/G/R and preserve the decoded alpha channel. This
    // produces a true black silhouette without an opacity-mask draw call.
    for (size_t pixel = 0; pixel + 3 < pixels.size(); pixel += 4) {
        pixels[pixel + 0] = 0;
        pixels[pixel + 1] = 0;
        pixels[pixel + 2] = 0;
    }
    ComPtr<IWICBitmap> blackBitmap;
    if (FAILED(g.wicFactory->CreateBitmapFromMemory(
            width, height, GUID_WICPixelFormat32bppPBGRA,
            stride, static_cast<UINT>(pixels.size()), pixels.data(),
            blackBitmap.GetAddressOf())) ||
        FAILED(g.target->CreateBitmapFromWicBitmap(
            blackBitmap.Get(), nullptr, output.GetAddressOf()))) {
        output.Reset();
        return false;
    }
    return true;
}

void LoadEmbeddedAssets() {
    LoadEmbeddedBitmap(IDR_DEADLOCK_LOGO, g.logoBitmap);
    const UINT iconIds[4]{IDR_ICON_EYE, IDR_ICON_CROSSHAIR,
                          IDR_ICON_SPROUT, IDR_ICON_SETTINGS};
    for (int i = 0; i < 4; ++i)
        LoadEmbeddedBitmap(iconIds[i], g.tabIcons[i]);
    for (int i = 0; i < static_cast<int>(std::size(kPreviewHeroIds)); ++i)
        LoadEmbeddedBitmap(IDR_HERO_PORTRAIT_BASE + i,
                           g.previewHeroPortraits[i]);
}

void EnsurePreviewAbilityAssets(int heroIndex) {
    const int heroCount = static_cast<int>(std::size(kPreviewHeroIds));
    if (heroIndex < 0 || heroIndex >= heroCount) heroIndex = -1;
    if (g.previewAbilityHeroIndex == heroIndex) return;
    for (auto& icon : g.previewAbilityIcons) icon.Reset();
    g.previewAbilityHeroIndex = heroIndex;
    if (heroIndex < 0) return;
    const UINT firstResource = IDR_ABILITY_INFERNUS_1 + heroIndex * 4;
    for (int slot = 0; slot < 4; ++slot)
        LoadEmbeddedBitmap(firstResource + slot,
                           g.previewAbilityIcons[slot], true);
}

bool BindPreview3DFrame(const Preview3DFrame& frame,
                        ID3D11DeviceContext* context) {
    if (!frame.texture || !g.target || !context) return false;
    const auto readFramePixels = [&](std::vector<uint8_t>& pixels,
                                     uint32_t& width, uint32_t& height,
                                     uint32_t& stride) -> bool {
        D3D11_TEXTURE2D_DESC sourceDescription{};
        frame.texture->GetDesc(&sourceDescription);
        const bool bgra = sourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          sourceDescription.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgba = sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                          sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if ((!bgra && !rgba) || sourceDescription.SampleDesc.Count != 1)
            return false;
        if (!g.previewReadbackTexture ||
            g.previewReadbackWidth != sourceDescription.Width ||
            g.previewReadbackHeight != sourceDescription.Height ||
            g.previewReadbackFormat != sourceDescription.Format) {
            g.previewReadbackTexture.Reset();
            ComPtr<ID3D11Device> sourceDevice;
            frame.texture->GetDevice(sourceDevice.GetAddressOf());
            if (!sourceDevice) return false;
            D3D11_TEXTURE2D_DESC staging = sourceDescription;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;
            if (FAILED(sourceDevice->CreateTexture2D(
                    &staging, nullptr, g.previewReadbackTexture.GetAddressOf())))
                return false;
            g.previewReadbackWidth = sourceDescription.Width;
            g.previewReadbackHeight = sourceDescription.Height;
            g.previewReadbackFormat = sourceDescription.Format;
        }
        context->CopyResource(g.previewReadbackTexture.Get(), frame.texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(g.previewReadbackTexture.Get(), 0,
                                D3D11_MAP_READ, 0, &mapped))) return false;
        width = sourceDescription.Width;
        height = sourceDescription.Height;
        stride = width * 4;
        pixels.resize(static_cast<size_t>(stride) * height);
        for (uint32_t row = 0; row < height; ++row) {
            const auto* source = static_cast<const uint8_t*>(mapped.pData) +
                static_cast<size_t>(row) * mapped.RowPitch;
            auto* destination = pixels.data() + static_cast<size_t>(row) * stride;
            if (bgra) {
                std::memcpy(destination, source, stride);
            } else {
                for (uint32_t column = 0; column < width; ++column) {
                    destination[column * 4 + 0] = source[column * 4 + 2];
                    destination[column * 4 + 1] = source[column * 4 + 1];
                    destination[column * 4 + 2] = source[column * 4 + 0];
                    destination[column * 4 + 3] = source[column * 4 + 3];
                }
            }
        }
        context->Unmap(g.previewReadbackTexture.Get(), 0);
        return true;
    };
    if (g.preview3dTexture.Get() != frame.texture || !g.preview3dBitmap) {
        g.preview3dBitmap.Reset();
        g.preview3dTexture.Reset();
        g.preview3dShared = false;
        ComPtr<IDXGISurface> surface;
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        // WIC software targets cannot share a DXGI surface. Use the staging
        // texture readback below for those targets.
        if (!g.softwareTarget &&
            SUCCEEDED(frame.texture->QueryInterface(
                IID_PPV_ARGS(surface.GetAddressOf()))) &&
            SUCCEEDED(g.target->CreateSharedBitmap(
                __uuidof(IDXGISurface), surface.Get(), &properties,
                g.preview3dBitmap.GetAddressOf()))) {
            g.preview3dShared = true;
        } else {
            uint32_t width = 0, height = 0, stride = 0;
            if (!readFramePixels(g.preview3dPixels, width, height, stride) ||
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
        if (!readFramePixels(g.preview3dPixels, width, height, stride) ||
            FAILED(g.preview3dBitmap->CopyFromMemory(
                nullptr, g.preview3dPixels.data(), stride))) {
            return false;
        }
    }
    g.preview3dFrame = frame;
    return true;
}

bool PrepareBackgroundBlur(UINT width, UINT height, UINT captureX = 0,
                           UINT captureY = 0, UINT captureWidth = 0,
                           UINT captureHeight = 0) {
    if (!width || !height || !g.target) return false;
    if (!captureWidth) captureWidth = width;
    if (!captureHeight) captureHeight = height;
    if (!g.sceneBitmap || g.blurSourceSize.width != captureWidth ||
        g.blurSourceSize.height != captureHeight) {
        g.sceneBitmap.Reset();
        g.blurBitmap.Reset();
        g.blurTarget.Reset();
        g.blurSourceSize = {};

        const D2D1_PIXEL_FORMAT pixelFormat = g.target->GetPixelFormat();
        if (FAILED(g.target->CreateBitmap(
                D2D1::SizeU(captureWidth, captureHeight), nullptr, 0,
                D2D1::BitmapProperties(pixelFormat, 96.0f, 96.0f),
                g.sceneBitmap.GetAddressOf()))) {
            return false;
        }
        // Render at half resolution: the filter below keeps the same visual
        // radius, while the enlarged result no longer exposes low-res blocks.
        const UINT blurWidth = (std::max)(1u, captureWidth / 2u);
        const UINT blurHeight = (std::max)(1u, captureHeight / 2u);
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
        g.blurSourceSize = D2D1::SizeU(captureWidth, captureHeight);
    }

    if (g.softwareTarget) {
        // The WIC target holds only our transparent menu layer.  Read the game
        // backbuffer instead, before this layer is uploaded, so the blur never
        // contains labels, icons or a previous menu frame.
        ComPtr<ID3D11Texture2D> backBuffer;
        if (!g.swapChain || !g.deviceContext ||
            FAILED(g.swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()))))
            return false;
        D3D11_TEXTURE2D_DESC source{};
        backBuffer->GetDesc(&source);
        const bool bgra = source.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                          source.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rgba = source.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                          source.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if ((!bgra && !rgba) || source.SampleDesc.Count != 1 ||
            captureX + captureWidth > source.Width ||
            captureY + captureHeight > source.Height)
            return false;
        if (!g.sceneReadbackTexture || g.sceneReadbackWidth != captureWidth ||
            g.sceneReadbackHeight != captureHeight || g.sceneReadbackFormat != source.Format) {
            g.sceneReadbackTexture.Reset();
            D3D11_TEXTURE2D_DESC staging = source;
            staging.Width = captureWidth;
            staging.Height = captureHeight;
            staging.Usage = D3D11_USAGE_STAGING;
            staging.BindFlags = 0;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.MiscFlags = 0;
            ComPtr<ID3D11Device> device;
            backBuffer->GetDevice(device.GetAddressOf());
            if (!device || FAILED(device->CreateTexture2D(
                    &staging, nullptr, g.sceneReadbackTexture.GetAddressOf())))
                return false;
            g.sceneReadbackWidth = captureWidth;
            g.sceneReadbackHeight = captureHeight;
            g.sceneReadbackFormat = source.Format;
        }
        const D3D11_BOX captureBox{captureX, captureY, 0,
                                   captureX + captureWidth,
                                   captureY + captureHeight, 1};
        g.deviceContext->CopySubresourceRegion(g.sceneReadbackTexture.Get(), 0,
                                                0, 0, 0, backBuffer.Get(), 0,
                                                &captureBox);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g.deviceContext->Map(g.sceneReadbackTexture.Get(), 0,
                                        D3D11_MAP_READ, 0, &mapped)))
            return false;
        const UINT stride = captureWidth * 4;
        g.scenePixels.resize(static_cast<size_t>(stride) * captureHeight);
        for (UINT row = 0; row < captureHeight; ++row) {
            const uint8_t* input = static_cast<const uint8_t*>(mapped.pData) +
                                   static_cast<size_t>(row) * mapped.RowPitch;
            uint8_t* output = g.scenePixels.data() + static_cast<size_t>(row) * stride;
            if (bgra) {
                std::memcpy(output, input, stride);
            } else {
                for (UINT column = 0; column < captureWidth; ++column) {
                    output[column * 4 + 0] = input[column * 4 + 2];
                    output[column * 4 + 1] = input[column * 4 + 1];
                    output[column * 4 + 2] = input[column * 4 + 0];
                    output[column * 4 + 3] = input[column * 4 + 3];
                }
            }
        }
        g.deviceContext->Unmap(g.sceneReadbackTexture.Get(), 0);
        // Blur the full-resolution backbuffer.  Downscaling a small bitmap and
        // expanding it again creates the blocks visible through the glass.
        // A separable box filter keeps the result smooth at native resolution.
        g.sceneBlurPixels.resize(g.scenePixels.size());
        constexpr int kBlurRadius = 10;
        const auto blurHorizontal = [&](const std::vector<uint8_t>& source,
                                        std::vector<uint8_t>& destination) {
            for (UINT y = 0; y < captureHeight; ++y) {
                for (int channel = 0; channel < 4; ++channel) {
                    int sum = 0;
                    for (int sample = -kBlurRadius; sample <= kBlurRadius; ++sample) {
                        const UINT x = static_cast<UINT>(std::clamp(sample, 0,
                            static_cast<int>(captureWidth) - 1));
                        sum += source[(static_cast<size_t>(y) * captureWidth + x) * 4 + channel];
                    }
                    for (UINT x = 0; x < captureWidth; ++x) {
                        destination[(static_cast<size_t>(y) * captureWidth + x) * 4 + channel] =
                            static_cast<uint8_t>(sum / (kBlurRadius * 2 + 1));
                        const UINT removeX = static_cast<UINT>(std::clamp(
                            static_cast<int>(x) - kBlurRadius, 0,
                            static_cast<int>(captureWidth) - 1));
                        const UINT addX = static_cast<UINT>(std::clamp(
                            static_cast<int>(x) + kBlurRadius + 1, 0,
                            static_cast<int>(captureWidth) - 1));
                        sum -= source[(static_cast<size_t>(y) * captureWidth + removeX) * 4 + channel];
                        sum += source[(static_cast<size_t>(y) * captureWidth + addX) * 4 + channel];
                    }
                }
            }
        };
        const auto blurVertical = [&](const std::vector<uint8_t>& source,
                                      std::vector<uint8_t>& destination) {
            for (UINT x = 0; x < captureWidth; ++x) {
                for (int channel = 0; channel < 4; ++channel) {
                    int sum = 0;
                    for (int sample = -kBlurRadius; sample <= kBlurRadius; ++sample) {
                        const UINT y = static_cast<UINT>(std::clamp(sample, 0,
                            static_cast<int>(captureHeight) - 1));
                        sum += source[(static_cast<size_t>(y) * captureWidth + x) * 4 + channel];
                    }
                    for (UINT y = 0; y < captureHeight; ++y) {
                        destination[(static_cast<size_t>(y) * captureWidth + x) * 4 + channel] =
                            static_cast<uint8_t>(sum / (kBlurRadius * 2 + 1));
                        const UINT removeY = static_cast<UINT>(std::clamp(
                            static_cast<int>(y) - kBlurRadius, 0,
                            static_cast<int>(captureHeight) - 1));
                        const UINT addY = static_cast<UINT>(std::clamp(
                            static_cast<int>(y) + kBlurRadius + 1, 0,
                            static_cast<int>(captureHeight) - 1));
                        sum -= source[(static_cast<size_t>(removeY) * captureWidth + x) * 4 + channel];
                        sum += source[(static_cast<size_t>(addY) * captureWidth + x) * 4 + channel];
                    }
                }
            }
        };
        blurHorizontal(g.scenePixels, g.sceneBlurPixels);
        blurVertical(g.sceneBlurPixels, g.scenePixels);
        if (FAILED(g.sceneBitmap->CopyFromMemory(nullptr, g.scenePixels.data(), stride)))
            return false;
        return true;
    } else if (FAILED(g.sceneBitmap->CopyFromRenderTarget(nullptr, g.target.Get(), nullptr))) {
        return false;
    }
    g.blurTarget->BeginDraw();
    g.blurTarget->Clear(Color(0, 0, 0, 0));
    const D2D1_SIZE_F blurSize = g.blurTarget->GetSize();
    // A 3x3 tent filter produces the original strong glass blur while the
    // 1/4-resolution target keeps its edges smooth rather than blocky.
    // Doubled offsets preserve the previous on-screen blur radius after the
    // target resolution was raised from one quarter to one half.
    constexpr float kOffsets[] = {-4.0f, 0.0f, 4.0f};
    for (const float offsetY : kOffsets) {
        for (const float offsetX : kOffsets) {
            const float weight = (offsetX == 0.0f && offsetY == 0.0f) ? 0.22f : 0.17f;
            g.blurTarget->DrawBitmap(
                g.sceneBitmap.Get(),
                Rect(offsetX, offsetY, blurSize.width + offsetX,
                     blurSize.height + offsetY),
                weight, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }
    return SUCCEEDED(g.blurTarget->EndDraw());
}

void ResetTarget() {
    g.menuLayer.Reset();
    g.logoBitmap.Reset();
    g.previewHeroBitmap.Reset();
    g.preview3dBitmap.Reset();
    g.preview3dTexture.Reset();
    g.previewReadbackTexture.Reset();
    g.previewReadbackFormat = DXGI_FORMAT_UNKNOWN;
    g.previewReadbackWidth = 0;
    g.previewReadbackHeight = 0;
    g.preview3dFrame = {};
    g.preview3dPixels.clear();
    g.preview3dShared = false;
    g.preview3dActive = false;
    g.previewWasDragging = false;
    g.previewFreezeAfterDrag = false;
    g.previewFreezeSerial = 0;
    for (auto& icon : g.tabIcons) icon.Reset();
    for (auto& icon : g.previewAbilityIcons) icon.Reset();
    for (auto& portrait : g.previewHeroPortraits) portrait.Reset();
    g.previewAbilityHeroIndex = -1;
    g.blurBitmap.Reset();
    g.blurTarget.Reset();
    g.sceneBitmap.Reset();
    g.sceneReadbackTexture.Reset();
    g.sceneReadbackFormat = DXGI_FORMAT_UNKNOWN;
    g.sceneReadbackWidth = 0;
    g.sceneReadbackHeight = 0;
    g.scenePixels.clear();
    g.sceneBlurPixels.clear();
    g.deviceContext.Reset();
    g.swapChain.Reset();
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
    device->GetImmediateContext(g.deviceContext.GetAddressOf());
    if (!g.deviceContext) {
        ResetTarget();
        return false;
    }
    g.swapChain = swapChain;
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

void DrawMenuSettingsPanel(const Layout& l);

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

    // Software mode captures the swap-chain backbuffer in
    // PrepareBackgroundBlur; hardware mode reads its D2D surface directly.
    const UINT displayWidth = static_cast<UINT>(io.DisplaySize.x);
    const UINT displayHeight = static_cast<UINT>(io.DisplaySize.y);
    const UINT blurLeft = static_cast<UINT>(std::clamp(
        static_cast<int>(std::floor(l.x)), 0, static_cast<int>(displayWidth)));
    const UINT blurTop = static_cast<UINT>(std::clamp(
        static_cast<int>(std::floor(l.y)), 0, static_cast<int>(displayHeight)));
    const UINT blurWidth = (std::min)(
        static_cast<UINT>(std::ceil(298.0f * safeScale)), displayWidth - blurLeft);
    const UINT blurHeight = (std::min)(
        static_cast<UINT>(std::ceil(kDesignHeight * safeScale)), displayHeight - blurTop);
    const bool blurReady = g.softwareTarget
        ? PrepareBackgroundBlur(displayWidth, displayHeight,
                                blurLeft, blurTop, blurWidth, blurHeight)
        : PrepareBackgroundBlur(displayWidth, displayHeight);
    ID2D1Bitmap* backdropBlur = g.softwareTarget ? g.sceneBitmap.Get()
                                                  : g.blurBitmap.Get();
    // Keep one complete D2D preview snapshot through a drag and until Panorama
    // has actually captured the resumed panel at its new position.  Without
    // the serial fence, fresh ESP bounds are drawn over the previous texture
    // for one frame after mouse release.
    const uint64_t captureSerial = GetPanoramaPreviewCaptureSerial();
    if (g.draggingWindow && !g.previewWasDragging) {
        g.previewFreezeAfterDrag = true;
        g.previewFreezeSerial = captureSerial;
    }
    if (!g.draggingWindow && g.previewFreezeAfterDrag &&
        captureSerial > g.previewFreezeSerial) {
        g.previewFreezeAfterDrag = false;
    }
    const bool freezePreview = (g.draggingWindow || g.previewFreezeAfterDrag) &&
                               g.preview3dActive && g.preview3dBitmap;
    g.previewWasDragging = g.draggingWindow;
    if (!freezePreview) {
        g.preview3dActive = false;
    }
    if (!freezePreview && g.tab == 0 && pDevice && pContext) {
        Preview3DFrame previewFrame{};
        SetPanoramaPreviewRole(g.visualTeam);
        ID3D11Texture2D* panoramaTexture = GetPanoramaPreviewTexture();
        if (panoramaTexture) {
            previewFrame.texture = panoramaTexture;
            previewFrame.left = 0.0f;
            previewFrame.top = 0.0f;
            previewFrame.right = 1.0f;
            previewFrame.bottom = 1.0f;
            std::array<Preview3DPoint, 18> skeleton{};
            if (GetPanoramaPreviewSkeleton(skeleton.data(), skeleton.size())) {
                previewFrame.skeleton = skeleton;
                float left = 1.0f, top = 1.0f, right = 0.0f, bottom = 0.0f;
                size_t visibleJoints = 0;
                for (const Preview3DPoint& point : skeleton) {
                    if (!point.visible) continue;
                    left = (std::min)(left, point.x);
                    top = (std::min)(top, point.y);
                    right = (std::max)(right, point.x);
                    bottom = (std::max)(bottom, point.y);
                    ++visibleJoints;
                }
                if (visibleJoints >= 6 && right > left && bottom > top) {
                    const float padX = (std::max)(0.035f, (right - left) * 0.16f);
                    const float padTop = (std::max)(0.035f, (bottom - top) * 0.08f);
                    const float padBottom = (std::max)(0.030f, (bottom - top) * 0.05f);
                    previewFrame.left = std::clamp(left - padX, 0.0f, 1.0f);
                    previewFrame.top = std::clamp(top - padTop, 0.0f, 1.0f);
                    previewFrame.right = std::clamp(right + padX, 0.0f, 1.0f);
                    previewFrame.bottom = std::clamp(bottom + padBottom, 0.0f, 1.0f);
                }
            }
            g.preview3dActive = BindPreview3DFrame(previewFrame, pContext);
            ReportPanoramaPreviewBinding(g.preview3dActive);
        }
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
    if (!g.softwareTarget && blurReady && backdropBlur) {
        g.target->PushAxisAlignedClip(
            Rect(l.x, l.y, l.x + kDesignWidth * safeScale,
                 l.y + kDesignHeight * safeScale),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        g.target->DrawBitmap(
            backdropBlur, Rect(0, 0, io.DisplaySize.x, io.DisplaySize.y),
            1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.target->PopAxisAlignedClip();
    }
    // Keep the scene visible beneath the menu.  This is intentionally only a
    // light global dimmer; the glass layer below supplies the sidebar tint.
    FillRect(Rect(0, 0, io.DisplaySize.x, io.DisplaySize.y),
             Color(0.008f, 0.010f, 0.016f, 0.10f));
    g.target->SetTransform(D2D1::Matrix3x2F(safeScale, 0, 0, safeScale, l.x, l.y));

    // The preview is deliberately not part of this rectangle: it is a
    // companion window attached to the right edge, like the reference UI.
    const D2D1_RECT_F window = Rect(0, 0, kMainWindowWidth, kDesignHeight);
    GlowRounded(window, 12, Color(0, 0, 0, 0.78f), 10, 3.0f);
    // The opaque base starts after the sidebar.  Leaving the left area clear
    // lets its dedicated blur/tint layer behave like real glass.
    GradientRounded(Rect(298, 0, kMainWindowWidth, kDesignHeight), 0,
                    ThemeSurface(0.010f, 0.050f, 0.985f),
                    ThemeSurface(0.006f, 0.022f, 0.99f), true);
    // A subtle cool sheen keeps the panel translucent without turning it black.
    GradientRounded(Rect(1, 1, kMainWindowWidth - 1, 170), 13,
                    Color(0.20f, 0.24f, 0.31f, 0.055f),
                    Color(0.05f, 0.07f, 0.11f, 0.0f), true);
    StrokeRounded(window, 4, Color(1, 1, 1, 0.08f), 1.0f);

    // Sidebar glass: replay the blurred scene only under the left panel, then
    // tint it lightly instead of covering it with an opaque navigation block.
    if (blurReady && backdropBlur) {
        g.target->SetTransform(D2D1::Matrix3x2F::Identity());
        g.target->PushAxisAlignedClip(
            Rect(l.x, l.y, l.x + 298.0f * safeScale,
                 l.y + kDesignHeight * safeScale),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const D2D1_RECT_F blurDestination = g.softwareTarget
            ? Rect(static_cast<float>(blurLeft), static_cast<float>(blurTop),
                   static_cast<float>(blurLeft + blurWidth),
                   static_cast<float>(blurTop + blurHeight))
            : Rect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
        g.target->DrawBitmap(backdropBlur, blurDestination,
                             0.92f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        g.target->PopAxisAlignedClip();
        g.target->SetTransform(D2D1::Matrix3x2F(safeScale, 0, 0, safeScale, l.x, l.y));
    }
    // In the software renderer there is no clean scene texture to blur.  A
    // low-alpha tint is preferable to blurring a previous UI frame (ghosted
    // labels/icons) and still gives the sidebar a genuine glass appearance.
    FillRect(Rect(0, 0, 298, kDesignHeight), ThemeSurface(0.003f, 0.042f, 0.84f));
    GradientRounded(Rect(1, 1, 297, kDesignHeight - 1), 4,
                    ThemeSurface(0.035f, 0.070f, 0.30f),
                    ThemeSurface(0.002f, 0.018f, 0.17f), true);
    FillRect(Rect(298, 0, kMainWindowWidth, 70), ThemeSurface(0.005f, 0.026f, 0.99f));
    Line(D2D1::Point2F(0, 70), D2D1::Point2F(kMainWindowWidth, 70), Border());
    Line(D2D1::Point2F(298, 0), D2D1::Point2F(298, kDesignHeight), Border());

    DrawLogo();
    Text(L"AXIOM", Rect(104, 9, 285, 57), g.title.Get(), White());
    const D2D1_RECT_F save = Rect(326, 16, 438, 54);
    FillRounded(save, 3, Color(1, 1, 1, 0.025f));
    StrokeRounded(save, 3, Border(), 0.9f);
    Text(L"Save", save, g.centered.Get(), Muted());
    const D2D1_RECT_F profile = Rect(458, 16, 675, 54);
    FillRounded(profile, 3, Color(1, 1, 1, 0.025f));
    StrokeRounded(profile, 3, Border(), 0.9f);
    Text(L"Global", Rect(475, 16, 620, 54), g.regular.Get(), White());
    Line(D2D1::Point2F(648, 31), D2D1::Point2F(654, 37), Muted(), 1.3f);
    Line(D2D1::Point2F(654, 37), D2D1::Point2F(660, 31), Muted(), 1.3f);
    if (Clicked(l, save)) SaveConfig();
    const D2D1_RECT_F gear = Rect(874, 12, 916, 54);
    if (Clicked(l, gear)) g.settingsOpen = !g.settingsOpen;
    const D2D1_COLOR_F gearColor = g.settingsOpen ? Red() : Muted();
    SetBrush(gearColor);
    g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(895, 31), 8, 8), g.brush.Get(), 2.0f);
    for (int i = 0; i < 8; ++i) {
        const float a = i * 0.78539816f;
        Line(D2D1::Point2F(895 + std::cos(a) * 11, 31 + std::sin(a) * 11),
             D2D1::Point2F(895 + std::cos(a) * 15, 31 + std::sin(a) * 15), gearColor, 2.0f);
    }
    // Search icon beside the settings button, matching the compact toolbar.
    SetBrush(Muted());
    g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(947, 29), 6, 6), g.brush.Get(), 1.5f);
    Line(D2D1::Point2F(952, 34), D2D1::Point2F(959, 41), Muted(), 1.5f);
    Line(D2D1::Point2F(964, 24), D2D1::Point2F(976, 36), Muted(), 1.4f);
    Line(D2D1::Point2F(976, 24), D2D1::Point2F(964, 36), Muted(), 1.4f);
    if (Clicked(l, Rect(958, 8, 990, 53))) {
        SaveConfig();
        SetMenuOpen(false);
    }

    Layout settingsLayout = l;
    if (g.settingsOpen) {
        // The settings popup owns the pointer while it is visible; the
        // underlying navigation and controls must not receive this input.
        l.clicked = false;
        l.down = false;
    }

    auto section = [&](const wchar_t* label, float y) {
        Text(label, Rect(28, y, 260, y + 18), g.regular.Get(), Muted(0.52f));
    };
    auto nav = [&](const wchar_t* label, float y, int icon, bool selected,
                   auto activate) {
        const D2D1_RECT_F row = Rect(14, y, 284, y + 42);
        if (Clicked(l, row)) { activate(); g.pageAlpha = 0.0f; g.pageShift = 12.0f; g.openCombo = 0; }
        if (selected) {
            FillRounded(row, 3, Color(1, 1, 1, 0.105f));
            SetBrush(Red());
            g.target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(263, y + 21), 3.6f, 3.6f), g.brush.Get());
        } else if (Contains(row, l.mouse)) FillRounded(row, 3, Color(1, 1, 1, 0.028f));
        DrawNavigationIcon(icon, 43, y + 21, selected);
        Text(label, Rect(74, y + 5, 242, y + 36), g.medium.Get(), selected ? White() : Muted());
    };
    section(L"VISUALS", 100);
    nav(L"Enemy", 124, 0, g.tab == 0 && g.visualTeam == 0, [&] { g.tab = 0; g.visualTeam = 0; });
    nav(L"Ally", 168, 1, g.tab == 0 && g.visualTeam == 1, [&] { g.tab = 0; g.visualTeam = 1; });
    nav(L"Creep", 212, 2, g.tab == 0 && g.visualTeam == 2, [&] { g.tab = 0; g.visualTeam = 2; });
    section(L"AIM ASSIST", 278);
    nav(L"Player aim", 302, 3, g.tab == 1 && g.aimSubtab == 0, [&] { g.tab = 1; g.aimSubtab = 0; });
    nav(L"Creep aim", 346, 4, g.tab == 1 && g.aimSubtab != 0, [&] { g.tab = 1; g.aimSubtab = 1; });
    section(L"MISCELLANEOUS", 412);
    nav(L"Misc", 436, 5, g.tab == 2, [&] { g.tab = 2; });

    const float contentX = 349.0f + g.pageShift;
    const wchar_t* pageTitle = g.tab == 0 ? (g.visualTeam == 0 ? L"Enemy" :
                                             g.visualTeam == 1 ? L"Ally" : L"Creep") :
                                g.tab == 1 ? (g.aimSubtab == 0 ? L"Player aim" : L"Creep aim") :
                                             L"Misc";
    Text(pageTitle, Rect(contentX, 83, 700, 113), g.title.Get(),
         Color(White().r, White().g, White().b, g.pageAlpha));

    const float cardTop = 120.0f;

    const bool visualEditor = g.tab == 0;
    // All tabs share one bottom edge. Their top can differ (Aim has subtabs),
    // but the content cards must terminate at the same design-space Y.
    constexpr float contentPanelBottom = kContentPanelBottom;
    const float visualPanelBottom = contentPanelBottom;
    const float visualPanelHeight = visualPanelBottom - cardTop;
    const D2D1_RECT_F cardRect = Rect(334, cardTop,
                                      996.0f,
                                      contentPanelBottom);
    // The main window already supplies the page surface.  Do not add a second
    // dark card behind every settings area: it reads as a large black block.

    // The page title in the header already identifies the active tab.  Do not
    // repeat it as an "Overlay" card title or draw a redundant divider here.
    if (!visualEditor) Line(D2D1::Point2F(655, cardTop),
                            D2D1::Point2F(655, contentPanelBottom - 28), Border());

    const float leftX = visualEditor ? 350.0f : 330.0f;
    const float rightX = visualEditor ? 660.0f : 675.0f;
    const float leftColumnWidth = visualEditor ? 288.0f : 300.0f;
    // Leave a deliberate, consistent breathing margin at the right edge of
    // every page. The left columns keep their existing geometry; right-hand
    // content stops before the page border instead of touching it.
    const float rightColumnWidth = visualEditor ? 276.0f : 270.0f;
    const float leftColorWidth = leftColumnWidth;
    const float rightColorWidth = rightColumnWidth;
    const float columnWidth = leftColumnWidth;
    // The redundant card heading was removed, so the content occupies its
    // former space rather than leaving an empty strip at the top.
    const float firstY = cardTop;

    const float viewportTop = cardTop;
    const bool mouseInColumnViewport =
        l.mouse.y >= viewportTop && l.mouse.y <= contentPanelBottom;
    const float previousLeftMax = ColumnMaxScroll(g.leftContentBottom);
    const float previousRightMax = ColumnMaxScroll(g.rightContentBottom);
    if (mouseInColumnViewport && io.MouseWheel != 0.0f && !g.openCombo) {
        const float scrollStep = 72.0f * io.MouseWheel;
        if (l.mouse.x >= leftX && l.mouse.x < rightX)
            g.leftColumnScroll = std::clamp(g.leftColumnScroll + scrollStep,
                                            -previousLeftMax, 0.0f);
        else if (l.mouse.x >= rightX && l.mouse.x <= 996.0f)
            g.rightColumnScroll = std::clamp(g.rightColumnScroll + scrollStep,
                                             -previousRightMax, 0.0f);
    }
    if (!l.down) g.activeScrollColumn = 0;
    g.leftContentBottom = viewportTop;
    g.rightContentBottom = viewportTop;

    g.target->PushAxisAlignedClip(
        // Aim/Misc controls begin at leftX (330), while the card starts at
        // 334.  Clipping from the card edge cropped the first pixels of their
        // labels and combo boxes.
        Rect(visualEditor ? 334.0f : leftX, visualEditor ? 0.0f : viewportTop,
             visualEditor ? 1436.0f : 996.0f,
             visualEditor ? kDesignHeight : contentPanelBottom),
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
        bool* teamAbilities = g.visualTeam == 0 ? &enemyAbilitiesEnabled :
                              g.visualTeam == 1 ? &allyAbilitiesEnabled : &creepAbilitiesEnabled;
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
        float* teamMaxDistance = g.visualTeam == 0 ? &enemyEspMaxDistance :
                                 g.visualTeam == 1 ? &allyEspMaxDistance : &creepEspMaxDistance;
        float* teamBoxThickness = &boxThickness;
        float* teamCornerLength = &cornerBoxLength;
        const int previewHeroIndex = g.visualTeam < 2
            ? PreviewHeroIndex(GetPanoramaPreviewHeroForRole(g.visualTeam))
            : -1;
        const wchar_t* previewHeroName = previewHeroIndex >= 0
            ? kPreviewHeroNames[previewHeroIndex] : L"Creep";
        EnsurePreviewAbilityAssets(previewHeroIndex);
        DrawHeroEspPreview(1020.0f, 0.0f, 410.0f, kDesignHeight,
                           g.visualTeam == 0 ? L"Enemy preset" :
                           g.visualTeam == 1 ? L"Ally preset" : L"Creep preset",
                           previewHeroName,
                           *teamEsp,
                           *teamBoxes, *teamCornerBoxes, *teamBones,
                           *teamHealth, *teamHealthValues, *teamNames,
                           *teamPlayerNames, *teamDistance, *teamSnaplines, *teamAbilities,
                           teamBoxColor, teamSkeletonColor, teamHealthColor,
                           teamNameColor, teamPlayerColor, teamHealthValueColor,
                           *teamBoxThickness, *teamCornerLength, l,
                           teamEsp, teamBoxes, teamCornerBoxes, teamBones,
                           teamHealth, teamHealthValues, teamNames, teamPlayerNames,
                           teamDistance, teamSnaplines, teamAbilities);

        // Boolean ESP controls live in the preview companion window. Keep the
        // old in-card layout disabled so controls cannot be duplicated.
        if (false) { if (g.visualTeam == 2) {
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
            DrawSectionHeading(leftX, firstY + 10, leftColumnWidth, L"Main");
            DrawSectionHeading(rightX, firstY + 10, rightColumnWidth, L"Details");
            DrawEspChip(l, leftX, firstY + 50, leftColumnWidth, L"Enable ESP", teamEsp);
            DrawEspChip(l, rightX, firstY + 50, rightColumnWidth, L"Model glow",
                        teamGlowEnabled, teamGlowColor);
            DrawEspChip(l, leftX, firstY + 102, leftColumnWidth, L"Box",
                        teamBoxes, teamBoxColor);
            DrawEspChip(l, rightX, firstY + 102, rightColumnWidth, L"Corner box",
                        teamCornerBoxes);
            DrawEspChip(l, leftX, firstY + 154, leftColumnWidth, L"Health bar",
                        teamHealth, teamHealthColor);
            DrawEspChip(l, rightX, firstY + 154, rightColumnWidth, L"Health value",
                        teamHealthValues, teamHealthValueColor);
            DrawEspChip(l, leftX, firstY + 206, leftColumnWidth, L"Skeleton",
                        teamBones, teamSkeletonColor);
            DrawEspChip(l, rightX, firstY + 206, rightColumnWidth, L"Hero name",
                        teamNames, teamNameColor);
            DrawEspChip(l, leftX, firstY + 258, leftColumnWidth, L"Player name",
                        teamPlayerNames, teamPlayerColor);
            DrawEspChip(l, rightX, firstY + 258, rightColumnWidth, L"Distance",
                        teamDistance);
            DrawEspChip(l, leftX, firstY + 310, leftColumnWidth, L"Snapline",
                        teamSnaplines);
            DrawSectionHeading(leftX, firstY + 372,
                               rightX + rightColumnWidth - leftX, L"Appearance");
            DrawSlider(l, leftX, firstY + 418,
                       rightX + rightColumnWidth - leftX, L"Box thickness",
                       teamBoxThickness, 0.5f, 4.0f, L"%.2f px");
            DrawSlider(l, leftX, firstY + 470,
                       rightX + rightColumnWidth - leftX, L"Corner length",
                       teamCornerLength, 0.10f, 0.35f, L"%.2f");
            const wchar_t* glowModes[] = {L"HP-based fill", L"Normal fill"};
            int* teamGlowMode = g.visualTeam == 0 ? &enemyGlowMode : &allyGlowMode;
            DrawCombo(l, 401, leftX, firstY + 530,
                      rightX + rightColumnWidth - leftX, L"Glow mode",
                      teamGlowMode, glowModes, 2);
        }
        } else {
            const float fullWidth = rightX + rightColumnWidth - leftX;
            if (g.visualTeam < 2) {
                DrawSectionHeading(leftX, firstY + 18, fullWidth, L"General");
                int previewHero = PreviewHeroIndex(
                    GetPanoramaPreviewHeroForRole(g.visualTeam));
                DrawHeroCombo(l, g.visualTeam == 0 ? 501 : 502,
                              leftX, firstY + 74, fullWidth,
                              L"Preview hero", &previewHero);
                DrawToggle(l, leftX, firstY + 118, fullWidth,
                           L"Enable ESP", L"", teamEsp);
                DrawSlider(l, leftX, firstY + 188, fullWidth,
                           L"Max render distance", teamMaxDistance,
                           10.0f, 500.0f, L"%.0f m");
                DrawSectionHeading(leftX, firstY + 270, fullWidth, L"Appearance");
                DrawToggle(l, leftX, firstY + 310, fullWidth,
                           L"Model glow", L"", teamGlowEnabled);
                const wchar_t* glowModes[] = {L"HP-based fill", L"Normal fill"};
                int* teamGlowMode = g.visualTeam == 0 ? &enemyGlowMode : &allyGlowMode;
                DrawCombo(l, 401, leftX, firstY + 380, fullWidth, L"Glow mode",
                          teamGlowMode, glowModes, 2);
                DrawSlider(l, leftX, firstY + 450, fullWidth, L"Box thickness",
                           teamBoxThickness, 0.5f, 4.0f, L"%.2f px");
                DrawSlider(l, leftX, firstY + 520, fullWidth, L"Corner length",
                           teamCornerLength, 0.10f, 0.35f, L"%.2f");
            } else {
                DrawSectionHeading(leftX, firstY + 18, fullWidth, L"General");
                DrawToggle(l, leftX, firstY + 58, fullWidth,
                           L"Enable ESP", L"", teamEsp);
                DrawSlider(l, leftX, firstY + 128, fullWidth,
                           L"Max render distance", teamMaxDistance,
                           10.0f, 500.0f, L"%.0f m");
                DrawSectionHeading(leftX, firstY + 210, fullWidth, L"Appearance");
                DrawSlider(l, leftX, firstY + 250, fullWidth, L"Box thickness",
                           teamBoxThickness, 0.5f, 4.0f, L"%.2f px");
                DrawSlider(l, leftX, firstY + 320, fullWidth, L"Corner length",
                           teamCornerLength, 0.10f, 0.35f, L"%.2f");
            }
        }
    } else if (g.tab == 1) {
        if (g.aimSubtab == 0) {
        DrawToggle(l, leftX, firstY, columnWidth, L"Aim assist",
                   L"Enable player targeting", &aimAssist);
        if (aimAssist) {
        // Keep the key capture in the left column.  The old inline placement
        // crossed the column divider and overpainted the Target point combo.
        DrawKeyBind(l, leftX, firstY + 56, columnWidth,
                    aimKeyCapture, aimAssistKey, &aimKeyCapture);
        DrawToggle(l, leftX, firstY + 110, columnWidth, L"Visibility check",
                   L"Ignore occluded targets", &aimVisibilityCheck);
        int aimMode = aimMixedMode ? 2 : (aimSilentMode ? 1 : 0);
        const wchar_t* aimModes[] = {L"Normal", L"pSilent", L"Mixed"};
        DrawCombo(l, 101, leftX, firstY + 166, columnWidth, L"Aim mode",
                  &aimMode, aimModes, 3);
        aimSilentMode = aimMode == 1;
        aimMixedMode = aimMode == 2;
        int bindMode = aimToggleMode ? 1 : 0;
        const wchar_t* bindModes[] = {L"Hold", L"Toggle"};
        DrawCombo(l, 102, leftX, firstY + 239, columnWidth, L"Activation",
                  &bindMode, bindModes, 2);
        aimToggleMode = bindMode == 1;
        int targetMode = static_cast<int>(aimTargetMode);
        const wchar_t* targets[] = {L"Head", L"Body", L"Closest"};
        DrawCombo(l, 103, rightX, firstY, rightColumnWidth, L"Target point",
                  &targetMode, targets, 3);
        aimTargetMode = static_cast<AimTargetMode>(std::clamp(targetMode, 0, 2));
        int selectionMode = static_cast<int>(aimSelectionMode);
        const wchar_t* selections[] = {L"Crosshair", L"Distance", L"Health"};
        DrawCombo(l, 104, rightX, firstY + 73, rightColumnWidth, L"Target selection",
                  &selectionMode, selections, 3);
        aimSelectionMode = static_cast<AimSelectionMode>(std::clamp(selectionMode, 0, 2));
        const float previousAimFov = aimFov;
        const float previousPitchSmooth = aimPitchSmooth;
        const float previousYawSmooth = aimYawSmooth;
        DrawSlider(l, rightX, firstY + 146, rightColumnWidth, L"Aim FOV",
                   &aimFov, 40.0f, 600.0f, L"%.0f px");
        DrawSlider(l, rightX, firstY + 224, rightColumnWidth, L"Pitch smoothing",
                   &aimPitchSmooth, 1.0f, 20.0f, L"%.1f");
        DrawSlider(l, rightX, firstY + 302, rightColumnWidth, L"Yaw smoothing",
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
        DrawSectionHeading(leftX, firstY + 322, columnWidth, L"Accuracy");
        DrawSlider(l, leftX, firstY + 363, columnWidth, L"Hitchance",
                   &aimHitchance, 0.0f, 100.0f, L"%.0f%%");
        DrawToggle(l, leftX, firstY + 441, columnWidth, L"Backtrack",
                   L"Aim at a recent target position", &aimBacktrack);
        if (aimBacktrack)
            DrawSlider(l, leftX, firstY + 497, columnWidth, L"Backtrack time",
                       &aimBacktrackMs, 1.0f, 1000.0f, L"%.0f ms");
        DrawSectionHeading(rightX, firstY + 390, rightColumnWidth, L"Behavior");
        DrawToggle(l, rightX, firstY + 431, rightColumnWidth, L"Only Yaw",
                   L"Adjust horizontal aim only", &aimOnlyYaw);
        DrawToggle(l, rightX, firstY + 485, rightColumnWidth, L"Lock Target",
                   L"Keep the current target while valid", &aimLockTarget);
        DrawToggle(l, rightX, firstY + 539, rightColumnWidth, L"Draw FOV circle",
                   L"Show active target radius", &drawFovCircle);
        DrawSlider(l, rightX, firstY + 593, rightColumnWidth, L"FOV opacity",
                   &fovCircleAlpha, 0.0f, 255.0f, L"%.0f");
        }
        } else if (g.aimSubtab == 99) {
            DrawToggle(l, leftX, firstY, columnWidth, L"Creep aim",
                       L"Enable creep targeting", &farmAssist);
            if (farmAssist) {
            int farmMode = farmMixedMode ? 2 : (farmSilentMode ? 1 : 0);
            DrawCombo(l, 301, leftX, firstY + 70, columnWidth, L"Farm mode",
                      &farmMode, kFarmModes, 3);
            farmSilentMode = farmMode == 1;
            farmMixedMode = farmMode == 2;
            int farmBind = farmToggleMode ? 1 : 0;
            DrawCombo(l, 302, leftX, firstY + 140, columnWidth, L"Activation",
                      &farmBind, kFarmActivationModes, 2);
            farmToggleMode = farmBind == 1;
            DrawKeyBind(l, leftX, firstY + 210, columnWidth,
                        farmKeyCapture, farmAssistKey, &farmKeyCapture);
             if (!farmSilentMode || farmMixedMode) {
                 DrawSlider(l, leftX, firstY + 280, columnWidth, L"Farm FOV",
                            &farmFov, 40.0f, 600.0f, L"%.0f px");
                 DrawSlider(l, leftX, firstY + 350, columnWidth, L"Smoothing",
                            &farmAimSmooth, 1.0f, 20.0f, L"%.1f");
             }
             const float farmCircleY = (!farmSilentMode || farmMixedMode)
                ? firstY + 420.0f : firstY + 280.0f;
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
                DrawCombo(l, 301, leftX, firstY + 70, columnWidth, L"Farm mode",
                          &farmMode, kFarmModes, 3);
                farmSilentMode = farmMode == 1;
                farmMixedMode = farmMode == 2;
                int farmBind = farmToggleMode ? 1 : 0;
                DrawCombo(l, 302, leftX, firstY + 140, columnWidth, L"Activation",
                          &farmBind, kFarmActivationModes, 2);
                farmToggleMode = farmBind == 1;
                DrawKeyBind(l, leftX, firstY + 210, columnWidth,
                            farmKeyCapture, farmAssistKey, &farmKeyCapture);
                 if (!farmSilentMode || farmMixedMode) {
                     DrawSlider(l, leftX, firstY + 280, columnWidth, L"Farm FOV",
                                &farmFov, 40.0f, 600.0f, L"%.0f px");
                     DrawSlider(l, leftX, firstY + 350, columnWidth, L"Smoothing",
                                &farmAimSmooth, 1.0f, 20.0f, L"%.1f");
                 }
                 const float farmCircleY = (!farmSilentMode || farmMixedMode)
                     ? firstY + 420.0f : firstY + 280.0f;
                 DrawToggle(l, leftX, farmCircleY, columnWidth, L"Farm FOV circle",
                            L"Show the creep aim radius", &drawFarmFovCircle);
                 if (drawFarmFovCircle)
                     DrawSlider(l, leftX, farmCircleY + 72, columnWidth, L"FOV opacity",
                                &farmFovAlpha, 0.0f, 255.0f, L"%.0f");
             }

            DrawToggle(l, rightX, firstY, rightColumnWidth, L"Orb aim",
                       L"Aim at valid soul orbs", &autoLastHitOrbs);
            if (autoLastHitOrbs) {
                int fireMode = autoLastHitOrbsAutoFire ? 0 : 1;
                const wchar_t* fireModes[] = {L"Auto fire", L"Player fire"};
                DrawCombo(l, 203, rightX, firstY + 70, rightColumnWidth, L"Fire mode",
                          &fireMode, fireModes, 2);
                autoLastHitOrbsAutoFire = fireMode == 0;
                int orbBind = autoLastHitOrbsToggleMode ? 1 : 0;
                const wchar_t* binds[] = {L"Hold", L"Toggle"};
                DrawCombo(l, 204, rightX, firstY + 140, rightColumnWidth, L"Activation",
                          &orbBind, binds, 2);
                autoLastHitOrbsToggleMode = orbBind == 1;
                DrawKeyBind(l, rightX, firstY + 210, rightColumnWidth,
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
        DrawCombo(l, 302, leftX, firstY + 230, columnWidth, L"Activation",
                  &farmBind, kFarmActivationModes, 2);
        farmToggleMode = farmBind == 1;
        DrawSlider(l, leftX, firstY + 304, columnWidth, L"Farm FOV",
                   &farmFov, 40.0f, 600.0f, L"%.0f px");
        DrawSlider(l, leftX, firstY + 378, columnWidth, L"Smoothing",
                   &farmAimSmooth, 1.0f, 20.0f, L"%.1f");
        Text(L"Farm key", Rect(leftX, firstY + 452, leftX + columnWidth, firstY + 480),
             g.regular.Get(), Muted());
        DrawKeyBind(l, leftX, firstY + 486, columnWidth,
                    farmKeyCapture, farmAssistKey, &farmKeyCapture);

        DrawToggle(l, rightX, firstY, rightColumnWidth, L"Orb ESP",
                   L"Highlight active soul orbs", &drawOrbEsp);
        DrawToggle(l, rightX, firstY + 72, rightColumnWidth, L"Orb aim",
                   L"Aim at valid soul orbs", &autoLastHitOrbs);
        DrawToggle(l, rightX, firstY + 144, rightColumnWidth, L"Visibility check",
                   L"Ignore occluded orbs", &orbAimVisibilityCheck);
        int fireMode = autoLastHitOrbsAutoFire ? 0 : 1;
        const wchar_t* fireModes[] = {L"Auto fire", L"Player fire"};
        DrawCombo(l, 203, rightX, firstY + 226, rightColumnWidth, L"Fire mode",
                  &fireMode, fireModes, 2);
        autoLastHitOrbsAutoFire = fireMode == 0;
        int orbBind = autoLastHitOrbsToggleMode ? 1 : 0;
        DrawCombo(l, 204, rightX, firstY + 302, rightColumnWidth, L"Activation",
                  &orbBind, kFarmActivationModes, 2);
        autoLastHitOrbsToggleMode = orbBind == 1;
        Text(L"Orb key", Rect(rightX, firstY + 376, rightX + rightColumnWidth, firstY + 404),
             g.regular.Get(), Muted());
        DrawKeyBind(l, rightX, firstY + 410, rightColumnWidth,
                    autoLastHitOrbsKeyCapture, autoLastHitOrbsKey,
                    &autoLastHitOrbsKeyCapture);
    } else {
        DrawToggle(l, leftX, firstY, columnWidth, L"Auto parry",
                   L"Automatically use parry", &autoParry);
        DrawToggle(l, leftX, firstY + 54, columnWidth, L"Spectator list",
                   L"Show current observers", &drawSpectatorList);
        DrawToggle(l, leftX, firstY + 108, columnWidth, L"Free camera",
                   L"Detach camera from player", &freeCam);
        if (freeCam) {
            DrawKeyBind(l, leftX, firstY + 162, columnWidth,
                        freeCamKeyCapture, freeCamKey, &freeCamKeyCapture);
            DrawSlider(l, leftX, firstY + 214, columnWidth, L"Freecam speed",
                       &freeCamSpeed, 50.0f, 5000.0f, L"%.0f u/s");
        }

        DrawToggle(l, rightX, firstY, rightColumnWidth, L"FOV Changer",
                   L"Override the normal camera field of view", &fovChangerEnabled);
        DrawSlider(l, rightX, firstY + 54, rightColumnWidth, L"Camera FOV",
                   &cameraFov, 60.0f, 140.0f, L"%.0f deg");
        DrawToggle(l, rightX, firstY + 118, rightColumnWidth, L"Override Scope FOV",
                   L"Use a separate field of view while scoped", &overrideScopeFov);
        DrawSlider(l, rightX, firstY + 172, rightColumnWidth, L"Scoped FOV",
                   &scopedCameraFov, 20.0f, 140.0f, L"%.0f deg");

        DrawSectionHeading(rightX, firstY + 246, rightColumnWidth, L"Session");
        wchar_t status[80]{};
        std::swprintf(status, 80, L"%.0f FPS    %zu players", io.Framerate, playerCount);
        Text(status, Rect(rightX + 10, firstY + 286,
             rightX + rightColumnWidth, firstY + 321),
             g.regular.Get(), Muted());
        Text(L"Unload the module and restore all hooks safely.",
             Rect(rightX + 10, firstY + 334, rightX + rightColumnWidth, firstY + 374),
             g.regular.Get(), Muted());
        const D2D1_RECT_F unload = Rect(rightX, firstY + 392,
                                       rightX + rightColumnWidth, firstY + 436);
        GradientRounded(unload, 7, Color(1.0f, 0.10f, 0.19f), Color(0.60f, 0.01f, 0.06f), true);
        InnerGlow(unload, 7);
        Text(L"Unload DLL", unload, g.centered.Get(), White());
        if (Clicked(l, unload)) RequestUnload();
    }

    const float leftMax = ColumnMaxScroll(g.leftContentBottom);
    const float rightMax = ColumnMaxScroll(g.rightContentBottom);
    g.leftColumnScroll = std::clamp(g.leftColumnScroll, -leftMax, 0.0f);
    g.rightColumnScroll = std::clamp(g.rightColumnScroll, -rightMax, 0.0f);
    DrawColumnScrollbar(l, 642.0f, viewportTop, g.leftContentBottom,
                        g.leftColumnScroll);
    DrawColumnScrollbar(l, visualEditor ? 1422.0f : 984.0f,
                        viewportTop, g.rightContentBottom,
                        g.rightColumnScroll);
    g.target->PopAxisAlignedClip();
    DrawPopup(l);
    DrawMenuSettingsPanel(settingsLayout);
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
    } else if (popupSelectionId == 401) {
        if (g.visualTeam == 0) enemyGlowMode = popupSelectionValue;
        else if (g.visualTeam == 1) allyGlowMode = popupSelectionValue;
    } else if (popupSelectionId == 501) {
        SetPanoramaPreviewHeroForRole(
            0, kPreviewHeroIds[std::clamp(
                   popupSelectionValue, 0,
                   static_cast<int>(std::size(kPreviewHeroIds)) - 1)]);
    } else if (popupSelectionId == 502) {
        SetPanoramaPreviewHeroForRole(
            1, kPreviewHeroIds[std::clamp(
                   popupSelectionValue, 0,
                   static_cast<int>(std::size(kPreviewHeroIds)) - 1)]);
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

bool GetD2DPreviewCaptureRect(float& left, float& top,
                              float& right, float& bottom) {
    if (!menuOpen || !g.ready || g.tab != 0 || g.visualTeam >= 2 ||
        !g.positionInitialized || g.menuAlpha <= 0.01f ||
        g.draggingWindow) return false;
    ImGuiIO& io = ImGui::GetIO();
    const float scale = (std::min)(io.DisplaySize.x * 0.925f / kDesignWidth,
                                   io.DisplaySize.y * 0.958f / kDesignHeight) * 0.54f;
    const float safeScale = (std::max)(0.32f, scale);
    constexpr float cardTop = 0.0f;
    constexpr float x = 1020.0f;
    // Keep Panorama's physical capture exactly aligned with DrawHeroEspPreview.
    // The old height - 260 value was from the pre-flow tile layout and made
    // the captured animation stretch vertically into the larger stage.
    constexpr float tileRows = 3.0f;
    constexpr float tileHeight = 36.0f;
    constexpr float tileGap = 8.0f;
    constexpr float bottomPadding = 16.0f;
    constexpr float captureBottom = kDesignHeight -
        (tileRows * tileHeight + (tileRows - 1.0f) * tileGap + bottomPadding) - 16.0f;
    // This function is called before RenderD2DMenu. Predict the active drag
    // position here so Panorama never receives the previous frame's window
    // coordinates while the user moves the menu.
    float windowX = g.windowX;
    float windowY = g.windowY;
    if (g.draggingWindow && io.MouseDown[0]) {
        const float windowWidth = kDesignWidth * safeScale;
        const float visibleEdge = 70.0f * safeScale;
        windowX = std::clamp(io.MousePos.x - g.dragGrabX,
                             -windowWidth + visibleEdge,
                             io.DisplaySize.x - visibleEdge);
        windowY = std::clamp(io.MousePos.y - g.dragGrabY, 0.0f,
                             (std::max)(0.0f, io.DisplaySize.y - visibleEdge));
    }
    left = std::floor(windowX + (x + 15.0f) * safeScale);
    top = std::floor(windowY + (cardTop + 74.0f) * safeScale);
    right = std::floor(windowX + (x + 410.0f - 15.0f) * safeScale);
    bottom = std::floor(windowY + captureBottom * safeScale);
    return right > left && bottom > top;
}

void DrawMenuSettingsPanel(const Layout& l) {
    if (!g.settingsOpen) return;
    const D2D1_RECT_F settings = Rect(620, 68, 950, 412);
    GlowRounded(settings, 9, Color(0, 0, 0, 0.82f), 8, 2.0f);
    FillRounded(settings, 9, ThemeSurface(0.010f, 0.040f, 1.0f));
    StrokeRounded(settings, 9, Border(), 1.0f);
    Text(L"Menu settings", Rect(640, 83, 900, 113), g.semibold.Get(), White());
    Text(L"Theme palette", Rect(640, 116, 790, 140), g.regular.Get(), Muted());

    float hue{}, saturation{}, value{};
    RGBtoHSV(menuAccentColor[0], menuAccentColor[1], menuAccentColor[2],
             hue, saturation, value);
    const D2D1_RECT_F palette = Rect(640, 146, 840, 346);
    const D2D1_RECT_F saturationBar = Rect(854, 146, 878, 346);
    constexpr int paletteSteps = 64;
    for (int py = 0; py < paletteSteps; ++py) {
        const float brightness = 1.0f - static_cast<float>(py) / (paletteSteps - 1);
        for (int px = 0; px < paletteSteps; ++px) {
            const float colorHue = static_cast<float>(px) / (paletteSteps - 1);
            float r{}, g{}, b{};
            HSVtoRGB(colorHue, saturation, brightness, r, g, b);
            const float x0 = palette.left + px * (palette.right - palette.left) / paletteSteps;
            const float y0 = palette.top + py * (palette.bottom - palette.top) / paletteSteps;
            const float x1 = palette.left + (px + 1) * (palette.right - palette.left) / paletteSteps;
            const float y1 = palette.top + (py + 1) * (palette.bottom - palette.top) / paletteSteps;
            // Overlap cells slightly to prevent dark antialiasing seams.
            FillRect(Rect(x0 - 0.25f, y0 - 0.25f, x1 + 0.75f, y1 + 0.75f),
                     Color(r, g, b));
        }
    }
    for (int i = 0; i < paletteSteps; ++i) {
        float r{}, g{}, b{};
        const float sat = 1.0f - static_cast<float>(i) / (paletteSteps - 1);
        HSVtoRGB(hue, sat, value, r, g, b);
        const float y0 = saturationBar.top + i * (saturationBar.bottom - saturationBar.top) / paletteSteps;
        const float y1 = saturationBar.top + (i + 1) * (saturationBar.bottom - saturationBar.top) / paletteSteps;
        FillRect(Rect(saturationBar.left - 0.25f, y0 - 0.25f,
                      saturationBar.right + 0.25f, y1 + 0.75f), Color(r, g, b));
    }
    StrokeRounded(palette, 4, Border(), 1.0f);
    StrokeRounded(saturationBar, 4, Border(), 1.0f);
    const float markerX = palette.left + hue * (palette.right - palette.left);
    const float markerY = palette.top + (1.0f - value) * (palette.bottom - palette.top);
    SetBrush(White());
    g.target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(markerX, markerY), 6, 6),
                          g.brush.Get(), 1.5f);
    const float saturationY = saturationBar.top + (1.0f - saturation) *
                              (saturationBar.bottom - saturationBar.top);
    Line(D2D1::Point2F(saturationBar.left - 3, saturationY),
         D2D1::Point2F(saturationBar.right + 3, saturationY), White(), 2.0f);
    bool changed = false;
    if (l.down && Contains(palette, l.mouse)) {
        hue = std::clamp((l.mouse.x - palette.left) / (palette.right - palette.left), 0.0f, 1.0f);
        value = std::clamp(1.0f - (l.mouse.y - palette.top) / (palette.bottom - palette.top), 0.0f, 1.0f);
        changed = true;
    } else if (l.down && Contains(saturationBar, l.mouse)) {
        saturation = std::clamp(1.0f - (l.mouse.y - saturationBar.top) /
                                (saturationBar.bottom - saturationBar.top), 0.0f, 1.0f);
        changed = true;
    }
    if (changed) HSVtoRGB(hue, saturation, value,
                          menuAccentColor[0], menuAccentColor[1], menuAccentColor[2]);
    wchar_t hex[16]{};
    std::swprintf(hex, 16, L"#%02X%02X%02X",
                  static_cast<int>(menuAccentColor[0] * 255.0f),
                  static_cast<int>(menuAccentColor[1] * 255.0f),
                  static_cast<int>(menuAccentColor[2] * 255.0f));
    FillRounded(Rect(640, 362, 878, 392), 3, Color(1, 1, 1, 0.035f));
    StrokeRounded(Rect(640, 362, 878, 392), 3, Border(), 0.8f);
    Text(hex, Rect(640, 362, 878, 392), g.centered.Get(), White());
    static bool themeDirty = false;
    themeDirty = themeDirty || changed;
    if (themeDirty && !l.down) { SaveConfig(); themeDirty = false; }
}

void ShutdownD2DMenu() {
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
