#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwchar>

// Generated idle artwork only. Never called for video, suspension or error cards.
// It redraws on state/geometry changes, without an animation or polling timer.
inline HRESULT draw_idle_artwork(ID2D1RenderTarget* canvas,IDWriteFactory* writing,
    D2D1_RECT_F viewport,float corner,float scale,int state) {
    using Microsoft::WRL::ComPtr;
    const float w=viewport.right+viewport.left,h=viewport.bottom+viewport.top;
    const bool landscape=w>h*1.25f&&w>=600*scale&&h>=430*scale;
    const bool compact=!landscape&&(w<300*scale||h<600*scale);
    const bool connecting=state==1;
    ComPtr<ID2D1SolidColorBrush> brush;
    HRESULT hr=canvas->CreateSolidColorBrush(D2D1::ColorF(0xffffff),&brush);
    if(FAILED(hr))return hr;
    const D2D1_GRADIENT_STOP stops[]={
        {0,D2D1::ColorF(0x152238)},{.58f,D2D1::ColorF(0x101823)},{1,D2D1::ColorF(0x0d1118)}};
    ComPtr<ID2D1GradientStopCollection> colors;
    hr=canvas->CreateGradientStopCollection(stops,ARRAYSIZE(stops),&colors);if(FAILED(hr))return hr;
    ComPtr<ID2D1LinearGradientBrush> background;
    hr=canvas->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({w*.2f,0},{w*.8f,h}),colors.Get(),&background);
    if(FAILED(hr))return hr;
    canvas->FillRoundedRectangle(D2D1::RoundedRect(viewport,corner,corner),background.Get());
    canvas->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto color=[&](UINT rgb,float alpha=1){brush->SetColor(D2D1::ColorF(rgb,alpha));};
    HRESULT text_result=S_OK;
    auto label=[&](const wchar_t* value,float font,D2D1_RECT_F rect,UINT rgb,
                   DWRITE_FONT_WEIGHT weight=DWRITE_FONT_WEIGHT_NORMAL,
                   DWRITE_TEXT_ALIGNMENT align=DWRITE_TEXT_ALIGNMENT_CENTER){
        if(FAILED(text_result))return;
        ComPtr<IDWriteTextFormat> format;
        text_result=writing->CreateTextFormat(L"Segoe UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,font*scale,L"en-us",&format);
        if(FAILED(text_result))return;
        format->SetTextAlignment(align);format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        color(rgb);canvas->DrawText(value,UINT32(wcslen(value)),format.Get(),rect,brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const float cx=w*.5f,cy=h*.5f,pad=28*scale;
    if(compact){
        const bool narrow=w<280*scale;
        const float font=std::clamp((w-36*scale)/8.0f,10*scale,22*scale)/scale;
        label(L"AirMirrorLite",11,{18*scale,cy-(narrow?84:63)*scale,w-18*scale,cy-(narrow?60:39)*scale},0x9fb1c8,DWRITE_FONT_WEIGHT_MEDIUM);
        label(connecting?L"Connecting…":narrow?L"Ready to\nmirror":L"Ready to mirror",font,
            {18*scale,cy-(narrow?48:34)*scale,w-18*scale,cy+(narrow?12:0)*scale},0xf3f7ff,DWRITE_FONT_WEIGHT_SEMI_BOLD);
        label(connecting?L"Your screen will appear here":L"Screen Mirroring\nSelect AirMirrorLite",11,
            {16*scale,cy+(narrow?28:10)*scale,w-16*scale,cy+(narrow?72:54)*scale},0xa2b3c9);
        return text_result;
    }
    // A small status header stays separate from the central onboarding content.
    label(L"AirMirrorLite",13,{pad,32*scale,w-pad-82*scale,62*scale},0xe4ecf8,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,DWRITE_TEXT_ALIGNMENT_LEADING);
    const float badge_left=w-pad-(connecting?76:68)*scale;
    color(connecting?0x203450:0x17302e);
    canvas->FillRoundedRectangle(D2D1::RoundedRect({badge_left,35*scale,w-pad,59*scale},12*scale,12*scale),brush.Get());
    color(connecting?0x93b8ff:0x75d9b6);
    canvas->FillEllipse(D2D1::Ellipse({badge_left+12*scale,47*scale},2.5f*scale,2.5f*scale),brush.Get());
    label(connecting?L"Linking":L"Ready",10.5f,{badge_left+21*scale,35*scale,w-pad-8*scale,59*scale},
        connecting?0xb9d0ff:0xabe6d1,DWRITE_FONT_WEIGHT_MEDIUM);

    const float content_width=std::min(w-2*pad,720*scale);
    const float content_left=(w-content_width)*.5f;
    const float hero_x=landscape?content_left+150*scale:cx-3*scale;
    const float title_y=landscape?cy-104*scale:cy-18*scale;
    const float hero_y=landscape?cy-8*scale:title_y-104*scale;
    // Gentle static rings and two devices replace the old isolated app-icon tile.
    for(int i=2;i>=0;--i){
        color(0x4b83d5,.045f);
        canvas->FillEllipse(D2D1::Ellipse({hero_x,hero_y+10*scale},(87+i*19)*scale,(74+i*16)*scale),brush.Get());
    }
    auto rect=[&](float l,float t,float r,float b){return D2D1::RectF(hero_x+l*scale,hero_y+t*scale,hero_x+r*scale,hero_y+b*scale);};
    color(0x1b2b42);canvas->FillRoundedRectangle(D2D1::RoundedRect(rect(-65,-42,57,36),10*scale,10*scale),brush.Get());
    color(0x7c98bd);canvas->DrawRoundedRectangle(D2D1::RoundedRect(rect(-65,-42,57,36),10*scale,10*scale),brush.Get(),1.7f*scale);
    color(0x111d2e);canvas->FillRoundedRectangle(D2D1::RoundedRect(rect(-57,-34,49,27),5*scale,5*scale),brush.Get());
    color(0xa6c6f4);
    canvas->DrawLine({hero_x-4*scale,hero_y+37*scale},{hero_x-4*scale,hero_y+47*scale},brush.Get(),2*scale);
    canvas->DrawLine({hero_x-22*scale,hero_y+48*scale},{hero_x+14*scale,hero_y+48*scale},brush.Get(),2*scale);
    // Two luminous screens communicate mirroring without depicting user content.
    color(0x739dd9,.8f);canvas->DrawRoundedRectangle(D2D1::RoundedRect(rect(-26,-18,8,5),4*scale,4*scale),brush.Get(),1.5f*scale);
    color(0xb4d2ff);canvas->DrawRoundedRectangle(D2D1::RoundedRect(rect(-17,-10,17,13),4*scale,4*scale),brush.Get(),1.5f*scale);
    color(0x0b111b);canvas->FillRoundedRectangle(D2D1::RoundedRect(rect(29,-8,70,69),11*scale,11*scale),brush.Get());
    color(0xd3e4ff);canvas->DrawRoundedRectangle(D2D1::RoundedRect(rect(29,-8,70,69),11*scale,11*scale),brush.Get(),1.8f*scale);
    color(0x223754);canvas->FillRoundedRectangle(D2D1::RoundedRect(rect(35,-1,64,58),6*scale,6*scale),brush.Get());
    color(0xa6c6f4);canvas->DrawLine({hero_x+44*scale,hero_y+63*scale},{hero_x+55*scale,hero_y+63*scale},brush.Get(),1.7f*scale);

    const float card_width=std::min(320*scale,landscape?content_width-330*scale:w-2*pad);
    const float left=landscape?content_left+330*scale:cx-card_width*.5f;
    const auto align=landscape?DWRITE_TEXT_ALIGNMENT_LEADING:DWRITE_TEXT_ALIGNMENT_CENTER;
    label(connecting?L"Connecting…":L"Mirror your iPhone",24,
        {left,title_y,left+card_width,title_y+36*scale},0xf4f7ff,DWRITE_FONT_WEIGHT_SEMI_BOLD,align);
    label(connecting?L"Your screen will appear here.":L"Your screen and sound, together.",12,
        {left,title_y+40*scale,left+card_width,title_y+66*scale},0xa5b7cf,DWRITE_FONT_WEIGHT_NORMAL,align);
    const float card_y=title_y+88*scale,card_bottom=card_y+128*scale;
    auto card=D2D1::RoundedRect({left,card_y,left+card_width,card_bottom},16*scale,16*scale);
    color(0x162131,.92f);canvas->FillRoundedRectangle(card,brush.Get());
    color(0x32445d,.75f);canvas->DrawRoundedRectangle(card,brush.Get(),scale);
    if(connecting){
        for(int i=0;i<3;++i){color(0x8fb8ff,.4f+i*.25f);canvas->FillEllipse(D2D1::Ellipse(
            {left+card_width*.5f+(i-1)*12*scale,card_y+30*scale},2.5f*scale,2.5f*scale),brush.Get());}
        label(L"Opening the connection",12,{left+12*scale,card_y+48*scale,left+card_width-12*scale,card_y+72*scale},
            0xe4ecf8,DWRITE_FONT_WEIGHT_MEDIUM);
        label(L"Keep your iPhone nearby.",11,{left+12*scale,card_y+79*scale,left+card_width-12*scale,card_y+103*scale},0x9aadc5);
    }else{
        for(int i=0;i<2;++i){
            const float y=card_y+(i?91:35)*scale;
            color(0x263b59);canvas->FillEllipse(D2D1::Ellipse({left+25*scale,y},11*scale,11*scale),brush.Get());
            label(i?L"2":L"1",11,{left+14*scale,y-11*scale,left+36*scale,y+11*scale},0xbad4fb,DWRITE_FONT_WEIGHT_SEMI_BOLD);
            label(i?L"Screen Mirroring":L"Open Control Center",12,
                {left+47*scale,y-18*scale,left+card_width-14*scale,y+2*scale},0xe7eef9,DWRITE_FONT_WEIGHT_MEDIUM,DWRITE_TEXT_ALIGNMENT_LEADING);
            label(i?L"Choose AirMirrorLite":L"on your iPhone",11,
                {left+47*scale,y+4*scale,left+card_width-14*scale,y+22*scale},0x9cacc2,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        color(0x2c3c52,.7f);canvas->DrawLine({left+16*scale,card_y+63*scale},
            {left+card_width-16*scale,card_y+63*scale},brush.Get(),scale);
    }
    label(L"Same Wi-Fi or local network",11,{pad,h-87*scale,w-pad,h-65*scale},0x94a6bf);
    label(L"Drag to move  ·  Corner to resize",10,{pad,h-57*scale,w-pad,h-35*scale},0x788ba6);
    return text_result;
}
