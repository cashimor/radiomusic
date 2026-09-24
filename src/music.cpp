#include "music.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <limits>

namespace music {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr int fftSize = 1024, hop = 441;
void fft(std::array<std::complex<double>, fftSize>& a) {
    for (int i = 1, j = 0; i < fftSize; ++i) {
        int bit = fftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int n = 2; n <= fftSize; n *= 2) {
        auto wn = std::polar(1.0, -2 * pi / n);
        for (int i = 0; i < fftSize; i += n) {
            std::complex<double> w{1, 0};
            for (int j = 0; j < n / 2; ++j) {
                auto u = a[i + j], v = a[i + j + n / 2] * w;
                a[i + j] = u + v; a[i + j + n / 2] = u - v; w *= wn;
            }
        }
    }
}
double interp(const std::vector<double>& v, double x) {
    if (x < 0 || x + 1 >= v.size()) return 0;
    auto i = static_cast<size_t>(x);
    return v[i] + (v[i + 1] - v[i]) * (x - i);
}
void put16(std::ostream& s, uint16_t x) {
    s.put(static_cast<char>(x)); s.put(static_cast<char>(x >> 8));
}
void put32(std::ostream& s, uint32_t x) { put16(s, uint16_t(x)); put16(s, uint16_t(x >> 16)); }
uint16_t get16(std::istream& s) {
    auto a = static_cast<unsigned char>(s.get()), b = static_cast<unsigned char>(s.get());
    return uint16_t(a | (b << 8));
}
uint32_t get32(std::istream& s) { uint32_t a = get16(s); return a | (uint32_t(get16(s)) << 16); }
}

Analysis analyze(const std::vector<Frame>& audio) {
    Analysis result;
    if (audio.size() < sampleRate * 12) return result;
    double energy = 0;
    for (auto f : audio) energy += (double(f.l) * f.l + double(f.r) * f.r) / 2;
    result.rms = std::sqrt(energy / audio.size());
    if (result.rms < 0.008) return result;
    std::array<double, fftSize / 2> previous{};
    std::array<std::complex<double>, fftSize> spectrum;
    std::vector<double> onset;
    for (size_t start = 0; start + fftSize <= audio.size(); start += hop) {
        for (int i = 0; i < fftSize; ++i) {
            auto f = audio[start + i];
            spectrum[i] = (f.l + f.r) * 0.5 * (0.5 - 0.5 * std::cos(2 * pi * i / (fftSize - 1)));
        }
        fft(spectrum);
        double flux = 0;
        for (int k = 1; k < fftSize / 2; ++k) {
            double magnitude = std::log1p(std::abs(spectrum[k]));
            double weight = k < 6 ? 2.5 : (k < 120 ? 1.0 : 0.25);
            flux += std::max(0.0, magnitude - previous[k]) * weight;
            previous[k] = magnitude;
        }
        onset.push_back(flux);
    }
    // Remove slow changes in loudness; retain local attacks.
    auto raw = onset;
    double total = 0;
    for (size_t i = 0; i < onset.size(); ++i) {
        double mean = 0; int count = 0;
        for (int j = -10; j <= 10; ++j) {
            auto k = static_cast<int64_t>(i) + j;
            if (k >= 0 && k < static_cast<int64_t>(raw.size())) { mean += raw[size_t(k)]; ++count; }
        }
        onset[i] = std::max(0.0, raw[i] - mean / count);
        total += onset[i] * onset[i];
    }
    if (total < 1e-9) return result;
    // Search an explicit dance-music range. Half/double-time ambiguity remains possible.
    double best = -1, bestBpm = 0;
    auto correlation = [&](double lag) {
        double sum = 0, a2 = 0, b2 = 0;
        for (size_t i = 0; i + lag + 1 < onset.size(); ++i) {
            double a = onset[i], b = interp(onset, i + lag);
            sum += a * b; a2 += a * a; b2 += b * b;
        }
        return sum / std::sqrt(std::max(1e-12, a2 * b2));
    };
    for (double bpm = 90; bpm <= 175; bpm += 0.1) {
        double lag = 6000 / bpm;
        double score = 0.5 * correlation(lag) + 0.3 * correlation(lag * 2) + 0.2 * correlation(lag * 4);
        if (score > best) { best = score; bestBpm = bpm; }
    }
    result.confidence = std::clamp(best, 0.0, 1.0);
    if (best < 0.18) return result;
    result.bpm = bestBpm;
    double period = 6000 / bestBpm, bestPhase = 0, phaseScore = -1;
    for (double p = 0; p < period; p += 0.25) {
        double sum = 0;
        for (double t = p; t + 1 < onset.size(); t += period) sum += interp(onset, t);
        if (sum > phaseScore) { phaseScore = sum; bestPhase = p; }
    }
    // Spectral-flux windows straddle the attack; position the cut near the onset.
    result.firstBeat = (bestPhase * hop + fftSize * 0.25) / sampleRate;
    return result;
}

std::vector<ClipPtr> extract(const std::vector<Frame>& audio, const Analysis& a,
                             const std::string& prefix, const std::string& source) {
    std::vector<ClipPtr> clips;
    if (a.bpm < 90 || a.bpm > 175 || a.confidence < 0.18) return clips;
    double beatFrames = sampleRate * 60.0 / a.bpm;
    auto length = static_cast<size_t>(std::llround(beatFrames * loopBeats));
    for (int n = 0; n < 3; ++n) {
        auto start = static_cast<size_t>(std::llround(a.firstBeat * sampleRate + n * beatFrames * loopBeats));
        if (start + length > audio.size()) break;
        auto clip = std::make_shared<Clip>();
        clip->id = prefix + "-" + std::to_string(n);
        clip->source = source; clip->bpm = a.bpm; clip->confidence = a.confidence;
        clip->captureId = prefix; clip->firstSourceBar = n * 8;
        clip->sourceSeconds = double(start) / sampleRate;
        clip->audio.assign(audio.begin() + start, audio.begin() + start + length);
        double peak = 0, energy = 0;
        for (auto f : clip->audio) {
            peak = std::max({peak, double(std::abs(f.l)), double(std::abs(f.r))});
            energy += (double(f.l) * f.l + double(f.r) * f.r) / 2;
        }
        double rms = std::sqrt(energy / length);
        if (rms < 0.008) continue;
        double gain = std::min({2.0, 0.16 / rms, 0.88 / std::max(peak, 1e-9)});
        for (auto& f : clip->audio) { f.l *= float(gain); f.r *= float(gain); }
        auto key = estimateKey(clip->audio); clip->key = key.key; clip->keyConfidence = key.confidence;
        clips.push_back(std::move(clip));
    }
    return clips;
}

bool writeWave(const std::filesystem::path& path, const std::vector<Frame>& audio) {
    if (audio.size() > (UINT32_MAX - 36) / 4) return false;
    std::ofstream s(path, std::ios::binary);
    uint32_t bytes = static_cast<uint32_t>(audio.size() * 4);
    s.write("RIFF", 4); put32(s, 36 + bytes); s.write("WAVEfmt ", 8); put32(s, 16);
    put16(s, 1); put16(s, 2); put32(s, sampleRate); put32(s, sampleRate * 4);
    put16(s, 4); put16(s, 16); s.write("data", 4); put32(s, bytes);
    for (auto f : audio) for (float x : {f.l, f.r})
        put16(s, static_cast<uint16_t>(static_cast<int16_t>(std::clamp(x, -1.0f, 1.0f) * 32767)));
    s.flush(); return s.good();
}
bool saveClip(const std::filesystem::path& dir, const Clip& clip) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    auto wave = dir / (clip.id + ".wav"), temp = dir / (clip.id + ".part");
    if (!writeWave(temp, clip.audio)) return false;
    std::filesystem::rename(temp, wave, ec);
    if (ec) return false;
    std::ofstream meta(dir / (clip.id + ".txt"));
    meta << std::setprecision(12) << clip.bpm << ' ' << clip.confidence << ' ' << clip.beats << '\n'
         << std::quoted(clip.source) << '\n'
         << "RM2 " << clip.key << ' ' << clip.keyConfidence << ' ' << std::quoted(clip.captureId)
         << ' ' << clip.firstSourceBar << ' ' << clip.sourceSeconds << '\n';
    meta.flush(); return meta.good();
}
ClipPtr loadClip(const std::filesystem::path& wave, bool metadataOnly) {
    auto meta = wave; meta.replace_extension(".txt");
    auto rejected = wave; rejected.replace_extension(".rejected");
    std::error_code ec;
    if (std::filesystem::exists(rejected, ec)) return {};
    auto clip = std::make_shared<Clip>();
    std::ifstream m(meta);
    if (!(m >> clip->bpm >> clip->confidence >> clip->beats >> std::quoted(clip->source))) return {};
    if (!std::isfinite(clip->bpm) || clip->bpm < 90 || clip->bpm > 175 || clip->beats != loopBeats
        || !std::isfinite(clip->confidence) || clip->confidence < 0 || clip->confidence > 1) return {};
    std::ifstream s(wave, std::ios::binary);
    char header[4]{}; s.read(header, 4);
    if (std::string(header, 4) != "RIFF") return {};
    get32(s); s.read(header, 4); if (std::string(header, 4) != "WAVE") return {};
    s.read(header, 4); if (std::string(header, 4) != "fmt " || get32(s) != 16) return {};
    if (get16(s) != 1 || get16(s) != 2 || get32(s) != sampleRate) return {};
    get32(s); if (get16(s) != 4 || get16(s) != 16) return {};
    s.read(header, 4); if (std::string(header, 4) != "data") return {};
    auto bytes = get32(s);
    if (!s || bytes % 4 || bytes < sampleRate * 4 || bytes > uint32_t(sampleRate * 30 * 4)) return {};
    size_t expected = static_cast<size_t>(std::llround(sampleRate * 60.0 * clip->beats / clip->bpm));
    if (std::abs(double(bytes / 4) - expected) > 4 || std::filesystem::file_size(wave, ec) < uint64_t(bytes) + 44 || ec) return {};
    if (!metadataOnly) {
        clip->audio.resize(bytes / 4);
        for (auto& f : clip->audio) {
            f.l = static_cast<int16_t>(get16(s)) / 32768.0f;
            f.r = static_cast<int16_t>(get16(s)) / 32768.0f;
        }
    }
    if (!s) return {};
    clip->id = wave.stem().string();
    std::string marker;
    bool haveMetadata = bool(m >> marker) && marker == "RM2" &&
        bool(m >> clip->key >> clip->keyConfidence >> std::quoted(clip->captureId) >> clip->firstSourceBar >> clip->sourceSeconds);
    if (haveMetadata && (clip->key < -1 || clip->key > 23 || !std::isfinite(clip->keyConfidence) ||
        clip->keyConfidence < 0 || clip->keyConfidence > 1 || clip->firstSourceBar < 0 || clip->firstSourceBar > 100000 ||
        !std::isfinite(clip->sourceSeconds) || clip->sourceSeconds < 0)) return {};
    if (!haveMetadata) {
        clip->captureId = clip->id;
        auto dash = clip->id.rfind('-');
        if (dash != std::string::npos && dash + 2 == clip->id.size() && clip->id.back() >= '0' && clip->id.back() <= '2') {
            clip->captureId = clip->id.substr(0, dash); clip->firstSourceBar = (clip->id.back() - '0') * 8;
        }
        if (!metadataOnly) { auto key = estimateKey(clip->audio); clip->key = key.key; clip->keyConfidence = key.confidence; }
    }
    if (clip->captureId.empty()) clip->captureId = clip->id;
    return clip;
}

}
