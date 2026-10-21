// Diagnostic only: OS H.264 encoder makes synthetic bars in memory.
// No capture, recording or network traffic. Exercise the actual decoder/render path.
using ProbeValidator=int(*)(const uint8_t*,unsigned,unsigned);
struct VideoProbeContext {ProbeValidator validate;unsigned validated=0,dpb16_sps=0;};
// Test-only RBSP edits raise declared capacity, not actual reference usage. The
// encoded picture data is untouched and is checked by Rust before MF sees it.
struct ProbeBitEdit {size_t first,last;std::vector<bool> replacement;};
struct ProbeBits {
    std::vector<bool> bits;size_t pos=0;bool valid=true;std::vector<ProbeBitEdit> edits;
    uint32_t read(unsigned count){if(count>32||pos+count>bits.size()){valid=false;return 0;}uint32_t value=0;while(count--)value=(value<<1)|unsigned(bits[pos++]);return value;}
    void skip(unsigned count){if(pos+count>bits.size())valid=false;else pos+=count;}
    uint32_t ue(){unsigned zeros=0;while(valid&&read(1)==0){if(++zeros>24){valid=false;return 0;}}return valid?((1u<<zeros)-1+read(zeros)):0;}
    void replace(size_t first,size_t last,uint32_t value,unsigned count){std::vector<bool> replacement;for(unsigned i=count;i>0;--i)replacement.push_back(((value>>(i-1))&1)!=0);edits.push_back({first,last,std::move(replacement)});}
    void level(){size_t first=pos;read(8);replace(first,pos,153,8);}
    void ptl(unsigned sublayers){
        skip(88);level();std::array<bool,7> profile{},level_present{};
        for(unsigned i=0;i<sublayers;++i){profile[i]=read(1)!=0;level_present[i]=read(1)!=0;}
        if(sublayers)for(unsigned i=sublayers;i<8;++i)if(read(2))valid=false;
        for(unsigned i=0;i<sublayers;++i){if(profile[i])skip(88);if(level_present[i])level();}
    }
    void dpb16(){size_t first=pos;uint32_t original=ue();if(original>15)valid=false;replace(first,pos,16,9);/* ue(15) = 000010000 */}
};
static bool probe_dpb16_parameter(const uint8_t* nal,size_t length,std::vector<uint8_t>& result){
    if(length<3||length>16384)return false;const unsigned type=(nal[0]>>1)&63;if(type!=32&&type!=33)return false;
    ProbeBits b;unsigned zeros=0;
    for(size_t i=2;i<length;++i){uint8_t value=nal[i];if(zeros>=2&&value==3){if(i+1>=length||nal[i+1]>3)return false;zeros=0;continue;}
        for(int bit=7;bit>=0;--bit)b.bits.push_back(((value>>bit)&1)!=0);zeros=value==0?zeros+1:0;}
    while(!b.bits.empty()&&!b.bits.back())b.bits.pop_back();if(b.bits.empty())return false;
    unsigned layers=0;
    if(type==32){b.skip(6);if(b.read(6)!=0)return false;layers=b.read(3);b.skip(1);if(b.read(16)!=65535)return false;}
    else {b.skip(4);layers=b.read(3);b.skip(1);}
    if(layers>6)return false;b.ptl(layers);
    if(type==33){b.ue();if(b.ue()!=1)return false;b.ue();b.ue();if(b.read(1))for(int i=0;i<4;++i)b.ue();if(b.ue()!=0||b.ue()!=0)return false;b.ue();}
    const bool all=b.read(1)!=0;for(unsigned i=all?0:layers;i<=layers;++i){b.dpb16();if(b.ue()>15)b.valid=false;b.ue();}
    if(!b.valid||b.pos>=b.bits.size())return false;
    for(auto edit=b.edits.rbegin();edit!=b.edits.rend();++edit){b.bits.erase(b.bits.begin()+edit->first,b.bits.begin()+edit->last);b.bits.insert(b.bits.begin()+edit->first,edit->replacement.begin(),edit->replacement.end());}
    result={nal[0],nal[1]};zeros=0;
    for(size_t i=0;i<b.bits.size();i+=8){uint8_t value=0;for(unsigned bit=0;bit<8;++bit){value=uint8_t(value<<1);if(i+bit<b.bits.size()&&b.bits[i+bit])value|=1;}
        if(zeros>=2&&value<=3){result.push_back(3);zeros=0;}result.push_back(value);zeros=value==0?zeros+1:0;}
    if(type==33){
        // Independent byte-level readback of general_level_idc after escaping.
        std::vector<uint8_t> check;unsigned z=0;
        for(size_t i=2;i<result.size();++i){uint8_t v=result[i];if(z>=2&&v==3){z=0;continue;}check.push_back(v);z=v==0?z+1:0;}
        if(check.size()<13||check[12]!=eAVEncH265VLevel5_1)return false;
    }
    return result.size()<=16384;
}
static bool probe_dpb16_packet(std::vector<uint8_t>& packet,unsigned& sps_count){
    auto start=[&](size_t from,size_t& prefix){for(size_t i=from;i+2<packet.size();++i)if(packet[i]==0&&packet[i+1]==0){if(packet[i+2]==1){prefix=3;return i;}if(i+3<packet.size()&&packet[i+2]==0&&packet[i+3]==1){prefix=4;return i;}}prefix=0;return packet.size();};
    size_t prefix=0,pos=start(0,prefix);if(pos!=0||!prefix)return false;std::vector<uint8_t> rewritten;unsigned count=0;
    while(pos<packet.size()){
        const size_t first=pos+prefix;size_t next_prefix=0,next=start(first,next_prefix);if(next-first<2||++count>4096)return false;
        const unsigned type=(packet[first]>>1)&63;rewritten.insert(rewritten.end(),{0,0,0,1});
        if(type==32||type==33){std::vector<uint8_t> parameter;if(!probe_dpb16_parameter(packet.data()+first,next-first,parameter))return false;rewritten.insert(rewritten.end(),parameter.begin(),parameter.end());if(type==33)++sps_count;}
        else rewritten.insert(rewritten.end(),packet.begin()+first,packet.begin()+next);
        if(rewritten.size()>4u*1024*1024)return false;pos=next;prefix=next_prefix;
    }
    packet=std::move(rewritten);return true;
}
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
static HRESULT probe_video(App* app,VideoProbeContext& probe, UINT width, UINT height,UINT codec=264,bool continue_stream=false,bool dpb16=false,UINT rotation=0) {
    // These independent encoder fixtures represent separate phones/sessions.
    if(!continue_stream){app->shell.reset_video();CHECK(app->init_decoder(codec,width,height));}
    else if(!app->decoder_ready||app->codec!=codec)return E_UNEXPECTED;
    const auto pending_before=app->decode_pending.active_count;const auto inputs_before=probe.validated;const auto decoded_before=app->decoded;
    if(app->low_latency_attribute!=1||(SUCCEEDED(app->low_latency_get)&&app->low_latency_value!=1)){error_hr("decoder low-latency readback",E_FAIL);return E_FAIL;}
    ComPtr<IMFTransform> enc;if(codec==265){CHECK(create_hevc_transform(true,&enc));}else{CHECK(CoCreateInstance(CLSID_CMSH264EncoderMFT,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&enc)));}
    ComPtr<IMFMediaType> output; CHECK(MFCreateMediaType(&output));
    CHECK(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)); CHECK(output->SetGUID(MF_MT_SUBTYPE, codec==265?MFVideoFormat_HEVC:MFVideoFormat_H264));
    CHECK(output->SetUINT32(MF_MT_AVG_BITRATE, codec==265?16000000:4000000));
    // This OS encoder otherwise emits level_idc=15 on the test host. Specify a
    // conforming level so the generated fixtures satisfy the real parser too.
    if(codec==265){CHECK(output->SetUINT32(MF_MT_MPEG2_PROFILE,1));CHECK(output->SetUINT32(MF_MT_MPEG2_LEVEL,eAVEncH265VLevel5_1));}
    CHECK(output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
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
            std::vector<uint8_t> packet(bytes,bytes+len);LONGLONG pts;hr=sample->GetSampleTime(&pts);
            if(SUCCEEDED(hr)&&dpb16&&!probe_dpb16_packet(packet,probe.dpb16_sps)){error_hr("synthetic DPB16 parameter rewrite",E_FAIL);hr=E_FAIL;}
            if(SUCCEEDED(hr)){if(!probe.validate||probe.validate(packet.data(),unsigned(packet.size()),codec)!=1){error_hr("production parser rejected synthetic access unit",E_FAIL);hr=E_FAIL;}else ++probe.validated;}
            if(SUCCEEDED(hr))hr=app->submit(packet.data(),unsigned(packet.size()),pts/1e7,rotation);compressed->Unlock();CHECK(hr);
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
    const uint64_t inputs=probe.validated-inputs_before,outputs=app->decoded-decoded_before;
    const unsigned pending_after=app->decode_pending.active_count;
    fprintf(stderr,"synthetic decoder accounting codec=%u size=%ux%u rotation=%u continued=%u inputs=%llu outputs=%llu pending_before=%u pending_after=%u peak=%u\n",
        codec,width,height,rotation,unsigned(continue_stream),static_cast<unsigned long long>(inputs),static_cast<unsigned long long>(outputs),pending_before,pending_after,app->decode_pending.high_water);
    if(outputs>inputs+pending_before||inputs+pending_before-outputs!=pending_after
        ||(continue_stream&&pending_after>pending_before)){error_hr("decoder associations accumulated across format transition",E_FAIL);return E_FAIL;}
    // A paused redraw uses the retained private texture, not a live decoder surface.
    if(!app->last_frame.sample){error_hr("no retained frame after encoder drain",E_FAIL);return E_FAIL;}
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
    if(GetWindowLongPtrW(app->window,GWL_STYLE)&WS_CAPTION){error_hr("fullscreen caption regression",E_FAIL);return E_FAIL;}
    CHECK(redraw_paused());
    if(!SendMessageTimeoutW(app->window,WM_KEYDOWN,VK_F11,0,SMTO_ABORTIFHUNG,2000,&result))return HRESULT_FROM_WIN32(GetLastError());
    if((GetWindowLongPtrW(app->window,GWL_STYLE)&WS_CAPTION)||app->shell.fullscreen){error_hr("fullscreen return regression",E_FAIL);return E_FAIL;}
    CHECK(redraw_paused());
    if(!SetWindowPos(app->window,nullptr,original.left,original.top,original.right-original.left,original.bottom-original.top,SWP_NOZORDER|SWP_NOACTIVATE))return HRESULT_FROM_WIN32(GetLastError());
    CHECK(redraw_paused());
    // A queued future picture cannot be shown early, and wait0 still services already-due work.
    app->shell.redraw=false;const auto before=app->presentation_late.count;const double due=now_seconds()+.06;
    auto future_frame=app->last_frame;future_frame.pts=due;app->video.push_back(std::move(future_frame));
    if(am_pump(app,0)<0)return E_FAIL;
    if(app->presentation_late.count>before&&now_seconds()<due){error_hr("future picture early presentation",E_FAIL);return E_FAIL;}
    ULONGLONG deadline=GetTickCount64()+1000;
    while(!app->video.empty()&&GetTickCount64()<deadline)if(am_pump(app,100)<0)return E_FAIL;
    if(app->presentation_late.count!=before+1||!app->video.empty()){error_hr("future picture did not drain exactly once",E_FAIL);return HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
    const auto late=app->late_frames,shown=app->presentation_late.count;const double now=now_seconds();
    for(int i=0;i<3;++i){auto frame=app->last_frame;frame.pts=now-.03+i*.01;app->video.push_back(std::move(frame));}
    if(am_pump(app,0)<0)return E_FAIL;
    deadline=GetTickCount64()+1000;
    while(!app->video.empty()&&GetTickCount64()<deadline)if(am_pump(app,100)<0)return E_FAIL;
    if(app->presentation_late.count!=shown+1||!app->video.empty()||app->late_frames!=late+2){error_hr("latest-due backlog did not coalesce",E_FAIL);return E_FAIL;}
    return S_OK;
}
static HRESULT probe_letterbox_pixels(App* app,bool offset=false,VideoColor color={},UINT rotation=0) {
    // This readback is restricted to a freshly generated diagnostic pattern, never sender media.
    constexpr UINT width=640,height=360,border=16;
    const UINT ox=offset?16:0,oy=offset?8:0,coded_w=width+(offset?32:0),coded_h=height+(offset?24:0);
    std::vector<uint8_t> pixels(coded_w*coded_h*3/2,128);std::fill(pixels.begin(),pixels.begin()+coded_w*coded_h,uint8_t(16));
    const uint8_t levels[2][2]={{64,112},{160,208}};
    const uint8_t chroma[2][2][2]={{{192,192},{64,192}},{{192,64},{64,64}}};
    const uint8_t white=color.range==MFNominalRange_0_255?255:235;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        pixels[(y+oy)*coded_w+x+ox]=(x<border||y<border||x>=width-border||y>=height-border)?white:levels[y>=height/2][x>=width/2];
    for(UINT y=border;y<height-border;y+=2)for(UINT x=border;x<width-border;x+=2){size_t p=size_t(coded_w)*coded_h+(y+oy)/2*coded_w+x+ox;pixels[p]=chroma[y>=height/2][x>=width/2][0];pixels[p+1]=chroma[y>=height/2][x>=width/2][1];}
    D3D11_TEXTURE2D_DESC pattern_desc{};pattern_desc.Width=coded_w;pattern_desc.Height=coded_h;pattern_desc.MipLevels=1;pattern_desc.ArraySize=1;
    pattern_desc.Format=DXGI_FORMAT_NV12;pattern_desc.SampleDesc.Count=1;pattern_desc.Usage=D3D11_USAGE_DEFAULT;
    pattern_desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(),coded_w,UINT(pixels.size())};ComPtr<ID3D11Texture2D> pattern;
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
    const RECT source{LONG(ox),LONG(oy),LONG(ox+width),LONG(oy+height)};
    const ULONGLONG deadline=GetTickCount64()+1000;
    while(app->presented==before&&GetTickCount64()<deadline){
        if(!app->video_slot_reserved){DWORD ready=WaitForSingleObject(app->video_event.h,20);if(ready==WAIT_TIMEOUT)continue;if(ready!=WAIT_OBJECT_0)return E_FAIL;app->video_slot_reserved=true;}
        CHECK(app->draw(sample.Get(),width,height,readback.Get(),&source,&color,false,rotation));if(app->presented==before)Sleep(1);
    }
    const UINT display_width=rotation%180?height:width,display_height=rotation%180?width:height;
    if(app->presented!=before+1||!app->video.empty()||app->presented_width!=display_width||app->presented_height!=display_height||app->presented_rotation!=rotation)return E_FAIL;
    if(app->vieww!=read_desc.Width||app->viewh!=read_desc.Height)return E_FAIL;
    D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(app->context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    auto gray=[&](UINT x,UINT y,int expected,int tolerance) {
        auto p=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;
        if(std::abs(int(p[0])-expected)<=tolerance&&std::abs(int(p[1])-expected)<=tolerance
            &&std::abs(int(p[2])-expected)<=tolerance&&p[3]>=250)return true;
        char detail[180];sprintf_s(detail,"synthetic letterbox pixel (%u,%u) BGRA=%u,%u,%u,%u expected_gray=%d",x,y,p[0],p[1],p[2],p[3],expected);error_hr(detail,E_FAIL);return false;
    };
    const UINT dw=read_desc.Width,dh=UINT(double(dw)*display_height/display_width),top=(read_desc.Height-dh)/2;
    bool valid=top>8&&dh>32;
    if(valid){
        // Border corners survive scaling: a center crop would lose these markers.
        for(double x:{.01,.99})for(double y:{.02,.98})valid=gray(UINT(x*(dw-1)),top+UINT(y*(dh-1)),255,25)&&valid;
        for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x){
            const bool full=color.range==MFNominalRange_0_255,bt709=color.matrix==MFVideoTransferMatrix_BT709;
            double luma=full?levels[y][x]:(levels[y][x]-16)*255.0/219,u=(chroma[y][x][0]-128)*(full?1.0:255.0/224),v=(chroma[y][x][1]-128)*(full?1.0:255.0/224);
            double reference[3]={luma+(bt709?1.8556:1.772)*u,luma-(bt709?.187324:.344136)*u-(bt709?.468124:.714136)*v,luma+(bt709?1.5748:1.402)*v};
            // Follow each source quadrant through the explicit clockwise transform.
            UINT dx=x,dy=y;
            if(rotation==90){dx=1-y;dy=x;}else if(rotation==180){dx=1-x;dy=1-y;}else if(rotation==270){dx=y;dy=1-x;}
            UINT px=UINT((dx?.75:.25)*(dw-1)),py=top+UINT((dy?.75:.25)*(dh-1));auto pixel=static_cast<const uint8_t*>(mapped.pData)+size_t(py)*mapped.RowPitch+size_t(px)*4;
            for(int channel=0;channel<3;++channel)if(std::abs(int(pixel[channel])-int(std::lround(std::clamp(reference[channel],0.0,255.0))))>8){error_hr("synthetic aperture/color quadrant mismatch",E_FAIL);valid=false;}
        }
        for(double x:{.1,.5,.9}){
            valid=gray(UINT(x*(dw-1)),top/2,0,8)&&valid;
            valid=gray(UINT(x*(dw-1)),read_desc.Height-1-top/2,0,8)&&valid;
        }
    }
    auto pixel_hash=[&](const D3D11_MAPPED_SUBRESOURCE& data){
        uint64_t hash=1469598103934665603ull;
        for(UINT y=0;y<read_desc.Height;++y){const auto row=static_cast<const uint8_t*>(data.pData)+size_t(y)*data.RowPitch;
            for(size_t x=0;x<size_t(read_desc.Width)*4;++x){hash^=row[x];hash*=1099511628211ull;}}
        return hash;
    };
    std::vector<uint8_t> displayed_pixels(size_t(read_desc.Width)*read_desc.Height*4);
    for(UINT y=0;y<read_desc.Height;++y)memcpy(displayed_pixels.data()+size_t(y)*read_desc.Width*4,static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(read_desc.Width)*4);
    const auto displayed_hash=pixel_hash(mapped);app->context->Unmap(readback.Get(),0);
    if(!EqualRect(&app->last_frame.source,&source)||app->last_frame.color.matrix!=color.matrix||app->last_frame.color.range!=color.range||app->last_frame.rotation!=rotation)return E_FAIL;
    if(!valid)return E_FAIL;
    // A different picture/rotation that cannot Present must not replace paused
    // pixels or metadata. Mutate only this private synthetic source to black.
    std::fill(pixels.begin(),pixels.begin()+size_t(coded_w)*coded_h,uint8_t(color.range==MFNominalRange_0_255?0:16));
    std::fill(pixels.begin()+size_t(coded_w)*coded_h,pixels.end(),uint8_t(128));
    app->context->UpdateSubresource(pattern.Get(),0,nullptr,pixels.data(),coded_w,UINT(pixels.size()));
    if(!app->video_slot_reserved){if(WaitForSingleObject(app->video_event.h,1000)!=WAIT_OBJECT_0)return E_FAIL;app->video_slot_reserved=true;}
    const auto cached=app->last_frame.sample;const auto presented=app->presented;
    if(app->draw(sample.Get(),width,height,nullptr,&source,&color,true,(rotation+90)%360)!=S_FALSE
        ||app->presented!=presented||app->last_frame.sample.Get()!=cached.Get()||app->last_frame.rotation!=rotation||app->presented_rotation!=rotation)return E_FAIL;
    const ULONGLONG paused_deadline=GetTickCount64()+1000;
    do {CHECK(app->draw(cached.Get(),width,height,readback.Get(),&source,&color,false,rotation));if(app->presented>presented)break;Sleep(1);}while(GetTickCount64()<paused_deadline);
    if(app->presented!=presented+1)return E_FAIL;
    CHECK(app->context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
    const bool retained=pixel_hash(mapped)==displayed_hash;
    if(!retained){size_t changed=0;unsigned maximum=0;int reports=0;
        for(UINT y=0;y<read_desc.Height;++y)for(UINT x=0;x<read_desc.Width*4;++x){
            const auto value=*(static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+x),previous=displayed_pixels[size_t(y)*read_desc.Width*4+x];
            if(value!=previous){++changed;maximum=std::max(maximum,unsigned(std::abs(int(value)-int(previous))));if(reports++<5)fprintf(stderr,"paused pixel (%u,%u) channel%u old=%u now=%u\n",x/4,y,x%4,previous,value);}}
        fprintf(stderr,"paused synthetic comparison rotation=%u offset=%u changed=%zu max_delta=%u size=%ux%u\n",rotation,unsigned(offset),changed,maximum,read_desc.Width,read_desc.Height);}
    app->context->Unmap(readback.Get(),0);
    if(!retained){error_hr("busy rotation replaced paused synthetic pixels",E_FAIL);return E_FAIL;}return S_OK;
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
    CHECK(probe_letterbox_pixels(app,true));
    CHECK(probe_letterbox_pixels(app,true,{MFVideoTransferMatrix_BT601,MFNominalRange_16_235}));
    CHECK(probe_letterbox_pixels(app,true,{MFVideoTransferMatrix_BT601,MFNominalRange_0_255}));
    CHECK(probe_letterbox_pixels(app,true,{MFVideoTransferMatrix_BT709,MFNominalRange_0_255}));
    for(UINT rotation:{90u,180u,270u,0u})CHECK(probe_letterbox_pixels(app,true,{},rotation));
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
static HRESULT probe_rotation_schedule(App* app){
    if(!app->last_frame.sample||!app->video.empty()||!app->video_sync||app->last_frame.rotation)return E_FAIL;
    const auto original=app->last_frame;const UINT w=original.width,h=original.height;
    const auto lateness=app->presentation_late.count;const double first_due=now_seconds()+.15,second_due=first_due+.4;
    auto first=original;first.pts=first_due;first.rotation=90;
    auto second=original;second.pts=second_due;second.rotation=270;
    app->queue_video_frame(std::move(first));app->queue_video_frame(std::move(second));
    const double inspect=now_seconds()+.035;
    do {
        app->shell.redraw=true;if(am_pump(app,5)<0)return E_FAIL;
        if(now_seconds()>=first_due||app->presented_rotation||app->last_frame.rotation||app->video.size()!=2
            ||app->presented_width!=w||app->presented_height!=h)return E_FAIL;
    }while(now_seconds()<inspect);
    ULONGLONG deadline=GetTickCount64()+1000;
    while(app->presented_rotation!=90&&GetTickCount64()<deadline)if(am_pump(app,20)<0)return E_FAIL;
    if(app->presented_rotation!=90||app->last_frame.rotation!=90||app->video.size()!=1
        ||app->presented_width!=h||app->presented_height!=w||now_seconds()>=second_due)return E_FAIL;
    // Paused redraw while a newer orientation is queued must keep the old one.
    app->shell.redraw=true;if(am_pump(app,0)<0||app->presented_rotation!=90||app->last_frame.rotation!=90)return E_FAIL;
    deadline=GetTickCount64()+1000;
    while(!app->video.empty()&&GetTickCount64()<deadline)if(am_pump(app,20)<0)return E_FAIL;
    if(app->presented_rotation!=270||app->last_frame.rotation!=270||app->presentation_late.count!=lateness+2)return E_FAIL;
    auto restore=original;restore.pts=now_seconds();app->queue_video_frame(std::move(restore));
    deadline=GetTickCount64()+1000;
    while((!app->video.empty()||app->presented_rotation)&&GetTickCount64()<deadline)if(am_pump(app,20)<0)return E_FAIL;
    if(!app->video.empty()||app->presented_rotation||app->last_frame.rotation||app->presented_width!=w||app->presented_height!=h)return E_FAIL;
    bool settled=false;deadline=GetTickCount64()+1000;
    do {
        if(am_pump(app,10)<0)return E_FAIL;RECT client{};GetClientRect(app->window,&client);const int rim=2*PhoneShell::rim_pixels(app->shell.dpi_scale());
        settled=client.right>rim&&client.bottom>rim&&std::abs(double(client.right-rim)/(client.bottom-rim)-double(w)/h)<.02
            &&app->vieww==UINT(client.right-rim)&&app->viewh==UINT(client.bottom-rim);
    }while(!settled&&GetTickCount64()<deadline);
    if(!settled)return E_FAIL;
    // An unsupported capability or invalid angle must leave the displayed state intact.
    const bool support=app->processor_rotation;app->processor_rotation=false;
    HRESULT unsupported=app->draw(original.sample.Get(),w,h,nullptr,&original.source,&original.color,false,90);
    app->processor_rotation=support;
    if(unsupported!=DXGI_ERROR_UNSUPPORTED||app->presented_rotation||app->last_frame.rotation)return E_FAIL;
    if(app->draw(nullptr,w,h,nullptr,nullptr,nullptr,false,45)!=E_INVALIDARG)return E_FAIL;
    last_error.clear();return S_OK;
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
static HRESULT probe_first_present_commit(App* app){
    if(!app->last_frame.sample)return E_FAIL;auto sample=app->last_frame.sample;
    const UINT width=app->last_frame.width,height=app->last_frame.height;app->shell.reset_video();
    if(!app->video_slot_reserved){if(WaitForSingleObject(app->video_event.h,1000)!=WAIT_OBJECT_0)return E_FAIL;app->video_slot_reserved=true;}
    const auto before=app->presented;
    HRESULT busy=app->draw(sample.Get(),(width/2)&~1u,height,nullptr,nullptr,nullptr,true);
    if(busy!=S_FALSE||app->shell.has_video||app->shell.status==2||app->presented!=before)return E_FAIL;
    ULONGLONG deadline=GetTickCount64()+1000;
    do {CHECK(app->draw(sample.Get(),width,height));if(app->presented>before)break;Sleep(2);}while(GetTickCount64()<deadline);
    if(app->presented!=before+1||!app->shell.has_video)return E_FAIL;
    do {RECT r{};GetClientRect(app->window,&r);int rim=PhoneShell::rim_pixels(app->shell.dpi_scale());
        if(r.bottom>2*rim&&std::abs(double(r.right-2*rim)/(r.bottom-2*rim)-double(width)/height)<.02)return S_OK;
        if(am_pump(app,10)<0)return E_FAIL;
    }while(GetTickCount64()<deadline);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
static HRESULT probe_minimized_output(App* app){
    if(!app->last_frame.sample)return E_FAIL;WINDOWPLACEMENT original{sizeof(original)};if(!GetWindowPlacement(app->window,&original))return E_FAIL;
    const BOOL visible=IsWindowVisible(app->window);auto minimized=original;minimized.showCmd=SW_SHOWMINNOACTIVE;
    if(!SetWindowPlacement(app->window,&minimized))return E_FAIL;ShowWindow(app->window,SW_HIDE);
    const auto presented=app->presented,lateness=app->presentation_late.count;auto sample=app->last_frame.sample;const UINT width=app->last_frame.width,height=app->last_frame.height;
    for(int i=0;i<4;++i)app->queue_video_frame({sample,now_seconds()+.2,width,height});HRESULT result=app->tick();
    bool drained=IsIconic(app->window)&&app->presented==presented&&app->video.size()==1;
    SetWindowPlacement(app->window,&original);if(!visible)ShowWindow(app->window,SW_HIDE);app->shell.redraw=true;
    CHECK(result);if(!drained){error_hr("minimized output did not retain exactly one frame without Present",E_FAIL);return E_FAIL;}
    // Restore may redraw the last presented picture before the retained future frame is due.
    // Only consumption of that queue entry (and its synchronized timing sample) proves restore.
    ULONGLONG deadline=GetTickCount64()+1000;while(!app->video.empty()&&GetTickCount64()<deadline)if(am_pump(app,20)<0)return E_FAIL;
    if(app->presented<=presented||!app->video.empty()||app->presentation_late.count!=lateness+1){char detail[180];sprintf_s(detail,"minimized restore queued=%zu presented_delta=%llu iconic=%d",app->video.size(),static_cast<unsigned long long>(app->presented-presented),int(IsIconic(app->window)));error_hr(detail,E_FAIL);return E_FAIL;}return S_OK;
}
extern "C" int am_video_probe(void* p,ProbeValidator validate) {
    if(!p||!validate)return 0;
    auto app=static_cast<App*>(p);uint64_t before=app->decoder_surfaces;VideoProbeContext probe{validate};
    const char* stage="H264 rotated landscape";HRESULT hr=probe_video(app,probe,1920,1080,264,false,false,90);
    if(SUCCEEDED(hr)&&(app->presented_rotation!=90||app->presented_width!=1080||app->presented_height!=1920))hr=E_FAIL;
    if(SUCCEEDED(hr)){stage="H264 portrait";hr=probe_video(app,probe,1080,1920);}
    if(SUCCEEDED(hr)&&app->hevc_available){
        // A partial codec-initialization failure must be recoverable, not a null dereference.
        app->decoder.Reset();app->decoder_ready=false;const uint8_t dummy=0;
        if(app->submit(&dummy,1,now_seconds())!=MF_E_TRANSFORM_TYPE_NOT_SET)return 0;
        stage="HEVC landscape";uint64_t hevc_before=app->decoder_surfaces;hr=probe_video(app,probe,3840,2160,265);
        if(SUCCEEDED(hr)&&(app->decoder_surfaces-hevc_before<8||app->width!=3840||app->height!=2160))hr=E_FAIL;
        if(SUCCEEDED(hr)){stage="HEVC portrait";hr=probe_video(app,probe,992,2160,265);}
        if(SUCCEEDED(hr)&&(app->width!=992||app->height!=2160))hr=E_FAIL;
        // Retain one decoder and one learned phone aspect while real encoded
        // sequence headers change orientation/resolution. DPB16 is signaled
        // capacity; the OS encoder is not claimed to reference sixteen pictures.
        if(SUCCEEDED(hr))stage="HEVC DPB16 production-parser rotation matrix";
        for(auto shape:{std::pair<UINT,UINT>{496,1080},{1080,496},{2160,992},{992,2160}}){
            if(FAILED(hr))break;const auto surfaces=app->decoder_surfaces;const auto headers=probe.dpb16_sps;
            hr=probe_video(app,probe,shape.first,shape.second,265,true,true);
            if(SUCCEEDED(hr)&&(probe.dpb16_sps<=headers||app->decoder_surfaces<=surfaces||app->width!=shape.first||app->height!=shape.second))hr=E_FAIL;
        }
    }
    if(SUCCEEDED(hr)){stage="shape scheduling/aperture/color pixels";hr=probe_shape_schedule(app);}
    if(SUCCEEDED(hr)){stage="queued per-frame rotation and paused commitment";hr=probe_rotation_schedule(app);}
    if(SUCCEEDED(hr)){stage="interactive/synchronized transition";hr=probe_interactive_video(app);}
    if(SUCCEEDED(hr)){stage="first successful presentation commitment";hr=probe_first_present_commit(app);}
    if(SUCCEEDED(hr)){stage="minimized output/restore";hr=probe_minimized_output(app);}
    if(FAILED(hr)){error_hr(stage,hr);return 0;}
    if(app->decoder_surfaces-before<60){error_hr("insufficient hardware decoder surfaces",E_FAIL);return 0;}
    if(probe.validated<60){error_hr("insufficient production-validated synthetic access units",E_FAIL);return 0;}
    fprintf(stderr,"synthetic parser/GPU coverage: access_units=%u dpb16_sps=%u surfaces=%llu\n",probe.validated,probe.dpb16_sps,static_cast<unsigned long long>(app->decoder_surfaces-before));
    if(!app->input_prepare.count||!std::isfinite(app->input_prepare.average())||app->input_prepare.minimum<0||app->input_prepare.maximum<app->input_prepare.minimum)return 0;
    return 1;
}
static HRESULT probe_native_contracts(App* app){
    CHECK(probe_platform_contracts());
    {
        DecodeAssociations pending;DecodeAssociations::Entry value;
        if(!pending.add(100,1,0)||!pending.add(300,2,90)||!pending.add(200,3,270))return E_FAIL;
        if(!pending.take(200,value)||value.rotation!=270||value.began!=3
            ||!pending.take(100,value)||value.rotation!=0
            ||!pending.take(300,value)||value.rotation!=90||pending.take(300,value))return E_FAIL;
        // Duplicate timestamps are safe only when their transform is identical.
        if(!pending.add(400,4,180)||!pending.add(400,5,180)||pending.add(400,6,90)||pending.add(500,6,45))return E_FAIL;
        if(!pending.take(400,value)||value.began!=4||!pending.take(400,value)||value.began!=5)return E_FAIL;
        for(unsigned i=0;i<32;++i)if(!pending.add(i,1,(i%4)*90))return E_FAIL;
        if(pending.add(1000,2,0)||pending.active_count!=32||pending.high_water!=32)return E_FAIL;
        if(!pending.take(15,value)||value.rotation!=270||!pending.add(1000,2,0))return E_FAIL;
        if(!pending.take(0,value)||value.rotation!=0)return E_FAIL;
        pending={};if(pending.active_count||pending.high_water||pending.take(1000,value)||!pending.add(1000,3,270))return E_FAIL;
    }
    StageTiming short_samples;short_samples.add(.000003);short_samples.add(.000006);
    if(short_samples.percentile(.95)>short_samples.maximum||short_samples.percentile(.95)<short_samples.minimum)return E_FAIL;
    MFVideoArea area{};area.OffsetX.value=16;area.OffsetY.value=8;area.Area={640,360};RECT rect{};
    CHECK(source_area(672,384,&area,rect));if(rect.left!=16||rect.top!=8||rect.right!=656||rect.bottom!=368)return E_FAIL;
    area.OffsetX.fract=1;if(SUCCEEDED(source_area(672,384,&area,rect)))return E_FAIL;area.OffsetX.fract=0;
    area.OffsetX.value=-1;if(SUCCEEDED(source_area(672,384,&area,rect)))return E_FAIL;area.OffsetX.value=40;
    if(SUCCEEDED(source_area(672,384,&area,rect)))return E_FAIL;
    ComPtr<IMFAttributes> attrs;CHECK(MFCreateAttributes(&attrs,2));VideoColor color;
    CHECK(attrs->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT601));CHECK(attrs->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_0_255));CHECK(media_color(attrs.Get(),color));
    if(color.matrix!=MFVideoTransferMatrix_BT601||color.range!=MFNominalRange_0_255)return E_FAIL;
    CHECK(attrs->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,99));if(SUCCEEDED(media_color(attrs.Get(),color)))return E_FAIL;
    // Fake endpoint ordering uses the production mixer/prime sequence, without touching WASAPI.
    std::deque<AudioFrame> queue;std::array<int16_t,64> pcm{};uint64_t played=0,skipped=0;std::string order;
    queue.push_back({std::vector<int16_t>(64,1234),0,10});
    auto fill=[&]{order+='G';mix_audio(queue,pcm.data(),32,0,1,played,skipped);order+='R';return S_OK;};
    auto start=[&]{order+='S';return S_OK;};CHECK(prime_audio(fill,start));
    if(order!="GRS"||played||std::any_of(pcm.begin(),pcm.end(),[](int16_t v){return v!=0;}))return E_FAIL;
    queue.clear();order.clear();CHECK(prime_audio(fill,start));if(order!="GRS"||played)return E_FAIL;
    queue.push_back({std::vector<int16_t>(64,2345),0,0});mix_audio(queue,pcm.data(),32,0,1,played,skipped);
    if(played!=32||pcm.front()!=2345||pcm.back()!=2345)return E_FAIL;
    bool started=false;if(SUCCEEDED(prime_audio([]{return E_FAIL;},[&]{started=true;return S_OK;}))||started)return E_FAIL;
    const auto recoveries=app->audio_recoveries;app->endpoint_notice->OnDeviceAdded(L"synthetic unrelated capture endpoint");app->service_recovery();
    if(app->audio_pending||app->audio_recoveries!=recoveries)return E_FAIL;
    // Repeated recoverable failures stop after three attempts, then a device event can rearm.
    HWND window=app->window;if(!app->recover_fault(AUDCLNT_E_DEVICE_INVALIDATED))return E_FAIL;
    for(int i=0;i<4;++i){app->audio_retry_at=0;app->service_recovery(true);}
    if(app->audio_retries!=3||!app->audio_pending||app->shell.status!=5)return E_FAIL;
    app->endpoint_notice->OnDefaultDeviceChanged(eRender,eConsole,nullptr);
    if(am_dispatch(app)!=2||app->audio_pending||!app->audio||app->audio_running||app->audio_frames)return E_FAIL;
    if(!app->recover_fault(DXGI_ERROR_DEVICE_RESET))return E_FAIL;
    for(int i=0;i<4;++i){app->graphics_retry_at=0;app->service_recovery(true);}
    if(app->graphics_retries!=3||!app->graphics_pending||app->device||app->shell.status!=5)return E_FAIL;
    if(!am_reset(app))return E_FAIL;app->service_recovery();
    if(app->graphics_pending||!app->device||!app->decoder_ready||app->window!=window||!IsWindow(window))return E_FAIL;
    if(am_dispatch(app)!=2)return E_FAIL;
    app->shell.set_status(6,L"");return S_OK;
}
static HRESULT probe_wait_transition(App* app){
    Handle blocked,wake,audio;blocked.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);wake.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);audio.h=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!blocked.h||!wake.h||!audio.h)return E_FAIL;
    struct Restore{App* app;HANDLE handle,wake,audio;bool reserved;~Restore(){app->video.clear();app->video_event.h=handle;app->wake.h=wake;app->audio_event.h=audio;app->video_slot_reserved=reserved;am_video_sync(app,1);}} restore{app,app->video_event.h,app->wake.h,app->audio_event.h,app->video_slot_reserved};
    app->video_event.h=blocked.h;app->wake.h=wake.h;app->audio_event.h=audio.h;app->video_slot_reserved=false;app->shell.redraw=false;am_video_sync(app,0);
    app->video.push_back({{},now_seconds()+5,16,16});ResetEvent(app->wake.h);ResetEvent(app->audio_event.h);
    const auto presented=app->presented;auto waiting=std::async(std::launch::async,[&]{return am_wait(app,1000);});
    if(waiting.wait_for(std::chrono::milliseconds(20))!=std::future_status::timeout)return E_FAIL;
    SetEvent(app->wake.h);if(waiting.get()!=1||app->presented!=presented||app->video.size()!=1)return E_FAIL;
    // Main-thread reconciliation happens between wait and dispatch, never after dispatch.
    am_video_sync(app,1);app->sound.push_back({std::vector<int16_t>(64,1200),0,now_seconds()+5});am_flush_audio(app);
    if(am_dispatch(app)!=1||app->presented!=presented||!app->sound.empty()||app->audio_frames)return E_FAIL;
    return S_OK;
}
static HRESULT probe_idle_status_pixels(App* app){
    if(app->shell.has_video)return E_UNEXPECTED;
    RECT client{};if(!GetClientRect(app->window,&client))return E_FAIL;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=UINT(client.right);desc.Height=UINT(client.bottom);desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;CHECK(app->device->CreateTexture2D(&desc,nullptr,&readback));
    // Read only the generated idle base, never video or desktop pixels. The GDI
    // card is independently checked by the UI test; no hidden label may survive
    // in this base, including outside that card's rectangle.
    for(int status:{3,4,5}){
        app->shell.set_status(status,std::wstring(240,L'W'));
        CHECK(app->chrome.layout(app->shell,nullptr,readback.Get()));
        D3D11_MAPPED_SUBRESOURCE mapped{};CHECK(app->context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        bool plain=true;
        for(UINT y=desc.Height/8;y<desc.Height*7/8&&plain;++y)for(UINT x=desc.Width/4;x<desc.Width*3/4;++x){
            const auto pixel=static_cast<const uint8_t*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;
            if(pixel[0]!=24||pixel[1]!=19||pixel[2]!=17||pixel[3]!=255){plain=false;break;}
        }
        app->context->Unmap(readback.Get(),0);if(!plain){error_hr("status idle base contains duplicate artwork",E_FAIL);return E_FAIL;}
    }
    app->shell.set_status(6,L"");return S_OK;
}
extern "C" int am_ui_probe(void* p,const char* directory) {
    // Readiness polling must not consume an asynchronous completion signal.
    {Discovery d;if(!d.done.h)return 0;Discovery::callback(ERROR_SUCCESS,&d,nullptr);
        if(am_discovery_status(&d)!=1||am_discovery_status(&d)!=1){error_hr("discovery completion must remain signaled",E_FAIL);return 0;}
        ResetEvent(d.done.h);d.status=ERROR_IO_PENDING;if(am_discovery_status(&d)!=0)return 0;}
    auto app=static_cast<App*>(p);
    HRESULT contracts=probe_native_contracts(app);if(SUCCEEDED(contracts))contracts=probe_wait_transition(app);
    if(FAILED(contracts)){error_hr("native recovery/transition contracts",contracts);return 0;}
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
    app->shell.set_status(1,L"Waiting for picture and sound");
    if(app->shell.status!=1){error_hr("negotiated session waiting state",E_FAIL);return 0;}
    app->shell.set_status(0,L"");
    hr=probe_idle_status_pixels(app);if(FAILED(hr)){error_hr("idle status pixel regression",hr);return 0;}
    auto file=path+L"/idle.png";hr=app->chrome.layout(app->shell,path.empty()?nullptr:file.c_str());
    if(FAILED(hr)){error_hr("idle artwork diagnostic",hr);return 0;}
    if(!path.empty()){
        app->shell.set_status(1,L"");file=path+L"/connecting.png";hr=app->chrome.layout(app->shell,file.c_str());
        if(FAILED(hr))return 0;
        app->shell.set_status(0,L"");
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
