// Playback engine shared by the command-line tool and the GUI.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hid_win32.h"

namespace ds {

struct Settings {
    bool   test           = false; // play the built-in test loop instead of a file
    bool   sweep          = false; // test loop is a slow 40 Hz -> 1 kHz pitch sweep
    bool   system_audio   = false; // play the PC's output (WASAPI loopback)
    std::string loopback_device;   // empty = default output device
    bool   loop           = false;
    bool   haptics        = true;
    bool   speaker        = true;
    float  haptics_gain   = 1.0f;  // 0..4
    float  tone           = 0.9f;  // 0 = bass only, 1 = full-band haptics
    float  speaker_gain   = 1.0f;  // 0..2
    int    speaker_volume = 100;   // 0..100, controller's own volume register
    double start          = 0.0;   // seconds
    double duration       = 0.0;   // 0 = until the end
};

// DualSense interfaces, best Bluetooth candidate first.
std::vector<HidDeviceInfo> FindDualSensePads();

struct AudioOutputInfo {
    std::string       name;
    bool              is_default = false;
    std::vector<int>  channel_counts;
};

// Every Windows playback endpoint, with the channel counts it advertises.
std::vector<AudioOutputInfo> ListAudioOutputs();

class Player {
public:
    Player() = default;
    ~Player();
    Player(const Player&)            = delete;
    Player& operator=(const Player&) = delete;

    bool Start(const std::wstring& file, const HidDeviceInfo& device, const Settings& settings,
               std::string* error);
    void Stop();

    bool   Running() const { return running_.load(std::memory_order_relaxed); }
    double Position() const { return position_.load(std::memory_order_relaxed); }
    double Duration() const { return duration_.load(std::memory_order_relaxed); }
    double Elapsed() const { return elapsed_.load(std::memory_order_relaxed); }

    std::string Error() const;
    std::string Stats() const;

    // Live adjustments; the running thread picks them up on the next report.
    void SetHapticsGain(float value);
    void SetTone(float value);
    void SetSpeakerGain(float value);
    void SetSpeakerVolume(int value);
    void SetLoop(bool value);
    void SetHapticsEnabled(bool value);
    void SetSpeakerEnabled(bool value);

private:
    void Run(std::wstring file, std::unique_ptr<HidDevice> device, Settings settings);

    std::thread             thread_;
    std::atomic<bool>       stop_ {false};
    std::atomic<bool>       running_ {false};
    std::atomic<double>     position_ {0.0};
    std::atomic<double>     duration_ {0.0};
    std::atomic<double>     elapsed_ {0.0};
    std::atomic<float>      haptics_gain_ {1.0f};
    std::atomic<float>      tone_ {0.8f};
    std::atomic<float>      speaker_gain_ {1.0f};
    std::atomic<int>        speaker_volume_ {100};
    std::atomic<bool>       loop_ {false};
    std::atomic<bool>       haptics_on_ {true};
    std::atomic<bool>       speaker_on_ {true};
    mutable std::mutex      mutex_;
    std::string             error_;
    std::string             stats_;
};

} // namespace ds
