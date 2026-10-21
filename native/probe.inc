// Diagnostic only: OS H.264 encoder makes synthetic bars in memory.
// No capture, recording or network traffic. Exercise the actual decoder/render path.
static HRESULT probe_media_wait(App* app) {
    Handle wake,audio,slot;wake.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);audio.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);slot.h=CreateEventW(nullptr,FALSE,TRUE,nullptr);
    if(!wake.h||!audio.h||!slot.h)return HRESULT_FROM_WIN32(GetLastError());
    struct Restore {
        App* app;HANDLE wake,audio,slot;bool reserved,redraw;double retry;
        ~Restore(){app->wake.h=wake;app->audio_event.h=audio;app->video_event.h=slot;app->video_slot_reserved=reserved;app->shell.redraw=redraw;app->present_retry_at=retry;app->video.clear();CancelWaitableTimer(app->presentation_timer.h);}
    } restore{app,app->wake.h,app->audio_event.h,app->video_event.h,app->video_slot_reserved,app->shell.redraw,app->present_retry_at};
    app->wake.h=wake.h;app->audio_event.h=audio.h;app->video_event.h=slot.h;app->video_slot_reserved=false;app->shell.redraw=false;app->present_retry_at=0;
    double began=now_seconds();CHECK(app->wait_for_media(30));
    if(now_seconds()-began<.015||app->video_slot_reserved||WaitForSingleObject(slot.h,0)!=WAIT_OBJECT_0)return E_FAIL;
    const double due=now_seconds()+.04;app->video.push_back({{},due,16,16});
    CHECK(app->wait_for_media(0));
    if(app->video_slot_reserved)return E_FAIL;
    CHECK(app->wait_for_media(1000));if(now_seconds()<due-.001||app->video_slot_reserved)return E_FAIL;
    while(now_seconds()<due)CHECK(app->wait_for_media(1000));
    // A due-frame slot signal must be transferred to tick, never consumed twice.
    SetEvent(slot.h);CHECK(app->wait_for_media(1000));
    if(!app->video_slot_reserved||WaitForSingleObject(slot.h,0)!=WAIT_TIMEOUT)return E_FAIL;
    // Audio/close notifications interrupt a future video deadline.
    app->video_slot_reserved=false;app->video.front().pts=now_seconds()+1;SetEvent(audio.h);CHECK(app->wait_for_media(1000));
    if(WaitForSingleObject(audio.h,0)!=WAIT_TIMEOUT)return E_FAIL;
    SetEvent(wake.h);CHECK(app->wait_for_media(1000));if(WaitForSingleObject(wake.h,0)!=WAIT_TIMEOUT)return E_FAIL;
    return S_OK;
}
static HRESULT probe_video(App* app, UINT width, UINT height,UINT codec=264) {
    // These independent encoder fixtures represent separate phones/sessions.
    app->shell.reset_video();
    CHECK(app->init_decoder(codec,width,height));
    if(app->low_latency_attribute!=1||(SUCCEEDED(app->low_latency_get)&&app->low_latency_value!=1))return E_FAIL;
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
    CHECK(redraw_paused());
    // A queued future picture cannot be shown early, and wait0 still services already-due work.
    app->shell.redraw=false;const auto before=app->presented;const double due=now_seconds()+.06;
    app->video.push_back({app->last_frame.sample,due,app->last_frame.width,app->last_frame.height});
    if(am_pump(app,0)<0)return E_FAIL;
    if(app->presented>before&&now_seconds()<due)return E_FAIL;
    ULONGLONG deadline=GetTickCount64()+1000;
    while(app->presented==before&&GetTickCount64()<deadline)if(am_pump(app,100)<0)return E_FAIL;
    if(app->presented!=before+1||!app->video.empty())return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    const auto late=app->late_frames,shown=app->presented;const double now=now_seconds();
    for(int i=0;i<3;++i)app->video.push_back({app->last_frame.sample,now-.03+i*.01,app->last_frame.width,app->last_frame.height});
    if(am_pump(app,0)<0)return E_FAIL;
    deadline=GetTickCount64()+1000;
    while(app->presented==shown&&GetTickCount64()<deadline)if(am_pump(app,100)<0)return E_FAIL;
    if(app->presented!=shown+1||!app->video.empty()||app->late_frames!=late+2)return E_FAIL;
    return S_OK;
}
static HRESULT probe_letterbox_pixels(App* app) {
    // This readback is restricted to a freshly generated diagnostic pattern, never sender media.
    constexpr UINT width=640,height=360,border=16;
    std::vector<uint8_t> pixels(width*height*3/2,128);
    const uint8_t levels[2][2]={{64,112},{160,208}};
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        pixels[y*width+x]=(x<border||y<border||x>=width-border||y>=height-border)?235:levels[y>=height/2][x>=width/2];
    D3D11_TEXTURE2D_DESC pattern_desc{};pattern_desc.Width=width;pattern_desc.Height=height;pattern_desc.MipLevels=1;pattern_desc.ArraySize=1;
    pattern_desc.Format=DXGI_FORMAT_NV12;pattern_desc.SampleDesc.Count=1;pattern_desc.Usage=D3D11_USAGE_DEFAULT;
    pattern_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(),width,UINT(pixels.size())};ComPtr<ID3D11Texture2D> pattern;
    CHECK(app->device->CreateTexture2D(&pattern_desc,&initial,&pattern));
    ComPtr<IMFMediaBuffer> buffer;CHECK(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),pattern.Get(),0,FALSE,&buffer));
    ComPtr<IMFSample> sample;CHECK(MFCreateSample(&sample));CHECK(sample->AddBuffer(buffer.Get()));
    CHECK(app->chrome.layout(app->shell));
    if(app->vieww!=app->chrome.video_width||app->viewh!=app->chrome.video_height||app->vieww>=app->viewh)return E_FAIL;
    // Read the generated pattern before Present rotates the logical back buffer. Holding
    // a D3D11 buffer0 interface across Present does not preserve its former pixel contents.
    D3D11_TEXTURE2D_DESC read_desc{};read_desc.Width=app->vieww;read_desc.Height=app->viewh;read_desc.MipLevels=1;read_desc.ArraySize=1;
    read_desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;read_desc.SampleDesc.Count=1;read_desc.Usage=D3D11_USAGE_STAGING;read_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;CHECK(app->device->CreateTexture2D(&read_desc,nullptr,&readback));
    const uint64_t before=app->presented;
    const ULONGLONG deadline=GetTickCount64()+1000;
    while(app->presented==before&&GetTickCount64()<deadline){
        if(!app->video_slot_reserved){DWORD ready=WaitForSingleObject(app->video_event.h,20);if(ready==WAIT_TIMEOUT)continue;if(ready!=WAIT_OBJECT_0)return E_FAIL;app->video_slot_reserved=true;}
        CHECK(app->draw(sample.Get(),width,height,readback.Get()));if(app->presented==before)Sleep(1);
    }
    if(app->presented!=before+1||!app->video.empty()||app->presented_width!=width||app->presented_height!=height)return E_FAIL;
    if(app->vieww!=read_desc.Width||app->viewh!=read_desc.Height)return E_FAIL;
    D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(app->context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    auto gray=[&](UINT x,UINT y,int expected,int tolerance) {
        auto p=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;
        if(std::abs(int(p[0])-expected)<=tolerance&&std::abs(int(p[1])-expected)<=tolerance
            &&std::abs(int(p[2])-expected)<=tolerance&&p[3]>=250)return true;
        char detail[180];sprintf_s(detail,"synthetic letterbox pixel (%u,%u) BGRA=%u,%u,%u,%u expected_gray=%d",x,y,p[0],p[1],p[2],p[3],expected);error_hr(detail,E_FAIL);return false;
    };
    const UINT dw=read_desc.Width,dh=UINT(double(dw)*height/width),top=(read_desc.Height-dh)/2;
    bool valid=top>8&&dh>32;
    if(valid){
        // Border corners survive scaling: a center crop would lose these markers.
        for(double x:{.01,.99})for(double y:{.02,.98})valid=gray(UINT(x*(dw-1)),top+UINT(y*(dh-1)),255,25)&&valid;
        const int expected[2][2]={{56,112},{168,224}};
        for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x)valid=gray(UINT((x?.75:.25)*(dw-1)),top+UINT((y?.75:.25)*(dh-1)),expected[y][x],20)&&valid;
        for(double x:{.1,.5,.9}){
            valid=gray(UINT(x*(dw-1)),top/2,0,8)&&valid;
            valid=gray(UINT(x*(dw-1)),read_desc.Height-1-top/2,0,8)&&valid;
        }
    }
    app->context->Unmap(readback.Get(),0);return valid?S_OK:E_FAIL;
}
static HRESULT probe_shape_schedule(App* app) {
    if(!app->last_frame.sample||app->shell.fullscreen)return E_FAIL;
    // Reuse a synthetic private NV12 surface. Both aspect crops remain within its real bounds.
    auto sample=app->last_frame.sample;const UINT original_w=app->last_frame.width,original_h=app->last_frame.height;
    app->shell.reset_video();
    ComPtr<IMFMediaBuffer> buffer;CHECK(sample->GetBufferByIndex(0,&buffer));ComPtr<IMFDXGIBuffer> dxgi;CHECK(buffer.As(&dxgi));
    ComPtr<ID3D11Texture2D> texture;CHECK(dxgi->GetResource(IID_PPV_ARGS(&texture)));D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    const UINT edge=(std::min(desc.Width,desc.Height)/4)&~1u;if(edge<48)return E_FAIL;
    auto settled=[&](UINT width,UINT height,double frame_aspect) {
        RECT client{};if(!GetClientRect(app->window,&client))return false;const int rim=PhoneShell::rim_pixels(app->shell.dpi_scale());
        const int w=client.right-2*rim,h=client.bottom-2*rim;
        return app->presented_width==width&&app->presented_height==height&&w>0&&h>0
            &&std::abs(double(w)/h-frame_aspect)<.02;
    };
    auto show=[&](UINT width,UINT height,double pts,double frame_aspect) -> HRESULT {
        app->queue_video_frame({sample,pts,width,height});const ULONGLONG deadline=GetTickCount64()+1000;
        do {if(am_pump(app,20)<0)return E_FAIL;if(app->video.empty()&&settled(width,height,frame_aspect))return S_OK;}while(GetTickCount64()<deadline);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    };
    CHECK(show(edge,edge*2,now_seconds(),.5));
    RECT portrait{};if(!GetClientRect(app->window,&portrait))return HRESULT_FROM_WIN32(GetLastError());
    // A media-only 16:9 feed is fully drawn inside the learned portrait phone frame.
    const UINT unit=std::min(desc.Width/16,desc.Height/9)&~1u;if(unit<2)return E_FAIL;
    CHECK(show(unit*16,unit*9,now_seconds(),.5));
    RECT media_client{},destination{};BOOL destination_enabled=FALSE;
    const double media_until=now_seconds()+.05;
    do {
        if(am_pump(app,10)<0)return E_FAIL;
        if(!GetClientRect(app->window,&media_client)||media_client.right!=portrait.right||media_client.bottom!=portrait.bottom)return E_FAIL;
    } while(now_seconds()<media_until);
    app->vcontext->VideoProcessorGetStreamDestRect(app->processor.Get(),0,&destination_enabled,&destination);
    if(!destination_enabled||destination.top<=0||destination.bottom>=LONG(app->chrome.video_height)
        ||destination.right<=destination.left||destination.bottom<=destination.top
        ||std::abs(double(destination.right-destination.left)/(destination.bottom-destination.top)-16.0/9)>.02)return E_FAIL;
    CHECK(probe_letterbox_pixels(app));
    CHECK(show(edge,edge*2,now_seconds(),.5));
    const auto shape_changes=app->presented_shape_changes;const double future=now_seconds()+.25;
    app->queue_video_frame({sample,future,edge*2,edge});
    const double inspect_until=now_seconds()+.05;
    do {
        if(am_pump(app,5)<0)return E_FAIL;
        RECT before_due{};if(!GetClientRect(app->window,&before_due))return HRESULT_FROM_WIN32(GetLastError());
        if(now_seconds()>=future)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if(before_due.right!=portrait.right||before_due.bottom!=portrait.bottom
            ||app->presented_shape_changes!=shape_changes||!settled(edge,edge*2,.5))return E_FAIL;
    } while(now_seconds()<inspect_until);
    if(app->source_width!=edge*2||app->source_height!=edge)return E_FAIL;
    ULONGLONG deadline=GetTickCount64()+1000;
    while((!app->video.empty()||!settled(edge*2,edge,2))&&GetTickCount64()<deadline)if(am_pump(app,20)<0)return E_FAIL;
    if(!app->video.empty()||!settled(edge*2,edge,2))return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    CHECK(show(edge,edge*2,now_seconds(),.5));
    if(app->presented_shape_changes!=shape_changes+2)return E_FAIL;
    app->shell.reset_video();CHECK(show(original_w,original_h,now_seconds(),double(original_w)/original_h));return S_OK;
}
static HRESULT probe_interactive_video(App* app) {
    if(!app->video_sync||!app->video.empty()||!app->last_frame.sample||app->audio_running||!app->sound.empty())return E_FAIL;
    struct RestoreSync {App* app;~RestoreSync(){am_video_sync(app,1);}} restore{app};
    auto sample=app->last_frame.sample;const UINT width=app->last_frame.width,height=app->last_frame.height;
    const uint64_t before=app->presented,late=app->late_frames,skips=app->interactive_skips,lateness=app->presentation_late.count,instances=app->decoder_instances;
    const size_t limit=app->codec==265?4:8,count=limit+2;const double began=now_seconds(),future=began+1;
    app->shell.redraw=false;am_video_sync(app,0);
    // Decoded-only overload: queue limits remain intact, while the freshest picture wins.
    for(size_t i=0;i<count;++i){app->queue_video_frame({sample,future+i*.01,width-UINT((count-1-i)*2),height});if(app->video.size()>limit)return E_FAIL;}
    ULONGLONG deadline=GetTickCount64()+700;
    while(app->presented==before&&GetTickCount64()<deadline)if(am_pump(app,100)<0)return E_FAIL;
    if(app->presented!=before+1||!app->video.empty()||app->presented_width!=width||now_seconds()>=future
        ||app->interactive_skips!=skips+count-1||app->late_frames!=late||app->presentation_late.count!=lateness)return E_FAIL;
    // Returning to audio synchronization keeps queued PTS and decoder state intact.
    app->shell.redraw=false;const double scheduled=now_seconds()+.2;app->queue_video_frame({sample,scheduled,width,height});
    am_video_sync(app,1);
    if(app->video.size()!=1||app->video.front().pts!=scheduled||app->decoder_instances!=instances)return E_FAIL;
    const uint64_t shown=app->presented;const double inspect_until=now_seconds()+.04;
    do {
        if(am_pump(app,5)<0)return E_FAIL;
        if(app->video.size()!=1||app->video.front().pts!=scheduled||app->presentation_late.count!=lateness||now_seconds()>=scheduled)return E_FAIL;
    }while(now_seconds()<inspect_until);
    deadline=GetTickCount64()+1000;
    while(!app->video.empty()&&GetTickCount64()<deadline)if(am_pump(app,100)<0)return E_FAIL;
    if(app->presented<shown+1||!app->video.empty()||app->presentation_late.count!=lateness+1||app->audio_frames)return E_FAIL;
    return S_OK;
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
    if(SUCCEEDED(hr))hr=probe_shape_schedule(app);
    if(SUCCEEDED(hr))hr=probe_interactive_video(app);
    if(FAILED(hr)){error_hr("synthetic GPU probe",hr);return 0;}
    if(app->decoder_surfaces-before<60){error_hr("insufficient hardware decoder surfaces",E_FAIL);return 0;}return 1;
}
extern "C" int am_ui_probe(void* p,const char* directory) {
    // Readiness polling must not consume an asynchronous completion signal.
    {Discovery d;if(!d.done.h)return 0;Discovery::callback(ERROR_SUCCESS,&d,nullptr);
        if(am_discovery_status(&d)!=1||am_discovery_status(&d)!=1){error_hr("discovery completion must remain signaled",E_FAIL);return 0;}
        ResetEvent(d.done.h);d.status=ERROR_IO_PENDING;if(am_discovery_status(&d)!=0)return 0;}
    auto app=static_cast<App*>(p);
    HRESULT wait_probe=probe_media_wait(app);if(FAILED(wait_probe)){error_hr("deadline-driven media wait regression",wait_probe);return 0;}
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
