# Architecture

## Signal flow

```
Voices  ──┐
Clips   ──┤
Streams ──┼──▶ Bus (child) ──▶ Bus (master) ──▶ Limiter ──▶ Output
Mic     ──┘         │                │
                   FX chain         FX chain
              (filter/delay/    (filter/delay/
               comp/chorus/      comp/chorus/
               distortion/       distortion/
               reverb/EQ)        reverb/EQ)
```

Buses form a hierarchy under the master bus. Each bus runs its own FX chain, whose order is set by
`setBusEffectOrder`, and can send to any other bus. The master bus feeds a lookahead limiter before
output.

## Threading model

The audio callback is hard-realtime. It never locks, allocates, does I/O, or blocks. Scratch
buffers are pre-allocated. Two mechanisms carry data across the boundary.

**RCU snapshots.** Voice, clip, playback, and bus lists live in `AtomicSharedPtr`
(`atomic_shared_ptr.h`) over an `RcuDomain` (`rcu.h`, QSBR). The main thread publishes a new list
and retires the old one; the audio thread reads through a wait-free `ReadScope`. Reclamation runs
from `Engine::update()`, which callers must tick from a non-audio thread.

**Atomic parameter structs.** Per-object params (`dsp/params.h`) are `std::atomic` fields with
relaxed ordering, plus a `version` counter so the audio thread can batch-apply a coherent set of
changes.

Mutexes appear in the codebase (`voiceWriteMutex_`, `busWriteMutex_`, `mediaWriteMutex_`,
`fileStreamsMutex_`, and inside `FileStreamRunner`), but only to serialize writers on non-audio
threads, which is the single-writer half of the RCU protocol. The audio thread never touches them.

## Producer paths

Off-thread producers follow the same discipline:

- `pushStreamSamples` is single-producer.
- Mic taps and loopback callbacks run on capture threads and must do realtime-safe work only.
- `addMicTap` and `removeMicTap` are single-writer: call them from one thread, not concurrently.

Streaming sources, both live PCM and disk-streamed files, are fed through a ring the audio thread
drains. File decoding and resampling happen on a dedicated worker, never in the callback.
