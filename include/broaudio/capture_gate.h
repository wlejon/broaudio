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

#include <functional>
#include <string>

namespace broaudio {

using CaptureGate = std::function<std::string(const std::string& what)>;

void setCaptureGate(CaptureGate gate);

/// The gate's answer for `what` ("record from the microphone"): "" when
/// allowed or when no gate is set.
std::string checkCaptureGate(const std::string& what);

} // namespace broaudio
