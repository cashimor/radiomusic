#include <aaudio/AAudio.h>
#include <jni.h>
#include <android/log.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#include "library.hpp"
#include "music.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr char logTag[] = "RadioMusicCapture";
constexpr size_t captureFrames = size_t(music::sampleRate) * 48;
constexpr size_t radioBufferFrames = size_t(music::sampleRate) * 5;

class Resampler {
public:
    std::vector<music::Frame> convert(const short* pcm, int count, int channels, int rate) {
        if (rate_ != rate) { pending_.clear(); position_ = 0; rate_ = rate; }
        for (int i = 0; i < count; ++i)
            pending_.push_back({pcm[i * channels] / 32768.0f, pcm[i * channels + (channels == 2 ? 1 : 0)] / 32768.0f});
        std::vector<music::Frame> out;
        out.reserve(count * 2);
        double step = double(rate) / music::sampleRate;
        while (position_ + 1 < pending_.size()) {
            size_t i = static_cast<size_t>(position_);
            float frac = float(position_ - i);
            auto a = pending_[i], b = pending_[i + 1];
            out.push_back({a.l + (b.l - a.l) * frac, a.r + (b.r - a.r) * frac});
            position_ += step;
        }
        size_t consumed = std::min(static_cast<size_t>(position_), pending_.size());
        pending_.erase(pending_.begin(), pending_.begin() + consumed);
        position_ -= consumed;
        return out;
    }
private:
    int rate_ = 0;
    double position_ = 0;
    std::vector<music::Frame> pending_;
};

class Player {
public:
    explicit Player(std::filesystem::path directory) : directory_(std::move(directory)) {}
    ~Player() { stop(); }

    std::string start() {
        if (stream_) return {};
        store_ = std::make_unique<music::LibraryStore>(directory_);
        if (!store_->scan()) { store_.reset(); return "Cannot read the saved loop folder."; }
        auto initial = store_->workingSet(0, {}, {});
        if (initial.empty()) { store_.reset(); return "No playable saved loops in Documents/Radiomusic/loops."; }
        std::atomic_store(&clips_, std::make_shared<const std::vector<music::ClipPtr>>(std::move(initial)));
        mixer_ = std::make_unique<music::Mixer>();
        failed_ = false; mixBlend_ = 0; liveGain_ = 0;
        radioCapture_.clear(); radioCapture_.reserve(captureFrames);
        mp3dec_init(&decoder_); mp3Buffer_.clear(); resampler_ = Resampler{};

        AAudioStreamBuilder* builder = nullptr;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) return "Could not create Android audio output.";
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, music::sampleRate);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(builder, &Player::audioCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, &Player::errorCallback, this);
        result = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        if (result != AAUDIO_OK) {
            stream_ = nullptr;
            store_.reset();
            std::atomic_store(&clips_, std::shared_ptr<const std::vector<music::ClipPtr>>{});
            return std::string("Could not open Android audio output: ") + AAudio_convertResultToText(result);
        }
        result = AAudioStream_requestStart(stream_);
        if (result != AAUDIO_OK) {
            AAudioStream_close(stream_); stream_ = nullptr;
            mixer_.reset(); store_.reset();
            std::atomic_store(&clips_, std::shared_ptr<const std::vector<music::ClipPtr>>{});
            return std::string("Could not start Android audio: ") + AAudio_convertResultToText(result);
        }
        startLibraryRefresh();
        return {};
    }

    void pause() {
        if (stream_) AAudioStream_requestPause(stream_);
    }
    void resume() {
        if (stream_) AAudioStream_requestStart(stream_);
    }
    void hearRadio(bool enabled) { hearRadio_ = enabled; }
    void nextSequence() { nextRequested_.store(true); }
    void repeatCurrent() { repeatRequested_.store(true); }
    void resetRadioInput() {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        mp3dec_init(&decoder_); mp3Buffer_.clear(); resampler_ = Resampler{};
        radioCapture_.clear();
    }

    void feedMp3(const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(decoderMutex_);
        mp3Buffer_.insert(mp3Buffer_.end(), data, data + length);
        std::array<short, MINIMP3_MAX_SAMPLES_PER_FRAME> pcm{};
        size_t used = 0;
        while (mp3Buffer_.size() - used >= 16384) {
            mp3dec_frame_info_t info{};
            int samples = mp3dec_decode_frame(&decoder_, mp3Buffer_.data() + used,
                static_cast<int>(mp3Buffer_.size() - used), pcm.data(), &info);
            if (info.frame_bytes <= 0) break;
            used += info.frame_bytes;
            if (samples > 0 && info.hz >= 8000 && info.hz <= 48000 && (info.channels == 1 || info.channels == 2)) {
                if (!decodedMp3_.exchange(true)) __android_log_print(ANDROID_LOG_INFO, logTag, "Decoded MP3: %d Hz, %d channels, %d kbps", info.hz, info.channels, info.bitrate_kbps);
                auto frames = resampler_.convert(pcm.data(), samples, info.channels, info.hz);
                for (const auto& frame : frames) {
                    pushRadio(frame);
                    radioCapture_.push_back(frame);
                    if (radioCapture_.size() == captureFrames) {
                        std::lock_guard<std::mutex> jobsLock(jobsMutex_);
                        if (captureJobs_.size() < 2) {
                            captureJobs_.push_back(std::move(radioCapture_));
                            __android_log_print(ANDROID_LOG_INFO, logTag, "Queued a 48-second radio capture (%zu frames)", captureFrames);
                        }
                        radioCapture_.clear(); radioCapture_.reserve(captureFrames);
                        refreshWake_.notify_one();
                    }
                }
            }
        }
        if (used) mp3Buffer_.erase(mp3Buffer_.begin(), mp3Buffer_.begin() + used);
        if (mp3Buffer_.size() > 1024 * 1024) mp3Buffer_.erase(mp3Buffer_.begin(), mp3Buffer_.end() - 16384);
    }

    void stop() {
        refreshEnabled_ = false;
        refreshWake_.notify_all();
        if (refreshThread_.joinable()) refreshThread_.join();
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
        mixer_.reset();
        std::atomic_store(&clips_, std::shared_ptr<const std::vector<music::ClipPtr>>{});
        store_.reset();
        { std::lock_guard<std::mutex> lock(jobsMutex_); captureJobs_.clear(); }
        radioRead_.store(0); radioWrite_.store(0); hearRadio_ = false;
    }

private:
    void startLibraryRefresh() {
        refreshEnabled_ = true;
        refreshThread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(refreshMutex_);
            auto nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (refreshEnabled_) {
                lock.unlock();
                std::vector<music::Frame> capture;
                {
                    std::lock_guard<std::mutex> jobsLock(jobsMutex_);
                    if (!captureJobs_.empty()) { capture = std::move(captureJobs_.front()); captureJobs_.pop_front(); }
                }
                store_->setProtectedCaptures(protectedCapturesSnapshot());
                if (!capture.empty()) {
                    auto analysis = music::analyze(capture);
                    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    auto extracted = music::extract(capture, analysis, std::to_string(now), "Technolovers Trance");
                    size_t saved = 0;
                    for (const auto& clip : extracted) if (store_->add(*clip)) ++saved;
                    __android_log_print(ANDROID_LOG_INFO, logTag, "Analyzed radio capture: %.2f BPM, confidence %.3f, extracted %zu loops, saved %zu", analysis.bpm, analysis.confidence, extracted.size(), saved);
                    auto updated = store_->workingSet(0, {}, {});
                    if (!updated.empty()) std::atomic_store(&clips_, std::make_shared<const std::vector<music::ClipPtr>>(std::move(updated)));
                }
                store_->setProtectedCaptures(protectedCapturesSnapshot());
                if (std::chrono::steady_clock::now() >= nextScan && store_->scan()) {
                    auto updated = store_->workingSet(0, {}, {});
                    if (!updated.empty()) std::atomic_store(&clips_, std::make_shared<const std::vector<music::ClipPtr>>(std::move(updated)));
                    nextScan = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                }
                lock.lock();
                refreshWake_.wait_until(lock, nextScan);
            }
        });
    }

    void pushRadio(const music::Frame& frame) {
        auto write = radioWrite_.load(std::memory_order_relaxed);
        auto read = radioRead_.load(std::memory_order_acquire);
        if (write - read >= radioBufferFrames) return;
        radioRing_[write % radioBufferFrames] = frame;
        radioWrite_.store(write + 1, std::memory_order_release);
    }

    static aaudio_data_callback_result_t audioCallback(AAudioStream*, void* user, void* audio, int32_t frames) {
        return static_cast<Player*>(user)->render(static_cast<float*>(audio), frames);
    }
    static void errorCallback(AAudioStream*, void* user, aaudio_result_t) {
        static_cast<Player*>(user)->failed_ = true;
    }
    aaudio_data_callback_result_t render(float* output, int32_t frames) {
        auto library = std::atomic_load(&clips_);
        if (failed_ || !mixer_ || !library || library->empty()) {
            std::fill(output, output + size_t(frames) * 2, 0.0f);
            return failed_ ? AAUDIO_CALLBACK_RESULT_STOP : AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        auto read = radioRead_.load(std::memory_order_relaxed);
        auto write = radioWrite_.load(std::memory_order_acquire);
        for (int32_t i = 0; i < frames; ++i) {
            if (nextRequested_.exchange(false)) mixer_->requestEvolution();
            if (repeatRequested_.exchange(false)) {
                mixer_->repeatCurrent();
                std::unordered_set<std::string> protectedCaptures;
                for (const auto& clip : mixer_->sources())
                    if (clip && !clip->captureId.empty()) protectedCaptures.insert(clip->captureId);
                {
                    std::lock_guard<std::mutex> lock(protectionMutex_);
                    protectedCaptures_ = std::move(protectedCaptures);
                }
                refreshWake_.notify_one();
            }
            auto frame = mixer_->next(*library);
            if (mixer_->takeRepeatFinished()) {
                { std::lock_guard<std::mutex> lock(protectionMutex_); protectedCaptures_.clear(); }
                refreshWake_.notify_one();
            }
            bool haveRadio = read < write;
            if (haveRadio) lastRadio_ = radioRing_[read++ % radioBufferFrames];
            double targetMix = mixer_->ready() && !hearRadio_ ? 1.0 : 0.0;
            double fadeStep = mixer_->bpm() > 0 ? mixer_->bpm() / (60.0 * music::sampleRate * 16) : 1.0 / music::sampleRate;
            mixBlend_ = std::clamp(mixBlend_ + (targetMix > mixBlend_ ? fadeStep : -fadeStep), 0.0, 1.0);
            liveGain_ = std::clamp(liveGain_ + ((haveRadio ? 1.0 : 0.0) - liveGain_) * 0.001, 0.0, 1.0);
            output[i * 2] = float(lastRadio_.l * liveGain_ * (1 - mixBlend_) * 0.6 + frame.l * mixBlend_);
            output[i * 2 + 1] = float(lastRadio_.r * liveGain_ * (1 - mixBlend_) * 0.6 + frame.r * mixBlend_);
        }
        radioRead_.store(read, std::memory_order_release);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    std::unordered_set<std::string> protectedCapturesSnapshot() {
        std::lock_guard<std::mutex> lock(protectionMutex_);
        return protectedCaptures_;
    }

    std::filesystem::path directory_;
    std::unique_ptr<music::LibraryStore> store_;
    std::shared_ptr<const std::vector<music::ClipPtr>> clips_;
    std::unique_ptr<music::Mixer> mixer_;
    AAudioStream* stream_ = nullptr;
    std::atomic<bool> failed_{false};
    std::atomic<bool> nextRequested_{false}, repeatRequested_{false};
    std::atomic<bool> refreshEnabled_{false};
    std::mutex refreshMutex_;
    std::condition_variable refreshWake_;
    std::thread refreshThread_;
    std::mutex protectionMutex_;
    std::unordered_set<std::string> protectedCaptures_;
    mp3dec_t decoder_{};
    Resampler resampler_;
    std::vector<uint8_t> mp3Buffer_;
    std::mutex decoderMutex_;
    std::array<music::Frame, radioBufferFrames> radioRing_{};
    std::atomic<uint64_t> radioWrite_{0}, radioRead_{0};
    std::atomic<bool> hearRadio_{false};
    std::atomic<bool> decodedMp3_{false};
    music::Frame lastRadio_{};
    double mixBlend_ = 0, liveGain_ = 0;
    std::vector<music::Frame> radioCapture_;
    std::deque<std::vector<music::Frame>> captureJobs_;
    std::mutex jobsMutex_;
};
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeCreate(JNIEnv* env, jclass, jstring directory) {
    const char* path = env->GetStringUTFChars(directory, nullptr);
    auto* player = new Player(std::filesystem::path(path));
    env->ReleaseStringUTFChars(directory, path);
    return reinterpret_cast<jlong>(player);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeStart(JNIEnv* env, jclass, jlong handle) {
    auto* player = reinterpret_cast<Player*>(handle);
    auto message = player->start();
    return env->NewStringUTF(message.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativePause(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<Player*>(handle)->pause();
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeResume(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<Player*>(handle)->resume();
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeNextSequence(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<Player*>(handle)->nextSequence();
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeRepeatCurrent(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<Player*>(handle)->repeatCurrent();
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeStop(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<Player*>(handle)->stop();
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<Player*>(handle);
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeFeedMp3(JNIEnv* env, jclass, jlong handle, jbyteArray data) {
    auto* player = reinterpret_cast<Player*>(handle);
    jsize length = env->GetArrayLength(data);
    jbyte* bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return;
    player->feedMp3(reinterpret_cast<const uint8_t*>(bytes), static_cast<size_t>(length));
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeHearRadio(JNIEnv*, jclass, jlong handle, jboolean enabled) {
    reinterpret_cast<Player*>(handle)->hearRadio(enabled == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_cashimor_radiomusic_PlaybackService_nativeResetRadioInput(JNIEnv*, jclass, jlong handle) {
    reinterpret_cast<Player*>(handle)->resetRadioInput();
}
