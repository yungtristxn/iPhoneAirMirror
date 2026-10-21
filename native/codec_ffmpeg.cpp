// Experimental replacement for codec.cpp, selected only by the FFmpeg build feature.
// No demuxer, parser, resampler, file or network API is used here. This runs in the
// existing capability-free, 256-MiB codec worker. Backend selection is build-time.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace {
constexpr unsigned max_packet = 8192;
constexpr unsigned max_output = 4096; // Interleaved int16 samples, including both channels.
thread_local char open_error[256] = "no FFmpeg decoder error";

void describe(char (&destination)[256], const char* stage, int code = 0) {
    char reason[AV_ERROR_MAX_STRING_SIZE]{};
    if (code < 0) av_strerror(code, reason, sizeof(reason));
    std::snprintf(destination, sizeof(destination), "%s%s%s%s", stage,
                  code < 0 ? ": " : "", reason, code < 0 ? " (FFmpeg)" : "");
    for (char* p = destination; *p; ++p)
        if (static_cast<unsigned char>(*p) < 32 || static_cast<unsigned char>(*p) > 126) *p = ' ';
}

bool stereo(const AVChannelLayout& layout) {
    return layout.nb_channels == 2 && av_channel_layout_check(&layout) == 1
        && av_channel_layout_channel_from_index(&layout, 0) == AV_CHAN_FRONT_LEFT
        && av_channel_layout_channel_from_index(&layout, 1) == AV_CHAN_FRONT_RIGHT;
}

bool float_to_i16(float value, int16_t& output) {
    if (!std::isfinite(value)) return false;
    // Saturate before integer conversion, including finite values outside [-1, 1].
    // This is instantaneous PCM saturation, not FDK's lookahead peak limiter.
    const double scaled = std::clamp(static_cast<double>(value), -1.0, 1.0) * 32768.0;
    output = static_cast<int16_t>(std::clamp(std::lround(scaled), -32768L, 32767L));
    return true;
}

struct Decoder {
    AVCodecContext* context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    unsigned mode = 0;
    int expected_frames = 0;
    int64_t next_pts = 0;
    bool contract_failed = false;
    bool allocation_rejected = false;
    char error[256] = "no FFmpeg decoder error";
    std::array<int16_t, max_output> converted{};

    explicit Decoder(unsigned codec) : mode(codec), expected_frames(codec == 8 ? 480 : 1024) {}
    ~Decoder() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
    }

    static int bounded_buffer(AVCodecContext* context, AVFrame* frame, int flags) {
        // FFmpeg's AAC decoder initially allocates 2048 samples per channel even
        // for a 480-sample ELD frame. Bound that allocation before it happens.
        auto decoder = static_cast<Decoder*>(context->opaque);
        if (context->codec_type != AVMEDIA_TYPE_AUDIO || frame->sample_rate != 44100
            || !stereo(frame->ch_layout) || frame->nb_samples < 1 || frame->nb_samples > 2048
            || frame->format != AV_SAMPLE_FMT_FLTP) {
            decoder->allocation_rejected = true;
            return AVERROR_INVALIDDATA;
        }
        return avcodec_default_get_buffer2(context, frame, flags);
    }

    bool initialize_context() {
        av_frame_unref(frame);
        avcodec_free_context(&context);
        const AVCodec* codec = avcodec_find_decoder_by_name("aac");
        if (!codec || codec->id != AV_CODEC_ID_AAC || codec->type != AVMEDIA_TYPE_AUDIO) {
            describe(error, "native float AAC decoder is unavailable");
            return false;
        }
        context = avcodec_alloc_context3(codec);
        if (!context) { describe(error, "AAC context allocation failed"); return false; }
        context->thread_count = 1;
        context->thread_type = 0; // No frame-thread delay.
        context->sample_rate = 44100;
        av_channel_layout_default(&context->ch_layout, 2);
        context->max_samples = max_output; // FFmpeg counts nb_samples * channels.
        context->get_buffer2 = bounded_buffer;
        context->opaque = this;
        context->pkt_timebase = AVRational{1, 44100};
        context->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        constexpr uint8_t eld[] = {0xf8, 0xe8, 0x50, 0x00};
        constexpr uint8_t lc[] = {0x12, 0x10};
        const uint8_t* config = mode == 8 ? eld : lc;
        context->extradata_size = static_cast<int>(mode == 8 ? sizeof(eld) : sizeof(lc));
        context->extradata = static_cast<uint8_t*>(av_mallocz(
            static_cast<size_t>(context->extradata_size) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!context->extradata) { describe(error, "AAC configuration allocation failed"); return false; }
        std::memcpy(context->extradata, config, static_cast<size_t>(context->extradata_size));
        const int result = avcodec_open2(context, codec, nullptr);
        if (result < 0) { describe(error, "open native AAC with negotiated ASC", result); return false; }
        next_pts = 0;
        allocation_rejected = false;
        return true;
    }

    int contract_error(const char* reason, int result, int api_error = 0) {
        describe(error, reason, api_error);
        contract_failed = true;
        // Do not feed another packet or manufacture/retimestamp PCM after a delay
        // or multi-output contract violation. Reconfiguration must close/reopen.
        return result;
    }

    int malformed(const char* stage, int code) {
        describe(error, stage, code);
        av_frame_unref(frame);
        // A hidden flush/reopen would restart priming without informing the
        // host's timestamp generation. Fail until explicit reconfiguration.
        contract_failed = true;
        return -3;
    }

    bool output_contract() const {
        if (frame->sample_rate != 44100 || !stereo(frame->ch_layout)
            || frame->format != AV_SAMPLE_FMT_FLTP || frame->nb_samples != expected_frames
            || frame->nb_samples * 2 > static_cast<int>(max_output)
            || !frame->extended_data || !frame->extended_data[0] || !frame->extended_data[1]
            || frame->linesize[0] < frame->nb_samples * static_cast<int>(sizeof(float))) return false;
        return true;
    }

    bool convert() {
        auto left = reinterpret_cast<const float*>(frame->extended_data[0]);
        auto right = reinterpret_cast<const float*>(frame->extended_data[1]);
        for (int i = 0; i < frame->nb_samples; ++i)
            if (!float_to_i16(left[i], converted[2 * i]) || !float_to_i16(right[i], converted[2 * i + 1])) return false;
        return true;
    }
};
}

extern "C" {
const char* am_aac_backend() { return "ffmpeg-native-aac-experimental"; }
const char* am_aac_last_error(void* handle) {
    return handle ? static_cast<Decoder*>(handle)->error : open_error;
}
int am_aac_delay(void* handle) {
    // Diagnostic only. FFmpeg defines this as decoder warm-up/preroll samples;
    // it is not automatically the same quantity as FDK's additional outputDelay.
    auto decoder = static_cast<Decoder*>(handle);
    return decoder && decoder->context ? decoder->context->delay : -1;
}

void* am_aac_open(unsigned codec) {
    if (codec != 8 && codec != 4) { describe(open_error, "unsupported AAC mode"); return nullptr; }
    // stderr shares the worker IPC pipe: decoder diagnostics must never enter it.
    av_log_set_level(AV_LOG_QUIET);
    av_max_alloc(32u * 1024 * 1024);
    auto decoder = std::unique_ptr<Decoder>(new (std::nothrow) Decoder(codec));
    if (!decoder) { describe(open_error, "AAC wrapper allocation failed"); return nullptr; }
    decoder->frame = av_frame_alloc();
    decoder->packet = av_packet_alloc();
    if (!decoder->frame || !decoder->packet || av_new_packet(decoder->packet, static_cast<int>(max_packet)) < 0) {
        describe(open_error, "AAC bounded packet/frame allocation failed"); return nullptr;
    }
    std::memset(decoder->packet->data, 0, max_packet + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!decoder->initialize_context()) { std::memcpy(open_error, decoder->error, sizeof(open_error)); return nullptr; }
    describe(open_error, "no FFmpeg decoder error");
    return decoder.release();
}

void am_aac_close(void* handle) { delete static_cast<Decoder*>(handle); }

int am_aac_decode_timed(void* handle, const uint8_t* data, unsigned length, int16_t* output,
                        unsigned capacity, uint32_t* additional_delay) {
    if (!handle) return -1;
    auto& decoder = *static_cast<Decoder*>(handle);
    if (!data || !output || !additional_delay || !length || length > max_packet || capacity < max_output) {
        describe(decoder.error, "invalid bounded AAC input/output buffer");return -1;
    }
    if (decoder.contract_failed) return -8;
    // Keep the reusable buffer's capacity fixed even if FFmpeg retained a reference.
    decoder.packet->size = static_cast<int>(max_packet);
    int result = av_packet_make_writable(decoder.packet);
    if (result < 0) return decoder.malformed("make bounded AAC packet writable", result);
    std::memcpy(decoder.packet->data, data, length);
    std::memset(decoder.packet->data + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    decoder.packet->size = static_cast<int>(length);
    if (decoder.next_pts > std::numeric_limits<int64_t>::max() - decoder.expected_frames)
        return decoder.contract_error("AAC packet timestamp counter exhausted", -5);
    const int64_t packet_pts = decoder.next_pts;
    decoder.packet->pts = decoder.packet->dts = packet_pts;
    decoder.packet->duration = decoder.expected_frames;
    decoder.allocation_rejected = false;
    result = avcodec_send_packet(decoder.context, decoder.packet);
    if (result == AVERROR(EAGAIN)) return decoder.contract_error("AAC send requires unread output: timing contract unsupported", -5, result);
    if (result == AVERROR_EOF) return decoder.contract_error("AAC decoder unexpectedly entered drain state", -7, result);
    if (decoder.allocation_rejected) return decoder.contract_error("AAC output allocation exceeded negotiated format/budget", -4);
    if (result < 0) return decoder.malformed("AAC packet decode rejected", result);
    decoder.next_pts += decoder.expected_frames;
    result = avcodec_receive_frame(decoder.context, decoder.frame);
    if (result == AVERROR(EAGAIN)) return decoder.contract_error("AAC packet produced no frame: delayed-output contract unsupported", -5, result);
    if (result == AVERROR_EOF) return decoder.contract_error("AAC receive unexpectedly reached drain EOF", -7, result);
    if (decoder.allocation_rejected) return decoder.contract_error("AAC output allocation exceeded negotiated format/budget", -4);
    if (result < 0) return decoder.malformed("AAC frame decode rejected", result);
    if (decoder.frame->decode_error_flags || (decoder.frame->flags & (AV_FRAME_FLAG_CORRUPT | AV_FRAME_FLAG_DISCARD)))
        return decoder.malformed("AAC output marked corrupt/discarded", AVERROR_INVALIDDATA);
    if (decoder.frame->pts != packet_pts) return decoder.contract_error("AAC output belongs to a different packet timestamp", -5);
    if (!decoder.output_contract()) return decoder.contract_error("AAC output is not stereo FLTP 44100 Hz with negotiated frame size", -4);
    if (!decoder.convert()) return decoder.malformed("AAC output contains a non-finite sample", AVERROR_INVALIDDATA);
    const int samples = decoder.frame->nb_samples * 2;
    av_frame_unref(decoder.frame);
    // Exhaust this packet before accepting the next. More than one output frame
    // cannot fit the existing timestamped one-packet/one-PCM-block worker contract.
    result = avcodec_receive_frame(decoder.context, decoder.frame);
    if (result == 0) return decoder.contract_error("AAC packet produced multiple frames: timing contract unsupported", -6);
    if (result != AVERROR(EAGAIN)) {
        if (result == AVERROR_EOF) return decoder.contract_error("AAC decoder unexpectedly drained after a frame", -7, result);
        return decoder.malformed("AAC trailing packet data rejected", result);
    }
    std::memcpy(output, decoder.converted.data(), static_cast<size_t>(samples) * sizeof(int16_t));
    // This wrapper has no additional lookahead/concealment/limiter DSP stages.
    // AVCodecContext::delay is a separate warm-up diagnostic, not this value.
    *additional_delay = 0;
    describe(decoder.error, "no FFmpeg decoder error");
    return samples;
}
int am_aac_decode(void* handle, const uint8_t* data, unsigned length, int16_t* output, unsigned capacity) {
    uint32_t ignored = 0;
    return am_aac_decode_timed(handle, data, length, output, capacity, &ignored);
}
}
