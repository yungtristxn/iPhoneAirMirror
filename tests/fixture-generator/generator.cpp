// Development-only synthetic AAC fixtures. This encoder is not linked into AirMirrorLite.
#include <aacenc_lib.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <array>
extern "C" int generate(const char* path,unsigned codec){
    if(codec!=4&&codec!=8)return 1;
    const unsigned samples=codec==8?480:1024;
    HANDLE_AACENCODER enc=nullptr;if(aacEncOpen(&enc,0,2)!=AACENC_OK)return 1;
    struct Guard{HANDLE_AACENCODER* p;~Guard(){aacEncClose(p);}}guard{&enc};
    for(auto p: {std::pair<AACENC_PARAM,UINT>{AACENC_AOT,codec==8?39u:2u},{AACENC_SAMPLERATE,44100},{AACENC_CHANNELMODE,MODE_2},{AACENC_BITRATE,128000},{AACENC_TRANSMUX,0},{AACENC_GRANULE_LENGTH,samples}}){if(aacEncoder_SetParam(enc,p.first,p.second)!=AACENC_OK)return 2;}
    if(aacEncEncode(enc,nullptr,nullptr,nullptr,nullptr)!=AACENC_OK)return 3;
    FILE* file=nullptr;if(fopen_s(&file,path,"wb")||!file)return 4;
    for(int frame=0;frame<120;++frame){
        std::array<int16_t,2048> pcm{};
        for(unsigned i=0;i<samples;++i){
            if(codec==8){double t=double(frame*samples+i)/44100;int16_t v=int16_t(1000*sin(t*440*6.283185307179586));pcm[i*2]=pcm[i*2+1]=v;}
            else for(unsigned channel=0;channel<2;++channel){
                // Integer-only, distinct 440/660 Hz triangles make LC source
                // reproducible and provide a channel/content control.
                unsigned phase=((frame*samples+i)*(channel?660u:440u))%44100;
                unsigned ramp=phase<22050?phase:44100-phase;
                pcm[i*2+channel]=int16_t(int(ramp*2000/22050)-1000);
            }
        }
        unsigned char output[8192];void* ip=pcm.data();void* op=output;
        INT iid=IN_AUDIO_DATA,oid=OUT_BITSTREAM_DATA,isize=INT(samples*4),osize=8192,ielem=2,oelem=1;
        AACENC_BufDesc in{1,&ip,&iid,&isize,&ielem},out{1,&op,&oid,&osize,&oelem};
        AACENC_InArgs ia{};ia.numInSamples=INT(samples*2);AACENC_OutArgs oa{};
        if(aacEncEncode(enc,&in,&out,&ia,&oa)!=AACENC_OK){fclose(file);return 5;}
        if(oa.numOutBytes>0){uint32_t size=oa.numOutBytes;if(fwrite(&size,4,1,file)!=1||fwrite(output,1,size,file)!=size){fclose(file);return 6;}}
    }
    return fclose(file)==0?0:7;
}
