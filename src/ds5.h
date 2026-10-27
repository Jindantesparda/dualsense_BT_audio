// DualSense Bluetooth audio protocol.
//
// Report 0x39 (547 bytes) carries, in one packet:
//   - 2 x 32 frames of stereo 8-bit actuator ("haptic audio") samples at 3 kHz
//   - 2 x 200 byte Opus frames (480 samples @ 48 kHz each) for the speaker
// so the controller consumes 960 Opus samples per 64/3000 s = 21.333 ms, i.e.
// the speaker stream really runs at 45 kHz. Audio is therefore resampled to
// 45 kHz before being encoded and labelled 48 kHz.
//
// Report 0x31 (78 bytes) sets the output path, speaker volume and preamp.
//
// Layout follows KytyPS5 (src/libs/dualSenseBluetooth.cpp), which in turn
// documents report 0x39 from awalol/DS5Dongle (MIT). Both reports are sent
// with a CRC-32 over a synthetic 0xA2 HIDP header, as Windows/Linux expect
// for Bluetooth output reports.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ds5 {

constexpr size_t kAudioReportSize   = 547; // report 0x39 with report id
constexpr size_t kControlReportSize = 78;  // report 0x31 with report id

constexpr int kHapticRate    = 3000;
constexpr int kSpeakerRate   = 45000; // playback rate of the Opus stream
constexpr int kOpusRate      = 48000; // rate the Opus stream is labelled with
constexpr int kHapticFrames  = 64;    // per report (2 blocks of 32)
constexpr int kSpeakerFrames = 960;   // per report (2 frames of 480)
constexpr int kOpusBytes     = 200;   // per 480-sample frame, CBR 160 kbit/s
constexpr int kOpusFrame     = 480;

// Standard CRC-32 (zlib/SDL flavour), usable incrementally.
uint32_t Crc32(uint32_t crc, const void* data, size_t len);

// 0x31: route audio to the built-in speaker, headphones muted, +6 dB preamp.
// volume is 0..100.
void BuildAudioControl(uint8_t out[kControlReportSize], uint8_t volume);

// 0x39: haptics[128] are interleaved signed 8-bit actuator samples
// (2 channels x 32 frames x 2 blocks), opus holds two encoded frames.
void BuildAudioReport(uint8_t out[kAudioReportSize], uint8_t sequence, uint8_t counter,
                      const int8_t haptics[kHapticFrames * 2],
                      const uint8_t opus[2 * kOpusBytes], bool include_speaker);

} // namespace ds5
