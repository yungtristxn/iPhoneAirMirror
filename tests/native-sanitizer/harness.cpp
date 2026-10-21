// Test-only process. No network, device media, or Windows UI.
#include <array>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <windows.h>
#include <bcrypt.h>
extern "C" { void playfair_decrypt(const uint8_t*,const uint8_t*,uint8_t*);void* am_aac_open(unsigned);void am_aac_close(void*);int am_aac_decode(void*,const uint8_t*,unsigned,int16_t*,unsigned); }
static uint32_t state=0x12345678;
static uint32_t next(){state=state*1664525+1013904223;return state;}
static std::vector<uint8_t> read(const char* path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static bool pcm_hash(const std::vector<int16_t>& output,char hex[65]){
    BCRYPT_ALG_HANDLE algorithm=nullptr;unsigned char digest[32];
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0)return false;
    auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<int16_t*>(output.data())),ULONG(output.size()*sizeof(int16_t)),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);if(status!=0)return false;
    for(unsigned i=0;i<32;++i)sprintf_s(hex+i*2,3,"%02x",digest[i]);
    return true;
}
static bool signal_control(const std::vector<int16_t>& output,double left_hz,double right_hz){
    if(output.size()<=8194)return false;
    unsigned crossings[2]={};int peak=0;
    for(size_t i=8192;i+2<output.size();i+=2)for(unsigned channel=0;channel<2;++channel){
        int sample=output[i+channel];if(abs(sample)>peak)peak=abs(sample);
        if(sample<=0&&output[i+2+channel]>0)++crossings[channel];
    }
    double seconds=double(output.size()-8192)/2/44100;
    return peak>=500&&peak<=2000&&fabs(crossings[0]/seconds-left_hz)<=3&&fabs(crossings[1]/seconds-right_hz)<=3;
}
int main(int argc,char** argv){
    if(argc!=4)return 1;
    auto known=read(argv[1]);if(known.size()!=252)return 2;
    std::array<uint8_t,16> key{};playfair_decrypt(known.data(),known.data()+164,key.data());if(memcmp(key.data(),known.data()+236,16))return 3;
    for(unsigned mode=0;mode<4;++mode)for(unsigned iteration=0;iteration<256;++iteration){
        std::array<uint8_t,164> message;std::array<uint8_t,72> encrypted;
        for(auto& b:message)b=uint8_t(next()>>24);for(auto& b:encrypted)b=uint8_t(next()>>24);
        message[12]=uint8_t(mode);playfair_decrypt(message.data(),encrypted.data(),key.data());
    }
    void* decoder=am_aac_open(8);if(!decoder)return 4;
    auto audio=read(argv[2]);if(audio.empty()){am_aac_close(decoder);return 5;}size_t pos=0;unsigned frames=0;
    std::array<int16_t,4098> pcm{};std::vector<int16_t> eld_output;
    while(pos<audio.size()){
        if(pos+4>audio.size())return 5;uint32_t size;memcpy(&size,audio.data()+pos,4);pos+=4;if(size>8192||pos+size>audio.size())return 6;
        pcm.front()=12345;pcm.back()=12345;int n=am_aac_decode(decoder,audio.data()+pos,size,pcm.data()+1,4096);
        if(n!=960||pcm.front()!=12345||pcm.back()!=12345)return 7;
        eld_output.insert(eld_output.end(),pcm.data()+1,pcm.data()+1+n);pos+=size;++frames;
    }
    am_aac_close(decoder);
    for(unsigned iteration=0;iteration<1024;++iteration){
        if(iteration%16==0){decoder=am_aac_open(8);if(!decoder)return 8;}
        std::vector<uint8_t> input(1+next()%8192);for(auto& b:input)b=uint8_t(next()>>24);
        pcm.front()=12345;pcm.back()=12345;am_aac_decode(decoder,input.data(),unsigned(input.size()),pcm.data()+1,4096);
        if(pcm.front()!=12345||pcm.back()!=12345)return 9;
        if(iteration%16==15)am_aac_close(decoder);
    }
    auto lc=read(argv[3]);if(lc.empty())return 10;
    decoder=am_aac_open(4);if(!decoder)return 11;
    pos=0;unsigned lc_frames=0;std::vector<int16_t> output;
    while(pos<lc.size()){
        if(pos+4>lc.size())return 12;uint32_t size;memcpy(&size,lc.data()+pos,4);pos+=4;
        if(size==0||size>8192||pos+size>lc.size())return 13;
        pcm.front()=12345;pcm.back()=12345;
        int n=am_aac_decode(decoder,lc.data()+pos,size,pcm.data()+1,4096);
        if(n!=2048||pcm.front()!=12345||pcm.back()!=12345)return 14;
        output.insert(output.end(),pcm.data()+1,pcm.data()+1+n);pos+=size;++lc_frames;
    }
    am_aac_close(decoder);
    char hex[65],eld_hex[65];if(!pcm_hash(output,hex)||!pcm_hash(eld_output,eld_hex))return 15;
    if(lc_frames!=120||output.size()!=245760||strcmp(hex,"ec0e0ecf994a43665ab2ef72c245ca49385d5927353a531e97dbe84707b59ae9"))return 18;
    printf("LC_PCM_SHA256=%s frames=%u samples=%zu\n",hex,lc_frames,output.size());
    // Independent source-content control: generated LC is a 440/660 Hz stereo
    // triangle. Skip codec priming and count positive zero crossings per channel.
    if(!signal_control(output,440,660)||!signal_control(eld_output,440,440))return 17;
    if(frames!=120||eld_output.size()!=115200||strcmp(eld_hex,"c5679c95ef23189c71327d0e78e26ffd94c596f40902ccdfb7f5feaf256f8f6f"))return 19;
    printf("ELD_PCM_SHA256=%s frames=%u samples=%zu\n",eld_hex,frames,eld_output.size());
    printf("PASS ASan: FairPlay known answer + 1024 fixed-size valid-mode mutations; %u AAC-ELD and %u AAC-LC fixture frames, pinned PCM hashes and known sine/triangle frequency/channel controls + 1024 bounded malformed packets; PCM canaries intact.\n",frames,lc_frames);return 0;
}
