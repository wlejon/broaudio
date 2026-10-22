#include "api.h"
#include "host_audio_internal.h"
#include "object_builder.h"

namespace broaudio::api {

static broaudio::Engine* s_customEngine = nullptr;
static std::unique_ptr<broaudio::Engine> s_defaultEngine = nullptr;
static std::function<broaudio::Engine*()> s_engineProvider;

broaudio::Engine* getAudioEngine() {
    if (s_customEngine) return s_customEngine;
    if (s_engineProvider) {
        s_customEngine = s_engineProvider();
        if (s_customEngine) return s_customEngine;
    }
    if (!s_defaultEngine) {
        s_defaultEngine = std::make_unique<broaudio::Engine>();
        if (!s_defaultEngine->init()) {
            s_defaultEngine->initHeadless();
        }
    }
    return s_defaultEngine.get();
}

broaudio::Engine* existingAudioEngine() {
    return s_customEngine ? s_customEngine : s_defaultEngine.get();
}

void setAudioEngine(broaudio::Engine* engine) {
    s_customEngine = engine;
    s_engineProvider = nullptr;
}

void setAudioEngineProvider(std::function<broaudio::Engine*()> provider) {
    s_customEngine = nullptr;
    s_engineProvider = std::move(provider);
}

void shutdownAudio() {
    // Background clip loads hold the engine pointer: join them before the
    // engine they decode into goes away.
    shutdownActiveMic();
    shutdownAsyncJobs();
    shutdownLiveParams();
    if (s_defaultEngine) {
        s_defaultEngine->shutdown();
        s_defaultEngine.reset();
    }
    s_customEngine = nullptr;
    s_engineProvider = nullptr;
}

void installAudio() {
    installAudioGlobals();
    installSynthGraph();
}

} // namespace broaudio::api
