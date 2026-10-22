#pragma once
#include <dcomp.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include "phone_shell.h"

// Two small flip chains: GPU video and an infrequently redrawn native frame.
// No CPU readback of decoded pixels and no animation timer while idle.
struct PhoneCompositor {
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, picture,blackout;
    ComPtr<IDCompositionSurface> black_surface;
    ComPtr<IDCompositionRectangleClip> clip;
    ComPtr<IDXGISwapChain1> frame,video_surface;
    ComPtr<ID2D1Factory> drawing;
    ComPtr<IDWriteFactory> writing;
    UINT width=0,height=0;
    UINT video_width=1,video_height=1;
    uint64_t redraws=0;
    bool masked=false,video_attached=false;
    bool pending=false;double retry_at=0,present_seconds=0,present_max_seconds=0;uint64_t present_busy=0;
    HRESULT init(ID3D11Device* device,IDXGIFactory2* factory,HWND window,IDXGISwapChain1* video) {
        ComPtr<IDXGIDevice> dxgi;CHECK(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
        CHECK(DCompositionCreateDevice(dxgi.Get(),IID_PPV_ARGS(&composition)));
        CHECK(composition->CreateTargetForHwnd(window,TRUE,&target));
        CHECK(composition->CreateVisual(&root));CHECK(composition->CreateVisual(&picture));
        CHECK(composition->CreateRectangleClip(&clip));
        DXGI_SWAP_CHAIN_DESC1 sc{};sc.Width=sc.Height=1;sc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc.Count=1;sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sc.BufferCount=2;
        sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;sc.Scaling=DXGI_SCALING_STRETCH;sc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        CHECK(factory->CreateSwapChainForComposition(device,&sc,nullptr,&frame));
        video_surface=video;
        CHECK(root->SetContent(frame.Get()));CHECK(picture->SetClip(clip.Get()));
        CHECK(root->AddVisual(picture.Get(),TRUE,nullptr));CHECK(target->SetRoot(root.Get()));
        CHECK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,drawing.GetAddressOf()));
        CHECK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writing.GetAddressOf())));
        CHECK(composition->CreateVisual(&blackout));CHECK(picture->AddVisual(blackout.Get(),TRUE,nullptr));
        CHECK(initialize_black(device));
        return S_OK;
    }
    HRESULT initialize_black(ID3D11Device* device,ID3D11Texture2D* synthetic_pixel=nullptr){
        // A constant 1x1 opaque surface covers the viewport independently of
        // either flip chain's readiness. The optional copy is intrinsic-test only.
        ComPtr<IDCompositionSurface> next;CHECK(composition->CreateSurface(1,1,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&next));
        ComPtr<IDXGISurface> surface;POINT offset{};CHECK(next->BeginDraw(nullptr,IID_PPV_ARGS(&surface),&offset));
        auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        ComPtr<ID2D1RenderTarget> canvas;HRESULT hr=drawing->CreateDxgiSurfaceRenderTarget(surface.Get(),&properties,&canvas);
        ComPtr<ID2D1SolidColorBrush> brush;
        if(SUCCEEDED(hr))hr=canvas->CreateSolidColorBrush(D2D1::ColorF(0x000000,1.0f),&brush);
        if(SUCCEEDED(hr)){
            canvas->BeginDraw();canvas->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            canvas->FillRectangle(D2D1::RectF(float(offset.x),float(offset.y),float(offset.x+1),float(offset.y+1)),brush.Get());hr=canvas->EndDraw();
        }
        if(SUCCEEDED(hr)&&synthetic_pixel){
            D3D11_TEXTURE2D_DESC desc{};synthetic_pixel->GetDesc(&desc);
            if(desc.Width!=1||desc.Height!=1||desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||desc.Usage!=D3D11_USAGE_STAGING)hr=E_INVALIDARG;
            else {ComPtr<ID3D11Texture2D> texture;hr=surface.As(&texture);if(SUCCEEDED(hr)){
                ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
                D3D11_BOX box{UINT(offset.x),UINT(offset.y),0,UINT(offset.x+1),UINT(offset.y+1),1};
                context->CopySubresourceRegion(synthetic_pixel,0,0,0,0,texture.Get(),0,&box);
            }}
        }
        HRESULT ended=next->EndDraw();CHECK(hr);CHECK(ended);black_surface=std::move(next);
        CHECK(blackout->SetContent(masked?black_surface.Get():nullptr));return S_OK;
    }
    HRESULT set_screen_visibility(bool blank,bool has_video,bool geometry=false){
        const bool attach=has_video&&!blank;
        if(masked==blank&&video_attached==attach&&!geometry)return S_OK;
        CHECK(picture->SetContent(attach?video_surface.Get():nullptr));CHECK(blackout->SetContent(blank?black_surface.Get():nullptr));
        CHECK(composition->Commit());masked=blank;video_attached=attach;return S_OK;
    }
    // Explicit diagnostic only: save the idle artwork, never a sender/video surface.
    HRESULT save_idle_artwork(IDXGISurface* surface,const wchar_t* path) {
        ComPtr<ID3D11Texture2D> texture;CHECK(surface->QueryInterface(IID_PPV_ARGS(&texture)));
        ComPtr<ID3D11Device> device;texture->GetDevice(&device);ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=0;desc.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;CHECK(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),texture.Get());
        ComPtr<IWICImagingFactory> factory;CHECK(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
        ComPtr<IWICStream> stream;CHECK(factory->CreateStream(&stream));CHECK(stream->InitializeFromFilename(path,GENERIC_WRITE));
        ComPtr<IWICBitmapEncoder> encoder;CHECK(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));CHECK(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
        ComPtr<IWICBitmapFrameEncode> bitmap;CHECK(encoder->CreateNewFrame(&bitmap,nullptr));CHECK(bitmap->Initialize(nullptr));CHECK(bitmap->SetSize(desc.Width,desc.Height));
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;CHECK(bitmap->SetPixelFormat(&format));
        D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        HRESULT hr=bitmap->WritePixels(desc.Height,mapped.RowPitch,mapped.RowPitch*desc.Height,static_cast<BYTE*>(mapped.pData));context->Unmap(staging.Get(),0);CHECK(hr);
        CHECK(bitmap->Commit());return encoder->Commit();
    }
    HRESULT layout(PhoneShell& shell,const wchar_t* diagnostic_path=nullptr,ID3D11Texture2D* idle_readback=nullptr) {
        const bool diagnostic=diagnostic_path||idle_readback;
        if(pending&&now_seconds()<retry_at&&!diagnostic&&!shell.screen_blank)return S_OK;
        if(diagnostic && shell.has_video)return E_INVALIDARG;
        RECT r{};GetClientRect(shell.window,&r);UINT w=UINT(r.right),h=UINT(r.bottom);
        if(!w||!h||IsIconic(shell.window))return S_OK;
        // Bound composition allocations even for programmatically oversized windows.
        if(w>8192||h>8192||uint64_t(w)*h>16777216)return E_INVALIDARG;
        bool changed=w!=width||h!=height;
        if(!shell.chrome_dirty.exchange(false)&&!changed&&!diagnostic)return S_OK;
        float s=shell.dpi_scale(),rim=shell.fullscreen?0:float(PhoneShell::rim_pixels(s));
        float radius=shell.fullscreen?0:PhoneShell::corner_radius(w,h,s);
        video_width=UINT(std::max(1.0f,float(w)-2*rim));video_height=UINT(std::max(1.0f,float(h)-2*rim));
        CHECK(picture->SetOffsetX(rim));CHECK(picture->SetOffsetY(rim));
        CHECK(clip->SetLeft(0.0f));CHECK(clip->SetTop(0.0f));CHECK(clip->SetRight(float(video_width)));CHECK(clip->SetBottom(float(video_height)));
        float cr=std::max(0.0f,radius-rim);
        CHECK(clip->SetTopLeftRadiusX(cr));CHECK(clip->SetTopLeftRadiusY(cr));
        CHECK(clip->SetTopRightRadiusX(cr));CHECK(clip->SetTopRightRadiusY(cr));
        CHECK(clip->SetBottomLeftRadiusX(cr));CHECK(clip->SetBottomLeftRadiusY(cr));
        CHECK(clip->SetBottomRightRadiusX(cr));CHECK(clip->SetBottomRightRadiusY(cr));
        CHECK(blackout->SetTransform(D2D1::Matrix3x2F::Scale(float(video_width),float(video_height))));
        // Mask/geometry commits must not wait for a busy chrome Present.
        CHECK(set_screen_visibility(shell.screen_blank,shell.has_video,shell.screen_blank));
        if(changed){CHECK(frame->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0));width=w;height=h;}
        ComPtr<IDXGISurface> surface;CHECK(frame->GetBuffer(0,IID_PPV_ARGS(&surface)));
        auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        ComPtr<ID2D1RenderTarget> canvas;CHECK(drawing->CreateDxgiSurfaceRenderTarget(surface.Get(),&properties,&canvas));
        ComPtr<ID2D1SolidColorBrush> brush;CHECK(canvas->CreateSolidColorBrush(D2D1::ColorF(0x22242a),&brush));
        canvas->BeginDraw();canvas->Clear(D2D1::ColorF(0,0));
        auto outer=D2D1::RoundedRect(D2D1::RectF(.5f,.5f,float(w)-.5f,float(h)-.5f),radius,radius);
        canvas->FillRoundedRectangle(outer,brush.Get());brush->SetColor(D2D1::ColorF(0x888d98,.28f));canvas->DrawRoundedRectangle(outer,brush.Get(),1.0f);
        if(!shell.fullscreen){
            // A subtle curved grip sits in the rim, outside the video content.
            brush->SetColor(D2D1::ColorF(0xabb3c0,.55f));float grip=radius-1.5f*s;
            for(int degrees=35;degrees<60;degrees+=5){
                float a=degrees*3.14159265f/180,b=(degrees+5)*3.14159265f/180;
                canvas->DrawLine(D2D1::Point2F(w-radius+grip*std::cos(a),h-radius+grip*std::sin(a)),D2D1::Point2F(w-radius+grip*std::cos(b),h-radius+grip*std::sin(b)),brush.Get(),std::max(1.0f,s));
            }
        }
        if(!shell.has_video){
            // Roomy typography adapts to both portrait and landscape idle windows.
            float scale=std::min(s,float(w)/300.0f),cx=w*.5f,cy=h*.5f;
            brush->SetColor(D2D1::ColorF(0x111318));
            auto screen=D2D1::RoundedRect(D2D1::RectF(rim,rim,float(w)-rim,float(h)-rim),cr,cr);canvas->FillRoundedRectangle(screen,brush.Get());
            // Error/recovery text belongs solely to the graphics-independent
            // status card. Idle labels must not bleed out from underneath it.
            if(shell.idle_artwork_visible()){
            brush->SetColor(D2D1::ColorF(0x8f9baa));
            const bool compact=h<340*s;
            if(!compact){auto phone=D2D1::RoundedRect(D2D1::RectF(cx-17*scale,cy-111*scale,cx+17*scale,cy-53*scale),7*scale,7*scale);
            canvas->DrawRoundedRectangle(phone,brush.Get(),1.7f*scale);
            canvas->DrawLine(D2D1::Point2F(cx-5*scale,cy-60*scale),D2D1::Point2F(cx+5*scale,cy-60*scale),brush.Get(),1.5f*scale);}
            auto label=[&](const wchar_t* text,float font,float y,float box_h,UINT color,DWRITE_FONT_WEIGHT weight){
                ComPtr<IDWriteTextFormat> format;
                if(FAILED(writing->CreateTextFormat(L"Segoe UI",nullptr,weight,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,font*scale,L"en-us",&format)))return;
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                brush->SetColor(D2D1::ColorF(color));canvas->DrawText(text,UINT32(wcslen(text)),format.Get(),D2D1::RectF(18*scale,y,float(w)-18*scale,y+box_h*scale),brush.Get());
            };
            label(L"AirMirrorLite",11,cy-(compact?63:33)*scale,24,0x9299a6,DWRITE_FONT_WEIGHT_MEDIUM);
            const int state=shell.status;
            label(state==1?L"Connecting…":L"Ready to mirror",23,cy-(compact?35:2)*scale,35,0xf1f2f6,DWRITE_FONT_WEIGHT_SEMI_BOLD);
            label(state==1?L"Your screen will appear here":L"Open Screen Mirroring\non your iPhone",12,cy+(compact?8:44)*scale,42,0x939aa7,DWRITE_FONT_WEIGHT_NORMAL);
            if(!compact)label(L"Drag to move  ·  Resize from the corner",10,float(h)-66*scale,24,0x666e7b,DWRITE_FONT_WEIGHT_NORMAL);
            }
        }
        CHECK(canvas->EndDraw());if(diagnostic_path){CHECK(save_idle_artwork(surface.Get(),diagnostic_path));}
        if(idle_readback){
            // Private intrinsic-test target, restricted above to idle artwork.
            // Copy before flip; holding buffer0 across Present is not a snapshot.
            D3D11_TEXTURE2D_DESC desc{};idle_readback->GetDesc(&desc);
            if(desc.Width!=w||desc.Height!=h||desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM||desc.Usage!=D3D11_USAGE_STAGING)return E_INVALIDARG;
            ComPtr<ID3D11Texture2D> texture;CHECK(surface.As(&texture));ComPtr<ID3D11Device> device;texture->GetDevice(&device);
            ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);context->CopyResource(idle_readback,texture.Get());
        }
        const double began=now_seconds();HRESULT present=frame->Present(0,DXGI_PRESENT_DO_NOT_WAIT);const double elapsed=now_seconds()-began;
        present_seconds+=elapsed;present_max_seconds=std::max(present_max_seconds,elapsed);
        if(present==DXGI_ERROR_WAS_STILL_DRAWING){pending=true;retry_at=now_seconds()+.005;++present_busy;shell.chrome_dirty=true;return S_OK;}
        CHECK(present);pending=false;CHECK(composition->Commit());++redraws;return S_OK;
    }
};
