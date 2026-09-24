#include "music.hpp"
#include "library.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
constexpr double tau = 6.283185307179586;
std::vector<music::Frame> clicks(double bpm) {
    std::vector<music::Frame> result(music::sampleRate * 32);
    for (double beat = 0.3; beat < 32; beat += 60.0 / bpm) {
        size_t start = size_t(beat * music::sampleRate);
        for (size_t j = 0; j < 3500 && start + j < result.size(); ++j) {
            double t = double(j) / music::sampleRate;
            float value = float(0.8 * std::exp(-t * 65) * std::sin(tau * (90 * t + 0.1 * (1 - std::exp(-t * 80)))));
            result[start + j] = {value, value};
        }
    }
    return result;
}
music::ClipPtr testClip(std::string id, double bpm = 120) {
    auto c = std::make_shared<music::Clip>(); c->id = id; c->captureId = id; c->bpm = bpm; c->confidence = 0.9;
    c->source = "test source"; c->key = 0;
    c->audio.resize(size_t(std::llround(music::sampleRate * 60.0 * music::loopBeats / bpm)), {0.2f, -0.2f});
    return c;
}
double rms(const std::vector<music::Frame>& frames) {
    double sum = 0; size_t start = std::min(size_t(music::sampleRate), frames.size() / 2);
    for (size_t i = start; i < frames.size(); ++i) sum += double(frames[i].l) * frames[i].l;
    return std::sqrt(sum / (frames.size() - start));
}
int main() {
    try {
        for (double bpm : {120.0, 138.0, 155.0}) {
            auto audio = clicks(bpm); auto a = music::analyze(audio);
            check(std::abs(a.bpm - bpm) < 0.8, "Tempo estimate outside tolerance");
            auto clips = music::extract(audio, a, "click-test", "synthetic");
            check(!clips.empty(), "No loops extracted from clear beats");
            check(std::abs(double(clips[0]->audio.size()) / music::sampleRate - 32 * 60 / bpm) < 0.08, "Wrong phrase length");
        }
        check(music::analyze(std::vector<music::Frame>(music::sampleRate * 15)).bpm == 0, "Silence accepted as a beat grid");
        check(music::compatibleKeys(0, 21) && music::compatibleKeys(0, 7) && !music::compatibleKeys(0, 1), "Key compatibility failed");
        std::vector<music::Frame> chord(music::sampleRate * 6);
        for (size_t i = 0; i < chord.size(); ++i) {
            double t = double(i) / music::sampleRate;
            float v = float(0.30 * std::sin(tau * 261.6256 * t) + 0.18 * std::sin(tau * 329.6276 * t) + 0.20 * std::sin(tau * 391.9954 * t));
            chord[i] = {v, v};
        }
        check(music::estimateKey(chord).key == 0, "Clear major chord detection failed");
        check(music::estimateKey(std::vector<music::Frame>(music::sampleRate)).key == -1, "Silence assigned a key");

        for (double frequency : {50.0, 150.0, 1000.0}) {
            music::Clip tone; tone.audio.resize(music::sampleRate * 3);
            for (size_t i = 0; i < tone.audio.size(); ++i) {
                float x = float(0.3 * std::sin(tau * frequency * i / music::sampleRate)); tone.audio[i] = {x, x};
            }
            music::prepareBands(tone); auto sum = tone.low;
            for (size_t i = 0; i < sum.size(); ++i) sum[i].l += tone.high[i].l;
            double reference = rms(tone.audio), low = rms(tone.low) / reference, high = rms(tone.high) / reference;
            check(std::abs(rms(sum) / reference - 1) < 0.005, "Crossover bands do not sum to flat magnitude");
            if (frequency == 150) check(std::abs(low - 0.5) < 0.005 && std::abs(high - 0.5) < 0.005, "Crossover is not -6 dB at 150 Hz");
            if (frequency == 50) check(low > 0.98 && high < 0.015, "Bass separation failed");
            if (frequency == 1000) check(high > 0.995 && low < 0.001, "Upper-band separation failed");
        }
        std::cout << "150 Hz four-pole crossover: separation and flat summation passed.\n";

        check(music::compatibleTempo(130, 140) && music::compatibleTempo(140, 130), "130-140 BPM sources should match in both directions");
        check(music::compatibleTempo(100, 110) && music::compatibleTempo(100, 90), "Speed-change boundary rejected");
        check(!music::compatibleTempo(100, 111) && !music::compatibleTempo(100, 89) && !music::compatibleTempo(0, 130), "Severe/invalid tempo mismatch accepted");
        auto a = testClip("test-a"), b = testClip("test-b", 130), wrongTempo = testClip("wrong-tempo", 155), clash = testClip("clash");
        b->key = 8; clash->key = 1; // Ab source slowed to ~G; compatible with C.
        check(music::playbackKey(*b, 120) == 7, "Speed-induced key shift was not accounted for");
        music::Mixer timed; timed.setPlayful(false); timed.setSplit(false);
        std::vector<music::ClipPtr> library{a}; timed.next(library); library = {a, b, wrongTempo, clash};
        int start = -1, finish = -1; constexpr int phrase = music::sampleRate * 16;
        for (int frame = 1; frame < phrase * 3 + 8; ++frame) {
            auto out = timed.next(library); check(std::isfinite(out.l) && std::abs(out.l) <= 0.201, "Transition output invalid");
            if (timed.transitioning() && start < 0) start = frame;
            if (!timed.transitioning() && start >= 0) { finish = frame; break; }
        }
        check(std::abs(start - phrase * 2) <= 3, "Automatic transition did not start after exactly two passes");
        check(std::abs((finish - start) - phrase) <= 3, "Crossfade did not last exactly one pass");
        check(timed.audible().size() == 1 && timed.audible()[0] == b, "Key/tempo matching chose the wrong source");
        check(timed.passes() == 0, "Incoming sequence did not receive two fresh steady passes");
        check(std::abs(timed.bpm() - 120) < 0.001, "Tempo changed when the 130 BPM source joined the 120 BPM mix");
        b->rejected = true;
        for (int i = 0; i < music::sampleRate; ++i) timed.next(library);
        for (auto& c : timed.sources()) check(c != b, "Rejected clip remained in a sequence");
        std::cout << "Sequence timing: two steady passes + one transition pass; wider tempo matching passed.\n";

        b->rejected = false;
        music::Mixer faster; faster.setSplit(false); faster.setPlayful(false); faster.next({b}); faster.requestEvolution();
        std::vector<music::ClipPtr> speedUpSources{a, b};
        start = finish = -1;
        for (int i = 0; i < music::sampleRate * 31; ++i) {
            faster.next(speedUpSources);
            if (faster.transitioning() && start < 0) start = i;
            if (!faster.transitioning() && start >= 0) { finish = i; break; }
        }
        check(start >= 0 && finish >= 0 && std::abs((finish - start) - music::sampleRate * 60.0 * music::loopBeats / 130) <= 2,
              "Fractional-frame tempo added an extra transition cycle");
        check(std::abs(faster.bpm() - 130) < 0.001 && faster.audible()[0] == a, "Slower source was not sped up to the fixed 130 BPM mix");

        auto patterned = testClip("pattern"); patterned->firstSourceBar = 8;
        for (size_t i = 0; i < patterned->audio.size(); ++i) patterned->audio[i] = {float(0.05 + int(double(i) / patterned->audio.size() * 32) * 0.002), 0};
        music::Mixer chops; chops.setSplit(false); std::vector<music::ClipPtr> one{patterned}; chops.next(one);
        auto arrangement = chops.bars(); check(arrangement[0].changed, "First sequence was left unchanged");
        for (int i = 0; i < phrase - 10; ++i) {
            double position = chops.beat(); int beat = int(position); auto out = chops.next(one);
            double fraction = position - beat;
            if (fraction > 0.2 && fraction < 0.8 && i > music::sampleRate) {
                const auto& bar = arrangement[beat / 4];
                int original = (bar.sourceBar - 9) * 4 + bar.beats[beat % 4];
                check(std::abs(out.l - float(0.05 + original * 0.002)) < 0.0001, "Displayed chop map differs from rendered samples");
            }
        }
        b->rejected = false;
        for (auto source : {a, b}) for (size_t i = 0; i < source->audio.size(); ++i) {
            double t = double(i) / music::sampleRate;
            float value = float(0.12 * std::sin(tau * (source == a ? 50 : 70) * t) + 0.08 * std::sin(tau * (source == a ? 600 : 1200) * t));
            source->audio[i] = {value, value};
        }
        music::prepareBands(*a); music::prepareBands(*b);
        music::Mixer split; split.next({a, b}); auto map = split.bars();
        check(map[0].split && map[0].clipId != map[0].lowClipId, "Split did not select distinct sources");
        auto sourceFor = [&](const std::string& id) { return id == a->id ? a : b; };
        for (int i = 0; i < phrase - 10; ++i) {
            double position = split.beat(); int beat = int(position); double fraction = position - beat;
            auto out = split.next({a, b});
            if (fraction > 0.2 && fraction < 0.8 && i > music::sampleRate) {
                auto info = map[beat / 4]; auto high = sourceFor(info.clipId), low = sourceFor(info.lowClipId);
                double hp = ((info.sourceBar - high->firstSourceBar - 1) * 4 + info.beats[beat % 4] + fraction) / 32;
                double lp = ((info.lowSourceBar - low->firstSourceBar - 1) * 4 + info.lowBeats[beat % 4] + fraction) / 32;
                auto interpolate = [](const std::vector<music::Frame>& audio, double phase) {
                    double position = phase * audio.size(); size_t i = size_t(position); float fraction = float(position - i);
                    return audio[i].l + (audio[(i + 1) % audio.size()].l - audio[i].l) * fraction;
                };
                float expected = (interpolate(high->high, hp) + interpolate(low->low, lp)) * 0.72f;
                check(std::abs(out.l - expected) < 0.0001, "Displayed bass/high provenance differs from rendered samples");
            }
        }

        auto dir = std::filesystem::temp_directory_path() / ("radiomusic-core-test-" + std::to_string(std::random_device{}()));
        check(music::saveClip(dir, *a), "Saving clip failed");
        auto restored = music::loadClip(dir / "test-a.wav"), metadata = music::loadClip(dir / "test-a.wav", true);
        check(restored && restored->key == 0 && restored->audio.size() == a->audio.size(), "Persistence round trip failed");
        check(metadata && metadata->audio.empty() && metadata->captureId == a->captureId, "Catalogue loaded PCM instead of metadata only");
        { std::ofstream legacy(dir / "test-a.txt"); legacy << "120 0.9 32\n\"legacy source\"\n"; }
        check(bool(music::loadClip(dir / "test-a.wav")), "Version-one recording failed to load");
        // Hard links exercise a larger catalogue without duplicating audio on disk.
        for (int i = 0; i < 80; ++i) {
            std::string id = "catalog-" + std::to_string(i);
            std::filesystem::create_hard_link(dir / "test-a.wav", dir / (id + ".wav"));
            std::ofstream meta(dir / (id + ".txt"));
            meta << "120 0.9 32\n\"test\"\nRM2 0 0.9 " << std::quoted(id) << " 0 0\n";
        }
        music::LibraryStore store(dir); check(store.scan() && store.size() == 81, "Catalogue still limited to 48 recordings");
        auto pool = store.workingSet(120, 0, {}, {});
        check(pool.size() == music::maxResidentClips, "Resident working set is not bounded");
        auto pinned = pool.front(); std::unordered_set<std::string> seen;
        for (const auto& c : pool) seen.insert(c->id);
        auto rotated = store.workingSet(120, 0, {pinned}, pool);
        check(rotated.size() == music::maxResidentClips && rotated.front() == pinned, "Playing source was not pinned through pool rotation");
        bool fresh = false; for (const auto& c : rotated) fresh = fresh || !seen.count(c->id);
        check(fresh, "Working set did not rotate across the larger library");
        check(store.reject(*pinned) && !music::loadClip(dir / (pinned->id + ".wav")), "Persistent rejection failed");
        auto siblingA = testClip("sibling-a"), siblingB = testClip("sibling-b");
        siblingA->captureId = siblingB->captureId = "shared-capture";
        check(store.add(*siblingA) && store.add(*siblingB), "Sibling fixture save failed");
        auto beforeBan = store.size();
        check(store.reject(*siblingA) && store.size() == beforeBan - 2, "Sibling loops survived capture rejection");
        check(!music::loadClip(dir / "sibling-b.wav"), "Rejected sibling audio remained available");
        music::LibraryStore restarted(dir);
        check(restarted.scan() && restarted.size() == store.size(), "Bans did not survive restart");
        siblingB->id = "renamed-sibling";
        check(!restarted.add(*siblingB), "Banned capture was re-added under a new loop ID");
        check(music::saveClip(dir, *siblingB) && restarted.scan() && restarted.size() == store.size(), "Restored banned capture reappeared in catalogue");
        auto corrupt = testClip("truncated"); check(music::saveClip(dir, *corrupt), "Fixture save failed");
        std::filesystem::resize_file(dir / "truncated.wav", 50);
        check(!music::loadClip(dir / "truncated.wav") && !music::loadClip(dir / "truncated.wav", true), "Truncated WAV accepted");
        pool.clear(); rotated.clear(); pinned.reset();
        // Only files in this test's newly created temporary directory; no recursive deletion.
        for (const auto& item : std::filesystem::directory_iterator(dir)) std::filesystem::remove(item.path());
        std::filesystem::remove(dir);
        std::cout << "Expanded catalogue, bounded rotating pool, provenance and persistence passed.\nAll core tests passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
