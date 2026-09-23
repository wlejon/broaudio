// File paths and background work for the AudioContext binding.
//
// Paths: every loader and saver (createClipFromFile, createStreamFromFile,
// decodeAudioFile, saveWav, exportRecordingToWav, savePreset, loadPreset)
// goes through the host's resolver when one is set (api.h setPathResolver),
// so a relative path or a mount path means what it means to the host's `fs`,
// anchored at the app, rather than at the process's working directory.
//
// Background work: createClipFromFileAsync decodes + resamples on its own
// thread (Engine::createClipFromFileEx is documented safe off the audio and
// main threads — it takes the control-plane media mutex) and its promise is
// settled from tickAsyncJobs() on the JS thread. Nothing JS-side ever crosses
// a thread: the worker owns a plain ClipLoadWork, the JS thread owns the
// Persistent promise, and the two meet only through an atomic flag.

#include "host_audio_internal.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace broaudio::api {

namespace {

std::function<std::string(const std::string&)>& pathResolver() {
    static std::function<std::string(const std::string&)> r;
    return r;
}

// A file to be WRITTEN does not exist yet, so the host's resolver — which
// answers by finding the file — would hand a relative path back unchanged
// and the save would land in the process's working directory rather than
// beside the directory an `fs.mkdirSync` just made under the app. Resolve
// the parent directory instead, which does exist, and put the file in it.
std::string resolveWritePath(const std::string& path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    if (p.is_absolute() || !p.has_parent_path()) return resolveAudioPath(path);
    std::string dir = resolveAudioPath(p.parent_path().generic_string());
    return (fs::path(dir) / p.filename()).generic_string();
}

// The worker's half of a clip load: no JS values, so the worker thread may
// own it outright.
struct ClipLoadWork {
    broaudio::Engine* engine = nullptr;
    std::string path;
    std::string error;   // written by the worker before `done`
    int clipId = -1;
    std::atomic<bool> done{false};
};

// The JS thread's half: the promise and the thread to join.
struct ClipLoadJob {
    std::shared_ptr<ClipLoadWork> work;
    std::thread worker;
    ev::Persistent promise;

    ~ClipLoadJob() {
        if (worker.joinable()) worker.join();
    }
};

// Per thread, like the Persistent slots the jobs hold: the thread that
// launched a job is the one that settles it.
std::vector<std::unique_ptr<ClipLoadJob>>& jobs() {
    static thread_local std::vector<std::unique_ptr<ClipLoadJob>> v;
    return v;
}

void settle(ClipLoadJob& job) {
    if (job.worker.joinable()) job.worker.join();
    ClipLoadWork& w = *job.work;
    if (w.clipId >= 0) {
        ev::resolvePromise(job.promise.get(), ev::fromDouble(w.clipId));
        return;
    }
    std::string msg = w.path + ": " + (w.error.empty() ? "failed to load audio file" : w.error);
    ev::Persistent err(hostMakeDomError("Error", msg));
    ev::rejectPromise(job.promise.get(), err.get());
}

}  // namespace

void setPathResolver(std::function<std::string(const std::string&)> resolver) {
    pathResolver() = std::move(resolver);
}

std::string resolveAudioPath(const std::string& path) {
    auto& r = pathResolver();
    return r ? r(path) : path;
}

std::string resolveAudioWritePath(const std::string& path) {
    return resolveWritePath(path);
}

Value launchClipLoad(const std::string& resolvedPath) {
    ev::Persistent promise(ev::createPromise());

    auto job = std::make_unique<ClipLoadJob>();
    job->work = std::make_shared<ClipLoadWork>();
    job->work->engine = getAudioEngine();
    job->work->path = resolvedPath;
    job->promise = promise;

    // The worker holds its own reference to the work: the job may be dropped
    // (shutdown joins first, but a join is all it needs) without the worker
    // ever seeing a dangling pointer.
    std::shared_ptr<ClipLoadWork> work = job->work;
    job->worker = std::thread([work] {
        if (work->engine) {
            work->clipId = work->engine->createClipFromFileEx(work->path.c_str(), &work->error);
        } else {
            work->error = "audio engine not initialized";
        }
        work->done.store(true, std::memory_order_release);
    });

    jobs().push_back(std::move(job));
    return promise.get();
}

void tickAsyncJobs() {
    // Scheduled param automation reaches playing sources, and finished
    // buffer sources get their `onended` (host_audio_live.cpp).
    tickLiveParams();
    auto& v = jobs();
    if (v.empty()) return;
    // Take the finished jobs out first: settling allocates and may run user
    // code (a thenable getter) that launches another load onto this vector.
    std::vector<std::unique_ptr<ClipLoadJob>> finished;
    for (size_t i = 0; i < v.size();) {
        if (v[i]->work->done.load(std::memory_order_acquire)) {
            finished.push_back(std::move(v[i]));
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (auto& job : finished) settle(*job);
}

void shutdownAsyncJobs() {
    auto& v = jobs();
    std::vector<std::unique_ptr<ClipLoadJob>> all;
    all.swap(v);
    // ~ClipLoadJob joins; the promises are left pending on purpose — the
    // realm that would run their reactions is going away with the engine.
    all.clear();
}

}  // namespace broaudio::api
