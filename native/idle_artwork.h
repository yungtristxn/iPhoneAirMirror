#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwchar>
#include "ui_icons.h"

namespace idle_colors {
constexpr UINT background=0x080808,primary=0xe6e6e6,secondary=0xa3a3a3,symbol=0x8c8c8c;
}

// A quiet waiting state, not an onboarding page. The mirrored image always
// replaces it completely. There is no animation timer or idle polling.
inline HRESULT draw_idle_artwork(ID2D1RenderTarget* canvas,IDWriteFactory* writing,
    D2D1_RECT_F viewport,float corner,float scale,int state) {
    using Microsoft::WRL::ComPtr;
    const float w=viewport.right+viewport.left,h=viewport.bottom+viewport.top;
    const bool narrow=w<240*scale,tiny=w<160*scale,short_window=h<260*scale;
    const bool connecting=state==1;
    ComPtr<ID2D1SolidColorBrush> brush;
    HRESULT hr=canvas->CreateSolidColorBrush(D2D1::ColorF(idle_colors::background),&brush);
    if(FAILED(hr))return hr;
    canvas->FillRoundedRectangle(D2D1::RoundedRect(viewport,corner,corner),brush.Get());
    canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const float cx=w*.5f,cy=h*.5f;
    if(!short_window){
        ComPtr<IDWriteTextFormat> symbol;
        hr=create_ui_icon_format(writing,tiny?16*scale:narrow?24*scale:32*scale,&symbol);
        if(FAILED(hr))return hr;
        const wchar_t connect=L'\xe703';
        brush->SetColor(D2D1::ColorF(idle_colors::symbol));
        canvas->DrawText(&connect,1,symbol.Get(),{cx-24*scale,cy-88*scale,cx+24*scale,cy-40*scale},brush.Get());
    }
    const float pad=(tiny?12.0f:18.0f)*scale;
    const float width=std::min(280*scale,std::max(1.0f,w-2*pad));
    const float left=cx-width*.5f,right=cx+width*.5f;
    auto label=[&](const wchar_t* value,float size,float line_height,float baseline,
                   float top,float bottom,UINT color,DWRITE_FONT_WEIGHT weight)->HRESULT{
        ComPtr<IDWriteTextFormat> format;
        HRESULT result=writing->CreateTextFormat(L"Segoe UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,size*scale,L"en-us",&format);
        if(FAILED(result))return result;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,line_height*scale,baseline*scale);
        brush->SetColor(D2D1::ColorF(color));
        canvas->DrawText(value,UINT32(wcslen(value)),format.Get(),{left,top,right,bottom},
            brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return S_OK;
    };
    const wchar_t* title=connecting?L"Connecting…":tiny?L"Ready":narrow?L"Ready to\nmirror":L"Ready to mirror";
    const float title_top=cy-(short_window?40:24)*scale;
    const float title_bottom=title_top+(narrow&&!tiny?56:28)*scale;
    hr=label(title,tiny?16.0f:20.0f,28,21,title_top,title_bottom,idle_colors::primary,DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if(FAILED(hr))return hr;
    const wchar_t* hint=connecting?L"Your screen will appear here.":tiny?L"Connect\nfrom iPhone":
        narrow?L"Screen Mirroring\niPhone Air Mirror":L"Open Screen Mirroring\nand choose iPhone Air Mirror.";
    const float hint_top=title_bottom+12*scale;
    return label(hint,tiny?12.0f:14.0f,20,15,hint_top,hint_top+44*scale,idle_colors::secondary,DWRITE_FONT_WEIGHT_NORMAL);
}
