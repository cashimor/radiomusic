#include "music.hpp"
#include <cmath>
#include <algorithm>

namespace music {
namespace {
// Constant stereo-linked gain preserves dynamics and stereo balance. Limit
// boosts to 12 dB and leave headroom; near-silent material is never boosted.
float levelGain(const std::vector<Frame>& audio, double target) {
    double energy = 0, peak = 0;
    for (auto f : audio) {
        energy += (double(f.l) * f.l + double(f.r) * f.r) * 0.5;
        peak = std::max({peak, double(std::abs(f.l)), double(std::abs(f.r))});
    }
    double rms = std::sqrt(energy / audio.size());
    if (rms < 0.008) return 1;
    return float(std::min({4.0, target / rms, 0.8 / std::max(peak, 1e-9)}));
}
// Two cascaded Q=1/sqrt(2) sections: fourth-order Linkwitz-Riley at 220 Hz.
// Low + high has flat magnitude, with the expected common all-pass phase shift.
struct Biquad {
    double b0, b1, b2, a1, a2;
    double z1[2]{}, z2[2]{};
    explicit Biquad(bool high) {
        constexpr double pi = 3.14159265358979323846;
        double w = 2 * pi * crossoverHz / sampleRate, c = std::cos(w), alpha = std::sin(w) / std::sqrt(2.0);
        double a0 = 1 + alpha;
        b0 = (high ? 1 + c : 1 - c) / (2 * a0);
        b1 = (high ? -(1 + c) : 1 - c) / a0; b2 = b0;
        a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
    }
    Frame next(Frame in) {
        Frame out;
        for (int channel = 0; channel < 2; ++channel) {
            double x = channel ? in.r : in.l;
            double y = b0 * x + z1[channel];
            z1[channel] = b1 * x - a1 * y + z2[channel]; z2[channel] = b2 * x - a2 * y;
            (channel ? out.r : out.l) = float(y);
        }
        return out;
    }
};
}
struct FrequencyMonitor::Impl {
    Biquad low1{false}, low2{false}, high1{true}, high2{true};
    double upper = 0, bass = 0;
};
FrequencyMonitor::FrequencyMonitor() : impl_(std::make_unique<Impl>()) {}
FrequencyMonitor::~FrequencyMonitor() = default;
Frame FrequencyMonitor::process(Frame input, int mode) {
    auto& s = *impl_;
    auto low = s.low2.next(s.low1.next(input));
    auto high = s.high2.next(s.high1.next(input));
    // Keep both filters warm and crossfade the monitor over about 20 ms.
    s.upper += ((mode == 1 ? 1.0 : 0.0) - s.upper) * 0.005;
    s.bass += ((mode == 2 ? 1.0 : 0.0) - s.bass) * 0.005;
    double dry = 1 - s.upper - s.bass;
    return {float(input.l * dry + high.l * s.upper + low.l * s.bass),
            float(input.r * dry + high.r * s.upper + low.r * s.bass)};
}
void prepareBands(Clip& clip) {
    if (clip.audio.empty() || clip.low.size() == clip.audio.size()) return;
    Biquad low1(false), low2(false), high1(true), high2(true);
    // Warm up on the preceding tail to avoid an artificial start transient on a looping source.
    size_t warm = clip.audio.size() > sampleRate ? clip.audio.size() - sampleRate : 0;
    for (size_t i = warm; i < clip.audio.size(); ++i) {
        low2.next(low1.next(clip.audio[i])); high2.next(high1.next(clip.audio[i]));
    }
    clip.low.resize(clip.audio.size()); clip.high.resize(clip.audio.size());
    for (size_t i = 0; i < clip.audio.size(); ++i) {
        clip.low[i] = low2.next(low1.next(clip.audio[i]));
        clip.high[i] = high2.next(high1.next(clip.audio[i]));
    }
    clip.playbackGain = levelGain(clip.audio, 0.16);
    clip.lowGain = levelGain(clip.low, 0.10);
    clip.highGain = levelGain(clip.high, 0.125);
}
}
