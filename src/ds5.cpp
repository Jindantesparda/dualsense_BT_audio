#include "ds5.h"

#include <cstring>

namespace ds5 {
namespace {

constexpr uint8_t kHidpHeader = 0xA2; // part of the CRC seed on Bluetooth

uint32_t g_table[256];
bool     g_table_ready = false;

void MakeTable() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        g_table[i] = c;
    }
    g_table_ready = true;
}

void Finish(uint8_t* report, size_t size) {
    uint32_t crc = Crc32(0, &kHidpHeader, 1);
    crc          = Crc32(crc, report, size - 4);
    report[size - 4] = static_cast<uint8_t>(crc);
    report[size - 3] = static_cast<uint8_t>(crc >> 8);
    report[size - 2] = static_cast<uint8_t>(crc >> 16);
    report[size - 1] = static_cast<uint8_t>(crc >> 24);
}

} // namespace

uint32_t Crc32(uint32_t crc, const void* data, size_t len) {
    if (!g_table_ready) {
        MakeTable();
    }
    const auto* p = static_cast<const uint8_t*>(data);
    crc           = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = g_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

void BuildAudioControl(uint8_t out[kControlReportSize], uint8_t volume) {
    std::memset(out, 0, kControlReportSize);
    out[0] = 0x31; // Bluetooth effects report
    out[1] = 0x00; // tag/sequence
    out[2] = 0x10; // magic
    uint8_t* e = out + 3;
    e[0]  = 0x80 | 0x20; // audio control valid + speaker volume valid
    e[1]  = 0x80;        // audio control 2 valid
    e[5]  = volume;      // speaker volume
    e[7]  = 0x30;        // output path: speaker, headphones muted
    e[37] = 0x02;        // speaker preamp +6 dB
    Finish(out, kControlReportSize);
}

void BuildAudioReport(uint8_t out[kAudioReportSize], uint8_t sequence, uint8_t counter,
                      const int8_t haptics[kHapticFrames * 2],
                      const uint8_t opus[2 * kOpusBytes], bool include_speaker) {
    std::memset(out, 0, kAudioReportSize);
    out[0] = 0x39;
    out[1] = static_cast<uint8_t>(sequence << 4);
    out[2] = 0x91;
    out[3] = 6;
    out[4] = 0x7E;
    out[5] = 48;
    out[6] = 48;
    out[7] = 48;
    out[8] = 48;
    out[9] = counter;

    // Haptic sub-packet: 128 signed 8-bit samples.
    out[10] = 0xD2;
    out[11] = 64;
    std::memcpy(out + 12, haptics, kHapticFrames * 2);

    // Speaker sub-packet: two Opus frames.
    if (include_speaker) {
        out[140] = 0xD3;
        out[141] = kOpusBytes;
        std::memcpy(out + 142, opus, 2 * kOpusBytes);
    }

    Finish(out, kAudioReportSize);
}

} // namespace ds5
