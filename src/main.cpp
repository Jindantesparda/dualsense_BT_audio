// ds5play - command-line front end for the DualSense music player.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <io.h>
#include <string>
#include <thread>
#include <vector>

#include "hid_win32.h"
#include "player.h"

namespace {

std::atomic<bool> g_stop {false};

BOOL WINAPI OnConsoleCtrl(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = true;
        return TRUE;
    default:
        return FALSE;
    }
}

struct Options {
    std::wstring file;
    int          device         = -1;
    bool         list           = false;
    bool         list_audio     = false;
    int          audio_device   = -1;
    bool         test           = false;
    bool         sweep          = false;
    bool         system_audio   = false;
    bool         loop           = false;
    bool         haptics        = true;
    bool         speaker        = true;
    bool         verbose        = false;
    double       start          = 0.0;
    double       duration       = 0.0;
    double       haptics_gain   = 1.0;
    double       tone           = 90.0; // percent, 100 = full-band
    double       speaker_gain   = 1.0;
    int          speaker_volume = 100;
};

void PrintUsage() {
    std::printf(
        "ds5play - play music on a DualSense over Bluetooth (speaker + haptics)\n"
        "\n"
        "Usage:\n"
        "  ds5play [options] <audio file>\n"
        "  ds5play --test [options]\n"
        "\n"
        "Audio files: wav, mp3, flac, ogg.  A GUI is available as DualSenseMusic.exe.\n"
        "\n"
        "Options:\n"
        "  -l, --list               list DualSense HID interfaces and exit\n"
        "  -d, --device N           use interface N from --list\n"
        "  -t, --test               play a built-in test loop (no file needed)\n"
        "      --system-audio       play the PC's output (WASAPI loopback)\n"
        "      --loop               repeat the file forever\n"
        "      --start SECONDS      start at this position\n"
        "      --duration SECONDS   stop after this many seconds\n"
        "      --haptics-gain X     actuator level multiplier (default 1.0)\n"
        "      --tone N             haptic tone, 0 = bass only, 100 = full band (default 80)\n"
        "      --speaker-gain X     speaker level multiplier (default 1.0)\n"
        "      --speaker-volume N   controller speaker volume 0..100 (default 100)\n"
        "      --no-haptics         speaker only\n"
        "      --no-speaker         haptics only\n"
        "  -v, --verbose            print timing statistics\n"
        "  -h, --help               this text\n"
        "\n"
        "Ctrl+C stops playback.\n");
}

std::string FormatTime(double seconds) {
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) {
        seconds = 0.0;
    }
    const int total = static_cast<int>(seconds + 0.5);
    char      text[32];
    std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

bool ParseDouble(const wchar_t* text, double* value) {
    wchar_t*     end    = nullptr;
    const double parsed = std::wcstod(text, &end);
    if (end == text || !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

bool ParseInt(const wchar_t* text, int* value) {
    wchar_t*   end    = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (end == text) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

void PrintHintNoController() {
    std::printf(
        "No DualSense controller was found.\n"
        "  - Turn the controller on and make sure Windows shows it as connected.\n"
        "  - Bluetooth pairing: hold Create + PS until the light bar flashes, then\n"
        "    add it in Settings > Bluetooth & devices > Add device.\n"
        "  - Run \"ds5play --list\" to see the HID interfaces Windows exposes.\n");
}

} // namespace

int Run(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    Options options;
    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        auto next = [&](const wchar_t** out) {
            if (i + 1 >= argc) {
                return false;
            }
            *out = argv[++i];
            return true;
        };
        const wchar_t* value = nullptr;

        if (arg == L"-h" || arg == L"--help") {
            PrintUsage();
            return 0;
        } else if (arg == L"-l" || arg == L"--list") {
            options.list = true;
        } else if (arg == L"--list-audio") {
            options.list_audio = true;
        } else if (arg == L"--audio-device") {
            if (!next(&value) || !ParseInt(value, &options.audio_device)) {
                std::printf("error: --audio-device needs a number\n");
                return 1;
            }
        } else if (arg == L"-t" || arg == L"--test") {
            options.test = true;
        } else if (arg == L"--system-audio") {
            options.system_audio = true;
        } else if (arg == L"--sweep") {
            options.test  = true;
            options.sweep = true;
            options.speaker = false;
            options.tone    = 100.0;
            options.loop    = true;
        } else if (arg == L"--loop") {
            options.loop = true;
        } else if (arg == L"--no-haptics") {
            options.haptics = false;
        } else if (arg == L"--no-speaker") {
            options.speaker = false;
        } else if (arg == L"-v" || arg == L"--verbose") {
            options.verbose = true;
        } else if (arg == L"-d" || arg == L"--device") {
            if (!next(&value) || !ParseInt(value, &options.device)) {
                std::printf("error: --device needs a number\n");
                return 1;
            }
        } else if (arg == L"--start") {
            if (!next(&value) || !ParseDouble(value, &options.start) || options.start < 0.0) {
                std::printf("error: --start needs a non-negative number of seconds\n");
                return 1;
            }
        } else if (arg == L"--duration") {
            if (!next(&value) || !ParseDouble(value, &options.duration) || options.duration <= 0.0) {
                std::printf("error: --duration needs a positive number of seconds\n");
                return 1;
            }
        } else if (arg == L"--haptics-gain") {
            if (!next(&value) || !ParseDouble(value, &options.haptics_gain)) {
                std::printf("error: --haptics-gain needs a number\n");
                return 1;
            }
        } else if (arg == L"--tone") {
            if (!next(&value) || !ParseDouble(value, &options.tone)) {
                std::printf("error: --tone needs 0..100\n");
                return 1;
            }
            options.tone = std::clamp(options.tone, 0.0, 100.0);
        } else if (arg == L"--speaker-gain") {
            if (!next(&value) || !ParseDouble(value, &options.speaker_gain) ||
                options.speaker_gain < 0.0) {
                std::printf("error: --speaker-gain needs a non-negative number\n");
                return 1;
            }
        } else if (arg == L"--speaker-volume") {
            if (!next(&value) || !ParseInt(value, &options.speaker_volume)) {
                std::printf("error: --speaker-volume needs 0..100\n");
                return 1;
            }
            options.speaker_volume = std::clamp(options.speaker_volume, 0, 100);
        } else if (!arg.empty() && arg[0] == L'-') {
            std::printf("error: unknown option %s\n", ToUtf8(arg).c_str());
            PrintUsage();
            return 1;
        } else {
            options.file = arg;
        }
    }

    if (options.list_audio) {
        std::printf("Windows playback devices:\n");
        int index = 0;
        for (const auto& device: ds::ListAudioOutputs()) {
            std::string channels;
            for (int count: device.channel_counts) {
                if (!channels.empty()) {
                    channels += ",";
                }
                channels += std::to_string(count);
            }
            std::printf("  %2d %s %-52s ch %s\n", index,
                        device.is_default ? "*" : " ", device.name.c_str(),
                        channels.empty() ? "?" : channels.c_str());
            index++;
        }
        return 0;
    }

    if (!options.test && !options.system_audio && options.file.empty() && !options.list) {
        PrintUsage();
        return 1;
    }

    const std::vector<HidDeviceInfo> pads = ds::FindDualSensePads();

    if (options.list) {
        if (pads.empty()) {
            PrintHintNoController();
            return 1;
        }
        std::printf("DualSense HID interfaces:\n");
        for (size_t i = 0; i < pads.size(); i++) {
            const auto& pad = pads[i];
            std::printf("  %2d  %-10s out=%-4u  %s\n", static_cast<int>(i),
                        pad.bluetooth ? "Bluetooth" : "USB", pad.output_report_bytes,
                        pad.name.empty() ? "(no name)" : pad.name.c_str());
            std::printf("      %s\n", pad.instance.c_str());
        }
        return 0;
    }

    if (pads.empty()) {
        PrintHintNoController();
        return 1;
    }

    const int chosen = options.device >= 0 ? options.device : 0;
    if (chosen >= static_cast<int>(pads.size())) {
        std::printf("error: device index out of range; use --list\n");
        return 1;
    }
    const HidDeviceInfo& pad = pads[static_cast<size_t>(chosen)];

    if (!pad.bluetooth) {
        std::printf(
            "error: the selected controller is connected over USB.\n"
            "This program uses the Bluetooth-only audio reports. Unplug the cable and\n"
            "connect the DualSense over Bluetooth, then try again.\n");
        return 1;
    }

    ds::Settings settings;
    settings.test           = options.test;
    settings.sweep          = options.sweep;
    settings.system_audio   = options.system_audio;
    if (options.system_audio && options.audio_device >= 0) {
        const std::vector<ds::AudioOutputInfo> outputs = ds::ListAudioOutputs();
        if (options.audio_device >= static_cast<int>(outputs.size())) {
            std::printf("error: audio device index out of range; use --list-audio\n");
            return 1;
        }
        settings.loopback_device = outputs[static_cast<size_t>(options.audio_device)].name;
    }
    settings.loop           = options.loop;
    settings.haptics        = options.haptics;
    settings.speaker        = options.speaker;
    settings.haptics_gain   = static_cast<float>(options.haptics_gain);
    settings.tone           = static_cast<float>(options.tone / 100.0);
    settings.speaker_gain   = static_cast<float>(options.speaker_gain);
    settings.speaker_volume = options.speaker_volume;
    settings.start          = options.start;
    settings.duration       = options.duration;

    ds::Player player;
    std::string error;
    if (!player.Start(options.file, pad, settings, &error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }

    std::printf("DualSense Music\n");
    std::printf("  controller : %s (Bluetooth)\n",
                pad.name.empty() ? "Wireless Controller" : pad.name.c_str());
    if (options.test) {
        std::printf("  source     : built-in test loop\n");
    } else {
        std::printf("  file       : %s\n", ToUtf8(options.file).c_str());
    }
    std::printf("  haptics    : %s (tone %.0f%%, gain %.2f)\n", options.haptics ? "on" : "off",
                options.tone, options.haptics_gain);
    std::printf("  speaker    : %s (volume %d, gain %.2f)\n\n", options.speaker ? "on" : "off",
                options.speaker_volume, options.speaker_gain);

    const bool interactive = _isatty(_fileno(stdout)) != 0;
    while (player.Running() && !g_stop) {
        if (interactive) {
            const double position = player.Position();
            const double duration = player.Duration();
            char         bar[64]  = {};
            if (duration > 0.0) {
                const double progress = std::min(1.0, position / duration);
                const int    filled   = static_cast<int>(progress * 28.0);
                std::memset(bar, '=', static_cast<size_t>(filled));
                std::memset(bar + filled, ' ', 28u - static_cast<size_t>(filled));
                bar[28] = 0;
                std::printf("\r  %s / %s  [%s] %3.0f%%   ", FormatTime(position).c_str(),
                            FormatTime(duration).c_str(), bar, progress * 100.0);
            } else {
                std::printf("\r  %s        ", FormatTime(player.Elapsed()).c_str());
            }
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    player.Stop();

    const std::string run_error = player.Error();
    if (interactive) {
        std::printf("\r%*s\r", 60, "");
    }
    if (options.verbose) {
        const std::string stats = player.Stats();
        if (!stats.empty()) {
            std::printf("stats: %s\n", stats.c_str());
        }
    }
    if (!run_error.empty()) {
        std::printf("error: %s\n", run_error.c_str());
        return 1;
    }
    std::printf("%s\n", g_stop ? "stopped." : "done.");
    return 0;
}

int main() {
    int       argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);
    const int result = Run(argc, argv);
    LocalFree(argv);
    return result;
}
