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
#include <dxgi1_3.h>
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
#include <limits>
#include "platform_support.h"
using Microsoft::WRL::ComPtr;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { error_hr(#x, hr_); return hr_; } } while (0)

static double now_seconds() {
    static const double frequency=[] {LARGE_INTEGER f;QueryPerformanceFrequency(&f);return double(f.QuadPart);}();
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return double(q.QuadPart) / frequency;
}
static bool valid_media_time(double pts) {
    // A finite double can still overflow Media Foundation's signed 100-ns clock.
    return std::isfinite(pts) && pts>=0 && pts<double(std::numeric_limits<LONGLONG>::max())/1e7;
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
struct VideoColor {UINT32 matrix=MFVideoTransferMatrix_BT709,range=MFNominalRange_16_235;};
static HRESULT source_area(UINT w,UINT h,const MFVideoArea* area,RECT& source){
    source={0,0,LONG(w),LONG(h)};if(!area)return S_OK;
    if(area->OffsetX.fract||area->OffsetY.fract||area->OffsetX.value<0||area->OffsetY.value<0||area->Area.cx<=0||area->Area.cy<=0)return MF_E_INVALIDMEDIATYPE;
    const int64_t right=int64_t(area->OffsetX.value)+area->Area.cx,bottom=int64_t(area->OffsetY.value)+area->Area.cy;
    if(right>w||bottom>h)return MF_E_INVALIDMEDIATYPE;
    source={area->OffsetX.value,area->OffsetY.value,LONG(right),LONG(bottom)};return S_OK;
}
static HRESULT media_color(IMFAttributes* attrs,VideoColor& color){
    color={};UINT32 value=0;
    if(SUCCEEDED(attrs->GetUINT32(MF_MT_YUV_MATRIX,&value))&&value){if(value!=MFVideoTransferMatrix_BT709&&value!=MFVideoTransferMatrix_BT601)return MF_E_INVALIDMEDIATYPE;color.matrix=value;}
    if(SUCCEEDED(attrs->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,&value))&&value){if(value!=MFNominalRange_0_255&&value!=MFNominalRange_16_235)return MF_E_INVALIDMEDIATYPE;color.range=value;}
    return S_OK;
}
struct VideoFrame { ComPtr<IMFSample> sample; double pts; UINT width; UINT height; RECT source{};VideoColor color{}; };
struct AudioFrame { std::vector<int16_t> pcm; size_t pos = 0; double pts = 0; };
static void mix_audio(std::deque<AudioFrame>& queue,int16_t* out,UINT32 count,double target,float gain,uint64_t& played,uint64_t& skipped){
    memset(out,0,size_t(count)*4);
        for (UINT32 f = 0; f < count; ++f) {
            double t = target + double(f)/44100;
            while (!queue.empty()) {
                auto& a = queue.front(); double delta = a.pts + double(a.pos/2)/44100 - t;
                if (delta < -0.010) {
                    // Clamp before conversion; an untrusted timestamp must not overflow size_t.
                    const double skip=std::min(double((a.pcm.size()-a.pos)/2),(-delta-0.002)*44100);
                    a.pos += size_t(skip)*2;skipped+=size_t(skip);
                }
                if (a.pos >= a.pcm.size()) { queue.pop_front(); continue; }
                if (delta > 0.003) break;
                out[2*f] = int16_t(a.pcm[a.pos++]*gain); out[2*f+1] = int16_t(a.pcm[a.pos++]*gain); ++played;
                if (a.pos >= a.pcm.size()) queue.pop_front(); break;
            }
        }
}
struct StageTiming {
    uint64_t count=0;double total=0,minimum=0,maximum=0;
    std::array<uint64_t,65> histogram{};
    void add(double value){if(!count)minimum=maximum=value;else {minimum=std::min(minimum,value);maximum=std::max(maximum,value);}++count;total+=value;if(value>=0)++histogram[size_t(std::min(value*2000,64.0))];}
    double average()const{return count?total/count:0;}
    double percentile(double quantile)const{uint64_t seen=0,target=uint64_t(std::ceil(count*quantile));if(!target)return 0;for(size_t i=0;i<histogram.size();++i){seen+=histogram[i];if(seen>=target)return i==64?maximum:std::min(maximum,(i+1)/2000.0);}return maximum;}
};
template<class Fill,class Start>static HRESULT prime_audio(Fill fill,Start start){HRESULT hr=fill();return SUCCEEDED(hr)?start():hr;}
class EndpointNotice final:public IMMNotificationClient {
    std::atomic<ULONG> references{1};
public:
    Handle wake;std::atomic<bool> changed{false};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result)override{if(!result)return E_POINTER;*result=nullptr;if(iid==__uuidof(IUnknown)||iid==__uuidof(IMMNotificationClient)){*result=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references;}
    ULONG STDMETHODCALLTYPE Release()override{ULONG left=--references;if(!left)delete this;return left;}
    void notify(){changed=true;SetEvent(wake.h);}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,ERole role,LPCWSTR)override{if(flow==eRender&&role==eConsole)notify();return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD)override{notify();return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR)override{notify();return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR)override{notify();return S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY)override{return S_OK;}
};
#include "phone_compositor.h"
struct App {
    HWND window = nullptr; Handle wake, audio_event, video_event, presentation_timer;
    PhoneShell shell;
    bool hevc_available=false,decoder_ready=false;UINT codec=264,expected_fps=60;
    PhoneCompositor chrome;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap; ComPtr<ID3D11VideoDevice> vdevice; ComPtr<ID3D11VideoContext> vcontext;
    ComPtr<IMFDXGIDeviceManager> manager; ComPtr<IMFTransform> decoder,h264_cache,hevc_cache;
    ComPtr<ID3D11VideoProcessorEnumerator> venum; ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<IAudioClient> audio; ComPtr<IAudioRenderClient> render;
    ComPtr<IMMDeviceEnumerator> audio_enumerator;ComPtr<EndpointNotice> endpoint_notice;bool endpoint_registered=false;
    std::wstring audio_endpoint_id;
    bool audio_pending=false,graphics_pending=false,external_unavailable=false;unsigned audio_retries=0,graphics_retries=0;double audio_retry_at=0,graphics_retry_at=0;
    uint64_t audio_recoveries=0,graphics_recoveries=0,audio_silence=0,audio_skips=0;double audio_event_observed=0;
    StageTiming audio_service_gap,audio_fill_time,audio_event_service,audio_queue_age;double last_audio_service=0;
    UINT32 audio_capacity = 0; double audio_latency = 0; bool audio_running = false;
    UINT width = 0, height = 0, vieww = 0, viewh = 0,processor_width=0,processor_height=0;
    RECT output_aperture{};VideoColor output_color{};
    UINT source_width=0,source_height=0,presented_width=0,presented_height=0;
    uint64_t source_shape_changes=0,presented_shape_changes=0;
    std::deque<VideoFrame> video; std::deque<AudioFrame> sound;
    VideoFrame last_frame{};
    bool video_slot_reserved=false,video_sync=true;
    bool high_resolution_timer=false;
    double present_retry_at=0;
    ComPtr<ID3D11Texture2D> paused_texture;
    uint64_t decoder_instances=0;
    uint64_t decoded = 0, presented = 0, gpu_frames = 0, decoder_surfaces = 0, late_frames = 0, audio_frames = 0, underruns = 0;
    size_t video_high_water=0,audio_high_water=0;
    uint64_t video_waits=0,present_busy=0,submit_calls=0,interactive_skips=0,minimized_skips=0;
    double submit_seconds=0,submit_max_seconds=0,present_seconds=0,present_max_seconds=0;
    StageTiming output_ahead,presentation_late,decoder_residence,input_prepare;
    struct DecodePending {LONGLONG pts=0;double began=0;bool active=false;};
    std::array<DecodePending,32> decode_pending{};
    HRESULT low_latency_set=E_NOINTERFACE,low_latency_get=E_NOINTERFACE;
    int low_latency_value=-1,low_latency_attribute=-1;
    std::string adapter; bool com = false, mf = false;
    ~App() {
        if(endpoint_registered)audio_enumerator->UnregisterEndpointNotificationCallback(endpoint_notice.Get());endpoint_notice.Reset();audio_enumerator.Reset();
        if (audio) audio->Stop();
        sound.clear(); video.clear(); last_frame.sample.Reset(); paused_texture.Reset();decoder.Reset();h264_cache.Reset();hevc_cache.Reset(); render.Reset(); audio.Reset();
        processor.Reset(); venum.Reset(); manager.Reset(); swap.Reset(); vcontext.Reset(); vdevice.Reset(); context.Reset(); device.Reset();
        chrome=PhoneCompositor{};shell.shutdown();
        if (mf) MFShutdown(); if (com) CoUninitialize();
    }
    HRESULT init_audio_notifications(){
        CHECK(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&audio_enumerator)));
        endpoint_notice.Attach(new EndpointNotice);
        if(!DuplicateHandle(GetCurrentProcess(),wake.h,GetCurrentProcess(),&endpoint_notice->wake.h,EVENT_MODIFY_STATE,FALSE,0))return HRESULT_FROM_WIN32(GetLastError());
        CHECK(audio_enumerator->RegisterEndpointNotificationCallback(endpoint_notice.Get()));endpoint_registered=true;return S_OK;
    }
    HRESULT init(bool hidden) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); com = true;
        CHECK(MFStartup(MF_VERSION, MFSTARTUP_FULL)); mf = true;
        wake.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        audio_event.h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake.h || !audio_event.h) return HRESULT_FROM_WIN32(GetLastError());
        presentation_timer.h=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
        high_resolution_timer=presentation_timer.h!=nullptr;
        if(!presentation_timer.h)presentation_timer.h=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        if(!presentation_timer.h)return HRESULT_FROM_WIN32(GetLastError());
        CHECK(shell.create(wake.h,hidden));window=shell.window;
        CHECK(init_graphics());CHECK(init_audio_notifications());CHECK(init_audio());
        if (!hidden) shell.show();
        return S_OK;
    }
    HRESULT init_graphics(){
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
        sc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        CHECK(factory->CreateSwapChainForComposition(device.Get(), &sc, nullptr, &swap));
        ComPtr<IDXGISwapChain2> paced_swap;CHECK(swap.As(&paced_swap));CHECK(paced_swap->SetMaximumFrameLatency(1));
        video_event.h=paced_swap->GetFrameLatencyWaitableObject();if(!video_event.h)return E_UNEXPECTED;
        CHECK(chrome.init(device.Get(),factory.Get(),window,swap.Get()));CHECK(chrome.layout(shell));
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        UINT token; CHECK(MFCreateDXGIDeviceManager(&token, &manager)); CHECK(manager->ResetDevice(device.Get(), token));
        {
            ComPtr<IMFTransform> probe;ComPtr<IMFAttributes> attrs;UINT32 aware=0;
            D3D11_VIDEO_DECODER_DESC capability_desc{D3D11_DECODER_PROFILE_HEVC_VLD_MAIN,3840,2160,DXGI_FORMAT_NV12};UINT configs=0;
            hevc_available=SUCCEEDED(create_hevc_transform(false,&probe))&&SUCCEEDED(probe->GetAttributes(&attrs))
                &&SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE,&aware))&&aware
                &&SUCCEEDED(vdevice->GetVideoDecoderConfigCount(&capability_desc,&configs))&&configs>0;
            if(hevc_available){hevc_cache=probe;++decoder_instances;}
        }
        CHECK(init_decoder());
        return S_OK;
    }
    HRESULT init_audio(){
        render.Reset();audio.Reset();audio_running=false;audio_capacity=0;
        ComPtr<IMMDevice> endpoint; CHECK(audio_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint));
        LPWSTR id=nullptr;CHECK(endpoint->GetId(&id));audio_endpoint_id=id;CoTaskMemFree(id);
        CHECK(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, &audio));
        WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 176400;
        CHECK(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0, 0, &format, nullptr));
        CHECK(audio->SetEventHandle(audio_event.h)); CHECK(audio->GetBufferSize(&audio_capacity));
        REFERENCE_TIME latency; CHECK(audio->GetStreamLatency(&latency)); audio_latency = latency / 1e7;
        CHECK(audio->GetService(IID_PPV_ARGS(&render)));
        return S_OK;
    }
    HRESULT flush_audio(){
        sound.clear();audio_running=false;last_audio_service=0;audio_event_observed=0;
        if(!audio)return S_OK;HRESULT stopped=audio->Stop();if(FAILED(stopped))return stopped;return audio->Reset();
    }
    void release_audio(){if(audio)audio->Stop();sound.clear();audio_running=false;render.Reset();audio.Reset();audio_endpoint_id.clear();audio_capacity=0;last_audio_service=0;audio_event_observed=0;}
    bool default_audio_changed(){
        if(audio_pending||!audio)return true;ComPtr<IMMDevice> current;
        if(FAILED(audio_enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&current)))return true;
        DWORD state=0;if(FAILED(current->GetState(&state))||state!=DEVICE_STATE_ACTIVE)return true;
        LPWSTR id=nullptr;if(FAILED(current->GetId(&id)))return true;bool changed=audio_endpoint_id!=id;CoTaskMemFree(id);return changed;
    }
    void release_graphics(){
        video.clear();last_frame.sample.Reset();paused_texture.Reset();decoder.Reset();h264_cache.Reset();hevc_cache.Reset();decoder_ready=false;decode_pending={};
        chrome=PhoneCompositor{};processor.Reset();venum.Reset();manager.Reset();swap.Reset();vcontext.Reset();vdevice.Reset();context.Reset();device.Reset();
        if(video_event.h){CloseHandle(video_event.h);video_event.h=nullptr;}video_slot_reserved=false;vieww=viewh=width=height=0;present_retry_at=0;
    }
    static bool audio_fault(HRESULT hr){return hr==AUDCLNT_E_DEVICE_INVALIDATED||hr==AUDCLNT_E_RESOURCES_INVALIDATED||hr==AUDCLNT_E_SERVICE_NOT_RUNNING;}
    static bool graphics_fault(HRESULT hr){return hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET||hr==DXGI_ERROR_DEVICE_HUNG||hr==D2DERR_RECREATE_TARGET;}
    void recovery_status(int kind,const wchar_t* reason){if(!external_unavailable)shell.set_status(kind,reason);}
    bool recover_fault(HRESULT hr){
        if(!audio_fault(hr)&&!graphics_fault(hr)&&device){HRESULT removed=device->GetDeviceRemovedReason();if(FAILED(removed))hr=DXGI_ERROR_DEVICE_REMOVED;}
        if(audio_fault(hr)){release_audio();audio_pending=true;audio_retries=0;audio_retry_at=now_seconds();}
        else if(graphics_fault(hr)){release_graphics();graphics_pending=true;graphics_retries=0;graphics_retry_at=now_seconds();}
        else return false;
        shell.reset_video();shell.disconnect=true;recovery_status(3,audio_fault(hr)?L"Audio device changed. Reconnecting…":L"Graphics device changed. Reconnecting…");return true;
    }
    void service_recovery(bool synthetic_failure=false){
        // Notifications are coalesced; only the owning thread queries the render/default route.
        // Capture or unrelated-device changes must not disconnect a healthy render session.
        if(endpoint_notice&&endpoint_notice->changed.exchange(false)&&default_audio_changed())recover_fault(AUDCLNT_E_DEVICE_INVALIDATED);
        const double now=now_seconds();bool recovered=false;
        if(audio_pending&&audio_retries<3&&now>=audio_retry_at){
            ++audio_retries;HRESULT hr=synthetic_failure?E_FAIL:init_audio();
            if(SUCCEEDED(hr)){audio_pending=false;++audio_recoveries;recovered=true;}
            else {release_audio();audio_retry_at=now+.25*audio_retries;recovery_status(audio_retries==3?5:3,L"Audio unavailable. Reconnect an output device, then reconnect mirroring.");}
        }
        if(graphics_pending&&graphics_retries<3&&now>=graphics_retry_at){
            ++graphics_retries;HRESULT hr=synthetic_failure?E_FAIL:init_graphics();
            if(SUCCEEDED(hr)){graphics_pending=false;++graphics_recoveries;recovered=true;}
            else {release_graphics();graphics_retry_at=now+.25*graphics_retries;recovery_status(graphics_retries==3?5:3,L"Graphics unavailable. Reconnect mirroring to retry, or restart the receiver.");}
        }
        if(recovered&&!audio_pending&&!graphics_pending)recovery_status(6,L"");
    }
    void reset_video_decoder(){
        video.clear();last_frame.sample.Reset();paused_texture.Reset();processor.Reset();venum.Reset();
        decode_pending={};present_retry_at=0;CancelWaitableTimer(presentation_timer.h);
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
        low_latency_set=low_latency_get=E_NOINTERFACE;low_latency_value=low_latency_attribute=-1;
        ComPtr<ICodecAPI> codec_api; if (SUCCEEDED(decoder.As(&codec_api))) {
            VARIANT v; VariantInit(&v);
            if(codec==264){v.vt=VT_UI4;v.ulVal=TRUE;}else {v.vt=VT_BOOL;v.boolVal=VARIANT_TRUE;}
            low_latency_set=codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
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
        CHECK(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
        UINT32 enabled=0;if(SUCCEEDED(attributes->GetUINT32(MF_LOW_LATENCY,&enabled)))low_latency_attribute=enabled!=0;
        if(codec_api){VARIANT value;VariantInit(&value);low_latency_get=codec_api->GetValue(&CODECAPI_AVLowLatencyMode,&value);
            if(SUCCEEDED(low_latency_get)){if(value.vt==VT_UI4)low_latency_value=value.ulVal!=0;else if(value.vt==VT_BOOL)low_latency_value=value.boolVal!=VARIANT_FALSE;}VariantClear(&value);}
        decoder_ready=true; return S_OK;
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
            HRESULT area=t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,reinterpret_cast<UINT8*>(&aperture),sizeof(aperture),&size);
            if(SUCCEEDED(area)&&size!=sizeof(aperture))return MF_E_INVALIDMEDIATYPE;
            if(FAILED(area)&&area!=MF_E_ATTRIBUTENOTFOUND)return area;
            CHECK(source_area(w,h,SUCCEEDED(area)?&aperture:nullptr,output_aperture));
            width=output_aperture.right-output_aperture.left;height=output_aperture.bottom-output_aperture.top;CHECK(media_color(t.Get(),output_color));
            video.clear(); last_frame.sample.Reset(); processor.Reset(); venum.Reset(); return S_OK;
        }
        return MF_E_INVALIDMEDIATYPE;
    }
    void queue_video_frame(VideoFrame frame) {
        if(source_width!=frame.width||source_height!=frame.height){source_width=frame.width;source_height=frame.height;++source_shape_changes;}
        if(video.size()==(codec==265?4:8)){video.pop_front();if(IsIconic(window))++minimized_skips;else if(video_sync)++late_frames;else ++interactive_skips;}
        video.push_back(std::move(frame));video_high_water=std::max(video_high_water,video.size());
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
            const double ready=now_seconds();output_ahead.add(pts/1e7-ready);
            for(auto& pending:decode_pending)if(pending.active&&pending.pts==pts){decoder_residence.add(ready-pending.began);pending.active=false;break;}
            queue_video_frame({s,pts/1e7,width,height,output_aperture,output_color});
        }
        return E_UNEXPECTED;
    }
    HRESULT submit(const uint8_t* bytes, unsigned len, double pts) {
        if(!decoder_ready||!decoder)return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (!bytes || !len || len > (codec==265?4u:2u)*1024*1024 || !valid_media_time(pts)) return E_INVALIDARG;
        const double began=now_seconds();
        ComPtr<IMFMediaBuffer> buffer; CHECK(MFCreateMemoryBuffer(len, &buffer)); BYTE* p;
        CHECK(buffer->Lock(&p, nullptr, nullptr)); memcpy(p, bytes, len); buffer->Unlock(); CHECK(buffer->SetCurrentLength(len));
        ComPtr<IMFSample> sample; CHECK(MFCreateSample(&sample)); CHECK(sample->AddBuffer(buffer.Get()));
        CHECK(sample->SetSampleTime((LONGLONG)(pts*1e7))); CHECK(sample->SetSampleDuration(10000000/expected_fps));
        input_prepare.add(now_seconds()-began);
        double accepted=now_seconds();HRESULT hr = decoder->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) { CHECK(output());accepted=now_seconds();hr = decoder->ProcessInput(0, sample.Get(), 0); }
        CHECK(hr);
        auto slot=std::find_if(decode_pending.begin(),decode_pending.end(),[](const DecodePending& entry){return !entry.active;});
        if(slot==decode_pending.end())slot=std::min_element(decode_pending.begin(),decode_pending.end(),[](const DecodePending& a,const DecodePending& b){return a.began<b.began;});
        *slot={LONGLONG(pts*1e7),accepted,true};CHECK(output());
        const double elapsed=now_seconds()-began;++submit_calls;submit_seconds+=elapsed;submit_max_seconds=std::max(submit_max_seconds,elapsed);return S_OK;
    }
    HRESULT draw(IMFSample* frame, UINT visible_width, UINT visible_height,ID3D11Texture2D* synthetic_probe_copy=nullptr,const RECT* aperture=nullptr,const VideoColor* metadata=nullptr,bool synthetic_busy=false) {
        CHECK(chrome.layout(shell));UINT vw=chrome.video_width,vh=chrome.video_height;
        if (!vw || !vh || !visible_width || !visible_height) return S_OK;
        if (vieww != vw || viewh != vh) {
            context->ClearState(); CHECK(swap->ResizeBuffers(0, vw, vh, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)); vieww = vw; viewh = vh;processor.Reset();venum.Reset();
        }
        ComPtr<IMFMediaBuffer> b; CHECK(frame->GetBufferByIndex(0, &b)); ComPtr<IMFDXGIBuffer> dxgi; CHECK(b.As(&dxgi));
        ComPtr<ID3D11Texture2D> texture; CHECK(dxgi->GetResource(IID_PPV_ARGS(&texture))); UINT slice; CHECK(dxgi->GetSubresourceIndex(&slice));
        D3D11_TEXTURE2D_DESC source_desc{};texture->GetDesc(&source_desc);
        RECT src=aperture&&aperture->right>aperture->left?*aperture:RECT{0,0,LONG(visible_width),LONG(visible_height)};
        if(src.left<0||src.top<0||src.right>LONG(source_desc.Width)||src.bottom>LONG(source_desc.Height)
            ||src.right-src.left!=LONG(visible_width)||src.bottom-src.top!=LONG(visible_height))return MF_E_INVALIDMEDIATYPE;
        if(processor&&(processor_width!=source_desc.Width||processor_height!=source_desc.Height)){processor.Reset();venum.Reset();}
        if (!processor) {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{}; desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            desc.InputWidth = source_desc.Width; desc.InputHeight = source_desc.Height; desc.OutputWidth = vw; desc.OutputHeight = vh;
            desc.InputFrameRate = {60, 1}; desc.OutputFrameRate = {60, 1}; desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            CHECK(vdevice->CreateVideoProcessorEnumerator(&desc, &venum)); CHECK(vdevice->CreateVideoProcessor(venum.Get(), 0, &processor));
            processor_width=source_desc.Width;processor_height=source_desc.Height;
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; iv.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> input; CHECK(vdevice->CreateVideoProcessorInputView(texture.Get(), venum.Get(), &iv, &input));
        ComPtr<ID3D11Texture2D> target; CHECK(swap->GetBuffer(0, IID_PPV_ARGS(&target)));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{}; ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output_view; CHECK(vdevice->CreateVideoProcessorOutputView(target.Get(), venum.Get(), &ov, &output_view));
        double scale = std::min(double(vw)/visible_width, double(vh)/visible_height);
        LONG dw = LONG(visible_width*scale), dh = LONG(visible_height*scale);
        RECT dst{(LONG(vw)-dw)/2, (LONG(vh)-dh)/2, (LONG(vw)+dw)/2, (LONG(vh)+dh)/2};
        D3D11_VIDEO_COLOR bg{}; bg.RGBA.A = 1;
        vcontext->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &bg);
        vcontext->VideoProcessorSetOutputAlphaFillMode(processor.Get(),D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE,0);
        vcontext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vcontext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
        vcontext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst);
        vcontext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        const VideoColor color=metadata?*metadata:VideoColor{};
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};cs.YCbCr_Matrix=color.matrix==MFVideoTransferMatrix_BT709;
        cs.Nominal_Range=color.range==MFNominalRange_0_255?D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255:D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vcontext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &cs);
        D3D11_VIDEO_PROCESSOR_STREAM stream{}; stream.Enable = TRUE; stream.pInputSurface = input.Get();
        HRESULT blt=vcontext->VideoProcessorBlt(processor.Get(), output_view.Get(), 0, 1, &stream);
        if(FAILED(blt)){
            D3D11_TEXTURE2D_DESC td{},od{};texture->GetDesc(&td);target->GetDesc(&od);
            char details[256];sprintf_s(details,"VideoProcessorBlt input=%ux%u fmt=%u array=%u slice=%u visible=%ux%u output=%ux%u fmt=%u",td.Width,td.Height,td.Format,td.ArraySize,slice,visible_width,visible_height,od.Width,od.Height,od.Format);
            error_hr(details,blt);return blt;
        }
        // Keep a GPU copy for paused-window redraw without retaining a decoder-pool surface.
        if(texture.Get()!=paused_texture.Get()) {
            D3D11_TEXTURE2D_DESC cached_desc{};
            if(paused_texture)paused_texture->GetDesc(&cached_desc);
            if(!paused_texture || cached_desc.Width!=source_desc.Width || cached_desc.Height!=source_desc.Height) {
                last_frame.sample.Reset();paused_texture.Reset();auto copy_desc=source_desc;copy_desc.ArraySize=1;copy_desc.MipLevels=1;copy_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;copy_desc.MiscFlags=0;
                CHECK(device->CreateTexture2D(&copy_desc,nullptr,&paused_texture));
                ComPtr<IMFMediaBuffer> cached_buffer;CHECK(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),paused_texture.Get(),0,FALSE,&cached_buffer));
                CHECK(MFCreateSample(&last_frame.sample));CHECK(last_frame.sample->AddBuffer(cached_buffer.Get()));
            }
            context->CopySubresourceRegion(paused_texture.Get(),0,0,0,0,texture.Get(),slice,nullptr);
        }
        last_frame.width=visible_width;last_frame.height=visible_height;last_frame.source=src;last_frame.color=color;
        if(synthetic_probe_copy){
            // Private synthetic-test hook only. Copy before Present rotates the logical flip buffer.
            // No exported capture API or runtime caller supplies this argument.
            D3D11_TEXTURE2D_DESC copy_desc{},target_desc{};synthetic_probe_copy->GetDesc(&copy_desc);target->GetDesc(&target_desc);
            if(copy_desc.Width!=target_desc.Width||copy_desc.Height!=target_desc.Height||copy_desc.Format!=target_desc.Format
                ||copy_desc.Usage!=D3D11_USAGE_STAGING||copy_desc.CPUAccessFlags!=D3D11_CPU_ACCESS_READ)return E_INVALIDARG;
            context->CopyResource(synthetic_probe_copy,target.Get());
        }
        // Audio shares this thread: a display queue must never block its deadline.
        const double began=now_seconds();HRESULT present=synthetic_busy?DXGI_ERROR_WAS_STILL_DRAWING:swap->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
        const double elapsed=now_seconds()-began;present_seconds+=elapsed;present_max_seconds=std::max(present_max_seconds,elapsed);
        // A failed nonblocking Present did not enqueue a frame or consume our slot.
        // Keep the reservation and retry on the next normal pump; DXGI need not signal it again.
        if(present==DXGI_ERROR_WAS_STILL_DRAWING){++present_busy;present_retry_at=now_seconds()+.001;return S_FALSE;}
        if(SUCCEEDED(present)&&present!=S_OK){++present_busy;present_retry_at=now_seconds()+.05;return S_FALSE;}
        video_slot_reserved=false;present_retry_at=0;
        CHECK(present);shell.video_size(visible_width,visible_height);if(presented_width!=visible_width||presented_height!=visible_height){presented_width=visible_width;presented_height=visible_height;++presented_shape_changes;}
        ++presented; return S_OK;
    }
    HRESULT fill_audio() {
        if(!audio||!render)return AUDCLNT_E_DEVICE_INVALIDATED;
        const double began=now_seconds();if(last_audio_service)audio_service_gap.add(began-last_audio_service);last_audio_service=began;
        if(!sound.empty())audio_queue_age.add(began-(sound.front().pts+double(sound.front().pos/2)/44100));
        if(audio_event_observed){audio_event_service.add(began-audio_event_observed);audio_event_observed=0;}
        UINT32 padding; CHECK(audio->GetCurrentPadding(&padding));
        if(padding>audio_capacity)return E_UNEXPECTED;
        UINT32 count = audio_capacity-padding; if (!count) return S_OK;
        BYTE* raw; CHECK(render->GetBuffer(count, &raw)); auto out = (int16_t*)raw;
        double target = now_seconds() + double(padding)/44100 + audio_latency;
        const float local_gain=shell.gain();
        const uint64_t before=audio_frames;
        mix_audio(sound,out,count,target,local_gain,audio_frames,audio_skips);
        HRESULT hr=render->ReleaseBuffer(count,0);
        if(SUCCEEDED(hr))audio_silence+=count-(audio_frames-before);
        else audio_frames=before; // A rejected buffer was never submitted to the endpoint.
        audio_fill_time.add(now_seconds()-began);return hr;
    }
    HRESULT tick_audio(){return audio_running?fill_audio():S_OK;}
    HRESULT start_audio(){HRESULT hr=prime_audio([&]{return fill_audio();},[&]{return audio->Start();});if(SUCCEEDED(hr))audio_running=true;return hr;}
    HRESULT wait_for_media(unsigned wait_ms) {
        if(shell.quit||shell.disconnect)return S_OK;
        HANDLE handles[4]={wake.h,audio_event.h};DWORD count=2,timeout=std::min(wait_ms,1000u);
        DWORD slot_index=MAXDWORD;double deadline=0;const double now=now_seconds();
        const bool visible=!IsIconic(window);
        const bool due=visible&&!video.empty()&&(!video_sync||video.front().pts<=now);
        const bool redraw=visible&&shell.redraw&&last_frame.sample;
        if(due||redraw){
            if(video_slot_reserved){if(present_retry_at>now)deadline=present_retry_at;else timeout=0;}
            else {slot_index=count;handles[count++]=video_event.h;}
        } else if(visible&&!video.empty())deadline=video.front().pts;
        auto earlier=[&](double value){if(!deadline||value<deadline)deadline=value;};
        if(audio_pending&&audio_retries<3)earlier(audio_retry_at);if(graphics_pending&&graphics_retries<3)earlier(graphics_retry_at);
        if(visible&&chrome.pending)earlier(chrome.retry_at);
        if(deadline&&deadline<=now)timeout=0;
        bool armed=false;
        if(deadline>now&&timeout){
            // Relative one-shot timer: QPC selects the deadline; tick verifies it again before drawing.
            LARGE_INTEGER relative;relative.QuadPart=-std::max<LONGLONG>(1,LONGLONG(std::ceil(std::min(deadline-now,1.0)*1e7)));
            if(!SetWaitableTimer(presentation_timer.h,&relative,0,nullptr,nullptr,FALSE))return HRESULT_FROM_WIN32(GetLastError());
            handles[count++]=presentation_timer.h;armed=true;
        }
        DWORD result=WaitForMultipleObjects(count,handles,FALSE,timeout);
        DWORD wait_error=result==WAIT_FAILED?GetLastError():ERROR_SUCCESS;
        if(armed)CancelWaitableTimer(presentation_timer.h);
        if(result==WAIT_FAILED)return HRESULT_FROM_WIN32(wait_error);
        // Waiting consumes an auto-reset DXGI signal. Hand its reservation to tick instead of polling twice.
        if(slot_index!=MAXDWORD&&result==WAIT_OBJECT_0+slot_index)video_slot_reserved=true;
        if(result==WAIT_OBJECT_0+1)audio_event_observed=now_seconds();
        return S_OK;
    }
    HRESULT tick() {
        CHECK(tick_audio());CHECK(chrome.layout(shell)); double n = now_seconds();bool redraw_needed=shell.redraw.exchange(false);
        const bool minimized=IsIconic(window)!=FALSE;
        while (video.size() > 1 && (minimized||!video_sync||video[1].pts <= n)) {video.pop_front();if(minimized)++minimized_skips;else if(video_sync)++late_frames;else ++interactive_skips;}
        if(minimized){shell.redraw=true;return S_OK;}
        const bool due=!video.empty()&&(!video_sync||video.front().pts<=n);
        if(!due&&!(redraw_needed&&last_frame.sample))return S_OK;
        if(present_retry_at>n){if(redraw_needed)shell.redraw=true;return S_OK;}
        // Poll only when drawing is useful. A permanently ready display must not spin the idle pump.
        if(!video_slot_reserved){
            DWORD ready=WaitForSingleObject(video_event.h,0);
            if(ready==WAIT_TIMEOUT){++video_waits;if(redraw_needed)shell.redraw=true;return S_OK;}
            if(ready!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(GetLastError());
            video_slot_reserved=true;
        }
        if(due){auto& frame=video.front();const auto before=presented;HRESULT hr=draw(frame.sample.Get(),frame.width,frame.height,nullptr,&frame.source,&frame.color);CHECK(hr);
            if(hr==S_OK){if(presented>before&&video_sync)presentation_late.add(now_seconds()-frame.pts);video.pop_front();}}
        else {HRESULT hr=draw(last_frame.sample.Get(),last_frame.width,last_frame.height,nullptr,&last_frame.source,&last_frame.color);CHECK(hr);if(hr==S_FALSE)shell.redraw=true;}
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

struct Worker { Handle process, job, input, output, read_event, write_event;
    ~Worker(){if(job.h)TerminateJobObject(job.h,0);if(process.h)WaitForSingleObject(process.h,3000);}
};
static bool transfer_worker(Worker* w, void* data, DWORD size, ULONGLONG deadline,bool write) {
    DWORD total = 0; auto p = static_cast<uint8_t*>(data);
    HANDLE pipe=write?w->input.h:w->output.h;
    while (total < size) {
        if(GetTickCount64()>=deadline)return false;
        OVERLAPPED operation{}; operation.hEvent=write?w->write_event.h:w->read_event.h; ResetEvent(operation.hEvent);
        DWORD n=0;
        BOOL complete=write?WriteFile(pipe,p+total,size-total,&n,&operation):ReadFile(pipe,p+total,size-total,&n,&operation);
        if(!complete) {
            if(GetLastError()!=ERROR_IO_PENDING)return false;
            HANDLE wait_handles[]={operation.hEvent,w->process.h};
            ULONGLONG now=GetTickCount64();DWORD timeout=now<deadline?DWORD(deadline-now):0;
            if(WaitForMultipleObjects(w->process.h?2:1,wait_handles,FALSE,timeout)!=WAIT_OBJECT_0){
                // The stack OVERLAPPED and caller buffer stay alive until cancellation completes.
                CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&n,TRUE);return false;
            }
            if(!GetOverlappedResult(pipe,&operation,&n,FALSE))return false;
        }
        if(!n)return false;
        total += n;
    }
    return true;
}
static bool read_worker(Worker* w,void* data,DWORD size,ULONGLONG deadline){return transfer_worker(w,data,size,deadline,false);}
static bool write_worker(Worker* w,const void* data,DWORD size,ULONGLONG deadline){return transfer_worker(w,const_cast<void*>(data),size,deadline,true);}
static bool worker_pipe(const std::wstring& name,bool parent_writes,Handle& parent,Handle& child,Handle& event,DWORD capacity=65536) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    parent.h=CreateNamedPipeW(name.c_str(),(parent_writes?PIPE_ACCESS_OUTBOUND:PIPE_ACCESS_INBOUND)|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,capacity,capacity,0,nullptr);
    if(parent.h==INVALID_HANDLE_VALUE)return false;
    child.h=CreateFileW(name.c_str(),parent_writes?GENERIC_READ:GENERIC_WRITE,0,&inherit,OPEN_EXISTING,0,nullptr);
    if(child.h==INVALID_HANDLE_VALUE)return false;
    event.h=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event.h)return false;
    // The local child end is already connected, so this cannot leave pending stack I/O.
    OVERLAPPED connection{};connection.hEvent=event.h;
    if(!ConnectNamedPipe(parent.h,&connection)){
        DWORD error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return true;
        if(error==ERROR_IO_PENDING){CancelIoEx(parent.h,&connection);DWORD ignored=0;GetOverlappedResult(parent.h,&connection,&ignored,TRUE);}
        return false;
    }
    return true;
}
extern "C" {
const char* am_error() { return last_error.c_str(); }
int am_lan_ipv4(const uint8_t* requested,uint8_t* address, uint32_t* index, uint32_t* prefix) {
    if(!address||!index||!prefix){error_hr("LAN adapter output",E_POINTER);return 0;}
    ULONG bytes=16384;std::vector<uint8_t> storage(bytes);
    auto adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG status=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,adapters,&bytes);
    if(status==ERROR_BUFFER_OVERFLOW && bytes<=1024*1024){storage.resize(bytes);adapters=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());status=GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_GATEWAYS,nullptr,adapters,&bytes);}
    if(status!=NO_ERROR){error_hr("LAN adapter query",HRESULT_FROM_WIN32(status));return 0;}
    ComPtr<INetworkListManager> networks;HRESULT hr=CoCreateInstance(CLSID_NetworkListManager,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&networks));
    if(FAILED(hr)){error_hr("network profile query",hr);return 0;}
    std::vector<LanCandidate> candidates;
    for(auto a=adapters;a;a=a->Next){
        if(a->OperStatus!=IfOperStatusUp || (a->IfType!=IF_TYPE_ETHERNET_CSMACD && a->IfType!=IF_TYPE_IEEE80211) || !a->FirstGatewayAddress)continue;
        auto name=wide(a->AdapterName);GUID adapter_id{};if(FAILED(CLSIDFromString(name.c_str(),&adapter_id)))continue;
        ComPtr<IEnumNetworkConnections> connections;if(FAILED(networks->GetNetworkConnections(&connections)))continue;
        bool private_profile=false;for(;;){ComPtr<INetworkConnection> c;ULONG fetched=0;if(connections->Next(1,&c,&fetched)!=S_OK)break;
            GUID id{};if(FAILED(c->GetAdapterId(&id)) || id!=adapter_id)continue;ComPtr<INetwork> network;NLM_NETWORK_CATEGORY category;
            if(SUCCEEDED(c->GetNetwork(&network)) && SUCCEEDED(network->GetCategory(&category)) && category==NLM_NETWORK_CATEGORY_PRIVATE)private_profile=true;
        }
        if(!private_profile)continue;
        for(auto u=a->FirstUnicastAddress;u;u=u->Next){if(!u->Address.lpSockaddr||u->Address.iSockaddrLength<static_cast<int>(sizeof(sockaddr_in))||u->Address.lpSockaddr->sa_family!=AF_INET)continue;
            auto ip=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);auto b=reinterpret_cast<uint8_t*>(&ip->sin_addr);
            LanCandidate candidate;std::copy_n(b,4,candidate.address.begin());candidate.index=a->IfIndex;candidate.prefix=u->OnLinkPrefixLength;
            candidate.up=true;candidate.ethernet_or_wifi=true;candidate.gateway=true;candidate.private_profile=true;
            candidates.push_back(candidate);
        }
    }
    LanCandidate chosen;if(select_lan_candidate(candidates,requested,chosen)){
        std::copy(chosen.address.begin(),chosen.address.end(),address);*index=chosen.index;*prefix=chosen.prefix;return 1;
    }
    error_hr(requested?"Requested address is not on an active private Ethernet/Wi-Fi LAN with a gateway":"No active private Ethernet/Wi-Fi IPv4 LAN with a gateway",E_FAIL);return 0;
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
void am_title(void* p, const char* title) { auto s = wide(title); static_cast<App*>(p)->shell.set_title(s); }
void am_status(void* p,unsigned kind,const char* reason){auto a=static_cast<App*>(p);if(kind==5)a->external_unavailable=true;else if(kind==6)a->external_unavailable=false;a->shell.set_status(int(kind),wide(reason));}
int am_status_dismissed(void* p){return static_cast<App*>(p)->shell.status_dismissed.exchange(false)?1:0;}
int am_wait(void* p,unsigned wait_ms){
    auto a = static_cast<App*>(p);
    HRESULT hr=a->wait_for_media(wait_ms);if(FAILED(hr)){error_hr("media wait",hr);return -1;}
    return a->shell.quit?0:1;
}
int am_dispatch(void* p){
    auto a=static_cast<App*>(p);if(a->shell.quit)return 0;
    a->service_recovery();
    if(a->shell.disconnect.exchange(false))return 2;
    HRESULT hr=a->graphics_pending?S_OK:a->tick();
    if(FAILED(hr)){if(!a->recover_fault(hr)){error_hr("media presentation",hr);return -1;}}
    if (a->shell.disconnect.exchange(false)) return 2; return 1;
}
int am_pump(void* p,unsigned wait_ms){int ready=am_wait(p,wait_ms);return ready<=0?ready:am_dispatch(p);}
unsigned long long am_presented(void* p){return static_cast<App*>(p)->presented;}
void am_flush_audio(void* p);
void am_video_sync(void* p,int enabled){
    // Media-thread policy supplied by negotiated audio ownership, never by mute or silence.
    auto a=static_cast<App*>(p);const bool sync=enabled!=0;
    // No residual PCM may play in interactive mode, including at a teardown boundary.
    if(!sync&&(a->audio_running||!a->sound.empty()))am_flush_audio(p);
    if(a->video_sync==sync)return;
    a->video_sync=sync;CancelWaitableTimer(a->presentation_timer.h);SetEvent(a->wake.h);
}
int am_prepare_video(void* p,unsigned codec,unsigned width,unsigned height,unsigned fps){
    auto a=static_cast<App*>(p);if((codec!=264&&codec!=265)||(fps!=30&&fps!=60))return 0;
    if(a->graphics_pending||a->audio_pending){error_hr("native devices are recovering",MF_E_NOTACCEPTING);return 0;}
    a->expected_fps=fps;if(a->decoder_ready&&a->codec==codec)return 1;
    HRESULT hr=a->init_decoder(codec,width,height);if(FAILED(hr)){a->recover_fault(hr);error_hr("prepare video decoder",hr);return 0;}return 1;
}
int am_hevc_available(void* p) {return static_cast<App*>(p)->hevc_available?1:0;}
int am_video(void* p, const uint8_t* data, unsigned len, double pts, unsigned codec,unsigned width,unsigned height) {
    auto app=static_cast<App*>(p);if(codec!=264&&codec!=265)return 0;
    if(app->graphics_pending||app->audio_pending){error_hr("native devices are recovering",MF_E_NOTACCEPTING);return 0;}
    if(app->codec!=codec||!app->decoder_ready){HRESULT hr=app->init_decoder(codec,width,height);if(FAILED(hr)){error_hr("video codec selection",hr);return 0;}}
    HRESULT hr = app->submit(data, len, pts); if (FAILED(hr)) {app->recover_fault(hr);error_hr("video decode", hr); return 0; } return 1;
}
int am_video_ready(void* p) {auto a=static_cast<App*>(p);return a->video.size()<(a->codec==265?3:6);}
int am_audio(void* p, const int16_t* pcm, unsigned samples, double pts) {
    auto a = static_cast<App*>(p);
    if (!pcm || !samples || samples > 4096 || samples % 2 || !valid_media_time(pts)) {error_hr("invalid PCM sample",E_INVALIDARG);return 0;}
    if(a->audio_pending||a->graphics_pending){error_hr("native devices are recovering",MF_E_NOTACCEPTING);return 0;}
    if (a->sound.size() >= 32) {
        UINT32 padding=0;a->audio->GetCurrentPadding(&padding);char detail[300];
        sprintf_s(detail,"audio queue full padding=%u capacity=%u latency=%.4f now=%.4f first_pts=%.4f played=%llu",padding,a->audio_capacity,a->audio_latency,now_seconds(),a->sound.front().pts,(unsigned long long)a->audio_frames);
        error_hr(detail,E_FAIL);return 0;
    }
    // This native boundary also enforces synchronization before starting playback.
    am_video_sync(p,1);
    a->sound.push_back({std::vector<int16_t>(pcm, pcm+samples), 0, pts});
    a->audio_high_water=std::max(a->audio_high_water,a->sound.size());
    if (!a->audio_running) {HRESULT hr=a->start_audio();if(FAILED(hr)){a->recover_fault(hr);error_hr("WASAPI prefill/start",hr);return 0;}}
    return 1;
}
void am_flush_audio(void* p) {
    auto a=static_cast<App*>(p);HRESULT hr=a->flush_audio();if(FAILED(hr)){a->recover_fault(App::audio_fault(hr)?hr:AUDCLNT_E_DEVICE_INVALIDATED);error_hr("WASAPI stop/reset",hr);}
}
int am_reset(void* p) {
    auto a = static_cast<App*>(p); am_flush_audio(p); a->video.clear();
    a->last_frame.sample.Reset();
    a->shell.reset_video();
    a->reset_video_decoder();a->output_ahead={};a->presentation_late={};a->decoder_residence={};
    a->source_width=a->source_height=a->presented_width=a->presented_height=0;a->source_shape_changes=a->presented_shape_changes=0;
    a->video_sync=true;a->interactive_skips=0;
    if(a->audio_pending&&a->audio_retries>=3){a->audio_retries=0;a->audio_retry_at=now_seconds();}
    if(a->graphics_pending&&a->graphics_retries>=3){a->graphics_retries=0;a->graphics_retry_at=now_seconds();}
    return 1;
}
const char* am_stats(void* p) {
    auto a = static_cast<App*>(p); static thread_local std::string text;
    RECT client{};GetClientRect(a->window,&client);
    text = "adapter=" + a->adapter + " decoded=" + std::to_string(a->decoded) + " dxgi_frames=" + std::to_string(a->gpu_frames)
        + " decoder_surfaces=" + std::to_string(a->decoder_surfaces) + " presented=" + std::to_string(a->presented) + " late=" + std::to_string(a->late_frames)
        + " audio_samples=" + std::to_string(a->audio_frames) + " resolution=" + std::to_string(a->width) + "x" + std::to_string(a->height)
        + " source_shape="+std::to_string(a->source_width)+"x"+std::to_string(a->source_height)+" source_changes="+std::to_string(a->source_shape_changes)
        + " presented_shape="+std::to_string(a->presented_width)+"x"+std::to_string(a->presented_height)+" presented_changes="+std::to_string(a->presented_shape_changes)
        + " client_shape="+std::to_string(client.right-client.left)+"x"+std::to_string(client.bottom-client.top)
        + " decoder_instances="+std::to_string(a->decoder_instances) + " codec=" + std::to_string(a->codec) + " hevc_available=" + std::to_string(a->hevc_available)
        + " local_volume=" + std::to_string(int(a->shell.volume*100)) + " muted=" + std::to_string(a->shell.muted.load())
        + " audio_capacity="+std::to_string(a->audio_capacity)+" audio_latency_ms="+std::to_string(a->audio_latency*1000)
        + " video_queue_max="+std::to_string(a->video_high_water)+" audio_queue_max="+std::to_string(a->audio_high_water)
        + " video_waits="+std::to_string(a->video_waits)+" present_busy="+std::to_string(a->present_busy)
        + " video_sync="+std::to_string(a->video_sync)+" interactive_skips="+std::to_string(a->interactive_skips)+" minimized_skips="+std::to_string(a->minimized_skips)
        + " submit_avg_ms="+std::to_string(a->submit_calls?a->submit_seconds*1000/a->submit_calls:0)
        + " submit_max_ms="+std::to_string(a->submit_max_seconds*1000)
        + " input_prepare_avg_ms="+std::to_string(a->input_prepare.average()*1000)+" input_prepare_p95_ms="+std::to_string(a->input_prepare.percentile(.95)*1000)+" input_prepare_max_ms="+std::to_string(a->input_prepare.maximum*1000)
        + " present_avg_ms="+std::to_string(a->presented+a->present_busy?a->present_seconds*1000/(a->presented+a->present_busy):0)
        + " present_max_ms="+std::to_string(a->present_max_seconds*1000)
        + " output_ahead_avg_ms="+std::to_string(a->output_ahead.average()*1000)
        + " output_ahead_min_ms="+std::to_string(a->output_ahead.minimum*1000)
        + " due_late_avg_ms="+std::to_string(a->presentation_late.average()*1000)
        + " due_late_max_ms="+std::to_string(a->presentation_late.maximum*1000)
        + " mf_residence_avg_ms="+std::to_string(a->decoder_residence.average()*1000)
        + " mf_residence_max_ms="+std::to_string(a->decoder_residence.maximum*1000)
        + " mf_residence_samples="+std::to_string(a->decoder_residence.count)
        + " high_resolution_timer="+std::to_string(a->high_resolution_timer)
        + " lowlat_attr="+std::to_string(a->low_latency_attribute)+" lowlat_value="+std::to_string(a->low_latency_value)
        + " lowlat_set_hr="+std::to_string(uint32_t(a->low_latency_set))+" lowlat_get_hr="+std::to_string(uint32_t(a->low_latency_get))
        + " audio_gap_p95_ms="+std::to_string(a->audio_service_gap.percentile(.95)*1000)+" audio_gap_p99_ms="+std::to_string(a->audio_service_gap.percentile(.99)*1000)
        + " audio_gap_max_ms="+std::to_string(a->audio_service_gap.maximum*1000)+" audio_wake_to_fill_max_ms="+std::to_string(a->audio_event_service.maximum*1000)
        + " audio_queue_age_avg_ms="+std::to_string(a->audio_queue_age.average()*1000)+" audio_queue_age_min_ms="+std::to_string(a->audio_queue_age.minimum*1000)+" audio_queue_age_max_ms="+std::to_string(a->audio_queue_age.maximum*1000)
        + " audio_fill_max_ms="+std::to_string(a->audio_fill_time.maximum*1000)+" audio_silence_frames="+std::to_string(a->audio_silence)+" audio_skip_frames="+std::to_string(a->audio_skips)
        + " chrome_present_max_ms="+std::to_string(a->chrome.present_max_seconds*1000)+" chrome_present_busy="+std::to_string(a->chrome.present_busy)
        + " audio_recoveries="+std::to_string(a->audio_recoveries)+" graphics_recoveries="+std::to_string(a->graphics_recoveries);
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
int am_worker_provision() {
    PSID sid=nullptr;HRESULT hr=worker_profile_sid(&sid);
    if(FAILED(hr)){error_hr("AppContainer profile",hr);return 0;}
    struct SidGuard { PSID s;~SidGuard(){FreeSid(s);} } guard{sid};
    std::wstring path;hr=current_executable(path);
    if(SUCCEEDED(hr))hr=worker_executable_access(path,sid,true);
    if(FAILED(hr)){error_hr("Provision worker execute access on this executable as its owner",hr);return 0;}
    return 1;
}
void* am_worker_start() {
    auto w = std::make_unique<Worker>();
    PSID sid = nullptr;
    HRESULT hr = worker_profile_sid(&sid);
    if (FAILED(hr)) { error_hr("AppContainer profile", hr); return nullptr; }
    struct SidGuard { PSID s; ~SidGuard() { FreeSid(s); } } sg{sid};
    std::wstring path;hr=current_executable(path);
    if(SUCCEEDED(hr))hr=worker_executable_access(path,sid,false);
    if(FAILED(hr)){error_hr("Worker execute access unavailable: run --provision-worker once as this executable's owner",hr);return nullptr;}
    Handle child_in, child_out;
    GUID pipe_id{};if(FAILED(CoCreateGuid(&pipe_id)))return nullptr;wchar_t guid[40];StringFromGUID2(pipe_id,guid,40);
    std::wstring pipe_name=L"\\\\.\\pipe\\AirMirrorLite-"+std::to_wstring(GetCurrentProcessId())+L"-"+guid;
    if(!worker_pipe(pipe_name+L"-request",true,w->input,child_in,w->write_event)
        ||!worker_pipe(pipe_name+L"-reply",false,w->output,child_out,w->read_event))return nullptr;
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
    wchar_t system_directory[32768];UINT system_length=GetSystemDirectoryW(system_directory,32768);
    if(!system_length||system_length>=32768){error_hr("Windows system directory",system_length?HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER):HRESULT_FROM_WIN32(GetLastError()));return nullptr;}
    if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, system_directory, &si.StartupInfo, &pi)) {
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
    uint32_t header[2] = {op, len};ULONGLONG deadline = GetTickCount64()+2000;
    if(!write_worker(w,header,sizeof(header),deadline)||!write_worker(w,input,len,deadline)){TerminateProcess(w->process.h,1);return -1;}
    uint32_t result=0;
    if (!read_worker(w, &result, 4, deadline) || result > cap) { TerminateProcess(w->process.h, 1); return -1; }
    if (!read_worker(w, output, result, deadline)) { TerminateProcess(w->process.h, 1); return -1; }
    return int(result);
}
}
#include "probe.inc"
