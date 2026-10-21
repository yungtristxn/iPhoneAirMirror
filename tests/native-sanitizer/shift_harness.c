/* Silent, narrow shift-UB regression. Not whole-program UB coverage. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
void playfair_decrypt(const unsigned char*, const unsigned char*, unsigned char*);
void modified_md5(unsigned char*, unsigned char*, unsigned char*);

int main(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#endif
    if (argc == 2 && strcmp(argv[1], "--positive-control") == 0) {
        volatile unsigned char byte = 255;
        volatile int result = byte << 24; /* Must trap when shift UBSan is active. */
        (void)result;
        return 0;
    }
    if (argc != 2) return 1;
    FILE* file = fopen(argv[1], "rb");
    if (!file) return 2;
    unsigned char vector[252], actual[16];
    unsigned vectors = 0;
    size_t count;
    while ((count = fread(vector, 1, sizeof(vector), file)) != 0) {
        if (count != sizeof(vector)) { fclose(file); return 4; }
        playfair_decrypt(vector, vector + 164, actual);
        if (memcmp(actual, vector + 236, 16) != 0) { fclose(file); return 3; }
        ++vectors;
    }
    if (ferror(file) || vectors != 20) { fclose(file); return 4; }
    fclose(file);
    uint32_t block_words[16], key_words[4], out_words[4];
    for (unsigned i = 0; i < 16; ++i) block_words[i] = UINT32_C(0x80fffe81) + i;
    for (unsigned i = 0; i < 4; ++i) key_words[i] = UINT32_C(0xfedcba98) + i;
    modified_md5((unsigned char*)block_words, (unsigned char*)key_words, (unsigned char*)out_words);
    puts("PASS: shift-UB instrumented PlayFair, 20 public known answers and high-bit byte assembly");
    return 0;
}
