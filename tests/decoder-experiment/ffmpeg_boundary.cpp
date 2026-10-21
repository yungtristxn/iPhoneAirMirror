// Standalone, silent C-ABI regression. The standard script links the experimental
// wrapper and pinned avcodec/avutil; the same checks also support the FDK wrapper.
// It creates no audio endpoint, window or socket.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
void* am_aac_open(unsigned);
void am_aac_close(void*);
int am_aac_decode(void*, const uint8_t*, unsigned, int16_t*, unsigned);
int am_aac_decode_timed(void*, const uint8_t*, unsigned, int16_t*, unsigned, uint32_t*);
const char* am_aac_backend();
const char* am_aac_last_error(void*);
int am_aac_delay(void*);
}

namespace {
constexpr uint32_t canary = 0xa17ba17bu;
constexpr int16_t untouched = 0x5a5a;
constexpr uint32_t untouched_delay = 0xdeadbeefu;
struct Delay {
    uint32_t before = canary, value = untouched_delay, after = canary;
    bool intact() const { return before == canary && after == canary; }
    bool unchanged() const { return intact() && value == untouched_delay; }
};
bool fdk_backend() { return std::strncmp(am_aac_backend(), "fdk-aac-", 8) == 0; }
struct Output {
    std::array<uint32_t, 8> before;
    std::array<int16_t, 4096> pcm;
    std::array<uint32_t, 8> after;
    Output() { before.fill(canary); pcm.fill(untouched); after.fill(canary); }
    bool intact() const {
        return std::all_of(before.begin(), before.end(), [](uint32_t v) { return v == canary; })
            && std::all_of(after.begin(), after.end(), [](uint32_t v) { return v == canary; });
    }
    bool unchanged() const {
        return intact() && std::all_of(pcm.begin(), pcm.end(), [](int16_t v) { return v == untouched; });
    }
};
struct Decoder {
    void* handle;
    unsigned mode;
    explicit Decoder(unsigned requested_mode) : handle(am_aac_open(requested_mode)), mode(requested_mode) {
        if (!handle) throw std::runtime_error(am_aac_last_error(nullptr));
        if (fdk_backend() && am_aac_delay(handle) != -1) {
            am_aac_close(handle);handle = nullptr;
            throw std::runtime_error("FDK delay valid before successful decode");
        }
    }
    ~Decoder() { am_aac_close(handle); }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
};
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
std::vector<uint8_t> first_packet(const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, 4> header{};
    require(bool(file.read(reinterpret_cast<char*>(header.data()), header.size())), "fixture header");
    uint32_t length = uint32_t(header[0]) | uint32_t(header[1]) << 8 | uint32_t(header[2]) << 16 | uint32_t(header[3]) << 24;
    require(length > 0 && length <= 8192, "bounded fixture packet");
    std::vector<uint8_t> packet(length);
    require(bool(file.read(reinterpret_cast<char*>(packet.data()), length)), "fixture packet");
    return packet;
}
void valid_packet(Decoder& decoder, const std::vector<uint8_t>& packet, int expected) {
    Output output;
    Delay delay;
    const int samples = am_aac_decode_timed(decoder.handle, packet.data(), static_cast<unsigned>(packet.size()), output.pcm.data(), 4096, &delay.value);
    if (samples != expected) throw std::runtime_error(am_aac_last_error(decoder.handle));
    require(output.intact(), "valid decode damaged guard");
    const uint32_t expected_delay = fdk_backend() && decoder.mode == 4 ? 1024u + (15u * 44100u) / 1000u : 0u;
    require(delay.intact() && delay.value == expected_delay && delay.value <= 4096, "PCM block additional-delay metadata mismatch");
    require(std::all_of(output.pcm.begin() + samples, output.pcm.end(), [](int16_t v) { return v == untouched; }), "write beyond returned PCM");
}
void run(unsigned mode, const char* path) {
    auto packet = first_packet(path);
    const int expected = mode == 8 ? 960 : 2048;
    Decoder decoder(mode);
    Output output;
    Delay delay;
    const auto length = static_cast<unsigned>(packet.size());
    require(am_aac_decode_timed(nullptr, packet.data(), length, output.pcm.data(), 4096, &delay.value) == -1, "null handle");
    require(am_aac_decode_timed(decoder.handle, nullptr, length, output.pcm.data(), 4096, &delay.value) == -1, "null input");
    require(am_aac_decode_timed(decoder.handle, packet.data(), length, nullptr, 4096, &delay.value) == -1, "null output");
    require(am_aac_decode_timed(decoder.handle, packet.data(), length, output.pcm.data(), 4096, nullptr) == -1, "null delay output");
    require(am_aac_decode_timed(decoder.handle, packet.data(), 0, output.pcm.data(), 4096, &delay.value) == -1, "zero input");
    require(am_aac_decode_timed(decoder.handle, packet.data(), 8193, output.pcm.data(), 4096, &delay.value) == -1, "oversize input");
    require(am_aac_decode_timed(decoder.handle, packet.data(), length, output.pcm.data(), 4095, &delay.value) == -1, "undersize capacity");
    require(am_aac_decode_timed(decoder.handle, packet.data(), length, output.pcm.data(), 0, &delay.value) == -1, "zero capacity");
    require(output.unchanged() && delay.unchanged(), "invalid request changed PCM or metadata");
    for (int i = 0; i < 3; ++i) valid_packet(decoder, packet, expected); // Delay remains stable; pre-call rejects do not poison state.
    require(am_aac_delay(decoder.handle) >= 0, "missing decoder delay diagnostic");
    Output legacy;
    require(am_aac_decode(decoder.handle, packet.data(), length, legacy.pcm.data(), 4096) == expected && legacy.intact(), "compatibility decode wrapper");

    unsigned latched = 0;
    for (unsigned size : {1u, 4u, 8192u}) {
        Decoder malformed(mode);
        std::vector<uint8_t> bytes(size, 0xff);
        Output failed;
        Delay failed_delay;
        int result = am_aac_decode_timed(malformed.handle, bytes.data(), size, failed.pcm.data(), 4096, &failed_delay.value);
        require(result < 0, "malformed fixture unexpectedly accepted");
        require(failed.unchanged() && failed_delay.unchanged(), "failed packet changed PCM, metadata or guard");
        require(result <= -2 && result >= -7, "unexpected malformed error category");
        require(am_aac_decode_timed(malformed.handle, packet.data(), length, failed.pcm.data(), 4096, &failed_delay.value) == -8, "decoder failure did not require configure");
        require(failed.unchanged() && failed_delay.unchanged(), "latched failure changed PCM or metadata");
        if (fdk_backend()) require(am_aac_delay(malformed.handle) == -1, "FDK delay remained valid after decoder failure");
        ++latched;
        Decoder reset(mode);
        valid_packet(reset, packet, expected);
    }
    std::printf("PASS mode=%u boundary_rejects=8 malformed_latched=%u explicit_reconfigures=%u delay_diagnostic=%d\n",
                mode, latched, latched, am_aac_delay(decoder.handle));
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: ffmpeg_boundary.exe ELD.frames LC.frames");
        require(am_aac_open(0) == nullptr && am_aac_open(7) == nullptr
            && am_aac_open(std::numeric_limits<unsigned>::max()) == nullptr, "invalid codec accepted");
        am_aac_close(nullptr);
        require(am_aac_delay(nullptr) == -1, "null delay diagnostic");
        run(8, argv[1]);
        run(4, argv[2]);
        std::printf("PASS backend=%s; no audio output, UI or network. Canary checks are not whole-library sanitizer coverage.\n", am_aac_backend());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
