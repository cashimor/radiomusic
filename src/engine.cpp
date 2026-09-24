#include "engine.hpp"
#include "library.hpp"
#include <windows.h>
#include <winhttp.h>
#include <mmsystem.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
constexpr size_t captureFrames = music::sampleRate * 48;
std::string utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET value) : h(value) {}
    ~Internet() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};
// Stateful interpolation preserves fractional positions across MP3 frames.
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
}

Engine::Engine(std::filesystem::path library) : directory_(std::move(library)) {
    music::LibraryStore store(directory_);
    bool scanned = store.scan();
    state_.librarySize = store.size();
    state_.activity = scanned ? L"Saved library ready. Press Play to listen." : L"Cannot read the library folder.";
}
Engine::~Engine() { stop(); }
bool Engine::start(const std::wstring& url, bool mute) {
    if (running_) return false;
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto savedCount = state_.librarySize;
        state_ = {}; state_.librarySize = savedCount; state_.running = true; state_.connection = L"Connecting to the station...";
        live_.clear(); capture_.clear(); jobs_.clear(); preview_.clear(); audible_.clear(); library_.clear(); pinned_.clear();
        rejectedIds_.clear();
        rejectedCaptures_.clear();
        capture_.reserve(captureFrames); source_ = utf8(url);
    }
    evolve_ = false; running_ = true;
    analysis_ = std::thread(&Engine::analysisLoop, this);
    output_ = std::thread(&Engine::outputLoop, this, mute);
    network_ = std::thread(&Engine::networkLoop, this, url);
    return true;
}
void Engine::stop() {
    running_ = false; wake_.notify_all();
    if (network_.joinable()) network_.join();
    if (output_.joinable()) output_.join();
    if (analysis_.joinable()) analysis_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    state_.running = false; state_.mixing = false; state_.transitioning = false;
    audible_.clear(); state_.audible.clear();
}
void Engine::connection(std::wstring text) {
    std::lock_guard<std::mutex> lock(mutex_); state_.connection = std::move(text);
}
Snapshot Engine::snapshot() { std::lock_guard<std::mutex> lock(mutex_); auto s = state_; s.running = running_; return s; }
void Engine::evolve() {
    evolve_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    state_.waiting = true;
    state_.activity = L"Evolution requested: waiting for a phrase boundary and a compatible loop.";
}
void Engine::reject() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (radio_ || audible_.empty()) { state_.activity = L"No sampled loop is audible yet. Rejection applies to the remix."; return; }
    for (auto& c : audible_) {
        rejectedCaptures_.insert(c->captureId);
        if (!c->rejected.exchange(true)) { erase_.push_back(c); rejectedIds_.insert(c->id); }
    }
    for (auto& c : library_) if (rejectedCaptures_.count(c->captureId)) c->rejected = true;
    for (auto& c : pinned_) if (rejectedCaptures_.count(c->captureId)) c->rejected = true;
    library_.erase(std::remove_if(library_.begin(), library_.end(), [](const auto& c) { return c->rejected.load(); }), library_.end());
    state_.residentClips = library_.size();
    state_.activity = L"Banning audible source captures, including all their bars and beats, from the saved library.";
    wake_.notify_one();
}
bool Engine::savePreview(const std::filesystem::path& path) {
    std::lock_guard<std::mutex> lock(mutex_); return music::writeWave(path, preview_);
}
void Engine::receive(const music::Frame* frames, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.decodedFrames += count;
    for (size_t i = 0; i < count; ++i) {
        live_.push_back(frames[i]);
        if (live_.size() > music::sampleRate * 5) live_.pop_front();
        capture_.push_back(frames[i]);
        if (capture_.size() == captureFrames) {
            if (jobs_.size() < 2) { jobs_.push_back(std::move(capture_)); wake_.notify_one(); }
            capture_.clear(); capture_.reserve(captureFrames);
        }
    }
    state_.captureSeconds = double(capture_.size()) / music::sampleRate;
}
void Engine::networkLoop(std::wstring url) {
    while (running_) {
        // Discard partial analysis windows after interruptions: a gap is not a continuous beat grid.
        { std::lock_guard<std::mutex> lock(mutex_); capture_.clear(); state_.captureSeconds = 0; }
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
            connection(L"Use a direct HTTP or HTTPS MP3 stream URL."); return;
        }
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        Internet session(WinHttpOpen(L"Radiomusic/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.h) { connection(L"Cannot initialize Windows networking."); return; }
        WinHttpSetTimeouts(session, 3000, 5000, 3000, 3000);
        Internet connectionHandle(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
        Internet request(WinHttpOpenRequest(connectionHandle, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        bool good = request.h && WinHttpSendRequest(request, L"Icy-MetaData: 0\r\n", DWORD(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
        DWORD code = 0, size = sizeof(code);
        if (good) good = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                             nullptr, &code, &size, nullptr) && code == 200;
        wchar_t type[256]{}; size = sizeof(type);
        if (good && WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, nullptr, type, &size, nullptr)) {
            std::wstring contentType(type);
            if (contentType.find(L"text/") != std::wstring::npos || contentType.find(L"mpegurl") != std::wstring::npos
                || contentType.find(L"aac") != std::wstring::npos) {
                connection(L"This prototype needs a direct MP3 stream (AAC/HLS/playlists are not supported)."); return;
            }
        }
        DWORD metadataInterval = 0;
        wchar_t metadata[64]{}; size = sizeof(metadata);
        if (good && WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"icy-metaint", metadata, &size, nullptr))
            metadataInterval = wcstoul(metadata, nullptr, 10);
        DWORD untilMetadata = metadataInterval, skipMetadata = 0;
        mp3dec_t decoder{}; mp3dec_init(&decoder);
        Resampler resampler;
        std::vector<uint8_t> buffer; buffer.reserve(65536);
        std::array<uint8_t, 8192> bytes{};
        std::array<short, MINIMP3_MAX_SAMPLES_PER_FRAME> pcm{};
        bool decoded = false;
        auto lastDecoded = std::chrono::steady_clock::now();
        if (good) connection(L"Connected. Buffering MP3 audio...");
        while (good && running_) {
            DWORD received = 0;
            if (!WinHttpReadData(request, bytes.data(), DWORD(bytes.size()), &received) || !received) break;
            for (DWORD i = 0; i < received; ++i) {
                if (metadataInterval) {
                    if (skipMetadata) { --skipMetadata; continue; }
                    if (!untilMetadata) { skipMetadata = bytes[i] * 16; untilMetadata = metadataInterval; continue; }
                    --untilMetadata;
                }
                buffer.push_back(bytes[i]);
            }
            size_t used = 0;
            // Keep ample lookahead so initial synchronization cannot mistake payload bytes for headers.
            while (buffer.size() - used >= 16384 && running_) {
                mp3dec_frame_info_t info{};
                int samples = mp3dec_decode_frame(&decoder, buffer.data() + used,
                    static_cast<int>(buffer.size() - used), pcm.data(), &info);
                if (info.frame_bytes <= 0) break;
                used += info.frame_bytes;
                if (samples && info.hz >= 8000 && info.hz <= 48000 && (info.channels == 1 || info.channels == 2)) {
                    auto frames = resampler.convert(pcm.data(), samples, info.channels, info.hz);
                    receive(frames.data(), frames.size()); lastDecoded = std::chrono::steady_clock::now();
                    if (!decoded) {
                        connection(L"Live MP3 stream  /  " + std::to_wstring(info.hz) + L" Hz  /  " + std::to_wstring(info.bitrate_kbps) + L" kbps");
                        decoded = true;
                    }
                }
            }
            buffer.erase(buffer.begin(), buffer.begin() + used);
            if (buffer.size() > 1024 * 1024 || std::chrono::steady_clock::now() - lastDecoded > std::chrono::seconds(12)) {
                connection(L"No decodable MP3 audio. Check that this is a direct MP3 station URL."); return;
            }
        }
        if (!running_) break;
        connection(L"Stream interrupted (HTTP " + std::to_wstring(code) + L"). Reconnecting; saved loops keep playing.");
        for (int i = 0; i < 30 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Engine::outputLoop(bool mute) {
    constexpr size_t blockFrames = 1024, blockCount = 4;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX format{}; format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2;
    format.nSamplesPerSec = music::sampleRate; format.wBitsPerSample = 16;
    format.nBlockAlign = 4; format.nAvgBytesPerSec = music::sampleRate * 4;
    HWAVEOUT device = nullptr;
    if (!event || waveOutOpen(&device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        if (event) CloseHandle(event);
        connection(L"Could not open the default audio output. Check your sound device.");
        running_ = false; wake_.notify_all(); return;
    }
    std::array<std::array<int16_t, blockFrames * 2>, blockCount> pcm{};
    std::array<WAVEHDR, blockCount> headers{};
    std::array<bool, blockCount> queued{};
    for (size_t i = 0; i < blockCount; ++i) {
        headers[i].lpData = reinterpret_cast<char*>(pcm[i].data()); headers[i].dwBufferLength = blockFrames * 4;
        if (waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) running_ = false;
    }
    music::Mixer mixer;
    double mixBlend = 0, volume = 0, liveGain = 0;
    bool liveStarted = false;
    music::Frame lastLive{};
    while (running_) {
        for (size_t b = 0; b < blockCount && running_; ++b) {
            if (queued[b] && !(headers[b].dwFlags & WHDR_DONE)) continue;
            std::vector<music::ClipPtr> library;
            std::array<music::Frame, blockFrames> live{};
            size_t liveCount = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_); library = library_;
                if (!liveStarted && live_.size() >= music::sampleRate) liveStarted = true;
                if (liveStarted) {
                    while (liveCount < blockFrames && !live_.empty()) { live[liveCount++] = live_.front(); live_.pop_front(); }
                    if (liveCount < blockFrames) { ++state_.liveUnderruns; liveStarted = false; }
                }
            }
            if (evolve_.exchange(false)) mixer.requestEvolution();
            mixer.setPlayful(playful_);
            mixer.setSplit(split_);
            double peak = 0;
            std::array<music::Frame, blockFrames> preview{};
            bool previewReady = false;
            for (size_t i = 0; i < blockFrames; ++i) {
                auto loop = mixer.next(library);
                bool wantMix = mixer.ready() && !radio_;
                double fadeStep = mixer.bpm() > 0 ? mixer.bpm() / (60.0 * music::sampleRate * 16) : 1.0 / music::sampleRate;
                mixBlend = std::clamp(mixBlend + (wantMix ? fadeStep : -1.0 / (music::sampleRate * 0.15)), 0.0, 1.0);
                bool haveLive = i < liveCount;
                liveGain = std::clamp(liveGain + (haveLive ? 0.001 : -0.001), 0.0, 1.0);
                if (haveLive) lastLive = live[i];
                // Smooth master gain, including slider changes, prevents discontinuities.
                volume += (double(volume_.load()) - volume) * 0.002;
                music::Frame f{float((lastLive.l * liveGain * (1 - mixBlend) * 0.6 + loop.l * mixBlend) * volume),
                               float((lastLive.r * liveGain * (1 - mixBlend) * 0.6 + loop.r * mixBlend) * volume)};
                peak = std::max({peak, double(std::abs(f.l)), double(std::abs(f.r))});
                for (int ch = 0; ch < 2; ++ch) {
                    float x = ch ? f.r : f.l;
                    pcm[b][i * 2 + ch] = mute ? 0 : static_cast<int16_t>(std::clamp(x, -0.98f, 0.98f) * 32767);
                }
                preview[i] = f;
                if (mixBlend >= 0.999) previewReady = true;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                audible_ = mixer.audible(); state_.audible.clear();
                pinned_ = mixer.sources();
                for (auto& clip : audible_) state_.audible.push_back(clip->id);
                state_.mixing = mixer.ready(); state_.transitioning = mixer.transitioning();
                if (state_.transitioning) state_.waiting = false;
                state_.bpm = mixer.bpm(); state_.beat = mixer.beat(); state_.peak = std::max(state_.peak, peak);
                state_.bars = mixer.bars(); state_.incomingBars = mixer.bars(true);
                state_.key = mixer.key(); state_.fade = mixer.fade();
                state_.passes = mixer.passes();
                state_.renderedFrames += blockFrames;
                if (previewReady && preview_.size() < music::sampleRate * 20)
                    preview_.insert(preview_.end(), preview.begin(), preview.end());
            }
            if (waveOutWrite(device, &headers[b], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                connection(L"Audio output stopped. Check the sound device, then stop and start again.");
                running_ = false; wake_.notify_all(); break;
            }
            queued[b] = true;
        }
        WaitForSingleObject(event, 20);
    }
    waveOutReset(device);
    for (auto& h : headers) if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(device, &h, sizeof(h));
    waveOutClose(device); CloseHandle(event);
}
