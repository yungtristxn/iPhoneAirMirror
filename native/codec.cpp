// Only called in the capability-free codec worker. Network framing lives in Rust.
#include <aacdecoder_lib.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

namespace {
constexpr unsigned max_output = 4096;
constexpr uint32_t max_delay = 4096;
thread_local const char* open_error = "no FDK decoder error";
struct Decoder {
    HANDLE_AACDECODER handle = nullptr;
    unsigned mode = 0;
    int observed_delay = -1;
    bool failed = false;
    const char* error = "no FDK decoder error";
    std::array<int16_t, max_output> pcm{};
    ~Decoder() { if (handle) aacDecoder_Close(handle); }
    int reject(const char* reason, int code) { error = reason; failed = true; return code; }
};
}
extern "C" {
const char* am_aac_backend() { return "fdk-aac-c41c3c7516ea8d0d78a53b96b30bed3a29745cc6"; }
const char* am_aac_last_error(void* h) { return h ? static_cast<Decoder*>(h)->error : open_error; }
int am_aac_delay(void* h) {
    auto decoder = static_cast<Decoder*>(h);
    // New configurations and failed decodes have no validated PCM delay yet.
    return decoder && !decoder->failed ? decoder->observed_delay : -1;
}
void* am_aac_open(unsigned codec) {
    if (codec != 8 && codec != 4) { open_error = "unsupported FDK AAC mode"; return nullptr; }
    auto decoder = std::unique_ptr<Decoder>(new (std::nothrow) Decoder);
    if (!decoder) { open_error = "FDK wrapper allocation failed"; return nullptr; }
    decoder->handle = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!decoder->handle) { open_error = "FDK decoder allocation failed"; return nullptr; }
    decoder->mode = codec;
    UCHAR eld[] = {0xf8, 0xe8, 0x50, 0x00};
    UCHAR lc[] = {0x12, 0x10};
    UCHAR* config = codec == 8 ? eld : lc;
    UINT length = static_cast<UINT>(codec == 8 ? sizeof(eld) : sizeof(lc));
    if (aacDecoder_ConfigRaw(decoder->handle, &config, &length) != AAC_DEC_OK
        || aacDecoder_SetParam(decoder->handle, AAC_PCM_MIN_OUTPUT_CHANNELS, 2) != AAC_DEC_OK
        || aacDecoder_SetParam(decoder->handle, AAC_PCM_MAX_OUTPUT_CHANNELS, 2) != AAC_DEC_OK) {
        open_error = "FDK fixed ASC/stereo configuration failed";
        return nullptr;
    }
    // Preserve the pinned FDK concealment and limiter policy. No padding or
    // limiter switch is used to make this backend resemble another decoder.
    open_error = "no FDK decoder error";
    return decoder.release();
}
void am_aac_close(void* h) { delete static_cast<Decoder*>(h); }
int am_aac_decode_timed(void* h, const uint8_t* data, unsigned len, int16_t* out,
                        unsigned cap, uint32_t* additional_delay) {
    if (!h) return -1;
    auto& decoder = *static_cast<Decoder*>(h);
    if (!data || !out || !additional_delay || !len || len > 8192 || cap < max_output) {
        decoder.error = "invalid bounded FDK input/output buffer";
        return -1;
    }
    if (decoder.failed) return -8;
    UCHAR* cursor = const_cast<UCHAR*>(data);
    UINT size = len, valid = len;
    if (aacDecoder_Fill(decoder.handle, &cursor, &size, &valid) != AAC_DEC_OK || valid)
        return decoder.reject("FDK packet fill failed; explicit configure required", -2);
    if (aacDecoder_DecodeFrame(decoder.handle, decoder.pcm.data(), static_cast<INT>(max_output), 0) != AAC_DEC_OK)
        return decoder.reject("FDK decode failed; explicit configure required", -3);
    const auto info = aacDecoder_GetStreamInfo(decoder.handle);
    const int expected_frames = decoder.mode == 8 ? 480 : 1024;
    if (!info || info->sampleRate != 44100 || info->numChannels != 2 ||
        info->frameSize != expected_frames)
        return decoder.reject("FDK output does not match fixed 44100 Hz stereo frame contract", -4);
    // Pinned LC policy: one concealment frame plus default 15-ms limiter
    // lookahead. ELD disables both. Validate actual metadata, never infer it.
    constexpr uint32_t lc_delay = 1024u + (15u * 44100u) / 1000u;
    const uint32_t expected_delay = decoder.mode == 8 ? 0u : lc_delay;
    const uint32_t actual_delay = info->outputDelay;
    if (actual_delay > max_delay || actual_delay != expected_delay
        || (decoder.observed_delay >= 0 && actual_delay != static_cast<uint32_t>(decoder.observed_delay)))
        return decoder.reject("FDK additional PCM delay changed or violates the pinned configuration", -4);
    const int samples = info->frameSize * 2;
    // Commit a complete block and its delay together; failures touch neither.
    std::memcpy(out, decoder.pcm.data(), static_cast<size_t>(samples) * sizeof(int16_t));
    *additional_delay = actual_delay; // Per-channel samples at 44100 Hz.
    decoder.observed_delay = static_cast<int>(actual_delay);
    decoder.error = "no FDK decoder error";
    return samples;
}
int am_aac_decode(void* h, const uint8_t* data, unsigned len, int16_t* out, unsigned cap) {
    uint32_t ignored = 0;
    return am_aac_decode_timed(h, data, len, out, cap, &ignored);
}
}
