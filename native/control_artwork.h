#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <initializer_list>
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
// The same vector geometry is used at every DPI, independent of installed icon fonts.
class ControlPainter {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> canvas;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> stroke;
    ComPtr<IDWriteFactory> writing;
    ComPtr<IDWriteTextFormat> text;
    static constexpr UINT INK=0x303640,SECONDARY=0x626b78,ACCENT=0x2868d8,SURFACE=0xf6f7f9;

    void color(UINT rgb,float alpha=1) { brush->SetColor(D2D1::ColorF(rgb,alpha)); }
    HRESULT line(std::initializer_list<D2D1_POINT_2F> points,bool closed=false,float width=1.7f) {
        ComPtr<ID2D1PathGeometry> geometry;
        HRESULT hr=factory->CreatePathGeometry(&geometry);if(FAILED(hr))return hr;
        ComPtr<ID2D1GeometrySink> sink;hr=geometry->Open(&sink);if(FAILED(hr))return hr;
        auto p=points.begin();sink->BeginFigure(*p++,D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLines(p,UINT32(points.size()-1));
        sink->EndFigure(closed?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);
        hr=sink->Close();if(FAILED(hr))return hr;
        canvas->DrawGeometry(geometry.Get(),brush.Get(),width,stroke.Get());return S_OK;
    }
    HRESULT wave(float x,float y,float height,float depth) {
        ComPtr<ID2D1PathGeometry> geometry;
        HRESULT hr=factory->CreatePathGeometry(&geometry);if(FAILED(hr))return hr;
        ComPtr<ID2D1GeometrySink> sink;hr=geometry->Open(&sink);if(FAILED(hr))return hr;
        sink->BeginFigure(D2D1::Point2F(x,y-height),D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(x+depth,y-height*.55f),
            D2D1::Point2F(x+depth,y+height*.55f),D2D1::Point2F(x,y+height)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);hr=sink->Close();if(FAILED(hr))return hr;
        canvas->DrawGeometry(geometry.Get(),brush.Get(),1.7f,stroke.Get());return S_OK;
    }
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
    HRESULT icon(int index,float x,float y,bool muted,bool fullscreen) {
        HRESULT hr=S_OK;
        if(index==0){
            hr=line({{x-9,y-3},{x-5,y-3},{x,y-7},{x,y+7},{x-5,y+3},{x-9,y+3}},true);
            if(FAILED(hr))return hr;
            if(muted){
                hr=line({{x+5,y-3},{x+11,y+3}});if(FAILED(hr))return hr;
                return line({{x+11,y-3},{x+5,y+3}});
            }
            hr=wave(x+4,y,3.5f,3);if(FAILED(hr))return hr;
            return wave(x+7,y,6.5f,5);
        }
        if(index==3){
            hr=line({{x-6,y-6},{x+6,y+6}});if(FAILED(hr))return hr;
            return line({{x+6,y-6},{x-6,y+6}});
        }
        if(index==1){
            canvas->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x-3.5f,y-6,x+3.5f,y+6),1.4f,1.4f),
                brush.Get(),1.6f,stroke.Get());
        }
        const float extent=index==1?9.0f:8.0f,length=index==1?3.0f:5.0f;
        for(float sx:{-1.0f,1.0f})for(float sy:{-1.0f,1.0f}){
            const float cx=x+sx*extent,cy=y+sy*extent;
            if(index==2&&fullscreen)
                hr=line({{cx,cy-sy*length},{cx-sx*length,cy-sy*length},{cx-sx*length,cy}});
            else hr=line({{cx-sx*length,cy},{cx,cy},{cx,cy-sy*length}});
            if(FAILED(hr))return hr;
        }
        return S_OK;
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
        if(!stroke){
            auto properties=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,
                D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND);
            PAINT_TRY(factory->CreateStrokeStyle(properties,nullptr,0,&stroke));
        }
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
        HRESULT artwork=S_OK;
        if(a.collapsed){
            // Invisible hover target is retained after the rail collapses.
            color(0,1.0f/255);canvas->FillRectangle(D2D1::RectF(a.arrow_x-12,10,a.arrow_x+12,206),brush.Get());
        }else{
            if(!a.fullscreen){
                color(0xc6cbd2);
                const float left=a.rail_x+(a.arrow_left?0:56);
                canvas->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left,89,left+20,127),7,7),brush.Get());
            }
            if(a.popup){
                color(0,1.0f/255);
                canvas->FillRectangle(D2D1::RectF(std::min(a.rail_x+38,a.flyout_x+106),18,std::max(a.rail_x+38,a.flyout_x+106),76),brush.Get());
                panel(D2D1::RectF(a.flyout_x,10,a.flyout_x+212,86),16,a.scale);
            }
        }
        const bool show_arrow=!a.collapsed||a.hot==4||a.focus==4;
        if(show_arrow){
            const float left=a.collapsed?a.arrow_x-12:a.arrow_x-(a.arrow_left?22:9);
            const float right=a.collapsed?a.arrow_x+12:a.arrow_x+(a.arrow_left?9:22);
            panel(D2D1::RectF(left,90,right,126),11,a.scale);
        }
        if(!a.collapsed){
            panel(D2D1::RectF(a.rail_x+10,10,a.rail_x+66,206),18,a.scale);
            for(int i=0;i<4&&SUCCEEDED(artwork);++i){
                const float y=40.0f+i*44;
                const bool selected=i==0&&(a.popup||a.muted||a.volume<=0);
                const auto cell=D2D1::RoundedRect(D2D1::RectF(a.rail_x+16,y-20,a.rail_x+60,y+20),12,12);
                if(a.hot==i||a.pressed==i||a.focus==i||selected){
                    color(i==3?0xfbe5e7:selected?0xe3ecfc:a.pressed==i?0xdce1e9:0xe9edf2);
                    canvas->FillRoundedRectangle(cell,brush.Get());
                }
                if(a.focus==i){color(ACCENT);canvas->DrawRoundedRectangle(cell,brush.Get(),1.5f);}
                color(i==3&&(a.hot==3||a.focus==3)?0xb82d40:selected?ACCENT:INK);
                artwork=icon(i,a.rail_x+38,y,a.muted||a.volume<=0,a.fullscreen);
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
        if(show_arrow&&SUCCEEDED(artwork)){
            if(a.focus==4){color(ACCENT);canvas->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a.arrow_x-10,92,a.arrow_x+10,124),9,9),brush.Get(),1.5f);}
            color(a.hot==4||a.focus==4?ACCENT:SECONDARY);
            const float direction=a.arrow_left?-1.0f:1.0f;
            artwork=line({{a.arrow_x-direction*2,104},{a.arrow_x+direction*2,108},{a.arrow_x-direction*2,112}},false,1.6f);
        }
        HRESULT result=canvas->EndDraw();
        if(result==D2DERR_RECREATE_TARGET){brush.Reset();canvas.Reset();}
        if(FAILED(artwork))return artwork;
        return result;
#undef PAINT_TRY
    }
};
