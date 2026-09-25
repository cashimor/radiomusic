# Radiomusic: desktop handoff and Android plan

Prepared 24 September 2026. This document carries the relevant laptop conversation to a new desktop session. Android work has been discussed only: no Android project or tools have been installed by this task. Ask the user to begin implementation before starting it.

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

## Android design discussed (not yet implemented)

1. Preserve/share the portable C++ music core and keep the Windows application working.
2. Add an Android project, probably a Kotlin touch interface with a small bridge into C++ using the NDK/CMake.
3. Replace WinHTTP with Android-capable networking. Existing minimp3 decoding can be reused.
4. Replace waveOut with Android audio output, preferably Oboe. Adapt sample-rate handling and keep allocations, disk access and blocking work out of the audio callback.
5. Move reusable reverb/DSP out of the Windows-specific engine where needed.
6. Support screen-off playback with Android's media playback foreground service/notification, audio focus, calls, route changes, and network interruptions.
7. Let the user select a library folder on the SD card using Android's Storage Access Framework. Persist access permission; this requires a storage adapter rather than assuming arbitrary C++ filesystem paths work with document URIs. A user-owned folder can survive uninstalling the app. Store the index/settings locally and preload selected audio to RAM; never depend on SD reads finishing in the playback callback. Handle missing cards gracefully. Include bans and metadata in library transfer/export.
8. Add Wi-Fi-only capture, enabled by default. On cellular, close/avoid the radio stream and remix the saved library. On Wi-Fi return, resume capture. With no saved material, show Waiting for Wi-Fi. Constrain the actual radio connection to Wi-Fi so a network handover cannot silently transfer it to cellular; checking connectivity occasionally is insufficient. Metered Wi-Fi/hotspots are a separate policy choice to settle when implementing.
9. First milestone: play existing saved loops on the real phone, with basic controls and provenance display. Then add radio capture, SD library management, and background operation. Measure CPU, RAM, glitches and battery use rather than promising performance from specifications alone.

## Tooling and planning estimates

Development tools belong on the desktop, not the phone. Testing with the actual phone over USB is preferred initially; the emulator is optional.

Approximate downloads discussed: Android Studio 1.5 GB; Android SDK/build tools 0.5–1.5 GB; Windows NDK about 730 MB; CMake/Gradle/project dependencies 0.3–1 GB. Allow roughly 3–5 GB downloads and 15–25 GB disk space without an emulator. An emulator/system image adds roughly 2–4 GB downloads. These are planning estimates, not pinned package requirements; verify current versions/sizes at installation time.

Official references consulted:
- https://developer.android.com/studio
- https://developer.android.com/ndk/downloads
- https://developer.android.com/studio/projects/install-ndk
- https://developer.android.com/games/sdk/oboe
- https://developer.android.com/media/media3/session/background-playback
- https://developer.android.com/training/data-storage/shared/documents-files
- https://en-us.support.motorola.com/app/answers/detail/a_id/179364/reg/749642

## Source map and Windows checks

- CMakeLists.txt: music_core shared code, Windows executable, core_tests.
- src/music.hpp and music.cpp: shared types, beat analysis, extraction, WAV/metadata persistence.
- src/features.cpp: key estimation and related helpers.
- src/crossover.cpp: cached bass/high bands.
- src/mixer.cpp: arrangement selection, playback, tempo matching, reverb-send timing, provenance.
- src/library.cpp/.hpp: catalogue, resident selection, persistent bans.
- src/engine.cpp/.hpp: Windows networking/audio, engine state and reverb implementation.
- src/analysis_worker.cpp: capture analysis and storage orchestration.
- src/main.cpp: Windows interface and diagnostic modes.
- tests/core_tests.cpp: audio and persistence regression tests.
- vendor/: minimp3 and license/revision information.

Windows build: Visual Studio 2022 Build Tools with Desktop development with C++ and CMake; run build.ps1. It configures/builds Release and runs CTest. Output: build/Release/Radiomusic.exe. Start Radiomusic.cmd launches it.

Last tested state: successful Release build and all core tests passed after adding loop/band normalization, frequency auditioning, and moving the crossover to 220 Hz. Android has not been tested.

The Windows process once remained alive after its window closed, blocking relinking. Close gracefully before rebuilding; investigate a remaining process if necessary. In the laptop agent sandbox, MSBuild needed an elevated/approved tool execution because duplicated PATH entries broke the sandboxed build environment; this may not apply on the desktop.

## Moving to the desktop

The accompanying Radiomusic-desktop-handoff.zip contains source, tests, vendor files, build instructions, and this document. It excludes generated builds, test output, old binary archives, and the personal audio library. Extract into a new desktop project folder; do not reuse the laptop's CMake build directory.

Optionally copy the entire library/ folder separately, including WAVs, text metadata, rejection markers and rejected-captures.txt. Copy while Radiomusic is stopped for a consistent snapshot. Existing binaries in older portable archives may be stale; build from the supplied source.

Open the extracted project in the desktop coding app and start with: "Read DESKTOP-HANDOFF.md and inspect the project. This is the Radiomusic project from my laptop. Help me continue planning the Android version; do not install tools or start the port until I ask."
