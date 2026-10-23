#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include "ui_icons.h"
#include "control_layout.h"
using Microsoft::WRL::ComPtr;

struct ControlArtwork {
    float scale=1,rail_x=0,flyout_x=0,volume=1,arrow_x=0;
    int width=0,height=0,hot=-1,pressed=-1,focus=-1;
    bool popup=false,fullscreen=false,muted=false,collapsed=false,arrow_left=false;
    bool operator==(const ControlArtwork& other) const {
        return scale==other.scale&&rail_x==other.rail_x&&flyout_x==other.flyout_x&&volume==other.volume&&arrow_x==other.arrow_x
            &&width==other.width&&height==other.height&&hot==other.hot&&pressed==other.pressed&&focus==other.focus
            &&popup==other.popup&&fullscreen==other.fullscreen&&muted==other.muted&&collapsed==other.collapsed&&arrow_left==other.arrow_left;
    }
};

// Only the small control surface is CPU-backed. Video remains on the GPU.
// Standard Windows glyphs use recommended 20/16-DIP sizes and one common grid.
class ControlPainter {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> canvas;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> writing;
    ComPtr<IDWriteTextFormat> text,icons,chevrons;
    static constexpr UINT INK=0x303640,SECONDARY=0x626b78,ACCENT=0x2868d8,SURFACE=0xf6f7f9;

    void color(UINT rgb,float alpha=1) { brush->SetColor(D2D1::ColorF(rgb,alpha)); }
    void panel(D2D1_RECT_F rect,float radius,float scale) {
        for(int i=4;i>0;--i){
            color(0x111820,.035f);
            canvas->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(rect.left-i*.8f,rect.top-i*.4f+1.5f,rect.right+i*.8f,rect.bottom+i*.8f+1.5f),
                radius+i*.8f,radius+i*.8f),brush.Get());
        }
        color(SURFACE);canvas->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),brush.Get());
        const float inset=.5f/scale;
        color(0xffffff,.95f);
        canvas->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(rect.left+inset,rect.top+inset,rect.right-inset,rect.bottom-inset),
            radius-inset,radius-inset),brush.Get(),1/scale);
    }
    void icon(int index,float x,float y,bool muted,bool fullscreen) {
        const wchar_t glyph=index==0?(muted?L'\xe74f':L'\xe767'):index==1?L'\xe9a6':
            index==2?(fullscreen?L'\xe73f':L'\xe740'):L'\xe711';
        canvas->DrawText(&glyph,1,icons.Get(),D2D1::RectF(x-12,y-12,x+12,y+12),brush.Get());
    }

public:
    HRESULT draw(HDC dc,const ControlArtwork& a) {
#define PAINT_TRY(call) do{HRESULT result=(call);if(FAILED(result))return result;}while(0)
        if(!factory)PAINT_TRY(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));
        if(!writing)PAINT_TRY(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writing.GetAddressOf())));
        if(!text){
            PAINT_TRY(writing->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_MEDIUM,
                DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12,L"en-us",&text));
            text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if(!icons)PAINT_TRY(create_ui_icon_format(writing.Get(),20,&icons));
        if(!chevrons)PAINT_TRY(create_ui_icon_format(writing.Get(),16,&chevrons));
        if(!canvas){
            auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            PAINT_TRY(factory->CreateDCRenderTarget(&properties,&canvas));
        }
        RECT bounds{0,0,a.width,a.height};PAINT_TRY(canvas->BindDC(dc,&bounds));
        if(!brush)PAINT_TRY(canvas->CreateSolidColorBrush(D2D1::ColorF(0xffffff),&brush));
        canvas->BeginDraw();canvas->SetTransform(D2D1::Matrix3x2F::Scale(a.scale,a.scale));
        canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        canvas->Clear(D2D1::ColorF(0,0));
        if(a.collapsed){
            // Invisible hover target is retained after the rail collapses.
            color(0,1.0f/255);canvas->FillRectangle(D2D1::RectF(a.arrow_x-12,10,a.arrow_x+12,206),brush.Get());
        }else{
            if(a.popup){
                color(0,1.0f/255);
                canvas->FillRectangle(D2D1::RectF(std::min(a.rail_x+control_layout::row_x,a.flyout_x+106),18,std::max(a.rail_x+control_layout::row_x,a.flyout_x+106),76),brush.Get());
                panel(D2D1::RectF(a.flyout_x,10,a.flyout_x+212,86),16,a.scale);
            }
        }
        const bool show_arrow=!a.collapsed||a.hot==4||a.focus==4;
        if(show_arrow){
            // This acts on the whole rail; do not attach it to the sound button.
            // A visible gap keeps it distinct from the volume flyout affordance.
            panel(D2D1::RectF(a.arrow_x-11,float(control_layout::arrow_top+2),
                a.arrow_x+11,float(control_layout::arrow_bottom-2)),10,a.scale);
        }
        if(!a.collapsed){
            panel(D2D1::RectF(a.rail_x+10,10,a.rail_x+66,206),18,a.scale);
            for(int i=0;i<4;++i){
                const float y=float(control_layout::row_center(i));
                const bool selected=i==0&&(a.popup||a.muted||a.volume<=0);
                const auto cell=D2D1::RoundedRect(D2D1::RectF(a.rail_x+control_layout::button_left,y-20,a.rail_x+control_layout::button_right,y+20),12,12);
                if(a.hot==i||a.pressed==i||a.focus==i||selected){
                    color(i==3?0xfbe5e7:selected?0xe3ecfc:a.pressed==i?0xdce1e9:0xe9edf2);
                    canvas->FillRoundedRectangle(cell,brush.Get());
                }
                if(a.focus==i){color(ACCENT);canvas->DrawRoundedRectangle(cell,brush.Get(),1.5f);}
                color(i==3&&(a.hot==3||a.focus==3)?0xb82d40:selected?ACCENT:INK);
                icon(i,a.rail_x+control_layout::row_x,y,a.muted||a.volume<=0,a.fullscreen);
            }
            color(0xd8dde5);canvas->DrawLine(D2D1::Point2F(a.rail_x+24,150.5f),D2D1::Point2F(a.rail_x+52,150.5f),brush.Get(),1/a.scale);
            if(a.popup){
                color(SECONDARY);text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                canvas->DrawText(a.muted?L"Muted":L"Volume",a.muted?5:6,text.Get(),D2D1::RectF(a.flyout_x+20,20,a.flyout_x+140,40),brush.Get());
                wchar_t value[12];swprintf_s(value,L"%d%%",int(std::round(a.volume*100)));
                color(INK);text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                canvas->DrawText(value,UINT32(wcslen(value)),text.Get(),D2D1::RectF(a.flyout_x+150,20,a.flyout_x+192,40),brush.Get());
                const float left=a.flyout_x+24,right=a.flyout_x+188,thumb=left+a.volume*(right-left);
                color(0xd9dfe7);canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,56,right,60),2,2),brush.Get());
                color(a.muted?0x8d99a8:ACCENT);
                if(thumb>left)canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,56,thumb,60),2,2),brush.Get());
                color(0x19283c,.12f);canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb,59),8.5f,8.5f),brush.Get());
                color(0xffffff);canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb,58),8,8),brush.Get());
                color(a.muted?0x8d99a8:ACCENT);canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb,58),3,3),brush.Get());
                if(a.focus==5){color(ACCENT);canvas->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left-14,42,right+14,74),8,8),brush.Get(),1.5f);}
            }
        }
        if(show_arrow){
            const float y=float(control_layout::arrow_y);
            if(a.focus==4){color(ACCENT);canvas->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a.arrow_x-10,y-16,a.arrow_x+10,y+16),9,9),brush.Get(),1.5f);}
            color(a.hot==4||a.focus==4?ACCENT:SECONDARY);
            const wchar_t glyph=a.arrow_left?L'\xe76b':L'\xe76c';
            canvas->DrawText(&glyph,1,chevrons.Get(),D2D1::RectF(a.arrow_x-10,y-10,a.arrow_x+10,y+10),brush.Get());
        }
        HRESULT result=canvas->EndDraw();
        if(result==D2DERR_RECREATE_TARGET){brush.Reset();canvas.Reset();}
        return result;
#undef PAINT_TRY
    }
};
