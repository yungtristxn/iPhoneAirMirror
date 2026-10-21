// Only called in the capability-free codec worker. Network framing lives in Rust.
#include <aacdecoder_lib.h>
#include <cstdint>
#include <cstring>
extern "C" {
void* am_aac_open(unsigned codec) {
    auto h = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!h) return nullptr;
    UCHAR eld[] = {0xf8, 0xe8, 0x50, 0x00};
    UCHAR lc[] = {0x12, 0x10};
    UCHAR* config = codec == 8 ? eld : lc;
    UINT len = codec == 8 ? sizeof(eld) : sizeof(lc);
    if ((codec != 8 && codec != 4) || aacDecoder_ConfigRaw(h, &config, &len) != AAC_DEC_OK) {
        aacDecoder_Close(h); return nullptr;
    }
    aacDecoder_SetParam(h, AAC_PCM_MIN_OUTPUT_CHANNELS, 2);
    aacDecoder_SetParam(h, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
    return h;
}
void am_aac_close(void* h) { if (h) aacDecoder_Close((HANDLE_AACDECODER)h); }
int am_aac_decode(void* h, const uint8_t* data, unsigned len, int16_t* out, unsigned cap) {
    if (!h || !data || !out || len == 0 || len > 8192 || cap < 4096) return -1;
    auto dec = (HANDLE_AACDECODER)h;
    UCHAR* p = const_cast<UCHAR*>(data);
    UINT size = len, valid = len;
    if (aacDecoder_Fill(dec, &p, &size, &valid) != AAC_DEC_OK || valid) return -2;
    auto err = aacDecoder_DecodeFrame(dec, out, (INT)cap, 0);
    if (err != AAC_DEC_OK) return -3;
    auto info = aacDecoder_GetStreamInfo(dec);
    if (!info || info->sampleRate != 44100 || info->numChannels != 2 ||
        info->frameSize < 1 || info->frameSize > 2048) return -4;
    return info->frameSize * 2;
}
}
