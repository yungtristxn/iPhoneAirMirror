// Windows-only implementation behind a length-checked, opaque C ABI.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netlistmgr.h>
#include <wrl/client.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windns.h>
#include <userenv.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <future>
using Microsoft::WRL::ComPtr;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { error_hr(#x, hr_); return hr_; } } while (0)

static double now_seconds() {
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return double(q.QuadPart) / double(f.QuadPart);
}
static thread_local std::string last_error;
static void error_hr(const char* stage, HRESULT hr) {
    char b[512]; sprintf_s(b, "%s: 0x%08lx", stage, (unsigned long)hr); last_error = std::string(b) + (last_error.empty() ? "" : "; " + last_error.substr(0,512));
}
static std::wstring wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    if (!n) return L"";
    std::wstring v(n, 0); MultiByteToWideChar(CP_UTF8, 0, s, -1, v.data(), n); v.pop_back(); return v;
}
struct Handle {
    HANDLE h = nullptr;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle() = default; Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
};
static HRESULT create_hevc_transform(bool encoder,IMFTransform** transform) {
    MFT_REGISTER_TYPE_INFO type{MFMediaType_Video,MFVideoFormat_HEVC};IMFActivate** list=nullptr;UINT32 count=0;
    HRESULT hr=MFTEnumEx(encoder?MFT_CATEGORY_VIDEO_ENCODER:MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT|MFT_ENUM_FLAG_LOCALMFT|MFT_ENUM_FLAG_SORTANDFILTER,encoder?nullptr:&type,encoder?&type:nullptr,&list,&count);
    if(FAILED(hr))return hr;hr=MF_E_TOPO_CODEC_NOT_FOUND;
    for(UINT32 i=0;i<count;++i){if(FAILED(hr))hr=list[i]->ActivateObject(IID_PPV_ARGS(transform));list[i]->Release();}CoTaskMemFree(list);return hr;
}
struct VideoFrame { ComPtr<IMFSample> sample; double pts; UINT width; UINT height; };
struct AudioFrame { std::vector<int16_t> pcm; size_t pos = 0; double pts = 0; };
#include "phone_compositor.h"
struct App {
    HWND window = nullptr; Handle wake, audio_event;
    PhoneShell shell;
    bool hevc_available=false,decoder_ready=false;UINT codec=264,expected_fps=60;
    PhoneCompositor chrome;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap; ComPtr<ID3D11VideoDevice> vdevice; ComPtr<ID3D11VideoContext> vcontext;
    ComPtr<IMFDXGIDeviceManager> manager; ComPtr<IMFTransform> decoder,h264_cache,hevc_cache;
    ComPtr<ID3D11VideoProcessorEnumerator> venum; ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<IAudioClient> audio; ComPtr<IAudioRenderClient> render;
    UINT32 audio_capacity = 0; double audio_latency = 0; bool audio_running = false;
    UINT width = 0, height = 0, vieww = 0, viewh = 0;
    std::deque<VideoFrame> video; std::deque<AudioFrame> sound;
    VideoFrame last_frame{};
    ComPtr<ID3D11Texture2D> paused_texture;
    uint64_t decoder_instances=0;
    uint64_t decoded = 0, presented = 0, gpu_frames = 0, decoder_surfaces = 0, late_frames = 0, audio_frames = 0, underruns = 0;
    std::string adapter; bool com = false, mf = false;
    ~App() {
        if (audio) audio->Stop();
        sound.clear(); video.clear(); last_frame.sample.Reset(); paused_texture.Reset();decoder.Reset();h264_cache.Reset();hevc_cache.Reset(); render.Reset(); audio.Reset();
        processor.Reset(); venum.Reset(); manager.Reset(); swap.Reset(); vcontext.Reset(); vdevice.Reset(); context.Reset(); device.Reset();
        chrome=PhoneCompositor{};shell.shutdown();
        if (mf) MFShutdown(); if (com) CoUninitialize();
    }
    HRESULT init(bool hidden) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); com = true;
        CHECK(MFStartup(MF_VERSION, MFSTARTUP_FULL)); mf = true;
        wake.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        audio_event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake.h || !audio_event.h) return HRESULT_FROM_WIN32(GetLastError());
        CHECK(shell.create(wake.h,hidden));window=shell.window;
        D3D_FEATURE_LEVEL level;
        CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context));
        ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(context.As(&mt))) mt->SetMultithreadProtected(TRUE);
        CHECK(device.As(&vdevice)); CHECK(context.As(&vcontext));
        ComPtr<IDXGIDevice> dxgi; CHECK(device.As(&dxgi));
        ComPtr<IDXGIAdapter> ad; CHECK(dxgi->GetAdapter(&ad)); DXGI_ADAPTER_DESC desc{}; ad->GetDesc(&desc);
        char adapter_utf8[256]{}; WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapter_utf8, 256, nullptr, nullptr); adapter = adapter_utf8;
        ComPtr<IDXGIFactory2> factory; CHECK(ad->GetParent(IID_PPV_ARGS(&factory)));
        DXGI_SWAP_CHAIN_DESC1 sc{}; sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc.Count = 1; sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.Width=sc.Height=1;sc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        sc.BufferCount = 2; sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; sc.Scaling = DXGI_SCALING_STRETCH;
        CHECK(factory->CreateSwapChainForComposition(device.Get(), &sc, nullptr, &swap));
        CHECK(chrome.init(device.Get(),factory.Get(),window,swap.Get()));CHECK(chrome.layout(shell));
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        UINT token; CHECK(MFCreateDXGIDeviceManager(&token, &manager)); CHECK(manager->ResetDevice(device.Get(), token));
        {
            ComPtr<IMFTransform> probe;ComPtr<IMFAttributes> attrs;UINT32 aware=0;
            D3D11_VIDEO_DECODER_DESC desc{D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,3840,2160,DXGI_FORMAT_NV12};UINT configs=0;
            hevc_available=SUCCEEDED(create_hevc_transform(false,&probe))&&SUCCEEDED(probe->GetAttributes(&attrs))
                &&SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE,&aware))&&aware
                &&SUCCEEDED(vdevice->GetVideoDecoderConfigCount(&desc,&configs))&&configs>0;
            if(hevc_available){hevc_cache=probe;++decoder_instances;}
        }
        CHECK(init_decoder());
        ComPtr<IMMDeviceEnumerator> en; CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&en)));
        ComPtr<IMMDevice> endpoint; CHECK(en->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint));
        CHECK(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &audio));
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 176400;
        CHECK(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            300000, 0, &format, nullptr));
        CHECK(audio->SetEventHandle(audio_event.h)); CHECK(audio->GetBufferSize(&audio_capacity));
        REFERENCE_TIME latency; CHECK(audio->GetStreamLatency(&latency)); audio_latency = latency / 1e7;
        CHECK(audio->GetService(IID_PPV_ARGS(&render)));
        if (!hidden) shell.show();
        return S_OK;
    }
    void reset_video_decoder(){
        video.clear();last_frame.sample.Reset();paused_texture.Reset();processor.Reset();venum.Reset();
        if(decoder){
            if(decoder_ready){decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM,0);decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0);decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING,0);}
            HRESULT out=decoder->SetOutputType(0,nullptr,0),in=decoder->SetInputType(0,nullptr,0);
            if(FAILED(out)||FAILED(in)){if(codec==265)hevc_cache.Reset();else h264_cache.Reset();}
        }
        decoder.Reset();decoder_ready=false;width=height=0;
    }
    HRESULT init_decoder(UINT next_codec=264,UINT initial_width=0,UINT initial_height=0) {
        reset_video_decoder();codec=next_codec;
        if(codec==265){
            if(!hevc_available)return MF_E_TOPO_CODEC_NOT_FOUND;
            if(!hevc_cache){CHECK(create_hevc_transform(false,&hevc_cache));++decoder_instances;}decoder=hevc_cache;
        }else{
            if(!h264_cache){CHECK(CoCreateInstance(CLSID_CMSH264DecoderMFT,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&h264_cache)));++decoder_instances;}decoder=h264_cache;
        }
        ComPtr<IMFAttributes> attributes; CHECK(decoder->GetAttributes(&attributes));
        UINT32 aware = 0; CHECK(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)); if (!aware) return E_NOINTERFACE;
        CHECK(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
        CHECK(decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get())));
        ComPtr<ICodecAPI> codec_api; if (SUCCEEDED(decoder.As(&codec_api))) {
            VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE;
            codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            v.vt = VT_UI4; v.ulVal = this->codec==265?3840:1920; codec_api->SetValue(&CODECAPI_AVDecVideoMaxCodedWidth, &v);
            v.ulVal = this->codec==265?2176:1920; codec_api->SetValue(&CODECAPI_AVDecVideoMaxCodedHeight, &v);
        }
        ComPtr<IMFMediaType> type; CHECK(MFCreateMediaType(&type));
        CHECK(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)); CHECK(type->SetGUID(MF_MT_SUBTYPE, codec==265?MFVideoFormat_HEVC:MFVideoFormat_H264));
        if(codec==265){
            if(initial_width<48||initial_width>3840||initial_height<48||initial_height>2160)return E_INVALIDARG;
            CHECK(type->SetUINT32(MF_MT_MPEG2_PROFILE,1));CHECK(MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,initial_width,initial_height));
            CHECK(MFSetAttributeRatio(type.Get(),MF_MT_FRAME_RATE,expected_fps,1));CHECK(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
        }
        CHECK(decoder->SetInputType(0, type.Get(), 0)); CHECK(set_output());
        CHECK(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        CHECK(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));decoder_ready=true; return S_OK;
    }
    HRESULT set_output() {
        for (DWORD i = 0; i < 32; ++i) {
            ComPtr<IMFMediaType> t; CHECK(decoder->GetOutputAvailableType(0, i, &t));
            GUID sub; CHECK(t->GetGUID(MF_MT_SUBTYPE, &sub));
            if (sub != MFVideoFormat_NV12) continue;
            UINT w = 0, h = 0; MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (codec==265?(w>3840||h>2176||uint64_t(w)*h>3840ull*2176):(w>1920||h>1920||uint64_t(w)*h>2088960)) return MF_E_INVALIDMEDIATYPE;
            video.clear();last_frame.sample.Reset();paused_texture.Reset();
            CHECK(decoder->SetOutputType(0, t.Get(), 0)); width = w; height = h;
            MFVideoArea aperture{}; UINT32 size=0;
            if(SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), &size)) && size==sizeof(aperture)
                && aperture.Area.cx>0 && aperture.Area.cy>0 && UINT(aperture.Area.cx)<=w && UINT(aperture.Area.cy)<=h) {
                width=aperture.Area.cx;height=aperture.Area.cy;
            }
            video.clear(); last_frame.sample.Reset(); processor.Reset(); venum.Reset(); return S_OK;
        }
        return MF_E_INVALIDMEDIATYPE;
    }
    HRESULT output() {
        for (int i = 0; i < 32; ++i) {
            MFT_OUTPUT_DATA_BUFFER out{}; DWORD status = 0;
            HRESULT hr = decoder->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            ComPtr<IMFSample> s; s.Attach(out.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { CHECK(set_output()); continue; }
            CHECK(hr); if (!s) return E_UNEXPECTED;
            LONGLONG pts; CHECK(s->GetSampleTime(&pts));
            ComPtr<IMFMediaBuffer> b; CHECK(s->GetBufferByIndex(0, &b)); ComPtr<IMFDXGIBuffer> gpu;
            // No silent CPU fallback. Every delivered frame must expose a DXGI surface.
            CHECK(b.As(&gpu));
            ComPtr<ID3D11Texture2D> decoded_texture; CHECK(gpu->GetResource(IID_PPV_ARGS(&decoded_texture)));
            D3D11_TEXTURE2D_DESC texture_desc{}; decoded_texture->GetDesc(&texture_desc);
            if (!(texture_desc.BindFlags & D3D11_BIND_DECODER))return E_NOINTERFACE;
            ++decoder_surfaces;
            ++gpu_frames; ++decoded;
            if (video.size() == (codec==265?4:8)) { video.pop_front(); ++late_frames; }
            shell.video_size(width,height);
            video.push_back({s, pts / 1e7, width, height});
        }
        return E_UNEXPECTED;
    }
    HRESULT submit(const uint8_t* bytes, unsigned len, double pts) {
        if(!decoder_ready||!decoder)return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (!bytes || !len || len > (codec==265?4u:2u)*1024*1024 || !std::isfinite(pts)) return E_INVALIDARG;
        ComPtr<IMFMediaBuffer> buffer; CHECK(MFCreateMemoryBuffer(len, &buffer)); BYTE* p;
        CHECK(buffer->Lock(&p, nullptr, nullptr)); memcpy(p, bytes, len); buffer->Unlock(); CHECK(buffer->SetCurrentLength(len));
        ComPtr<IMFSample> sample; CHECK(MFCreateSample(&sample)); CHECK(sample->AddBuffer(buffer.Get()));
        CHECK(sample->SetSampleTime((LONGLONG)(pts*1e7))); CHECK(sample->SetSampleDuration(10000000/expected_fps));
        HRESULT hr = decoder->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) { CHECK(output()); hr = decoder->ProcessInput(0, sample.Get(), 0); }
        CHECK(hr); return output();
    }
    HRESULT draw(IMFSample* frame, UINT width, UINT height) {
        CHECK(chrome.layout(shell));UINT vw=chrome.video_width,vh=chrome.video_height;
        if (!vw || !vh || !width || !height) return S_OK;
        if (vieww != vw || viewh != vh) {
            context->ClearState(); CHECK(swap->ResizeBuffers(0, vw, vh, DXGI_FORMAT_UNKNOWN, 0)); vieww = vw; viewh = vh;processor.Reset();venum.Reset();
        }
        ComPtr<IMFMediaBuffer> b; CHECK(frame->GetBufferByIndex(0, &b)); ComPtr<IMFDXGIBuffer> dxgi; CHECK(b.As(&dxgi));
        ComPtr<ID3D11Texture2D> texture; CHECK(dxgi->GetResource(IID_PPV_ARGS(&texture))); UINT slice; CHECK(dxgi->GetSubresourceIndex(&slice));
        if (!processor) {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{}; desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            desc.InputWidth = width; desc.InputHeight = height; desc.OutputWidth = vw; desc.OutputHeight = vh;
            desc.InputFrameRate = {60, 1}; desc.OutputFrameRate = {60, 1}; desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            CHECK(vdevice->CreateVideoProcessorEnumerator(&desc, &venum)); CHECK(vdevice->CreateVideoProcessor(venum.Get(), 0, &processor));
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; iv.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> input; CHECK(vdevice->CreateVideoProcessorInputView(texture.Get(), venum.Get(), &iv, &input));
        ComPtr<ID3D11Texture2D> target; CHECK(swap->GetBuffer(0, IID_PPV_ARGS(&target)));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{}; ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output_view; CHECK(vdevice->CreateVideoProcessorOutputView(target.Get(), venum.Get(), &ov, &output_view));
        double scale = std::min(double(vw)/width, double(vh)/height);
        LONG dw = LONG(width*scale), dh = LONG(height*scale);
        RECT src{0, 0, LONG(width), LONG(height)}, dst{(LONG(vw)-dw)/2, (LONG(vh)-dh)/2, (LONG(vw)+dw)/2, (LONG(vh)+dh)/2};
        D3D11_VIDEO_COLOR bg{}; bg.RGBA.A = 1;
        vcontext->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &bg);
        vcontext->VideoProcessorSetOutputAlphaFillMode(processor.Get(),D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE,0);
        vcontext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vcontext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
        vcontext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst);
        vcontext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{}; cs.YCbCr_Matrix = 1; // BT.709, studio range
        vcontext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &cs);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input.Get();
        HRESULT blt=vcontext->VideoProcessorBlt(processor.Get(), output_view.Get(), 0, 1, &stream);
        if(FAILED(blt)){
            D3D11_TEXTURE2D_DESC td{},od{};texture->GetDesc(&td);target->GetDesc(&od);
            char details[256];sprintf_s(details,"VideoProcessorBlt input=%ux%u fmt=%u array=%u slice=%u visible=%ux%u output=%ux%u fmt=%u",td.Width,td.Height,td.Format,td.ArraySize,slice,width,height,od.Width,od.Height,od.Format);
            error_hr(details,blt);return blt;
        }
        // Keep a GPU copy for paused-window redraw without retaining a decoder-pool surface.
        if(texture.Get()!=paused_texture.Get()) {
            D3D11_TEXTURE2D_DESC source_desc{},cached_desc{};texture->GetDesc(&source_desc);
            if(paused_texture)paused_texture->GetDesc(&cached_desc);
            if(!paused_texture || cached_desc.Width!=source_desc.Width || cached_desc.Height!=source_desc.Height) {
                last_frame.sample.Reset();paused_texture.Reset();auto copy_desc=source_desc;copy_desc.ArraySize=1;copy_desc.MipLevels=1;copy_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;copy_desc.MiscFlags=0;
                CHECK(device->CreateTexture2D(&copy_desc,nullptr,&paused_texture));
                ComPtr<IMFMediaBuffer> cached_buffer;CHECK(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),paused_texture.Get(),0,FALSE,&cached_buffer));
                CHECK(MFCreateSample(&last_frame.sample));CHECK(last_frame.sample->AddBuffer(cached_buffer.Get()));
            }
            context->CopySubresourceRegion(paused_texture.Get(),0,0,0,0,texture.Get(),slice,nullptr);last_frame.width=width;last_frame.height=height;
        }
        CHECK(swap->Present(0, 0)); ++presented; return S_OK;
    }
    HRESULT tick_audio() {
        if (!audio_running) return S_OK;
        UINT32 padding; CHECK(audio->GetCurrentPadding(&padding));
        UINT32 count = audio_capacity-padding; if (!count) return S_OK;
        BYTE* raw; CHECK(render->GetBuffer(count, &raw)); auto out = (int16_t*)raw;
        memset(raw, 0, count*4);
        double target = now_seconds() + double(padding)/44100 + audio_latency;
        const float local_gain=shell.gain();
        for (UINT32 f = 0; f < count; ++f) {
            double t = target + double(f)/44100;
            while (!sound.empty()) {
                auto& a = sound.front(); double delta = a.pts + double(a.pos/2)/44100 - t;
                if (delta < -0.010) {
                    size_t skip = std::min(a.pcm.size()-a.pos, size_t((-delta-0.002)*44100)*2);
                    a.pos += skip;
                }
                if (a.pos >= a.pcm.size()) { sound.pop_front(); continue; }
                if (delta > 0.003) break;
                out[2*f] = int16_t(a.pcm[a.pos++]*local_gain); out[2*f+1] = int16_t(a.pcm[a.pos++]*local_gain); ++audio_frames;
                if (a.pos >= a.pcm.size()) sound.pop_front(); break;
            }
        }
        return render->ReleaseBuffer(count, 0);
    }
    HRESULT tick() {
        CHECK(tick_audio());CHECK(chrome.layout(shell)); double n = now_seconds();bool redraw_needed=shell.redraw.exchange(false);
        while (video.size() > 1 && video[1].pts <= n) { video.pop_front(); ++late_frames; }
        if (!video.empty() && video.front().pts <= n) { auto frame = video.front(); video.pop_front(); CHECK(draw(frame.sample.Get(),frame.width,frame.height)); }
        else if(redraw_needed && last_frame.sample){CHECK(draw(last_frame.sample.Get(),last_frame.width,last_frame.height));}
        return S_OK;
    }
};

// DNS allocations are kept alive until asynchronous deregistration completes.
struct Discovery {
    DNS_SERVICE_REGISTER_REQUEST req{}; DNS_SERVICE_CANCEL cancel{};
    PDNS_SERVICE_INSTANCE instance = nullptr; Handle done; std::atomic<DWORD> status{ERROR_IO_PENDING};
    ULONGLONG started=GetTickCount64();
    std::wstring name,host;std::vector<std::wstring> keys,values;std::vector<PCWSTR> key_ptrs,value_ptrs;
    Discovery(){done.h=CreateEventW(nullptr,TRUE,FALSE,nullptr);}
    static void WINAPI callback(DWORD result, void* ctx, PDNS_SERVICE_INSTANCE returned) {
        auto d = static_cast<Discovery*>(ctx); d->status = result;
        if (returned) DnsServiceFreeInstance(returned); SetEvent(d->done.h);
    }
    ~Discovery() { if (instance) DnsServiceFreeInstance(instance); }
};

struct Worker { Handle process, job, input, output, read_event;
    ~Worker(){if(job.h)TerminateJobObject(job.h,0);if(process.h)WaitForSingleObject(process.h,3000);}
};
static bool read_worker(Worker* w, void* dest, DWORD size, ULONGLONG deadline) {
    DWORD total = 0; auto p = static_cast<uint8_t*>(dest);
    while (total < size) {
        OVERLAPPED operation{}; operation.hEvent=w->read_event.h; ResetEvent(operation.hEvent);
        DWORD n=0;
        if(!ReadFile(w->output.h,p+total,size-total,&n,&operation)) {
            if(GetLastError()!=ERROR_IO_PENDING)return false;
            HANDLE wait_handles[]={operation.hEvent,w->process.h};
            ULONGLONG now=GetTickCount64();DWORD timeout=now<deadline?DWORD(deadline-now):0;
            if(WaitForMultipleObjects(2,wait_handles,FALSE,timeout)!=WAIT_OBJECT_0){
                CancelIoEx(w->output.h,&operation);GetOverlappedResult(w->output.h,&operation,&n,TRUE);return false;
            }
            if(!GetOverlappedResult(w->output.h,&operation,&n,FALSE))return false;
        }
        if(!n)return false;
        total += n;
    }
    return true;
}
extern "C" {
const char* am_error() { return last_error.c_str(); }
int am_lan_ipv4(uint8_t* address, uint32_t* index, uint32_t* prefix) {
    ULONG bytes=16384;std::vector<uint8_t> storage(bytes);
    auto adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG status=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,adapters,&bytes);
    if(status==ERROR_BUFFER_OVERFLOW && bytes<=1024*1024){storage.resize(bytes);adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());status=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,adapters,&bytes);}
    if(status!=NO_ERROR){error_hr("LAN adapter query",HRESULT_FROM_WIN32(status));return 0;}
    ComPtr<INetworkListManager> networks;HRESULT hr=CoCreateInstance(CLSID_NetworkListManager,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&networks));
    if(FAILED(hr)){error_hr("network profile query",hr);return 0;}
    for(auto a=adapters;a;a=a->Next){
        if(a->OperStatus!=IfOperStatusUp || (a->IfType!=IF_TYPE_ETHERNET_CSMACD && a->IfType!=IF_TYPE_IEEE80211) || !a->FirstGatewayAddress)continue;
        auto name=wide(a->AdapterName);GUID adapter_id{};if(FAILED(CLSIDFromString(name.c_str(),&adapter_id)))continue;
        ComPtr<IEnumNetworkConnections> connections;if(FAILED(networks->GetNetworkConnections(&connections)))continue;
        bool private_profile=false;for(;;){ComPtr<INetworkConnection> c;ULONG fetched=0;if(connections->Next(1,&c,&fetched)!=S_OK)break;
            GUID id{};if(FAILED(c->GetAdapterId(&id)) || id!=adapter_id)continue;ComPtr<INetwork> network;NLM_NETWORK_CATEGORY category;
            if(SUCCEEDED(c->GetNetwork(&network)) && SUCCEEDED(network->GetCategory(&category)) && category==NLM_NETWORK_CATEGORY_PRIVATE)private_profile=true;
        }
        if(!private_profile)continue;
        for(auto u=a->FirstUnicastAddress;u;u=u->Next){if(u->Address.lpSockaddr->sa_family!=AF_INET)continue;
            auto ip=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);auto b=reinterpret_cast<uint8_t*>(&ip->sin_addr);
            if(!(b[0]==10 || (b[0]==172 && b[1]>=16 && b[1]<=31) || (b[0]==192 && b[1]==168)))continue;
            memcpy(address,b,4);*index=a->IfIndex;*prefix=u->OnLinkPrefixLength;return 1;
        }
    }
    error_hr("No active private Ethernet/Wi-Fi IPv4 LAN with a gateway",E_FAIL);return 0;
}
void am_show_error(const char* message) { auto s=wide(message);MessageBoxW(nullptr,s.c_str(),L"AirMirrorLite error",MB_OK|MB_ICONERROR); }
double am_now() { return now_seconds(); }
void* am_open(int hidden) { auto a = std::make_unique<App>(); HRESULT hr = a->init(hidden != 0); if (FAILED(hr)) { error_hr("Windows initialization", hr); return nullptr; } return a.release(); }
void am_close(void* p) { delete static_cast<App*>(p); }
void am_wake(void* p) { SetEvent(static_cast<App*>(p)->wake.h); }
void* am_wake_handle(void* p) {
    HANDLE handle=nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), static_cast<App*>(p)->wake.h, GetCurrentProcess(), &handle, EVENT_MODIFY_STATE, FALSE, 0)) return nullptr;
    return handle;
}
void am_signal(void* handle) { SetEvent(handle); }
void am_signal_close(void* handle) { CloseHandle(handle); }
void am_title(void* p, const char* title) { auto s = wide(title); static_cast<App*>(p)->shell.set_status(s); }
int am_pump(void* p, unsigned wait_ms) {
    auto a = static_cast<App*>(p); HANDLE hs[] = {a->wake.h, a->audio_event.h};
    WaitForMultipleObjects(2,hs,FALSE,std::min(wait_ms,1000u));
    if (a->shell.quit) return 0;
    HRESULT hr = a->tick(); if (FAILED(hr)) { error_hr("media presentation", hr); return -1; }
    if (a->shell.disconnect.exchange(false)) return 2; return 1;
}
unsigned long long am_presented(void* p){return static_cast<App*>(p)->presented;}
int am_prepare_video(void* p,unsigned codec,unsigned width,unsigned height,unsigned fps){
    auto a=static_cast<App*>(p);if((codec!=264&&codec!=265)||(fps!=30&&fps!=60))return 0;
    a->expected_fps=fps;if(a->decoder_ready&&a->codec==codec)return 1;
    HRESULT hr=a->init_decoder(codec,width,height);if(FAILED(hr)){error_hr("prepare video decoder",hr);return 0;}return 1;
}
int am_hevc_available(void* p) {return static_cast<App*>(p)->hevc_available?1:0;}
int am_video(void* p, const uint8_t* data, unsigned len, double pts, unsigned codec,unsigned width,unsigned height) {
    auto app=static_cast<App*>(p);if(codec!=264&&codec!=265)return 0;
    if(app->codec!=codec||!app->decoder_ready){HRESULT hr=app->init_decoder(codec,width,height);if(FAILED(hr)){error_hr("video codec selection",hr);return 0;}}
    HRESULT hr = app->submit(data, len, pts); if (FAILED(hr)) { error_hr("video decode", hr); return 0; } return 1;
}
int am_video_ready(void* p) {auto a=static_cast<App*>(p);return a->video.size()<(a->codec==265?3:6);}
int am_audio(void* p, const int16_t* pcm, unsigned samples, double pts) {
    auto a = static_cast<App*>(p);
    if (!pcm || samples > 4096 || samples % 2 || !std::isfinite(pts)) {error_hr("invalid PCM sample",E_INVALIDARG);return 0;}
    if (a->sound.size() >= 32) {
        UINT32 padding=0;a->audio->GetCurrentPadding(&padding);char detail[300];
        sprintf_s(detail,"audio queue full padding=%u capacity=%u latency=%.4f now=%.4f first_pts=%.4f played=%llu",padding,a->audio_capacity,a->audio_latency,now_seconds(),a->sound.front().pts,(unsigned long long)a->audio_frames);
        error_hr(detail,E_FAIL);return 0;
    }
    a->sound.push_back({std::vector<int16_t>(pcm, pcm+samples), 0, pts});
    if (!a->audio_running) { HRESULT hr=a->audio->Start(); if (FAILED(hr)) {error_hr("WASAPI start",hr);return 0;} a->audio_running=true; }
    return 1;
}
void am_flush_audio(void* p) {
    auto a=static_cast<App*>(p);a->sound.clear();a->audio->Stop();a->audio->Reset();a->audio_running=false;
}
int am_reset(void* p) {
    auto a = static_cast<App*>(p); am_flush_audio(p); a->video.clear();
    a->last_frame.sample.Reset();
    a->shell.reset_video();
    a->reset_video_decoder();return 1;
}
const char* am_stats(void* p) {
    auto a = static_cast<App*>(p); static thread_local std::string text;
    text = "adapter=" + a->adapter + " decoded=" + std::to_string(a->decoded) + " dxgi_frames=" + std::to_string(a->gpu_frames)
        + " decoder_surfaces=" + std::to_string(a->decoder_surfaces) + " presented=" + std::to_string(a->presented) + " late=" + std::to_string(a->late_frames)
        + " audio_samples=" + std::to_string(a->audio_frames) + " resolution=" + std::to_string(a->width) + "x" + std::to_string(a->height)
        + " decoder_instances="+std::to_string(a->decoder_instances) + " codec=" + std::to_string(a->codec) + " hevc_available=" + std::to_string(a->hevc_available)
        + " local_volume=" + std::to_string(int(a->shell.volume*100)) + " muted=" + std::to_string(a->shell.muted.load());
    return text.c_str();
}
void* am_discover(const char* name, const char* host, unsigned short port, const uint8_t* address, uint32_t interface_index, const char* const* keys, const char* const* values, unsigned count) {
    if (count > 32) return nullptr;
    auto d = std::make_unique<Discovery>();
    if(!d->done.h)return nullptr;
    for(unsigned i=0;i<count;++i){d->keys.push_back(wide(keys[i]));d->values.push_back(wide(values[i]));}
    for(unsigned i=0;i<count;++i){d->key_ptrs.push_back(d->keys[i].c_str());d->value_ptrs.push_back(d->values[i].c_str());}
    d->name=wide(name);d->host=wide(host);
    IP4_ADDRESS ipv4;memcpy(&ipv4,address,4);
    d->instance = DnsServiceConstructInstance(d->name.c_str(),d->host.c_str(),&ipv4,nullptr,port,0,0,count,d->key_ptrs.data(),d->value_ptrs.data());
    if (!d->instance) { error_hr("DNS instance", HRESULT_FROM_WIN32(GetLastError())); return nullptr; }
    d->req.Version = DNS_QUERY_REQUEST_VERSION1; d->req.pServiceInstance = d->instance;
    d->req.InterfaceIndex=interface_index;d->instance->dwInterfaceIndex=interface_index;
    d->req.pRegisterCompletionCallback = Discovery::callback; d->req.pQueryContext = d.get();
    DWORD rc = DnsServiceRegister(&d->req, &d->cancel);
    if (rc != DNS_REQUEST_PENDING) { error_hr("DNS register", HRESULT_FROM_WIN32(rc)); return nullptr; }
    return d.release();
}
int am_discovery_status(void* p){
    auto d=static_cast<Discovery*>(p);
    if(WaitForSingleObject(d->done.h,0)!=WAIT_OBJECT_0){
        if(GetTickCount64()-d->started<10000)return 0;
        error_hr("DNS register timeout",HRESULT_FROM_WIN32(ERROR_TIMEOUT));return -1;
    }
    if(d->status!=ERROR_SUCCESS){error_hr("DNS callback",HRESULT_FROM_WIN32(d->status));return -1;}
    return 1;
}
void am_undiscover(void* p) {
    auto d = static_cast<Discovery*>(p); if (!d) return;
    if(WaitForSingleObject(d->done.h,0)!=WAIT_OBJECT_0){
        DnsServiceRegisterCancel(&d->cancel);
        // Never free callback context while Windows could still be using it.
        // At most two pending contexts live until process exit on OS failure.
        if(WaitForSingleObject(d->done.h,1000)!=WAIT_OBJECT_0)return;
    }
    if(d->status!=ERROR_SUCCESS){delete d;return;}
    ResetEvent(d->done.h); DWORD rc = DnsServiceDeRegister(&d->req, nullptr);
    if (rc == DNS_REQUEST_PENDING && WaitForSingleObject(d->done.h, 5000) != WAIT_OBJECT_0) return;
    delete d;
}
int am_protect(const uint8_t* in, unsigned len, uint8_t* out, unsigned cap, int decrypt) {
    if (len > 4096) return -1; DATA_BLOB input{len, const_cast<BYTE*>(in)}, output{};
    BOOL ok = decrypt ? CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                      : CryptProtectData(&input, L"AirMirrorLite identity", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) return -1;
    int n = output.cbData <= cap ? int(output.cbData) : -1;
    if (n >= 0) memcpy(out, output.pbData, n); SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return n;
}
int am_is_sandboxed() {
    Handle token; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.h)) return 0;
    DWORD flag=0, bytes=0; if (!GetTokenInformation(token.h, TokenIsAppContainer, &flag, sizeof(flag), &bytes) || !flag) return 0;
    DWORD needed=0; GetTokenInformation(token.h, TokenCapabilities, nullptr, 0, &needed);
    std::vector<uint8_t> storage(needed); if (!GetTokenInformation(token.h, TokenCapabilities, storage.data(), needed, &needed)) return 0;
    return reinterpret_cast<TOKEN_GROUPS*>(storage.data())->GroupCount == 0;
}
void* am_worker_start() {
    auto w = std::make_unique<Worker>();
    PSID sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(L"AirMirrorLite.Codec", L"AirMirrorLite codec", L"No network capabilities", nullptr, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = DeriveAppContainerSidFromAppContainerName(L"AirMirrorLite.Codec", &sid);
    if (FAILED(hr)) { error_hr("AppContainer profile", hr); return nullptr; }
    struct SidGuard { PSID s; ~SidGuard() { FreeSid(s); } } sg{sid};
    wchar_t path[32768]; DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return nullptr;
    // Grant this worker identity read/execute access only to this exact executable.
    PACL oldacl = nullptr, newacl = nullptr; PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD rc = GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) { error_hr("worker executable ACL read", HRESULT_FROM_WIN32(rc)); return nullptr; }
    EXPLICIT_ACCESSW access{}; access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    access.grfAccessMode = GRANT_ACCESS; access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.ptstrName = (LPWSTR)sid;
    rc = SetEntriesInAclW(1, &access, oldacl, &newacl);
    if (rc == ERROR_SUCCESS) rc = SetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, newacl, nullptr);
    if (newacl) LocalFree(newacl); if (sd) LocalFree(sd);
    if (rc != ERROR_SUCCESS) { error_hr("worker executable ACL grant", HRESULT_FROM_WIN32(rc)); return nullptr; }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; Handle child_in, child_out;
    if (!CreatePipe(&child_in.h, &w->input.h, &sa, 65536)) return nullptr;
    GUID pipe_id{};if(FAILED(CoCreateGuid(&pipe_id)))return nullptr;wchar_t guid[40];StringFromGUID2(pipe_id,guid,40);
    std::wstring pipe_name=L"\\\\.\\pipe\\AirMirrorLite-"+std::to_wstring(GetCurrentProcessId())+L"-"+guid;
    w->output.h=CreateNamedPipeW(pipe_name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,nullptr);
    if(w->output.h==INVALID_HANDLE_VALUE)return nullptr;
    child_out.h=CreateFileW(pipe_name.c_str(),GENERIC_WRITE,0,&sa,OPEN_EXISTING,0,nullptr);
    if(child_out.h==INVALID_HANDLE_VALUE)return nullptr;
    w->read_event.h=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!w->read_event.h)return nullptr;
    OVERLAPPED connection{};connection.hEvent=w->read_event.h;
    if(!ConnectNamedPipe(w->output.h,&connection) && GetLastError()!=ERROR_PIPE_CONNECTED)return nullptr;
    SetHandleInformation(w->input.h, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(w->output.h, HANDLE_FLAG_INHERIT, 0);
    w->job.h = CreateJobObjectW(nullptr, nullptr); if (!w->job.h) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1; limits.ProcessMemoryLimit = 256*1024*1024;
    if (!SetInformationJobObject(w->job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return nullptr;
    SIZE_T bytes=0; InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes); std::vector<uint8_t> storage(bytes);
    auto attrs = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 2, 0, &bytes)) return nullptr;
    struct AttrGuard { PPROC_THREAD_ATTRIBUTE_LIST p; ~AttrGuard(){DeleteProcThreadAttributeList(p);} } ag{attrs};
    SECURITY_CAPABILITIES caps{}; caps.AppContainerSid = sid;
    HANDLE handles[] = {child_in.h, child_out.h};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &caps, sizeof(caps), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) return nullptr;
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); si.lpAttributeList = attrs;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = child_in.h;
    si.StartupInfo.hStdOutput = child_out.h; si.StartupInfo.hStdError = child_out.h;
    std::wstring cmd = L"\"" + std::wstring(path) + L"\" --codec-worker";
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path, cmd.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, L"C:\\Windows\\System32", &si.StartupInfo, &pi)) {
        error_hr("AppContainer launch", HRESULT_FROM_WIN32(GetLastError())); return nullptr;
    }
    w->process.h = pi.hProcess; Handle thread; thread.h = pi.hThread;
    if (!AssignProcessToJobObject(w->job.h, w->process.h)) { TerminateProcess(w->process.h, 1); return nullptr; }
    if (ResumeThread(thread.h) == DWORD(-1)) { TerminateProcess(w->process.h, 1); return nullptr; }
    uint32_t hello=0;
    if (!read_worker(w.get(), &hello, 4, GetTickCount64()+5000) || hello != 0x414d5342) {
        error_hr("AppContainer handshake", E_ACCESSDENIED); return nullptr;
    }
    return w.release();
}
void am_worker_close(void* p) { delete static_cast<Worker*>(p); }
int am_worker_call(void* p, unsigned op, const uint8_t* input, unsigned len, uint8_t* output, unsigned cap) {
    auto w = static_cast<Worker*>(p); if (len > 8192 || cap > 16384) return -1;
    uint32_t header[2] = {op, len}; DWORD n;
    if (!WriteFile(w->input.h, header, sizeof(header), &n, nullptr) || n != sizeof(header)) return -1;
    if (len && (!WriteFile(w->input.h, input, len, &n, nullptr) || n != len)) return -1;
    uint32_t result=0; ULONGLONG deadline = GetTickCount64()+2000;
    if (!read_worker(w, &result, 4, deadline) || result > cap) { TerminateProcess(w->process.h, 1); return -1; }
    if (!read_worker(w, output, result, deadline)) { TerminateProcess(w->process.h, 1); return -1; }
    return int(result);
}
}
#include "probe.inc"
