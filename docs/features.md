# Features

Capability list by subsystem. `Engine` (`include/broaudio/engine.h`) is the entry point for
everything below; the headers themselves carry the per-call contracts (threading, clamping,
degradation).

## Synthesis

- **Oscillators**: sine, square, sawtooth, triangle with polyBLEP anti-aliasing.
- **Wavetable**: mipmap-per-octave bank with cubic interpolation. Factory presets (saw, square,
  triangle) plus arbitrary waveform import.
- **Noise**: white, pink (Voss-McCartney), brown.
- **ADSR envelope**: per voice.
- **Per-voice filter**: biquad, 8 types (LP, HP, BP, notch, allpass, peaking, low/high shelf).
- **Pitch bend and pan**: per voice.
- **Unison / supersaw**: 1-8 sub-oscillators per voice with configurable detune spread and stereo
  width, gain-normalized by `1/sqrt(N)`.

## Voice management

- **Allocator**: configurable polyphony, 16 voices by default.
- **Steal policies**: oldest, quietest, same-note, or none (drop).
- **Setup callback**: configure waveform, ADSR, bus routing and so on at allocation time.

## Modulation

- **4 LFOs**: sine, triangle, square, saw up/down, sample-and-hold. Per-voice phase with optional
  note-on sync.
- **Modulation matrix**: up to 16 routes.
- **Sources**: LFO 1-4, envelope, velocity, key tracking, mod wheel, aftertouch.
- **Destinations**: pitch, gain, pan, filter frequency, filter Q, pulse width, delay send.

## Effects

Per bus, in a chain whose order is set by `setBusEffectOrder`.

- **Biquad filters**: up to 4 slots per bus, 8 filter types.
- **Delay**: feedback delay with wet/dry mix.
- **Compressor**: threshold, ratio, attack, release, with optional sidechain from another bus.
- **Reverb**: Freeverb-style stereo algorithm (8 comb + 4 allpass).
- **Chorus / flanger**: modulated stereo delay lines.
- **Distortion / waveshaper**: soft clip (tanh), hard clip, foldback, bitcrush, each with drive,
  mix, and output gain.
- **Equalizer**: 7-band parametric (60 Hz to 16 kHz) with presets.
- **Master limiter**: lookahead peak limiter on the master bus.

## Mixing

- **Bus hierarchy**: master bus plus dynamically created children.
- **Per-bus controls**: gain, pan, mute.
- **Solo**: `setBusSolo` silences non-soloed bus outputs. Soloed subtrees and the ancestors carrying
  their audio stay audible, mute wins over solo, and sources routed directly to an audible ancestor
  are untouched (Godot semantics).
- **Aux sends**: from voices, clips, or buses to any other bus.
- **Routing**: voices and clips can target any bus.
- **Metering**: per-bus peak and RMS (L/R) for level visualization.

## Audio clips

- **Sample playback**: load from raw float buffers (mono or stereo), play with gain, pan, and rate
  control.
- **Looping and regions**: loop points and sub-region playback.
- **Variable-rate playback**: time-stretching via playback rate.
- **Scheduled start**: `playClipAt` begins a clip when the audio clock reaches a given engine time,
  with no main-thread timer jitter.
- **Seek and position**: `seekPlayback` moves the cursor; `getPlaybackPosition` (normalized) and
  `getPlaybackPositionSeconds` report it.
- **Waveform extraction**: min/max binning for display.
- **Decode from file**: `createClipFromFile` loads WAV/FLAC/MP3/OGG straight into a clip.
  `createClipFromFileEx` reports an actionable error, `createClipFromFileAsync` decodes on a
  background thread, and `setMaxClipDecodedBytes` caps decoded size (default ~200 MB).

## Streaming sources

- **Live PCM streams**: `createStream` returns a persistent playback fed frame-by-frame via
  `pushStreamSamples`. The audio thread reads a ring the producer appends to, so network or voice
  audio plays click-free without per-chunk clip churn.
- **Disk-streamed files**: `createStreamFromFile` plays large files without decoding them into RAM.
  A worker thread decodes and resamples incrementally into the same ring, with configurable ring
  size, prebuffer, and seamless looping.
- **Full playback control**: streaming instances accept every `setPlayback*` and
  `setPlaybackSpatial*` call (gain, pan, bus, sends, 3D position). Disk streams also support
  `seekPlayback`.
- **Ring statistics**: `getStreamStats` reports decoded, played, and buffered frames, plus
  underruns and EOF.

## Audio file I/O

- **Decode**: `loadAudioFile` and `loadAudioFileFromMemory` decode WAV, FLAC, MP3, and OGG to
  interleaved float32 PCM via dr_libs. Opus requires `BROAUDIO_OPUS`.
- **Encode**: `saveWav` writes 32-bit float WAV; `exportRecordingToWav` dumps captured output.
- **Offline resampling**: polyphase Kaiser-windowed sinc `resample()` for arbitrary ratios.

## Spatial audio

- **3D listener**: position and orientation (forward and up vectors).
- **Per-source positioning**: on voices and on clip playback instances.
- **Distance models**: linear, inverse, exponential, with configurable ref/max distance and rolloff.
- **Angle-based stereo panning**: projects source direction onto the listener's right vector.
- **Head-shadow model**: optional HRTF-style ILD plus a per-ear one-pole lowpass for front/back and
  elevation cues, all caller-tunable.
- **Doppler**: per-source and listener velocities with a global `setDopplerFactor`. Clips compose
  the ratio into their resampling rate, voices fold it into pitch. The ratio is clamped to
  [0.5, 2.0]. Streaming playbacks are not shifted, since their ring mixer has no resampler.

## MIDI

- **Input**: hardware and virtual ports via libremidi.
- **Lock-free delivery**: SPSC ring buffer from the callback to the main thread.
- **VoiceAllocator integration**: automatic note on/off routing.
- **CC mapping**: per-CC callbacks.
- **Pitch bend and aftertouch**: dedicated callbacks.

## Sequencer

- **Beat-based timeline**: note events scheduled at beat positions with duration.
- **Tempo**: configurable BPM and time signature.
- **Transport**: play, stop, pause, resume.
- **Looping**: loop range with automatic wrap-around.
- **Automation lanes**: per-parameter breakpoint envelopes with step, linear, or smooth
  interpolation, applied each `update`.
- **VoiceAllocator integration**: fires noteOn/noteOff at sample-accurate engine time.
- **Main-thread driven**: call `update(engineTime)` from your frame loop.

## Presets and serialization

- **Plain-data structs**: `VoicePreset`, `BusPreset`, `ModPreset`, and `EnginePreset` describe a
  full patch with no atomics.
- **Apply to live objects**: `applyVoicePreset`, `applyBusPreset`, `applyModPreset`,
  `applyEnginePreset`.
- **JSON round-trip**: `toJson` and `*FromJson`, plus `savePresetToFile` and `loadPresetFromFile`.

## Analysis and recording

- **Analysis buffers**: output and mic ring buffers for visualization.
- **FFT spectrum**: Cooley-Tukey, up to 8192 bins.
- **Microphone capture**: monitor gain, mute, and bus routing.
- **Mic taps**: multi-consumer audio-thread mic dispatch. Each tap requests its own target rate
  (polyphase resampled), fixed chunk size, and optional AGC, with per-tap stats. One hook covers
  wake-word detectors, live ASR, and custom analyzers. `injectMicSamples` drives the same tap chain
  from synthetic audio for headless and test use.
- **Output recording**: captures up to 60 seconds of engine output.

## System-audio capture

`LoopbackCapture` captures what the machine is *playing* (the render side), independent of the
engine's mic path. Frames arrive as FP32 on the capture thread, optionally downmixed to mono and
resampled to a target rate.

- **Modes**: `SystemOutput` (the whole render endpoint), `ProcessInclude` and `ProcessExclude` (one
  application's process tree, or everything but it).
- **Backends**: Windows WASAPI loopback; macOS CoreAudio process taps (14.2+, needs
  `NSAudioCaptureUsageDescription`); Linux PulseAudio monitor source via libpulse, which also
  covers pipewire-pulse; a stub elsewhere.
- **Degradation**: `isSupported()` is false where unavailable, and the process modes report
  unsupported on PulseAudio, which has no public per-app capture API. `enumerateProcesses()` still
  lists apps holding a render session so callers can show a picker.

## Engine control and offline rendering

- **Headless mode**: `initHeadless()` opens no device. `renderBlock(numFrames)` drives the full
  pipeline (voices, clips, bus FX, mix, limiter, metering, recording) on the calling thread.
- **Master pause**: `setMasterPaused` suspends the output clock. The graph stops advancing and
  resumes exactly where it left off rather than muting.
- **Output latency**: `outputLatencySeconds()` estimates device-buffer latency. It is a lower
  bound; OS mixer and DAC latency are not visible.
- **Sample-accurate events**: `scheduleNoteOn` and `scheduleNoteOff` dispatch inside the audio
  callback at a given engine time.
- **Offline effects**: `processEffectsOffline` runs mono samples through a clone of a bus's effect
  chain without touching live audio.
