# Radiomusic 0.3

A C++17 Windows radio remixer. It saves beat-aligned source recordings, cuts them into new sequences, and combines the bass of one source with the upper frequencies of another.

## Run

Double-click **Start Radiomusic.cmd**, then **Start listening**. The Technolovers Trance station is already entered. The saved-loop count appears when the window opens, before playback. Existing recordings load automatically. With an empty library, the station plays while approximately 48 seconds of audio are collected and analyzed.

Both **Rearrangement** and **150 Hz split** start enabled. The first remix is already processed; it does not wait several repetitions before changing the source material. The station-to-remix fade takes 16 beats.

## Controls

- **Evolve / >:** request the next sequence at the next eight-bar boundary.
- **Rearrangement on/off:** enable or disable beat-sized cuts, beat repeats, bar reordering, and source substitutions. Changes take effect through a phrase-aligned transition.
- **150 Hz split on/off:** combine independently sourced bass and upper-frequency parts. Disable to compare against full-band rearrangement. It can also be used with rearrangement off.
- **Ban sources / −:** permanently ban the source captures under the playhead, including bass/upper sources and both sides of a crossfade. All loops from those 48-second captures are removed, so their bars and beats cannot return in another arrangement. Bans survive restarting. This does not recognize the same song rebroadcast later as a new capture.
- **Hear live radio:** monitor the original station while capture continues. Return to remix fades back to the processed sequence. Rejection applies to remix playback, not live monitoring.
- **Stop:** stop playback/capture and preserve the library. A blocked network request can take several seconds to stop.
- **Open library:** open the saved source recordings. The volume slider changes output smoothly.

Keyboard shortcuts apply when the station address field does not have focus. Effect detection, capture, the Play effect button, and its shortcut/playback code have been removed. Any previously saved files in `library/effects/` are ignored and left on disk.

## What changed in the sound

An eight-bar sequence contains 32 estimated beats. The upper-frequency part now uses one-beat cuts throughout the phrase: patterns such as `1 2 1 2`, `1 3 2 4`, and `3 4 1 2`, combined with bar reordering and source substitutions. The bass part keeps a steadier beat order and repeats two-bar groups. Each constructed sequence repeats consistently rather than randomizing on every pass.

The crossover is a fourth-order **Linkwitz–Riley split at 150 Hz**: two cascaded second-order Butterworth sections per band (24 dB/octave slopes). Low and high from the same recording sum to flat magnitude, with a common phase shift. Normally, the mixer selects different compatible recordings for the two bands. With only one suitable recording, its bands can use different cuts/positions instead.

This is frequency separation, not instrument separation: the bass part includes all low-frequency energy, and vocals, percussion, and melody can all remain in the upper part. Combining recordings leaves extra headroom and uses a soft peak limiter. Tiny fades at discontinuous cuts reduce clicks. Band preparation and disk access happen on the storage worker, not in the audio renderer.

**Automatic timing: two steady passes, then a crossfade lasting one complete pass.** At 128 BPM, each eight-bar pass is about 15 seconds, so automatic evolution begins after about 30 seconds and blends over the following 15 seconds. The incoming sequence is audible during the crossfade, then receives its own two steady passes. Pressing `>` requests an earlier transition on the next phrase boundary.

## Reading the display

The upper row is the current sequence; the lower row appears during a transition. The highlighted small number follows the beat approximately (audio buffers and display refresh introduce a small timing difference).

- **H:** the source of the upper-frequency material. When the split is off, this becomes a full-band source label without H.
- **L:** the source of the bass material. It has its own source color and original bar number.
- **Color + four-character label:** the original capture. Adjacent recordings extracted from one capture share a color. Short labels/colors can coincide; clicking a bar reveals the full identity.
- **Number after `/`:** the original bar within that capture, rather than its new playback position.
- **Four small numbers:** which beats of the upper/full-band source bar are being played. `1 2 1 2` repeats the first two beats. The bass retains `1 2 3 4` within its selected source bar.
- **Gold dot:** the upper/full-band bar differs from a straight-through version of its anchor recording.
- **Click a bar:** inspect both source recordings, their original bar numbers, and beat order.

This displays recording provenance, not instruments. Bar numbers use an estimated 4/4 grid; reliable musical downbeat detection is not yet implemented.

## Library and memory

The library now stores **1,024 recordings**, up from 48. A full library uses roughly **2–4 GB** of WAV audio depending on tempo. At capacity, new collection pauses; older recordings are not automatically erased.

The full catalogue holds metadata only. A rotating working set loads **up to 16 recordings**, including the sources referenced anywhere in the current and incoming sequences. Other candidates refresh about every 20 seconds and after captures/rejections. Tempo/key matches and different captures are preferred. Full-resolution audio and both filtered bands are cached only for that working set. Temporary overlap during a refresh and the capture buffers means memory use can still reach several hundred MB, but it does not grow with all 1,024 recordings.

Each original source is a 44.1 kHz, 16-bit stereo WAV under `library/` with a companion `.txt` containing tempo, confidence, source, key estimate, and capture/bar identity. **WAVs remain source material; cuts and band combinations are created during playback.** Keep each WAV together with its text file when backing up. Arrangements themselves are not saved between runs.

Older recordings remain compatible. Their key can be estimated on loading and their capture/bar identity inferred from the generated filenames. Original recordings are not rewritten. Files left over from the removed effect feature are not loaded.

## Musical limits

Tempo analysis searches **90–175 BPM**. The master tempo remains fixed. Sources are sped up or slowed down to match it, permitting playback-speed ratios from **0.90 to 1.10** (up to a 10% speed change). This admits the full 130–140 BPM range in either direction; larger mismatches are skipped. Both source selection and the rotating library use this limit. For example, 130 BPM played at 140 BPM runs at 1.077× speed.

Speed changes also shift pitch: pitch-preserving time stretching is not implemented. Key matching and displayed key estimates account for this shift, rounded to the nearest semitone. Fractional-semitone detuning and different chord progressions can still cause clashes.

Key estimates use spectral pitch-class profiles. Known incompatible keys are skipped; uncertain keys remain permissive. Estimates can be wrong or ambiguous, and key compatibility alone cannot guarantee matching chord progressions. Beat estimates can drift through track changes or choose half/double time. More varied source recordings and listening feedback remain important.

Only direct HTTP/HTTPS MP3 streams are supported: not AAC, HLS, playlist files, or station web pages. The original radio-to-remix fade is not synchronized to the delayed live station. These laptop buffers/filters are not yet an RP2350 implementation.

## Build and validation

Run `powershell -ExecutionPolicy Bypass -File .\build.ps1`. Requires Visual Studio 2022 Build Tools with Desktop development with C++ and CMake. The executable is `build/Release/Radiomusic.exe`.

Core tests cover known tempos and keys, frequency separation and flat crossover summation, exact two-plus-one transition timing, speed-up/slow-down matching and mismatch limits, displayed source maps versus rendered audio, backwards-compatible persistence, rejection, and an 81-recording catalogue with a bounded rotating working set.

- `Radiomusic.exe --smoke 75`: muted live test with a separate `test-output/library`, reports, and a short recorded remix preview. Requires Internet and a sound device. An optional argument after the duration supplies another direct MP3 URL.
- `Radiomusic.exe --offline-check`: after populating the test library, checks saved playback with an unavailable station and rejection of audible test sources.
- `Radiomusic.exe --ui-check --arrangement`: renders an explicitly illustrative arrangement to `test-output/interface.bmp` without capturing the desktop.

Portable audio code lives in `music.cpp`, `mixer.cpp`, `features.cpp`, `crossover.cpp`, and `library.cpp`. Windows streaming/output is in `engine.cpp`, storage orchestration in `analysis_worker.cpp`, and the desktop UI in `main.cpp`. The vendored MP3 decoder is minimp3 (CC0); see `vendor/` for its pinned revision and license.

DSP references: [Linkwitz's crossover filters](https://www.linkwitzlab.com/filters.htm), [W3C Audio EQ Cookbook](https://www.w3.org/TR/audio-eq-cookbook/), and [key profile estimation](https://essentia.upf.edu/reference/std_Key.html). These are small local implementations, not bundled DSP frameworks.
