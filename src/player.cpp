#include "player.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#include <opus.h>

#include "ds5.h"
#include "dsp.h"

namespace ds {
namespace {

constexpr double  kPi           = 3.14159265358979323846;
constexpr int64_t kPeriodNs     = 64000000000LL / ds5::kHapticRate; // 21.333 ms
constexpr uint16_t kSonyVendor  = 0x054C;
constexpr uint16_t kDaulSense   = 0x0CE6;
constexpr uint16_t kDaulSenseEdge = 0x0DF2;

using Clock = std::chrono::steady_clock;

// std::this_thread::sleep_until() on Windows is quantised to the system tick
// (15.6 ms), which made every other report late and caused audible stuttering.
// A high-resolution waitable timer is accurate to well under a millisecond, and
// the last ~1 ms is spun so reports leave on the exact 21.333 ms boundary.
class PreciseWaiter {
public:
    PreciseWaiter() {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (timer_ == nullptr) {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
    }
    ~PreciseWaiter() {
        if (timer_ != nullptr) {
            CloseHandle(timer_);
        }
    }
    PreciseWaiter(const PreciseWaiter&)            = delete;
    PreciseWaiter& operator=(const PreciseWaiter&) = delete;

    void WaitUntil(Clock::time_point target) {
        constexpr int64_t kSpinNs = 1200000; // 1.2 ms
        auto              now     = Clock::now();
        if (now >= target) {
            return;
        }
        if (timer_ != nullptr) {
            const auto remain = std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
            if (remain > kSpinNs) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((remain - kSpinNs) / 100);
                if (due.QuadPart == 0) {
                    due.QuadPart = -1;
                }
                if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer_, INFINITE);
                }
            }
        } else {
            const auto remain = std::chrono::duration_cast<std::chrono::nanoseconds>(target - now).count();
            if (remain > 2000000) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(remain - 1500000));
            }
        }
        while (Clock::now() < target) {
            YieldProcessor();
        }
    }

private:
    HANDLE timer_ = nullptr;
};

class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual size_t Read(float* stereo, size_t frames) = 0;
    virtual bool   Rewind()                          = 0;
    virtual double Duration() const                  = 0; // 0 when unknown
};

class FileSource final : public AudioSource {
public:
    ~FileSource() override {
        if (open_) {
            ma_decoder_uninit(&decoder_);
        }
    }

    bool Open(const std::wstring& path, double start_seconds) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, ds5::kSpeakerRate);
        config.resampling.linear.lpfOrder = 6;
        if (ma_decoder_init_file_w(path.c_str(), &config, &decoder_) != MA_SUCCESS) {
            return false;
        }
        open_ = true;
        if (start_seconds > 0.0) {
            ma_decoder_seek_to_pcm_frame(
                &decoder_, static_cast<ma_uint64>(start_seconds * ds5::kSpeakerRate));
        }
        return true;
    }

    size_t Read(float* stereo, size_t frames) override {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&decoder_, stereo, frames, &got) != MA_SUCCESS) {
            return 0;
        }
        return static_cast<size_t>(got);
    }

    bool Rewind() override { return ma_decoder_seek_to_pcm_frame(&decoder_, 0) == MA_SUCCESS; }

    double Duration() const override {
        ma_uint64 length = 0;
        if (!open_ || ma_decoder_get_length_in_pcm_frames(&decoder_, &length) != MA_SUCCESS) {
            return 0.0;
        }
        return static_cast<double>(length) / ds5::kSpeakerRate;
    }

private:
    mutable ma_decoder decoder_ {};
    bool               open_ = false;
};

// Test loop: a kick you feel and a melody you hear. The melody now also shows
// up in the actuators because the haptics are full-band by default.
class ToneSource final : public AudioSource {
public:
    size_t Read(float* stereo, size_t frames) override {
        for (size_t i = 0; i < frames; i++) {
            const double t    = static_cast<double>(position_) / ds5::kSpeakerRate;
            const double beat = std::fmod(t, 0.5);
            double       kick = 0.0;
            if (beat < 0.42) {
                const double freq = 45.0 + 95.0 * std::exp(-beat * 20.0);
                kick_phase_ += 2.0 * kPi * freq / ds5::kSpeakerRate;
                kick = std::sin(kick_phase_) * std::exp(-beat * 8.0);
            } else {
                kick_phase_ = 0.0;
            }

            const int    note = static_cast<int>(t / 0.5) % 4;
            const double nt   = std::fmod(t, 0.5);
            const double env  = std::min(1.0, nt / 0.012) * std::exp(-nt * 3.5);
            const double mel  = 0.45 * env * std::sin(2.0 * kPi * kNotes[note] * nt);
            // A second voice an octave down, so there is pitch movement to feel.
            const double low  = 0.30 * env * std::sin(2.0 * kPi * kNotes[note] * 0.5 * nt);

            stereo[i * 2 + 0] = static_cast<float>(0.60 * kick + 0.90 * mel + 0.70 * low);
            stereo[i * 2 + 1] = static_cast<float>(0.60 * kick + 0.60 * mel + 0.70 * low);
            position_++;
        }
        return frames;
    }

    bool   Rewind() override { position_ = 0; kick_phase_ = 0.0; return true; }
    double Duration() const override { return 0.0; }

private:
    static constexpr double kNotes[4] = {523.25, 659.25, 783.99, 1046.50};
    uint64_t position_   = 0;
    double   kick_phase_ = 0.0;
};

// Diagnostic: a slow 40 Hz -> 1 kHz glide. If the grips track the pitch, the
// audio-haptics path is working and only the tone shaping needs tuning.
class SweepSource final : public AudioSource {
public:
    size_t Read(float* stereo, size_t frames) override {
        constexpr double kDuration = 12.0;
        for (size_t i = 0; i < frames; i++) {
            const double t = std::fmod(position_ / ds5::kSpeakerRate, kDuration);
            const double f = 40.0 * std::pow(1000.0 / 40.0, t / kDuration);
            phase_ += 2.0 * kPi * f / ds5::kSpeakerRate;
            const auto value = static_cast<float>(0.7 * std::sin(phase_));
            stereo[i * 2 + 0] = value;
            stereo[i * 2 + 1] = value;
            position_++;
        }
        return frames;
    }
    bool   Rewind() override { position_ = 0; phase_ = 0.0; return true; }
    double Duration() const override { return 0.0; }

private:
    uint64_t position_ = 0;
    double   phase_    = 0.0;
};


// Single-producer/single-consumer ring of interleaved stereo floats.
class SampleRing {
public:
    explicit SampleRing(size_t frames) {
        size_t n = 1;
        while (n < frames) {
            n <<= 1;
        }
        mask_ = (n * 2) - 1;
        data_.assign(n * 2, 0.0f);
    }

    size_t Write(const float* src, size_t samples) {
        const size_t w        = write_.load(std::memory_order_relaxed);
        const size_t r        = read_.load(std::memory_order_acquire);
        const size_t capacity = mask_ + 1;
        size_t       free     = capacity - (w - r);
        if (samples > free) {
            samples = free;
        }
        for (size_t i = 0; i < samples; i++) {
            data_[(w + i) & mask_] = src[i];
        }
        write_.store(w + samples, std::memory_order_release);
        return samples;
    }

    size_t Read(float* dst, size_t samples) {
        const size_t r    = read_.load(std::memory_order_relaxed);
        const size_t w    = write_.load(std::memory_order_acquire);
        size_t       have = w - r;
        if (samples > have) {
            samples = have;
        }
        for (size_t i = 0; i < samples; i++) {
            dst[i] = data_[(r + i) & mask_];
        }
        read_.store(r + samples, std::memory_order_release);
        return samples;
    }

    size_t Available() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

    void Discard(size_t samples) {
        const size_t r    = read_.load(std::memory_order_relaxed);
        const size_t w    = write_.load(std::memory_order_acquire);
        size_t       have = w - r;
        if (samples > have) {
            samples = have;
        }
        read_.store(r + samples, std::memory_order_release);
    }

private:
    std::vector<float>  data_;
    size_t              mask_ = 0;
    std::atomic<size_t> write_ {0};
    std::atomic<size_t> read_ {0};
};

// Everything the PC is playing, captured from the default output device with
// WASAPI loopback. Windows exposes no audio endpoint for the DualSense over
// Bluetooth, so system audio has to be captured and forwarded over HID.
class SystemAudioSource final : public AudioSource {
public:
    ~SystemAudioSource() override {
        if (started_) {
            ma_device_uninit(&device_);
        }
    }

    bool Open(const std::string& device_name, std::string* error) {
        ma_device_id chosen {};
        bool         have_chosen = false;
        if (!device_name.empty()) {
            ma_context context;
            if (ma_context_init(nullptr, 0, nullptr, &context) == MA_SUCCESS) {
                ma_device_info* playback = nullptr;
                ma_uint32       count    = 0;
                if (ma_context_get_devices(&context, &playback, &count, nullptr,
                                           nullptr) == MA_SUCCESS) {
                    for (ma_uint32 i = 0; i < count; i++) {
                        ma_device_info info {};
                        if (ma_context_get_device_info(&context, ma_device_type_playback,
                                                       &playback[i].id, &info) ==
                                MA_SUCCESS &&
                            device_name == info.name) {
                            chosen      = info.id;
                            have_chosen = true;
                            break;
                        }
                    }
                }
                ma_context_uninit(&context);
            }
        }

        ma_device_config config         = ma_device_config_init(ma_device_type_loopback);
        config.capture.format           = ma_format_f32;
        config.capture.channels         = 2;
        config.sampleRate               = ds5::kSpeakerRate;
        config.capture.pDeviceID        = have_chosen ? &chosen : nullptr;
        config.periodSizeInMilliseconds = 10;
        config.dataCallback             = &SystemAudioSource::OnData;
        config.pUserData                = this;

        ma_result result = ma_device_init(nullptr, &config, &device_);
        if (result != MA_SUCCESS) {
            if (error != nullptr) {
                *error = ma_result_description(result);
            }
            return false;
        }
        result = ma_device_start(&device_);
        if (result != MA_SUCCESS) {
            ma_device_uninit(&device_);
            if (error != nullptr) {
                *error = ma_result_description(result);
            }
            return false;
        }
        started_ = true;
        return true;
    }

    size_t Read(float* stereo, size_t frames) override {
        const size_t wanted = frames * 2;
        const size_t got    = ring_.Read(stereo, wanted);
        if (got < wanted) {
            std::fill(stereo + got, stereo + wanted, 0.0f);
            underruns_++;
        }
        // The output device clock and our 21.333 ms report cadence can never
        // match exactly. Trim slowly instead of letting a backlog build up.
        const size_t backlog = ring_.Available();
        if (backlog > 54000) { // more than ~600 ms
            ring_.Discard(backlog - 9000);
        }
        return frames;
    }

    bool   Rewind() override { return true; }
    double Duration() const override { return 0.0; }

private:
    static void OnData(ma_device* device, void*, const void* input, ma_uint32 frameCount) {
        auto* self = static_cast<SystemAudioSource*>(device->pUserData);
        if (self == nullptr || input == nullptr) {
            return;
        }
        self->ring_.Write(static_cast<const float*>(input), static_cast<size_t>(frameCount) * 2);
    }

    ma_device           device_ {};
    SampleRing          ring_ {262144};
    bool                started_ = false;
    std::atomic<size_t> underruns_ {0};
};

void EncodeSilence(OpusEncoder* encoder, uint8_t* opus) {
    float zeros[ds5::kOpusFrame * 2] = {};
    if (encoder == nullptr) {
        std::memset(opus, 0, 2 * ds5::kOpusBytes);
        return;
    }
    for (int packet = 0; packet < 2; packet++) {
        const int bytes = opus_encode_float(encoder, zeros, ds5::kOpusFrame,
                                            opus + packet * ds5::kOpusBytes, ds5::kOpusBytes);
        if (bytes != ds5::kOpusBytes) {
            std::memset(opus + packet * ds5::kOpusBytes, 0, ds5::kOpusBytes);
        }
    }
}

} // namespace

std::vector<HidDeviceInfo> FindDualSensePads() {
    std::vector<HidDeviceInfo> pads;
    for (auto& info: EnumerateHidDevices()) {
        if (info.vid == kSonyVendor && (info.pid == kDaulSense || info.pid == kDaulSenseEdge)) {
            pads.push_back(std::move(info));
        }
    }
    std::stable_sort(pads.begin(), pads.end(), [](const HidDeviceInfo& a, const HidDeviceInfo& b) {
        const auto score = [](const HidDeviceInfo& d) {
            return (d.bluetooth ? 2 : 0) + (d.output_report_bytes >= ds5::kAudioReportSize ? 1 : 0);
        };
        return score(a) > score(b);
    });
    return pads;
}

std::vector<AudioOutputInfo> ListAudioOutputs() {
    std::vector<AudioOutputInfo> result;
    ma_context                   context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        return result;
    }
    ma_device_info* playback = nullptr;
    ma_uint32       count    = 0;
    if (ma_context_get_devices(&context, &playback, &count, nullptr, nullptr) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; i++) {
            ma_device_info info {};
            if (ma_context_get_device_info(&context, ma_device_type_playback, &playback[i].id,
                                           &info) != MA_SUCCESS) {
                continue;
            }
            AudioOutputInfo out;
            out.name       = info.name;
            out.is_default = info.isDefault != 0;
            for (ma_uint32 j = 0; j < info.nativeDataFormatCount; j++) {
                out.channel_counts.push_back(
                    static_cast<int>(info.nativeDataFormats[j].channels));
            }
            result.push_back(std::move(out));
        }
    }
    ma_context_uninit(&context);
    return result;
}

Player::~Player() {
    Stop();
}

bool Player::Start(const std::wstring& file, const HidDeviceInfo& device, const Settings& settings,
                   std::string* error) {
    Stop();

    if (!device.bluetooth) {
        if (error != nullptr) {
            *error = "the controller is connected over USB; this program needs Bluetooth";
        }
        return false;
    }

    auto hid = std::make_unique<HidDevice>();
    if (!hid->Open(device.path)) {
        if (error != nullptr) {
            *error = "could not open the controller: " + hid->last_error();
        }
        return false;
    }

    if (settings.system_audio) {
        duration_ = 0.0;
    } else if (!settings.test) {
        FileSource probe;
        if (!probe.Open(file, 0.0)) {
            if (error != nullptr) {
                *error = "could not open or decode that audio file";
            }
            return false;
        }
        duration_ = probe.Duration();
    } else {
        duration_ = 0.0;
    }

    {
        std::lock_guard lock(mutex_);
        error_.clear();
        stats_.clear();
    }

    haptics_gain_.store(settings.haptics_gain);
    tone_.store(settings.tone);
    speaker_gain_.store(settings.speaker_gain);
    speaker_volume_.store(settings.speaker_volume);
    loop_.store(settings.loop);
    haptics_on_.store(settings.haptics);
    speaker_on_.store(settings.speaker);
    position_ = settings.start;
    elapsed_  = 0.0;
    stop_     = false;
    running_  = true;

    thread_ = std::thread(&Player::Run, this, file, std::move(hid), settings);
    return true;
}

void Player::Stop() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    stop_    = false;
    running_ = false;
}

std::string Player::Error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

std::string Player::Stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void Player::SetHapticsGain(float value) {
    haptics_gain_ = std::clamp(value, 0.0f, 4.0f);
}

void Player::SetTone(float value) {
    tone_ = std::clamp(value, 0.0f, 1.0f);
}

void Player::SetSpeakerGain(float value) {
    speaker_gain_ = std::clamp(value, 0.0f, 2.0f);
}

void Player::SetSpeakerVolume(int value) {
    speaker_volume_ = std::clamp(value, 0, 100);
}

void Player::SetLoop(bool value) {
    loop_ = value;
}

void Player::SetHapticsEnabled(bool value) {
    haptics_on_ = value;
}

void Player::SetSpeakerEnabled(bool value) {
    speaker_on_ = value;
}

namespace {

struct ReportPacket {
    uint8_t data[ds5::kAudioReportSize];
};

// Single-producer/single-consumer queue of finished 547-byte reports.
class ReportQueue {
public:
    static constexpr size_t kCapacity = 32;

    bool Push(const ReportPacket& packet) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= kCapacity) {
            return false;
        }
        packets_[head % kCapacity] = packet;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool Pop(ReportPacket* packet) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        *packet = packets_[tail % kCapacity];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t Size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    ReportPacket        packets_[kCapacity];
    std::atomic<size_t> head_ {0};
    std::atomic<size_t> tail_ {0};
};

} // namespace

void Player::Run(std::wstring file, std::unique_ptr<HidDevice> device, Settings settings) {
    // This thread only paces and writes reports. Decoding, haptics and Opus
    // encoding run on the producer thread below, so a CPU spike while a game is
    // running cannot delay a report.
    DWORD  mmcss_task = 0;
    HANDLE mmcss      = AvSetMmThreadCharacteristicsW(L"Audio", &mmcss_task);
    if (mmcss != nullptr) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    std::unique_ptr<AudioSource> source;
    if (settings.system_audio) {
        auto        live = std::make_unique<SystemAudioSource>();
        std::string live_error;
        if (!live->Open(settings.loopback_device, &live_error)) {
            {
                std::lock_guard lock(mutex_);
                error_ = "could not capture system audio: " + live_error;
            }
            running_ = false;
            if (mmcss != nullptr) {
                AvRevertMmThreadCharacteristics(mmcss);
            }
            return;
        }
        source = std::move(live);
    } else if (settings.sweep) {
        source = std::make_unique<SweepSource>();
    } else if (settings.test) {
        source = std::make_unique<ToneSource>();
    } else {
        auto decoded = std::make_unique<FileSource>();
        if (!decoded->Open(file, settings.start)) {
            {
                std::lock_guard lock(mutex_);
                error_ = "could not open or decode that audio file";
            }
            running_ = false;
            if (mmcss != nullptr) {
                AvRevertMmThreadCharacteristics(mmcss);
            }
            return;
        }
        source = std::move(decoded);
    }

    int          opus_error = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(ds5::kOpusRate, 2, OPUS_APPLICATION_AUDIO, &opus_error);
    if (encoder == nullptr || opus_error != OPUS_OK) {
        {
            std::lock_guard lock(mutex_);
            error_ = "could not create the Opus encoder";
        }
        running_ = false;
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(ds5::kOpusBytes * 8 * 100));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(0));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(0));

    uint8_t control[ds5::kControlReportSize] {};
    ds5::BuildAudioControl(control, static_cast<uint8_t>(speaker_volume_.load()));
    int last_volume = speaker_volume_.load();
    if (speaker_on_.load() && !device->Write(control, sizeof(control))) {
        {
            std::lock_guard lock(mutex_);
            error_ = "could not send the speaker routing report: " + device->last_error();
        }
        opus_encoder_destroy(encoder);
        running_ = false;
        if (mmcss != nullptr) {
            AvRevertMmThreadCharacteristics(mmcss);
        }
        return;
    }

    ReportQueue         queue;
    std::atomic<bool>   producer_done {false};
    std::atomic<bool>   producer_stop {false};
    std::atomic<bool>   encoding_failed {false};

    // Producer: decode -> haptics -> Opus -> report, kept ~200 ms ahead.
    std::thread producer([&]() {
        dsp::HapticConverter haptics(ds5::kSpeakerRate);
        std::vector<float>   pcm(static_cast<size_t>(ds5::kSpeakerFrames) * 2);
        std::vector<float>   haptic_float(static_cast<size_t>(ds5::kHapticFrames) * 2);
        int8_t               haptic_bytes[ds5::kHapticFrames * 2] {};
        uint8_t              opus[2 * ds5::kOpusBytes] {};
        uint8_t              sequence = 0;
        uint8_t              counter  = 0;
        int                  silent   = 0;

        while (!stop_.load() && !producer_stop.load()) {
            if (queue.Size() >= 10) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            size_t got = source->Read(pcm.data(), ds5::kSpeakerFrames);
            if (got == 0) {
                if ((loop_.load() || settings.test || settings.sweep || settings.system_audio) &&
                    source->Rewind()) {
                    got = source->Read(pcm.data(), ds5::kSpeakerFrames);
                }
                if (got == 0) {
                    if (++silent > 12) {
                        break;
                    }
                }
            } else {
                silent = 0;
            }
            if (got < ds5::kSpeakerFrames) {
                std::fill(pcm.begin() + static_cast<ptrdiff_t>(got) * 2, pcm.end(), 0.0f);
            }

            const bool  haptics_enabled = haptics_on_.load();
            const bool  speaker_enabled = speaker_on_.load();

            if (haptics_enabled) {
                haptics.Process(pcm.data(), ds5::kSpeakerFrames, haptic_float.data(),
                                tone_.load(), haptics_gain_.load());
                for (int i = 0; i < ds5::kHapticFrames * 2; i++) {
                    float value = haptic_float[static_cast<size_t>(i)];
                    if (!std::isfinite(value)) {
                        value = 0.0f;
                    }
                    haptic_bytes[i] =
                        static_cast<int8_t>(std::lround(std::clamp(value, -1.0f, 1.0f) * 127.0f));
                }
            } else {
                std::memset(haptic_bytes, 0, sizeof(haptic_bytes));
            }

            if (speaker_enabled) {
                const float speaker_gain = speaker_gain_.load();
                for (auto& sample: pcm) {
                    sample = std::isfinite(sample)
                                 ? std::clamp(sample * speaker_gain, -1.0f, 1.0f)
                                 : 0.0f;
                }
                for (int packet = 0; packet < 2; packet++) {
                    const int bytes = opus_encode_float(
                        encoder, pcm.data() + static_cast<size_t>(packet) * ds5::kOpusFrame * 2,
                        ds5::kOpusFrame, opus + packet * ds5::kOpusBytes, ds5::kOpusBytes);
                    if (bytes != ds5::kOpusBytes) {
                        encoding_failed = true;
                        producer_done   = true;
                        return;
                    }
                }
            }

            ReportPacket packet {};
            ds5::BuildAudioReport(packet.data, sequence, counter, haptic_bytes, opus,
                                  speaker_enabled);
            while (!queue.Push(packet)) {
                if (stop_.load() || producer_stop.load()) {
                    producer_done = true;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            sequence = static_cast<uint8_t>((sequence + 1) & 0x0F);
            counter  = static_cast<uint8_t>(counter + 2);
        }
        producer_done = true;
    });

    // Give the producer a moment to get ahead, then prime the controller with
    // the first few real packets so a single late write cannot drain it.
    for (int i = 0; i < 200 && queue.Size() < 3 && !producer_done.load(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    uint8_t  sequence       = 0;
    uint8_t  counter        = 0;
    uint64_t index          = 0;
    int      write_failures = 0;
    int      underruns      = 0;
    bool     failed         = false;
    int64_t  write_total_us = 0;
    int64_t  write_max_us   = 0;
    int64_t  gap_max_us     = 0;
    int      gaps_late      = 0;

    for (int i = 0; i < 3; i++) {
        ReportPacket packet;
        if (!queue.Pop(&packet)) {
            break;
        }
        device->Write(packet.data, sizeof(packet.data));
        sequence = static_cast<uint8_t>(packet.data[1] >> 4);
        counter  = packet.data[9];
    }

    PreciseWaiter waiter;
    auto          started       = Clock::now();
    auto          previous_send = Clock::time_point {};

    while (!stop_) {
        if (settings.duration > 0.0 &&
            static_cast<double>(index) * kPeriodNs / 1e9 >= settings.duration) {
            break;
        }
        waiter.WaitUntil(started + std::chrono::nanoseconds(static_cast<int64_t>(index) * kPeriodNs));

        const bool   speaker_enabled = speaker_on_.load();
        const int    volume          = speaker_volume_.load();
        if (volume != last_volume) {
            last_volume = volume;
            ds5::BuildAudioControl(control, static_cast<uint8_t>(volume));
            if (speaker_enabled) {
                device->Write(control, sizeof(control));
            }
        }

        ReportPacket packet;
        const bool   have = queue.Pop(&packet);
        if (!have) {
            if (producer_done.load()) {
                if (encoding_failed.load()) {
                    std::lock_guard lock(mutex_);
                    error_ = "Opus encoding failed";
                    failed = true;
                }
                break;
            }
            underruns++;
        } else {
            sequence = static_cast<uint8_t>(packet.data[1] >> 4);
            counter  = packet.data[9];

            const auto write_start = Clock::now();
            const bool wrote       = device->Write(packet.data, sizeof(packet.data));
            const auto write_us =
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - write_start)
                    .count();
            const auto send_time = Clock::now();

            write_total_us += write_us;
            write_max_us = std::max(write_max_us, write_us);
            if (previous_send.time_since_epoch().count() != 0) {
                const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(
                                     send_time - previous_send)
                                     .count();
                gap_max_us = std::max(gap_max_us, gap);
                if (gap > 26000) {
                    gaps_late++;
                }
            }
            previous_send = send_time;

            if (!wrote) {
                if (++write_failures >= 3) {
                    std::lock_guard lock(mutex_);
                    error_ = "writing to the controller failed: " + device->last_error();
                    failed = true;
                    break;
                }
            } else {
                write_failures = 0;
            }
        }

        index++;
        elapsed_  = static_cast<double>(index) * kPeriodNs / 1e9;
        position_ = settings.start + elapsed_;

        const auto deadline =
            started + std::chrono::nanoseconds(static_cast<int64_t>(index) * kPeriodNs);
        const auto late = Clock::now() - deadline;
        if (late > std::chrono::nanoseconds(kPeriodNs * 3)) {
            started += late;
        }
    }

    producer_stop = true;
    if (producer.joinable()) {
        producer.join();
    }

    // Silence the actuators and let the last packets drain.
    if (speaker_on_.load()) {
        uint8_t silence_opus[2 * ds5::kOpusBytes] {};
        int8_t  silence_haptics[ds5::kHapticFrames * 2] {};
        EncodeSilence(encoder, silence_opus);
        for (int i = 0; i < 6 && !failed; i++) {
            uint8_t report[ds5::kAudioReportSize];
            sequence = static_cast<uint8_t>((sequence + 1) & 0x0F);
            counter  = static_cast<uint8_t>(counter + 2);
            ds5::BuildAudioReport(report, sequence, counter, silence_haptics, silence_opus, true);
            device->Write(report, sizeof(report));
            std::this_thread::sleep_for(std::chrono::milliseconds(21));
        }
    }

    device->Close();
    opus_encoder_destroy(encoder);
    if (mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss);
    }

    if (index > 0) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "%llu reports (%.2f s), avg write %.2f ms, max %.2f ms, max gap %.2f ms, "
                      "late slots %d, skipped %d",
                      static_cast<unsigned long long>(index),
                      static_cast<double>(index) * kPeriodNs / 1e9,
                      write_total_us / 1000.0 / static_cast<double>(index), write_max_us / 1000.0,
                      gap_max_us / 1000.0, gaps_late, underruns);
        std::lock_guard lock(mutex_);
        stats_ = buffer;
    }
    running_ = false;
}

} // namespace ds
