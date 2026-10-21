// Diagnostic only: OS H.264 encoder makes synthetic bars in memory.
// No capture, recording or network traffic. Exercise the actual decoder/render path.
static HRESULT probe_video(App* app, UINT width, UINT height,UINT codec=264) {
    CHECK(app->init_decoder(codec,width,height));
    ComPtr<IMFTransform> enc;if(codec==265){CHECK(create_hevc_transform(true,&enc));}else{CHECK(CoCreateInstance(CLSID_CMSH264EncoderMFT,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&enc)));}
    ComPtr<IMFMediaType> output; CHECK(MFCreateMediaType(&output));
    CHECK(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)); CHECK(output->SetGUID(MF_MT_SUBTYPE, codec==265?MFVideoFormat_HEVC:MFVideoFormat_H264));
    CHECK(output->SetUINT32(MF_MT_AVG_BITRATE, codec==265?16000000:4000000));if(codec==265){CHECK(output->SetUINT32(MF_MT_MPEG2_PROFILE,1));} CHECK(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK(MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height)); CHECK(MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, 30, 1));
    CHECK(MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1)); CHECK(enc->SetOutputType(0, output.Get(), 0));
    ComPtr<IMFMediaType> input; CHECK(MFCreateMediaType(&input)); CHECK(output->CopyAllItems(input.Get()));
    CHECK(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)); CHECK(enc->SetInputType(0, input.Get(), 0));
    CHECK(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)); CHECK(enc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    MFT_OUTPUT_STREAM_INFO oi{}; CHECK(enc->GetOutputStreamInfo(0, &oi));
    auto drain = [&]() -> HRESULT {
        for (int j=0;j<8;++j) {
            ComPtr<IMFSample> sample; CHECK(MFCreateSample(&sample));ComPtr<IMFMediaBuffer> buf;CHECK(MFCreateMemoryBuffer(std::max<DWORD>(oi.cbSize, 2u*1024*1024), &buf));CHECK(sample->AddBuffer(buf.Get()));
            MFT_OUTPUT_DATA_BUFFER b{};b.pSample=sample.Get();DWORD status=0;HRESULT hr=enc->ProcessOutput(0,1,&b,&status);
            if(b.pEvents)b.pEvents->Release();if(hr==MF_E_TRANSFORM_NEED_MORE_INPUT)return S_OK;CHECK(hr);
            ComPtr<IMFMediaBuffer> compressed;CHECK(sample->ConvertToContiguousBuffer(&compressed));BYTE* bytes;DWORD len;CHECK(compressed->Lock(&bytes,nullptr,&len));
            LONGLONG pts;hr=sample->GetSampleTime(&pts);if(SUCCEEDED(hr))hr=app->submit(bytes,len,pts/1e7);compressed->Unlock();CHECK(hr);
        }return S_OK;
    };
    double start=now_seconds();
    for (int f=0;f<(codec==265?15:45);++f) {
        DWORD size=width*height*3/2;ComPtr<IMFMediaBuffer> buf;CHECK(MFCreateMemoryBuffer(size,&buf));BYTE* bytes;CHECK(buf->Lock(&bytes,nullptr,nullptr));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)bytes[y*width+x]=BYTE(16+((x+f*12)/80%7)*30);
        memset(bytes+width*height,128,width*height/2);buf->Unlock();CHECK(buf->SetCurrentLength(size));
        ComPtr<IMFSample> sample;CHECK(MFCreateSample(&sample));CHECK(sample->AddBuffer(buf.Get()));CHECK(sample->SetSampleTime(LONGLONG((start+0.12+double(f)/30)*1e7)));CHECK(sample->SetSampleDuration(333333));
        HRESULT hr=enc->ProcessInput(0,sample.Get(),0);if(hr==MF_E_NOTACCEPTING){CHECK(drain());hr=enc->ProcessInput(0,sample.Get(),0);}CHECK(hr);CHECK(drain());
        while(now_seconds()<start+double(f+1)/30){CHECK(app->tick());Sleep(2);}
    }
    CHECK(enc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN,0));CHECK(drain());
    double until=now_seconds()+0.4;while(now_seconds()<until){CHECK(app->tick());Sleep(2);}
    // A paused redraw uses the retained private texture, not a live decoder surface.
    if(!app->last_frame.sample)return E_FAIL;
    app->video.clear();
    auto redraw_paused=[&]() -> HRESULT {
        const uint64_t before=app->presented;const ULONGLONG deadline=GetTickCount64()+1000;
        app->shell.redraw=true;
        do {
            CHECK(app->tick());if(app->presented>before)return S_OK;
            Sleep(2);
        } while(GetTickCount64()<deadline);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    };
    RECT original;GetWindowRect(app->window,&original);
    for(auto size: {std::pair<int,int>{800,500},{600,900},{1200,700}}){
        if(!SetWindowPos(app->window,nullptr,original.left,original.top,size.first,size.second,SWP_NOZORDER|SWP_NOACTIVATE))return HRESULT_FROM_WIN32(GetLastError());
        CHECK(redraw_paused());
    }
    DWORD_PTR result=0;
    if(!SendMessageTimeoutW(app->window,WM_KEYDOWN,VK_F11,0,SMTO_ABORTIFHUNG,2000,&result))return HRESULT_FROM_WIN32(GetLastError());
    if(GetWindowLongPtrW(app->window,GWL_STYLE)&WS_CAPTION)return E_FAIL;
    CHECK(redraw_paused());
    if(!SendMessageTimeoutW(app->window,WM_KEYDOWN,VK_F11,0,SMTO_ABORTIFHUNG,2000,&result))return HRESULT_FROM_WIN32(GetLastError());
    if((GetWindowLongPtrW(app->window,GWL_STYLE)&WS_CAPTION)||app->shell.fullscreen)return E_FAIL;
    CHECK(redraw_paused());
    if(!SetWindowPos(app->window,nullptr,original.left,original.top,original.right-original.left,original.bottom-original.top,SWP_NOZORDER|SWP_NOACTIVATE))return HRESULT_FROM_WIN32(GetLastError());
    CHECK(redraw_paused());return S_OK;
}
extern "C" int am_video_probe(void* p) {
    auto app=static_cast<App*>(p);uint64_t before=app->decoder_surfaces;
    HRESULT hr=probe_video(app,1920,1080);
    if(SUCCEEDED(hr))hr=probe_video(app,1080,1920);
    if(SUCCEEDED(hr)&&app->hevc_available){
        // A partial codec-initialization failure must be recoverable, not a null dereference.
        app->decoder.Reset();app->decoder_ready=false;const uint8_t dummy=0;
        if(app->submit(&dummy,1,now_seconds())!=MF_E_TRANSFORM_TYPE_NOT_SET)return 0;
        uint64_t hevc_before=app->decoder_surfaces;hr=probe_video(app,3840,2160,265);
        if(SUCCEEDED(hr)&&(app->decoder_surfaces-hevc_before<8||app->width!=3840||app->height!=2160))hr=E_FAIL;
        if(SUCCEEDED(hr))hr=probe_video(app,992,2160,265);
        if(SUCCEEDED(hr)&&(app->width!=992||app->height!=2160))hr=E_FAIL;
    }
    if(FAILED(hr)){error_hr("synthetic GPU probe",hr);return 0;}
    if(app->decoder_surfaces-before<60){error_hr("insufficient hardware decoder surfaces",E_FAIL);return 0;}return 1;
}
extern "C" int am_ui_probe(void* p,const char* directory) {
    // Readiness polling must not consume an asynchronous completion signal.
    {Discovery d;if(!d.done.h)return 0;Discovery::callback(ERROR_SUCCESS,&d,nullptr);
        if(am_discovery_status(&d)!=1||am_discovery_status(&d)!=1){error_hr("discovery completion must remain signaled",E_FAIL);return 0;}
        ResetEvent(d.done.h);d.status=ERROR_IO_PENDING;if(am_discovery_status(&d)!=0)return 0;}
    auto app=static_cast<App*>(p);
    // Exercise transport deadlines with local pipe ends that deliberately never drain/reply.
    // No worker codec, audio endpoint, desktop input or network is involved.
    {
        GUID id{};if(FAILED(CoCreateGuid(&id)))return 0;wchar_t guid[40];StringFromGUID2(id,guid,40);
        const std::wstring name=L"\\\\.\\pipe\\AirMirrorLite-probe-"+std::wstring(guid);
        Worker blocked;Handle reader,writer;
        if(!worker_pipe(name+L"-request",true,blocked.input,reader,blocked.write_event,4096)
            ||!worker_pipe(name+L"-reply",false,blocked.output,writer,blocked.read_event,4096))return 0;
        std::array<uint8_t,8192> bytes{};ULONGLONG began=GetTickCount64();
        if(write_worker(&blocked,bytes.data(),DWORD(bytes.size()),began+40)||GetTickCount64()-began>1000){error_hr("worker stalled-write deadline",E_FAIL);return 0;}
        began=GetTickCount64();
        if(read_worker(&blocked,bytes.data(),4,began+40)||GetTickCount64()-began>1000){error_hr("worker stalled-read deadline",E_FAIL);return 0;}
        if(write_worker(&blocked,bytes.data(),4,GetTickCount64())){error_hr("worker expired shared deadline",E_FAIL);return 0;}
    }
    {
        ComPtr<IDXGISwapChain2> paced;UINT latency=0;DXGI_SWAP_CHAIN_DESC1 desc{};
        if(FAILED(app->swap.As(&paced))||FAILED(paced->GetMaximumFrameLatency(&latency))||latency!=1
            ||FAILED(app->swap->GetDesc1(&desc))||!(desc.Flags&DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))return 0;
        // An unavailable presentation slot keeps the newest due frame queued without blocking.
        Handle unavailable;unavailable.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!unavailable.h)return 0;
        HANDLE actual=app->video_event.h;app->video_event.h=unavailable.h;
        bool reserved=app->video_slot_reserved;app->video_slot_reserved=false;
        const auto presented=app->presented,late=app->late_frames,waits=app->video_waits;const double now=now_seconds();
        for(int i=0;i<3;++i)app->video.push_back({{},now-1+i*.01,16,16});
        app->shell.redraw=true;HRESULT gate=app->tick();if(SUCCEEDED(gate))gate=app->tick();
        bool retained=app->video.size()==1&&app->video.front().pts==now-1+.02&&app->presented==presented&&app->shell.redraw;
        app->video.clear();app->video_event.h=actual;app->video_slot_reserved=reserved;app->late_frames=late;app->video_waits=waits;
        if(FAILED(gate)||!retained){error_hr("nonblocking video backpressure",E_FAIL);return 0;}
        const int16_t zero[2]{};const uint8_t dummy=0;
        if(valid_media_time(std::numeric_limits<double>::max())||valid_media_time(-1)||valid_media_time(std::numeric_limits<double>::infinity())
            ||app->submit(&dummy,1,std::numeric_limits<double>::max())!=E_INVALIDARG
            ||am_audio(app,zero,0,now)||am_audio(app,zero,2,std::numeric_limits<double>::max())
            ||app->audio_running||!app->sound.empty()||app->audio_frames){error_hr("native media timestamp boundary",E_FAIL);return 0;}
        if(!app->audio_capacity||!std::isfinite(app->audio_latency)||app->audio_latency<0)return 0;
        last_error.clear();
    }
    auto path=wide(directory);HRESULT hr=app->shell.self_test(path.c_str());
    if(FAILED(hr)){error_hr("native UI regression (line encoded in HRESULT)",hr);return 0;}
    app->shell.reset_video();
    app->shell.set_status(L"Connected — waiting for picture and sound");
    if(app->shell.status!=1){error_hr("negotiated session waiting state",E_FAIL);return 0;}
    app->shell.set_status(L"Ready");
    auto file=path+L"/idle.png";hr=app->chrome.layout(app->shell,path.empty()?nullptr:file.c_str());
    if(FAILED(hr)){error_hr("idle artwork diagnostic",hr);return 0;}
    if(!path.empty()){
        app->shell.set_status(L"Connecting");file=path+L"/connecting.png";hr=app->chrome.layout(app->shell,file.c_str());
        if(FAILED(hr))return 0;
        app->shell.set_status(L"Ready");
        SendMessageW(app->window,PhoneShell::WM_FORMAT,1920,1080);
        SendMessageW(app->window,WM_KEYDOWN,'0',0);
        file=path+L"/landscape-idle.png";hr=app->chrome.layout(app->shell,file.c_str());if(FAILED(hr))return 0;
        SetWindowPos(app->window,nullptr,0,0,328,188,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        file=path+L"/compact-idle.png";hr=app->chrome.layout(app->shell,file.c_str());if(FAILED(hr))return 0;
        SendMessageW(app->window,PhoneShell::WM_FORMAT,498,1080);SendMessageW(app->window,WM_KEYDOWN,'0',0);
        hr=app->chrome.layout(app->shell);if(FAILED(hr))return 0;
    }
    const auto redraws=app->chrome.redraws;
    for(int i=0;i<20;++i){hr=app->tick();if(FAILED(hr)){error_hr("idle tick regression",hr);return 0;}}
    if(app->chrome.redraws!=redraws){error_hr("idle chrome repainted without invalidation",E_FAIL);return 0;}
    return 1;
}
