#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
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
// Vector artwork rendered at the monitor's pixel density. Only this small UI
// surface is CPU-backed; mirrored frames stay on the GPU.
class ControlPainter {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> canvas;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> writing;
    ComPtr<IDWriteTextFormat> icons,text,chevrons;
public:
    HRESULT draw(HDC dc,const ControlArtwork& a){
#define PAINT_TRY(call) do{HRESULT result=(call);if(FAILED(result))return result;}while(0)
        if(!factory){
            PAINT_TRY(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()));
        }
        // Each resource may fail independently. Retrying an incomplete initial
        // draw must never treat a successful factory creation as full readiness.
        if(!writing){
            PAINT_TRY(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writing.GetAddressOf())));
        }
        if(!icons||!chevrons||!text){
            ComPtr<IDWriteFontCollection> fonts;PAINT_TRY(writing->GetSystemFontCollection(&fonts));UINT32 index;BOOL exists;
            PAINT_TRY(fonts->FindFamilyName(L"Segoe Fluent Icons",&index,&exists));
            if(!icons)PAINT_TRY(writing->CreateTextFormat(exists?L"Segoe Fluent Icons":L"Segoe MDL2 Assets",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,20,L"en-us",&icons));
            if(!chevrons)PAINT_TRY(writing->CreateTextFormat(exists?L"Segoe Fluent Icons":L"Segoe MDL2 Assets",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-us",&chevrons));
            if(!text)PAINT_TRY(writing->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,12,L"en-us",&text));
            icons->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);icons->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            chevrons->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);chevrons->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);text->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if(!canvas){
            auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            PAINT_TRY(factory->CreateDCRenderTarget(&properties,&canvas));
        }
        RECT bounds{0,0,a.width,a.height};PAINT_TRY(canvas->BindDC(dc,&bounds));
        if(!brush)PAINT_TRY(canvas->CreateSolidColorBrush(D2D1::ColorF(0xffffff),&brush));
        canvas->BeginDraw();canvas->SetTransform(D2D1::Matrix3x2F::Scale(a.scale,a.scale));canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);canvas->Clear(D2D1::ColorF(0,0));
        auto color=[&](UINT rgb,float alpha=1){brush->SetColor(D2D1::ColorF(rgb,alpha));};
        auto panel=[&](D2D1_RECT_F rect,float radius){
            rect.left=std::round(rect.left*a.scale)/a.scale;rect.top=std::round(rect.top*a.scale)/a.scale;
            rect.right=std::round(rect.right*a.scale)/a.scale;rect.bottom=std::round(rect.bottom*a.scale)/a.scale;
            for(int i=5;i>0;--i){color(0,.025f);canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rect.left-i*.6f,rect.top-i*.3f+2,rect.right+i*.6f,rect.bottom+i*.6f+2),radius+i*.6f,radius+i*.6f),brush.Get());}
            color(0x202126);canvas->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),brush.Get());
            float inset=.5f/a.scale;auto edge=D2D1::RectF(rect.left+inset,rect.top+inset,rect.right-inset,rect.bottom-inset);
            color(0xffffff,.10f);canvas->DrawRoundedRectangle(D2D1::RoundedRect(edge,radius-inset,radius-inset),brush.Get(),1/a.scale);
        };
        if(a.collapsed){color(0,1.0f/255);canvas->FillRectangle(D2D1::RectF(a.arrow_x-12,10,a.arrow_x+12,206),brush.Get());}
        bool show_arrow=!a.collapsed||a.hot==4||a.focus==4;
        if(show_arrow){
            if(a.hot==4||a.focus==4){color(0x2e3138);canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a.arrow_x-12,90,a.arrow_x+12,126),12,12),brush.Get());}
            color(0xe6e8ed,a.hot==4||a.focus==4?1.0f:.65f);wchar_t glyph=a.arrow_left?L'\xe76b':L'\xe76c';
            canvas->DrawText(&glyph,1,chevrons.Get(),D2D1::RectF(a.arrow_x-10,96,a.arrow_x+10,120),brush.Get());
        }
        if(!a.collapsed){
        if(a.popup){
            // The near-transparent bridge keeps pointer travel to the flyout safe.
            color(0,1.0f/255);canvas->FillRectangle(D2D1::RectF(std::min(a.rail_x+38,a.flyout_x+106),18,std::max(a.rail_x+38,a.flyout_x+106),76),brush.Get());
            panel(D2D1::RectF(a.flyout_x,10,a.flyout_x+212,86),16);
        }
        panel(D2D1::RectF(a.rail_x+10,10,a.rail_x+66,206),18);
        for(int i=0;i<4;++i){float y=40.0f+i*44;
            if(a.hot==i||a.pressed==i||a.focus==i||(i==0&&a.popup)){
                color(i==3?0xef6670:0xffffff,a.pressed==i?.16f:.075f);
                auto cell=D2D1::RoundedRect(D2D1::RectF(a.rail_x+16,y-22,a.rail_x+60,y+22),12,12);canvas->FillRoundedRectangle(cell,brush.Get());
                if(a.focus==i){color(0x9cc8ff);canvas->DrawRoundedRectangle(cell,brush.Get(),1/a.scale);}
            }
            wchar_t glyph[]={i==0?(a.muted||a.volume<=0?L'\xe74f':L'\xe767'):i==1?L'\xe72c':i==2?(a.fullscreen?L'\xe73f':L'\xe740'):L'\xe711',0};
            color(i==3&&a.hot==3?0xffacb3:0xe6e8ed);
            canvas->DrawText(glyph,1,icons.Get(),D2D1::RectF(a.rail_x+26,y-12,a.rail_x+50,y+12),brush.Get());
        }
        color(0xffffff,.08f);canvas->DrawLine(D2D1::Point2F(a.rail_x+26,150.5f),D2D1::Point2F(a.rail_x+50,150.5f),brush.Get(),1/a.scale);
        if(a.popup){
            color(0xaeb3bd);text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            canvas->DrawText(a.muted?L"Muted":L"Volume",a.muted?5:6,text.Get(),D2D1::RectF(a.flyout_x+20,20,a.flyout_x+140,40),brush.Get());
            wchar_t value[12];swprintf_s(value,L"%d%%",int(std::round(a.volume*100)));text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            canvas->DrawText(value,UINT32(wcslen(value)),text.Get(),D2D1::RectF(a.flyout_x+150,20,a.flyout_x+192,40),brush.Get());
            float left=a.flyout_x+24,right=a.flyout_x+188,thumb=left+a.volume*(right-left);
            color(0x484b54);canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,56,right,60),2,2),brush.Get());
            color(a.muted?0x8c929d:0x9cc8ff);canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,56,thumb,60),2,2),brush.Get());
            color(0xf4f6fa);canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb,58),8,8),brush.Get());
            color(a.muted?0x8c929d:0x709ed6);canvas->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb,58),3,3),brush.Get());
        }
        }
        HRESULT result=canvas->EndDraw();if(result==D2DERR_RECREATE_TARGET){brush.Reset();canvas.Reset();}return result;
#undef PAINT_TRY
    }
};
