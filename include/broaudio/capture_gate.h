#pragma once

// The host's say over recording: every way broaudio starts capturing sound
// (the microphone stream, a mic tap, loopback of other applications' audio)
// asks the gate first. A host that runs apps it does not fully trust (bro,
// which grants the microphone per app) installs one; with none installed,
// capture is allowed as it always was.
//
// The gate answers "" to allow, or the sentence the refusal carries. It may
// block to ask the user, so it is called on the thread that starts the
// capture (the main thread, for every entry point here), never on the audio
// thread. It is process-wide, set once before any capture starts.

#include <cstdint>
#include <functional>
#include <string>

namespace broaudio {

using CaptureGate = std::function<std::string(const std::string& what)>;

void setCaptureGate(CaptureGate gate);

/// The gate's answer for `what` ("record from the microphone"): "" when
/// allowed or when no gate is set.
std::string checkCaptureGate(const std::string& what);

// A capture the gate allowed is held while it runs, so a host that takes the
// grant back can end it: the holder registers it and returns a token (0 when
// it holds nothing); when the grant goes, the host runs `revoke` with the
// refusal, on the thread that started the capture, and the capture stops
// (the microphone stream closes, a tap is removed after its onRevoked, a
// loopback capture stops). A capture that stops first releases its token.
using CaptureRevoke = std::function<void(const std::string& refusal)>;
using CaptureHolder = std::function<uint64_t(const std::string& what, CaptureRevoke revoke)>;
using CaptureReleaser = std::function<void(uint64_t token)>;

void setCaptureHolder(CaptureHolder hold, CaptureReleaser release);

/// Hold a capture that started (0 when no holder is set).
uint64_t holdCapture(const std::string& what, CaptureRevoke revoke);

/// The capture stopped: release its token (and zero it). 0 is ignored.
void releaseCapture(uint64_t& token);

} // namespace broaudio
