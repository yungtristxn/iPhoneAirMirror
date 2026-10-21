// Test-only process. No network, device media, or Windows UI.
#include <array>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstring>
extern "C" { void playfair_decrypt(const uint8_t*,const uint8_t*,uint8_t*);void* am_aac_open(unsigned);void am_aac_close(void*);int am_aac_decode(void*,const uint8_t*,unsigned,int16_t*,unsigned); }
static uint32_t state=0x12345678;
static uint32_t next(){state=state*1664525+1013904223;return state;}
static std::vector<uint8_t> read(const char* path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){
    if(argc!=3)return 1;
    auto known=read(argv[1]);if(known.size()!=252)return 2;
    std::array<uint8_t,16> key{};playfair_decrypt(known.data(),known.data()+164,key.data());if(memcmp(key.data(),known.data()+236,16))return 3;
    for(unsigned mode=0;mode<4;++mode)for(unsigned iteration=0;iteration<256;++iteration){
        std::array<uint8_t,164> message;std::array<uint8_t,72> encrypted;
        for(auto& b:message)b=uint8_t(next()>>24);for(auto& b:encrypted)b=uint8_t(next()>>24);
        message[12]=uint8_t(mode);playfair_decrypt(message.data(),encrypted.data(),key.data());
    }
    void* decoder=am_aac_open(8);if(!decoder)return 4;
    auto audio=read(argv[2]);size_t pos=0;unsigned frames=0;
    std::array<int16_t,4098> pcm{};
    while(pos<audio.size()){
        if(pos+4>audio.size())return 5;uint32_t size;memcpy(&size,audio.data()+pos,4);pos+=4;if(size>8192||pos+size>audio.size())return 6;
        pcm.front()=12345;pcm.back()=12345;int n=am_aac_decode(decoder,audio.data()+pos,size,pcm.data()+1,4096);
        if(n!=960||pcm.front()!=12345||pcm.back()!=12345)return 7;pos+=size;++frames;
    }
    am_aac_close(decoder);
    for(unsigned iteration=0;iteration<1024;++iteration){
        if(iteration%16==0){decoder=am_aac_open(8);if(!decoder)return 8;}
        std::vector<uint8_t> input(1+next()%8192);for(auto& b:input)b=uint8_t(next()>>24);
        pcm.front()=12345;pcm.back()=12345;am_aac_decode(decoder,input.data(),unsigned(input.size()),pcm.data()+1,4096);
        if(pcm.front()!=12345||pcm.back()!=12345)return 9;
        if(iteration%16==15)am_aac_close(decoder);
    }
    printf("PASS ASan: FairPlay known answer + 1024 fixed-size valid-mode mutations; %u AAC-ELD fixture frames + 1024 bounded malformed packets; PCM canaries intact.\n",frames);return 0;
}
