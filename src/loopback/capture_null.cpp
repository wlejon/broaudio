// Loopback capture — unsupported-platform stub.
//
// Render-side / per-process audio capture is OS-specific. On platforms without
// an implementation (everything except Windows today) LoopbackCapture reports
// unsupported: isSupported() is false, enumerateProcesses() is empty, and
// start() always fails. Callers should hide the feature, not treat it as error.

#include "broaudio/loopback_capture.h"

namespace broaudio {

struct LoopbackCapture::Impl {};

LoopbackCapture::LoopbackCapture() : impl_(nullptr) {}
LoopbackCapture::~LoopbackCapture() = default;

bool LoopbackCapture::isSupported() { return false; }

std::vector<AudioProcess> LoopbackCapture::enumerateProcesses() { return {}; }

bool LoopbackCapture::start(const LoopbackConfig&, LoopbackCallback) { return false; }
void LoopbackCapture::stop() {}
bool LoopbackCapture::isCapturing() const { return false; }
int  LoopbackCapture::sampleRate() const { return 0; }
int  LoopbackCapture::channels() const { return 0; }
LoopbackStats LoopbackCapture::stats() const { return {}; }

} // namespace broaudio
