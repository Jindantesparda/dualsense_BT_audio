// Full-band haptics: the actuators get the music itself (up to the 1.5 kHz
// Nyquist limit of the 3 kHz haptic stream), optionally blended with a
// bass-only version for a heavier feel.

#pragma once

#include <algorithm>
#include <cmath>

namespace dsp {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    static Biquad LowPass(double sample_rate, double cutoff, double q) {
        Biquad f;
        const double w0    = 2.0 * 3.14159265358979323846 * cutoff / sample_rate;
        const double cosw  = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0    = 1.0 + alpha;
        f.b0 = (1.0 - cosw) * 0.5 / a0;
        f.b1 = (1.0 - cosw) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * cosw / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    static Biquad HighPass(double sample_rate, double cutoff, double q) {
        Biquad f;
        const double w0    = 2.0 * 3.14159265358979323846 * cutoff / sample_rate;
        const double cosw  = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0    = 1.0 + alpha;
        f.b0 = (1.0 + cosw) * 0.5 / a0;
        f.b1 = -(1.0 + cosw) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0 * cosw / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    static Biquad HighShelf(double sample_rate, double cutoff, double gain_db, double slope) {
        Biquad f;
        const double a     = std::pow(10.0, gain_db / 40.0);
        const double w0    = 2.0 * 3.14159265358979323846 * cutoff / sample_rate;
        const double cosw  = std::cos(w0);
        const double alpha = std::sin(w0) * 0.5 *
                             std::sqrt((a + 1.0 / a) * (1.0 / slope - 1.0) + 2.0);
        const double beta  = 2.0 * std::sqrt(a) * alpha;
        const double a0    = (a + 1.0) - (a - 1.0) * cosw + beta;
        f.b0 = a * ((a + 1.0) + (a - 1.0) * cosw + beta) / a0;
        f.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw) / a0;
        f.b2 = a * ((a + 1.0) + (a - 1.0) * cosw - beta) / a0;
        f.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosw) / a0;
        f.a2 = ((a + 1.0) - (a - 1.0) * cosw - beta) / a0;
        return f;
    }

    double Process(double x) {
        const double y = b0 * x + z1;
        z1             = b1 * x - a1 * y + z2;
        z2             = b2 * x - a2 * y;
        return y;
    }
};

class HapticConverter {
public:
    static constexpr int   kDecimation = 15;   // 45 kHz -> 3 kHz
    static constexpr double kBassCutoff = 180.0;

    explicit HapticConverter(double sample_rate) : sample_rate_(sample_rate) {
        for (int ch = 0; ch < 2; ch++) {
            // The actuators are much louder at 60 Hz than at 600 Hz, so a flat
            // feed feels like thumps. Cut the sub-bass and lift everything above
            // ~300 Hz so melody and texture come through as strongly as the kick.
            highpass_[ch] = Biquad::HighPass(sample_rate, 70.0, 0.7071);
            alias_a_[ch]  = Biquad::LowPass(sample_rate, 1200.0, 0.5412);
            alias_b_[ch]  = Biquad::LowPass(sample_rate, 1200.0, 1.3066);
            shelf_[ch]    = Biquad::HighShelf(sample_rate, 250.0, 12.0, 0.9);
            bass_[ch]     = Biquad::LowPass(sample_rate, kBassCutoff, 0.7071);
            envelope_[ch] = 0.0;
        }
        // 80 ms release; attack is instantaneous so a transient never clips.
        release_ = std::exp(-1.0 / (0.100 * 3000.0));
    }

    // in: frames * 2 interleaved samples at 45 kHz (frames divides by 15).
    // out: (frames / 15) * 2 interleaved samples at 3 kHz, limited to +-1.
    void Process(const float* in, int frames, float* out, float tone, float gain) {
        tone = std::clamp(tone, 0.0f, 1.0f);
        if (!std::isfinite(gain)) {
            gain = 1.0f;
        }
        int written = 0;
        for (int i = 0; i < frames; i++) {
            for (int ch = 0; ch < 2; ch++) {
                double x = in[static_cast<size_t>(i) * 2 + ch];
                x        = highpass_[ch].Process(x);
                x        = alias_a_[ch].Process(x);
                x        = alias_b_[ch].Process(x);
                x        = shelf_[ch].Process(x);

                const double bass = bass_[ch].Process(x);
                double       mixed = bass * (1.0 - tone) + x * tone;
                mixed *= gain;

                // Peak limiter: instant attack, slow release. Keeps the detail
                // of quiet passages without letting loud ones wrap around when
                // they are quantised to 8 bit.
                const double magnitude = std::fabs(mixed);
                envelope_[ch]          = std::max(magnitude, envelope_[ch] * release_);
                if (envelope_[ch] > 1.0) {
                    mixed /= envelope_[ch];
                }

                if (i % kDecimation == 0) {
                    out[written * 2 + ch] = static_cast<float>(mixed);
                }
            }
            if (i % kDecimation == 0) {
                written++;
            }
        }
    }

private:
    double sample_rate_;
    double release_ = 0.9958;
    Biquad highpass_[2];
    Biquad alias_a_[2];
    Biquad alias_b_[2];
    Biquad shelf_[2];
    Biquad bass_[2];
    double envelope_[2] = {0.0, 0.0};
};

} // namespace dsp
