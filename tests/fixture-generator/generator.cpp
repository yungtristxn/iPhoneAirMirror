// Development-only synthetic AAC-ELD fixture. This encoder is not linked into AirMirrorLite.
#include <aacenc_lib.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <array>
extern "C" int generate(const char* path){
    HANDLE_AACENCODER enc=nullptr;if(aacEncOpen(&enc,0,2)!=AACENC_OK)return 1;
    struct Guard{HANDLE_AACENCODER* p;~Guard(){aacEncClose(p);}}guard{&enc};
    for(auto p: {std::pair<AACENC_PARAM,UINT>{AACENC_AOT,39},{AACENC_SAMPLERATE,44100},{AACENC_CHANNELMODE,MODE_2},{AACENC_BITRATE,128000},{AACENC_TRANSMUX,0},{AACENC_GRANULE_LENGTH,480}}){if(aacEncoder_SetParam(enc,p.first,p.second)!=AACENC_OK)return 2;}
    if(aacEncEncode(enc,nullptr,nullptr,nullptr,nullptr)!=AACENC_OK)return 3;
    FILE* file=nullptr;if(fopen_s(&file,path,"wb")||!file)return 4;
    for(int frame=0;frame<120;++frame){
        std::array<int16_t,960> pcm;for(int i=0;i<480;++i){double t=double(frame*480+i)/44100;int16_t v=int16_t(1000*sin(t*440*6.283185307179586));pcm[i*2]=pcm[i*2+1]=v;}
        unsigned char output[8192];void* ip=pcm.data();void* op=output;
        INT iid=IN_AUDIO_DATA,oid=OUT_BITSTREAM_DATA,isize=1920,osize=8192,ielem=2,oelem=1;
        AACENC_BufDesc in{1,&ip,&iid,&isize,&ielem},out{1,&op,&oid,&osize,&oelem};
        AACENC_InArgs ia{};ia.numInSamples=960;AACENC_OutArgs oa{};
        if(aacEncEncode(enc,&in,&out,&ia,&oa)!=AACENC_OK){fclose(file);return 5;}
        if(oa.numOutBytes>0){uint32_t size=oa.numOutBytes;fwrite(&size,4,1,file);fwrite(output,1,size,file);}
    }
    fclose(file);return 0;
}
