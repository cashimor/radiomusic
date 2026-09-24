#include "music.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <sstream>

namespace music {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int nfft = 4096;
void spectrum(std::array<std::complex<double>, nfft>& a) {
    for (int i = 1, j = 0; i < nfft; ++i) {
        int bit = nfft >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit; if (i < j) std::swap(a[i], a[j]);
    }
    for (int n = 2; n <= nfft; n *= 2) {
        auto step = std::polar(1.0, -2 * pi / n);
        for (int i = 0; i < nfft; i += n) {
            std::complex<double> w{1, 0};
            for (int j = 0; j < n / 2; ++j) {
                auto u = a[i + j], v = a[i + j + n / 2] * w;
                a[i + j] = u + v; a[i + j + n / 2] = u - v; w *= step;
            }
        }
    }
}
}

uint32_t sourceColorId(const std::string& id) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : id) { hash ^= c; hash *= 16777619u; }
    return hash;
}
std::string sourceLabel(const std::string& id) {
    std::ostringstream s; s << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << (sourceColorId(id) & 0xffff);
    return s.str();
}
std::string keyName(int key) {
    static const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    if (key < 0 || key >= 24) return "uncertain";
    return std::string(names[key % 12]) + (key < 12 ? " major" : " minor");
}
bool compatibleKeys(int a, int b) {
    if (a < 0 || b < 0) return true; // Unknown keys are explicitly permissive, never invented.
    if (a == b) return true;
    if (a / 12 == b / 12) { int delta = (a - b + 24) % 12; return delta == 5 || delta == 7; }
    int major = a < 12 ? a : b, minor = a >= 12 ? a - 12 : b - 12;
    return (major + 9) % 12 == minor;
}
bool compatibleTempo(double sourceBpm, double targetBpm) {
    if (!std::isfinite(sourceBpm) || !std::isfinite(targetBpm) || sourceBpm <= 0 || targetBpm <= 0) return false;
    double speed = targetBpm / sourceBpm;
    return speed >= 1 - maxSpeedChange - 1e-9 && speed <= 1 + maxSpeedChange + 1e-9;
}
int playbackKey(const Clip& clip, double targetBpm) {
    if (clip.key < 0 || clip.key >= 24 || clip.bpm <= 0 || targetBpm <= 0) return -1;
    int shift = int(std::lround(12 * std::log2(targetBpm / clip.bpm)));
    return (clip.key / 12) * 12 + ((clip.key % 12 + shift) % 12 + 12) % 12;
}
KeyEstimate estimateKey(const std::vector<Frame>& audio) {
    if (audio.size() < sampleRate / 2) return {};
    // One-pole low-pass stages before decimation; analysis only, never alters playback.
    std::vector<double> mono; mono.reserve(audio.size() / 4);
    double low1 = 0, low2 = 0;
    for (size_t i = 0; i < audio.size(); ++i) {
        double x = (audio[i].l + audio[i].r) * 0.5;
        low1 += 0.24 * (x - low1); low2 += 0.24 * (low1 - low2);
        if (i % 4 == 0) mono.push_back(low2);
    }
    std::array<double, 12> chroma{};
    std::array<std::complex<double>, nfft> bins;
    double total = 0;
    for (size_t start = 0; start + nfft <= mono.size(); start += 2048) {
        for (int i = 0; i < nfft; ++i) bins[i] = mono[start + i] * (0.5 - 0.5 * std::cos(2 * pi * i / (nfft - 1)));
        spectrum(bins);
        for (int k = 26; k < 744; ++k) {
            double mag = std::abs(bins[k]);
            if (mag < 0.02 || mag <= std::abs(bins[k - 1]) || mag < std::abs(bins[k + 1])) continue;
            double left = std::log(std::max(1e-12, std::abs(bins[k - 1])));
            double center = std::log(mag), right = std::log(std::max(1e-12, std::abs(bins[k + 1])));
            double denominator = left - 2 * center + right;
            double offset = std::abs(denominator) > 1e-9 ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5) : 0;
            double freq = (k + offset) * (sampleRate / 4.0) / nfft;
            double note = 69 + 12 * std::log2(freq / 440.0);
            int nearest = int(std::lround(note));
            if (std::abs(note - nearest) > 0.3) continue;
            double weight = std::sqrt(mag);
            chroma[(nearest % 12 + 12) % 12] += weight; total += weight;
        }
    }
    if (total < 0.1) return {};
    // Krumhansl major/minor pitch-class profiles; correlation, not a calibrated probability.
    constexpr double profiles[2][12] = {
        {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88},
        {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17}};
    double mean = total / 12, variance = 0;
    for (double value : chroma) variance += (value - mean) * (value - mean);
    if (variance < total * total * 0.002) return {};
    double best = -2, second = -2; int key = -1;
    for (int mode = 0; mode < 2; ++mode) {
        double average = 0; for (double value : profiles[mode]) average += value / 12;
        for (int root = 0; root < 12; ++root) {
            double cross = 0, pv = 0;
            for (int note = 0; note < 12; ++note) {
                double p = profiles[mode][(note - root + 12) % 12] - average;
                cross += (chroma[note] - mean) * p; pv += p * p;
            }
            double score = cross / std::sqrt(variance * pv);
            if (score > best) { second = best; best = score; key = root + mode * 12; }
            else second = std::max(second, score);
        }
    }
    double confidence = std::clamp((best - second) * 5, 0.0, 1.0);
    if (best < 0.6 || confidence < 0.15) return {-1, confidence};
    return {key, confidence};
}

}
