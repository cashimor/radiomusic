#include "music.hpp"
#include <algorithm>
#include <cmath>

namespace music {
ClipPtr Mixer::pick(const std::vector<ClipPtr>& library, bool anyTempo, const ClipPtr& avoid) {
    ClipPtr choice; int count = 0, bestRank = -1;
    for (const auto& c : library) {
        if (c->rejected || c == current_ || c == incoming_ || c == avoid || c->audio.empty()) continue;
        if (!anyTempo && !compatibleTempo(c->bpm, bpm_)) continue;
        int rank = !anyTempo && c->captureId != current_->captureId ? 2 : 0;
        if (avoid && c->captureId != avoid->captureId) rank += 2;
        if (rank < bestRank) continue;
        if (rank > bestRank) { bestRank = rank; count = 0; }
        if (std::uniform_int_distribution<int>(1, ++count)(random_) == 1) choice = c;
    }
    return choice;
}
Mixer::Arrangement Mixer::straight(const ClipPtr& clip) const {
    Arrangement result;
    for (int i = 0; i < 8; ++i) { result[i].clip = clip; result[i].index = i; }
    return result;
}
Mixer::Sequence Mixer::compose(const std::vector<ClipPtr>& library) {
    ++variations_;
    auto upper = pick(library, false);
    if (!upper) upper = current_;
    auto bass = pick(library, false, upper);
    if (!bass) bass = current_;
    Sequence result; result.upper = straight(upper); result.bass = straight(bass);
    result.split = split_ && !upper->high.empty() && !bass->low.empty();
    if (playful_) {
        constexpr int orders[4][8] = {{0, 2, 0, 3, 4, 6, 4, 7}, {2, 3, 0, 1, 6, 7, 4, 5},
                                      {0, 1, 4, 5, 2, 3, 6, 7}, {4, 5, 0, 1, 4, 6, 2, 7}};
        constexpr std::array<int, 4> chops[] = {{{0, 1, 0, 1}}, {{0, 2, 1, 3}}, {{2, 3, 0, 1}}, {{0, 1, 2, 2}}};
        int variation = int((variations_ - 1) % 4);
        // Pick a few change points for the eight-bar phrase. Fewer cuts are
        // common, while five cuts occasionally create short one-bar surprises.
        constexpr int changeWeights[] = {0, 28, 30, 23, 14, 5};
        int draw = std::uniform_int_distribution<int>(1, 100)(random_), changes = 1;
        for (int count = 1; count <= 5; ++count) {
            if (draw <= changeWeights[count]) { changes = count; break; }
            draw -= changeWeights[count];
        }
        std::array<int, 7> possibleCuts{1, 2, 3, 4, 5, 6, 7};
        std::shuffle(possibleCuts.begin(), possibleCuts.end(), random_);
        std::array<bool, 8> cut{};
        for (int i = 0; i < changes; ++i) cut[possibleCuts[i]] = true;

        // The alternate anchor is used throughout the phrase; a third source
        // appears occasionally when the resident pool has another match.
        std::array<ClipPtr, 3> sources{upper, current_, {}};
        int sourceCount = 2;
        if (std::uniform_int_distribution<int>(1, 4)(random_) == 1) {
            auto third = pick(library, false, upper);
            if (third && third != current_) { sources[2] = third; sourceCount = 3; }
        }
        int runSource = 0; // Keep the phrase's first bar on its main selected source.
        for (int i = 0; i < 8; ++i) {
            result.upper[i].index = orders[variation][i];
            result.upper[i].beats = chops[(i + variation) % 4];
            if (i > 0 && cut[i]) runSource = (runSource + 1) % sourceCount;
            auto source = sources[runSource];
            if (source && (!result.split || !source->high.empty())) result.upper[i].clip = source;
            result.bass[i].index = (i / 4) * 4 + (i % 2);
        }
    }
    return result;
}
bool Mixer::reverbSend() const {
    if (!current_) return false;
    // Feed the effect from halfway through the outgoing bar until halfway
    // through the incoming bar; the effect's delay lines keep the tail alive.
    const int bar = int(std::floor(phase_ * 8 + 7.5)) % 8;
    const int next = (bar + 1) % 8;
    auto changeScore = [](const Bar& a, const Bar& b) {
        if (!a.clip || !b.clip) return a.clip == b.clip ? 0 : 3;
        int score = 0;
        if (a.clip->captureId != b.clip->captureId) score += 3;
        else if (a.clip != b.clip) score += 2;
        if (a.index != b.index) ++score;
        if (a.beats != b.beats) ++score;
        return score;
    };
    auto bigChange = [&](const Sequence& sequence) {
        int score = changeScore(sequence.upper[bar], sequence.upper[next]);
        if (sequence.split) score += changeScore(sequence.bass[bar], sequence.bass[next]);
        return score >= 2;
    };
    return bigChange(sequence_) || (changing_ && bigChange(nextSequence_));
}
void Mixer::evolve(const std::vector<ClipPtr>& library) {
    nextSequence_ = compose(library); incoming_ = nextSequence_.upper[0].clip;
    changing_ = true; fade_ = 0; transitionCycles_ = 0; requested_ = false;
}
Frame Mixer::read(const ClipPtr& clip, double phase, int band) const {
    if (!clip || clip->audio.empty()) return {};
    const auto& audio = band < 0 ? clip->low : band > 0 ? clip->high : clip->audio;
    if (audio.empty()) return {};
    double pos = phase * audio.size();
    size_t i = std::min(static_cast<size_t>(pos), audio.size() - 1), j = (i + 1) % audio.size();
    float f = float(pos - i);
    float gain = band < 0 ? clip->lowGain : band > 0 ? clip->highGain : clip->playbackGain;
    return {(audio[i].l + (audio[j].l - audio[i].l) * f) * gain,
            (audio[i].r + (audio[j].r - audio[i].r) * f) * gain};
}
Frame Mixer::readArrangement(const Arrangement& plan, double phase, int band) const {
    double position = phase * loopBeats;
    int beat = std::min(31, int(position));
    const auto& slice = plan[beat / 4];
    if (!slice.clip) return {};
    double fraction = position - beat;
    int sourceBeat = slice.index * 4 + slice.beats[beat % 4];
    auto out = read(slice.clip, (sourceBeat + fraction) / loopBeats, band);
    auto continuous = [&](int other, int expected) {
        const auto& neighbor = plan[other / 4];
        return neighbor.clip == slice.clip && neighbor.index * 4 + neighbor.beats[other % 4] == expected;
    };
    double edge = 1, framesPerBeat = double(slice.clip->audio.size()) / loopBeats;
    if (!continuous((beat + 31) % 32, sourceBeat - 1)) edge = std::min(edge, fraction * framesPerBeat / 160);
    if (!continuous((beat + 1) % 32, sourceBeat + 1)) edge = std::min(edge, (1 - fraction) * framesPerBeat / 160);
    out.l *= float(edge); out.r *= float(edge); return out;
}
Frame Mixer::readSequence(const Sequence& sequence, double phase) const {
    auto out = readArrangement(sequence.upper, phase, sequence.split ? 1 : 0);
    if (sequence.split) {
        auto bass = readArrangement(sequence.bass, phase, -1);
        // Independent full recordings need headroom when their bands are combined.
        out.l = (out.l + bass.l) * 0.72f; out.r = (out.r + bass.r) * 0.72f;
    }
    return out;
}
Frame Mixer::next(const std::vector<ClipPtr>& library) {
    auto rejectedPlan = [](const Sequence& sequence) {
        for (const auto& b : sequence.upper) if (b.clip && b.clip->rejected) return true;
        if (sequence.split) for (const auto& b : sequence.bass) if (b.clip && b.clip->rejected) return true;
        return false;
    };
    if (rejectedPlan(sequence_) || (changing_ && rejectedPlan(nextSequence_))) {
        gain_ = std::max(0.0, gain_ - 1.0 / (sampleRate * 0.08));
        if (gain_ == 0) { current_.reset(); incoming_.reset(); changing_ = false; sequence_ = {}; nextSequence_ = {}; }
    } else gain_ = std::min(1.0, gain_ + 1.0 / (sampleRate * 0.1));
    if (!current_) {
        current_ = pick(library, true);
        if (!current_) { gain_ = 0; return {}; }
        bpm_ = current_->bpm; phase_ = 0; repeats_ = 0; gain_ = 0;
        sequence_ = compose(library); current_ = sequence_.upper[0].clip;
        // Setting controls before startup should not shorten the initial two-pass residence.
        requested_ = false;
    }
    auto out = readSequence(sequence_, phase_);
    if (changing_) {
        auto other = readSequence(nextSequence_, phase_);
        out.l = float(out.l * (1 - fade_) + other.l * fade_);
        out.r = float(out.r * (1 - fade_) + other.r * fade_);
    }
    double advance = bpm_ / (60.0 * sampleRate * loopBeats);
    phase_ += advance;
    if (changing_) fade_ = std::min(1.0, fade_ + advance / transitionPasses);
    if (phase_ >= 1) {
        phase_ -= 1;
        if (changing_) {
            // Count boundaries: fractional frame lengths must not add an extra cycle.
            if (++transitionCycles_ >= transitionPasses) {
                sequence_ = nextSequence_; current_ = sequence_.upper[0].clip;
                incoming_.reset(); nextSequence_ = {}; changing_ = false; fade_ = 0; repeats_ = 0;
            }
        } else {
            ++repeats_;
            if (repeatProtectionPending_) {
                repeatProtectionPending_ = false;
                repeatFinished_.store(true);
            }
            if (requested_ || repeats_ >= steadyPasses) evolve(library);
        }
    }
    auto limit = [&](float x) {
        double v = x * gain_, magnitude = std::abs(v);
        if (magnitude > 0.8) v = std::copysign(0.8 + 0.18 * std::tanh((magnitude - 0.8) / 0.18), v);
        return float(v);
    };
    return {limit(out.l), limit(out.r)};
}
std::vector<ClipPtr> Mixer::audible() const {
    std::vector<ClipPtr> result;
    auto add = [&](const ClipPtr& c) { if (c && std::find(result.begin(), result.end(), c) == result.end()) result.push_back(c); };
    int bar = std::min(7, int(phase_ * 8));
    if (current_) { add(sequence_.upper[bar].clip); if (sequence_.split) add(sequence_.bass[bar].clip); }
    if (changing_) { add(nextSequence_.upper[bar].clip); if (nextSequence_.split) add(nextSequence_.bass[bar].clip); }
    return result;
}
std::vector<ClipPtr> Mixer::sources() const {
    std::vector<ClipPtr> result;
    auto add = [&](const Arrangement& plan) {
        for (const auto& b : plan) if (b.clip && std::find(result.begin(), result.end(), b.clip) == result.end()) result.push_back(b.clip);
    };
    if (current_) { add(sequence_.upper); if (sequence_.split) add(sequence_.bass); }
    if (changing_) { add(nextSequence_.upper); if (nextSequence_.split) add(nextSequence_.bass); }
    return result;
}
std::array<BarInfo, 8> Mixer::bars(bool incoming) const {
    std::array<BarInfo, 8> result;
    if (!current_ || (incoming && !changing_)) return result;
    const auto& sequence = incoming ? nextSequence_ : sequence_;
    for (int i = 0; i < 8; ++i) if (sequence.upper[i].clip) {
        const auto& b = sequence.upper[i]; const auto& low = sequence.bass[i]; auto& info = result[i];
        info.captureId = b.clip->captureId.empty() ? b.clip->id : b.clip->captureId; info.clipId = b.clip->id;
        info.sourceBar = b.clip->firstSourceBar + b.index + 1; info.key = playbackKey(*b.clip, bpm_); info.beats = b.beats;
        info.changed = b.index != i || b.clip != sequence.upper[0].clip || b.beats != std::array<int, 4>{0, 1, 2, 3};
        info.split = sequence.split;
        if (info.split && low.clip) {
            info.lowCaptureId = low.clip->captureId.empty() ? low.clip->id : low.clip->captureId;
            info.lowClipId = low.clip->id; info.lowSourceBar = low.clip->firstSourceBar + low.index + 1; info.lowBeats = low.beats;
        }
    }
    return result;
}
}
