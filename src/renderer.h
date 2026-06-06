// SPDX-License-Identifier: GPL-2.0
// renderer.h - Direct2D + DirectWrite 渲染引擎
// 完全透明背景: WIC + UpdateLayeredWindow + 逐字高亮
#pragma once

#include "lyrics_data.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace moekoe {

struct RendererSettings {
    std::string highlightColor{"#FFFFFF"};
    std::string normalColor{"#999999"};
    float       normalOpacity{0.85f};
    std::string fontFamily{"Microsoft YaHei UI"};
    int         fontSize{14};
    bool        enableKaraoke{true};
    bool        enableTranslation{true};
};

class TaskbarRenderer {
public:
    TaskbarRenderer();
    ~TaskbarRenderer();

    TaskbarRenderer(const TaskbarRenderer&) = delete;
    TaskbarRenderer& operator=(const TaskbarRenderer&) = delete;

    bool Initialize(HWND hwnd);
    void Shutdown();
    void ApplySettings(const RendererSettings& s);
    void Render(const RenderState& state);
    void Resize(UINT width, UINT height, UINT dpi);

private:
    void CreateRenderTarget();
    void DrawHighlightedTextPerCharacter(const std::wstring& text,
                                          double progress,
                                          bool enableKaraoke);
    void DrawTranslatedText(const std::wstring& text);
    void DrawCentered(const std::wstring& text, ID2D1Brush* brush, float yOffset);
    void PresentToLayeredWindow();

    static D2D1_COLOR_F ParseColor(const std::string& hex, float alpha = 1.0f);

    HWND hwnd_{nullptr};
    UINT width_{0};
    UINT height_{0};
    UINT dpi_{96};
    bool initialized_{false};

    Microsoft::WRL::ComPtr<ID2D1Factory>              d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget>        renderTarget_;
    Microsoft::WRL::ComPtr<IWICImagingFactory>        wicFactory_;
    Microsoft::WRL::ComPtr<IWICBitmap>                wicBitmap_;

    Microsoft::WRL::ComPtr<IDWriteFactory>       dwriteFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    textFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>    translationFormat_;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> highlightBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> normalBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> translationBrush_;

    RenderState lastState_;

    RendererSettings settings_;
};

} // namespace moekoe
