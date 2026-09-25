# Radiomusic: desktop handoff and Android plan

Prepared 24 September 2026; updated 25 September 2026. This document carries the relevant laptop conversation to a desktop session. The Windows app remains intact. The Android build now plays/remixes saved loops in the background and captures radio over Wi-Fi.

## Purpose and user preferences

Radiomusic listens to an Internet radio station, saves beat-aligned material, and builds an evolving remix from previously captured recordings. Prioritize good audio and longer repetitive musical phrases, with a simple interface. The default station is https://0nlineradio.radioho.st/technolovers-trance (direct MP3 stream).

The user wants to explore an Android version using their desktop for development rather than their laptop. Their phone is described as a Motorola G 2024, with an SD card installed. We provisionally identified it as the Moto G 5G (2024); verify the exact model and Android version before choosing device requirements. That model has a Snapdragon 4 Gen 1, 4 GB physical RAM, and microSD support up to 1 TB.

Keep C++ for the music engine. A distant possibility is an RP2350 radio project, but the current desktop/mobile versions may use more resources. Do not constrain Android to microcontroller limits.

## Current Windows implementation

- C++17, CMake, native Win32 UI, WinHTTP streaming, waveOut playback, vendored minimp3 decoder. No large DSP framework.
- Capture windows are 48 seconds. Saved loops contain eight bars / 32 estimated beats. Source WAVs are 44.1 kHz, 16-bit stereo with companion text metadata.
- Beat cuts/repeats, bar reordering, and source substitutions modify playback. Recent changes introduced randomized source-run lengths and occasional third-source substitutions.
- Fourth-order Linkwitz–Riley crossover at 220 Hz lets bass and upper frequencies come from different recordings. Cached filter bands, small fades at cuts, and output limiting support smooth playback.
- Each sequence plays two steady passes, followed by ONE complete eight-bar crossfade pass. Evolve requests a change at the next phrase boundary. Crossfade completion counts phrase boundaries, avoiding extra cycles at fractional sample lengths.
- Incoming sources match the fixed mix tempo by playback-speed adjustment; allowed speed ratio is 0.90–1.10. This covers 130–140 BPM in both directions. This is not pitch-preserving time stretching.
- IMPORTANT: current code has key estimation/display helpers, but current source selection no longer gates on compatible keys. Earlier conversation and portions of README describe key filtering; those are stale relative to the source. Preserve the current behavior unless the user requests otherwise.
- Optional reverb feeds from halfway through the measure BEFORE a significant change until halfway through the following measure. The reverb tail keeps decaying after the send closes. This is the latest requested sound change; tests and build passed afterward.
- Colored bar provenance shows source capture, original bar, high/low sources, and beat order. Clicking a bar opens details. There is also a count of distinct loops used during the session.
- Removed feature: isolated-effect capture and the old Play effect button. Do not reintroduce it by confusing it with the newer reverb feature.

## Latest audio updates

- Loop normalization uses fixed stereo-linked RMS gain, including independent bass and upper-band gains, boost/peak limits, and a near-silence guard. Existing WAVs are unchanged; gains are prepared when loaded.
- Hear: both / upper only / bass only cycles a final-output monitor at 220 Hz, including reverb and live radio. Both bypasses the extra monitor filter; mode changes are smoothed.
- The crossover remains fourth-order Linkwitz–Riley (24 dB/octave). Equal-gain aligned bands sum flat; unrelated sources can dip near the boundary. No corrective EQ boost was added.

## Library and rejection semantics

- Maximum 1,024 saved recordings, metadata catalogue, up to 16 resident audio sources rotated approximately every 20 seconds. Referenced sources remain pinned during playback/transitions.
- Saved-loop count is scanned when the window opens, before Start is pressed.
- Ban sources / minus bans captures represented by sources under the playhead, including bass/upper and both sides of an active transition. All loops from each implicated 48-second capture are removed, preventing their measures/beats from reappearing in other arrangements.
- Bans persist via rejected-captures.txt and individual .rejected markers. Keep these with backups! Tests cover sibling removal, restart persistence, re-add prevention and restored-file exclusion.
- Banning is provenance-based, not acoustic fingerprinting: the same music broadcast later could be captured under a new identity.
- Full library is approximately 2.5–2.7 GB at 130–140 BPM. Resident original audio plus two floating-point frequency bands can use several hundred MB, particularly during refresh. Phone memory tuning is required.
- Arrangements are not persisted; recordings, metadata, and bans are.

## Android milestone (radio capture implemented and device-verified)

1. The `android/` project builds a debug APK with a Java touch UI, JNI bridge and the existing C++ music core. Android audio uses NDK AAudio.
2. The app reads WAV loops and their metadata from the fixed internal path `/storage/emulated/0/Documents/Radiomusic/loops`. During playback, Android rescans that directory every 20 seconds and replaces its candidate working set with up to 16 resident loops, so recordings added to the directory become eligible without a UI refresh. The library catalogue itself is metadata-only. It currently relies on legacy external-storage access for this private, personal install. Android scoped-storage changes may require moving back to a Storage Access Framework folder grant and importing files to private app storage on newer target SDKs.
3. Controls include play/pause, start/stop radio capture, and live-radio/remix monitoring. A foreground service owns playback and the media session; watch, lock-screen, notification and headset transport controls use Android's system media controls.
4. Radio capture requests a validated Wi-Fi network and opens the station URL on that specific Android `Network`, preventing the station connection from falling back to cellular. Java streams the MP3 bytes to native minimp3 decoding. The native path resamples to 44.1 kHz, plays the live station, analyzes 48-second windows with the shared C++ analyzer and saves extracted WAV/metadata pairs into the loop folder. Reconnect resets the partial decoder/capture window; reconnect attempts remain Wi-Fi-only.
5. The audio renderer mixes live radio with the evolving loop mix. “Hear live radio” forces live monitoring; “Return to remix” fades toward the remix while capture continues. As newly captured loops are saved, the existing 20-second rescan rotates the resident candidate pool.
6. Device verification on 25 September: the station returned HTTP 200 `audio/mpeg` at 192 kbps. Native decoding ran at 44.1 kHz stereo; a capture analyzed at 136 BPM / 0.881 confidence and extracted/saved three loops. Nine new WAV loops appeared on the device during verification. Storage read/write permission is needed because this personal build targets API 28 and writes into shared Documents. Keep disk/network work outside the AAudio callback.
7. Next development steps: listen with the screen off, check reconnect and headset/watch behavior over longer sessions, and profile memory/CPU/battery during analysis and library rotation. Refine status reporting and test adding loops while the remix is already running. Radio capture still requires Wi-Fi; it does not use cellular as a fallback.

## Tooling and planning estimates

Development tools belong on the desktop, not the phone. This machine now has Microsoft OpenJDK 21, Gradle 8.11.1, Android SDK API 36, Build Tools 36.0.0, NDK 30.0.16248370, CMake 3.22.1 and platform-tools installed under `%LOCALAPPDATA%`. Google SDK package licenses were accepted at the user's direction. Testing with the actual phone over USB is preferred initially; the emulator remains optional.

Approximate downloads discussed: Android Studio 1.5 GB; Android SDK/build tools 0.5–1.5 GB; Windows NDK about 730 MB; CMake/Gradle/project dependencies 0.3–1 GB. Allow roughly 3–5 GB downloads and 15–25 GB disk space without an emulator. An emulator/system image adds roughly 2–4 GB downloads. These are planning estimates, not pinned package requirements; verify current versions/sizes at installation time.

Official references consulted:
- https://developer.android.com/studio
- https://developer.android.com/ndk/downloads
- https://developer.android.com/studio/projects/install-ndk
- https://developer.android.com/games/sdk/oboe
- https://developer.android.com/media/media3/session/background-playback
- https://developer.android.com/develop/connectivity/network-ops/reading-network-state
- https://developer.android.com/reference/android/net/Network#openConnection(java.net.URL)
- https://developer.android.com/training/data-storage/shared/documents-files
- https://en-us.support.motorola.com/app/answers/detail/a_id/179364/reg/749642

## Source map and Windows checks

- CMakeLists.txt: music_core shared code, Windows executable, Android native library and core_tests.
- src/music.hpp and music.cpp: shared types, beat analysis, extraction, WAV/metadata persistence.
- src/features.cpp: key estimation and related helpers.
- src/crossover.cpp: cached bass/high bands.
- src/mixer.cpp: arrangement selection, playback, tempo matching, reverb-send timing, provenance.
- src/library.cpp/.hpp: catalogue, resident selection, persistent bans.
- src/engine.cpp/.hpp: Windows networking/audio, engine state and reverb implementation.
- src/analysis_worker.cpp: capture analysis and storage orchestration.
- src/main.cpp: Windows interface and diagnostic modes.
- android/: Java activity, Wi-Fi-bound station downloader, foreground media service and notification, JNI/AAudio/MP3 player and capture analyzer, Gradle wrapper and Android packaging.
- tests/core_tests.cpp: audio and persistence regression tests.
- vendor/: minimp3 and license/revision information.

Windows build: Visual Studio 2022 Build Tools with Desktop development with C++ and CMake; run build.ps1. It configures/builds Release and runs CTest. Output: build/Release/Radiomusic.exe. Start Radiomusic.cmd launches it.

Windows check on 25 September 2026: Release build and core test passed. Android debug APK built and installed on the Moto G Power 5G (2024), model `moto_g_power_5G___2024`. Device log review previously fixed JNI exports naming the old Activity class and missing storage access. Radio capture now connects only on Wi-Fi, decodes the default station MP3, and saves extracted loops. The phone's loop directory grew from 16 to 25 WAVs during testing. Final radio-capture APK: `android/app/build/outputs/apk/debug/app-debug.apk`. Longer screen-off, audio-quality and battery verification remains.

The Windows process once remained alive after its window closed, blocking relinking. Close gracefully before rebuilding; investigate a remaining process if necessary. In the laptop agent sandbox, MSBuild needed an elevated/approved tool execution because duplicated PATH entries broke the sandboxed build environment; this may not apply on the desktop.

## Moving to the desktop

The accompanying Radiomusic-desktop-handoff.zip contains source, tests, vendor files, build instructions, and this document. It excludes generated builds, test output, old binary archives, and the personal audio library. Extract into a new desktop project folder; do not reuse the laptop's CMake build directory.

Optionally copy the entire library/ folder separately, including WAVs, text metadata, rejection markers and rejected-captures.txt. Copy while Radiomusic is stopped for a consistent snapshot. Existing binaries in older portable archives may be stale; build from the supplied source.

Build the APK from the project root with `cd android; .\build-apk.ps1`. The script sets this profile's JDK/SDK paths for the build; the Gradle wrapper downloads its distribution on first use. Install the resulting debug APK manually on an arm64 Android phone.
