// bro.ear.fit's external scorer glue and its async form.
//
// Scoring: a JS scorer function and a CLAP model both run on the JS thread,
// once per generation, over every candidate's clips (population x seeds) at
// once. In the synchronous form fit() calls them directly between
// generations (host_audio_ear_fit.cpp).
//
// Async (opts.onDone): ear::fit runs on a native thread (its renders fan out
// to more native threads). When it needs the generation scored, it posts the
// batch to the job and blocks; the JS thread scores it the next time it
// services the job — from tickAsyncJobs (bro's frame pump) or inside
// handle.wait() — and wakes it. Progress snapshots are coalesced (the latest
// wins) and delivered the same way, and so is completion: onDone(result,
// {cancelled, error?}). Nothing JS-side crosses a thread: the worker owns a
// FitShared (plain C++), the JS thread owns the FitJob with its Persistents.

#include "host_audio_ear_fit.h"
#include "host_audio_internal.h"

#include <broaudio/synth/synth_graph.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace broaudio::api {

namespace ear = broaudio::ear;

bool scoreBatchOnJsThread(JsScoring& s, const std::vector<const ear::Clip*>& clips, std::vector<double>& out,
                          std::string& error, ev::Persistent& thrown) {
    const size_t n = clips.size();
    out.assign(n, 0.0);
    ev::Persistent arr(hostArrayOf(n, [&](size_t i) { return earClipValue(*clips[i]); }));
    if (s.hasFn) {
        const Value args[1] = {arr.get()};
        ev::CallResult r = ev::call(s.fn.get(), ev::undefined(), std::span<const Value>(args, 1));
        if (r.thrown) {
            thrown.set(r.value);
            error = "scorer threw";
            return false;
        }
        ev::Persistent res(r.value);
        const bool arrayLike = ev::isObject(res.get()) && ev::isNumber(ev::getProperty(res.get(), "length"));
        if (!arrayLike || ev::toDouble(ev::getProperty(res.get(), "length")) != static_cast<double>(n)) {
            error = "scorer must return one number per clip (" + std::to_string(n) + ")";
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            Value e = ev::getElement(res.get(), static_cast<uint32_t>(i));
            if (!ev::isNumber(e)) {
                error = "scorer returned a non-number at " + std::to_string(i);
                return false;
            }
            const double d = ev::toDouble(e);
            out[i] += s.fnWeight * (std::isfinite(d) ? d : ear::kFitMissing);
        }
    }
    if (s.hasClap) {
        ev::Persistent score(ev::getProperty(s.clapModel.get(), "score"));
        if (!ev::isFunction(score.get())) {
            error = "clap.model has no score() method";
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            ev::Persistent clip(ev::getElement(arr.get(), static_cast<uint32_t>(i)));
            const Value args[3] = {clip.get(), s.clapPrompts.get(), s.clapOptions.get()};
            const size_t argc = ev::isUndefined(s.clapOptions.get()) ? 2 : 3;
            ev::CallResult r = ev::call(score.get(), s.clapModel.get(), std::span<const Value>(args, argc));
            if (r.thrown) {
                thrown.set(r.value);
                error = "clap.model.score threw";
                return false;
            }
            ev::Persistent res(r.value);
            Value scores = ev::isObject(res.get()) ? ev::getProperty(res.get(), "scores") : ev::undefined();
            Value v = ev::isObject(scores) ? ev::getElement(scores, static_cast<uint32_t>(s.clapIndex)) : ev::undefined();
            if (!ev::isNumber(v)) {
                error = "clap.model.score returned no scores[" + std::to_string(s.clapIndex) + "]";
                return false;
            }
            const double p = ev::toDouble(v);
            out[i] += s.clapWeight * (std::isfinite(p) ? 1.0 - p : ear::kFitMissing);
        }
    }
    return true;
}

namespace {

// The worker's half: no JS values.
struct FitShared {
    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> cancel{false};

    // A scoring request: the worker posts it and waits for `answered`, or
    // for a cancel the JS thread has not picked the batch up before.
    const std::vector<const ear::Clip*>* clips = nullptr;
    std::vector<double>* out = nullptr;
    bool requested = false, taken = false, answered = false, answerOk = false;

    // The latest progress snapshot.
    bool progressPending = false;
    ear::FitProgress progress;
    std::vector<double> progressValues;
    std::vector<ear::FitParamInfo> params;

    bool finished = false;
    ear::FitResult result;
    std::string error;  // fit() itself threw
};

// What the handle object's closures hold: no JS values, so a collected
// handle never runs a Persistent's destructor inside the sweep.
struct HandleState {
    bool done = false;
    bool cancelled = false;
};

// The JS thread's half.
struct FitJob {
    std::shared_ptr<FitShared> sh;
    std::shared_ptr<FitRequest> req;
    std::thread worker;
    JsScoring scoring;
    ev::Persistent onDone, onProgress;
    std::shared_ptr<HandleState> state = std::make_shared<HandleState>();
    std::string failure;  // the first scoring / onProgress failure
    bool settling = false;
    int inCallback = 0;  // scorer / onProgress running: wait() would deadlock

    ~FitJob() {
        if (worker.joinable()) {
            sh->cancel.store(true);
            sh->cv.notify_all();
            worker.join();
        }
    }
};

// Per thread: the thread that launched a job is the one that settles it.
std::vector<std::shared_ptr<FitJob>>& jobs() {
    static thread_local std::vector<std::shared_ptr<FitJob>> v;
    return v;
}

void dropJob(const FitJob* j) {
    auto& v = jobs();
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].get() == j) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void cancelJob(FitJob& j) {
    {
        std::lock_guard<std::mutex> l(j.sh->mu);
        j.sh->cancel.store(true);
    }
    j.sh->cv.notify_all();
}

// Runs what the job has waiting on the JS thread: a scoring request, a
// progress report, completion. True once the job is settled (onDone ran).
bool service(FitJob& j, ev::Persistent* thrownOut) {
    FitShared& sh = *j.sh;
    const std::vector<const ear::Clip*>* clips = nullptr;
    std::vector<double>* out = nullptr;
    {
        std::lock_guard<std::mutex> l(sh.mu);
        if (sh.requested && !sh.taken) {
            sh.taken = true;
            clips = sh.clips;
            out = sh.out;
        }
    }
    if (clips) {
        bool ok = false;
        if (!sh.cancel.load()) {
            std::string err;
            ev::Persistent thrown;
            ++j.inCallback;
            ok = scoreBatchOnJsThread(j.scoring, *clips, *out, err, thrown);
            --j.inCallback;
            if (!ok && j.failure.empty()) {
                j.failure = ev::isUndefined(thrown.get()) ? err : err + ": " + ev::toUtf8(thrown.get());
            }
        }
        {
            std::lock_guard<std::mutex> l(sh.mu);
            sh.answered = true;
            sh.answerOk = ok;
        }
        sh.cv.notify_all();
    }

    bool report = false;
    ear::FitProgress p;
    std::vector<double> values;
    std::vector<ear::FitParamInfo> params;
    {
        std::lock_guard<std::mutex> l(sh.mu);
        if (sh.progressPending) {
            sh.progressPending = false;
            report = ev::isFunction(j.onProgress.get()) && !j.state->done;
            p = sh.progress;
            values = sh.progressValues;
            params = sh.params;
        }
    }
    if (report) {
        ev::Persistent pv(fitProgressValue(p, values, params));
        const Value args[1] = {pv.get()};
        ++j.inCallback;
        ev::CallResult r = ev::call(j.onProgress.get(), ev::undefined(), std::span<const Value>(args, 1));
        --j.inCallback;
        if (r.thrown) {
            if (j.failure.empty()) j.failure = "onProgress threw: " + ev::toUtf8(r.value);
            cancelJob(j);
        }
    }

    bool finished = false;
    {
        std::lock_guard<std::mutex> l(sh.mu);
        finished = sh.finished;
    }
    if (!finished || j.settling) return false;
    j.settling = true;
    if (j.worker.joinable()) j.worker.join();
    j.state->done = true;
    ev::Persistent result;
    if (sh.error.empty()) result.set(fitResultValue(sh.result, *j.req));
    else result.set(ev::null());
    ObjectBuilder info;
    const bool cancelled = j.state->cancelled && j.failure.empty() && sh.error.empty();
    info.set("cancelled", cancelled);
    const std::string error = !sh.error.empty() ? sh.error : j.failure;
    if (!error.empty()) info.set("error", "bro.ear.fit: " + error);
    const Value args[2] = {result.get(), info.get()};
    ev::CallResult r = ev::call(j.onDone.get(), ev::undefined(), std::span<const Value>(args, 2));
    if (r.thrown && thrownOut) thrownOut->set(r.value);
    return true;
}

// Services `j` and drops it from the list once settled; the caller's
// shared_ptr keeps it alive through the callbacks (which may launch or wait
// on other jobs). A throw from onDone lands in `thrownOut` (wait() rethrows
// it; the host's tick has no script to throw into and drops it, as it does
// for bro.mic's onChunk).
bool serviceAndDrop(const std::shared_ptr<FitJob>& j, ev::Persistent* thrownOut) {
    const bool settled = service(*j, thrownOut);
    if (settled) dropJob(j.get());
    return settled;
}

Value waitFn(const std::shared_ptr<HandleState>& state, const std::weak_ptr<FitJob>& weak) {
    for (;;) {
        std::shared_ptr<FitJob> j = weak.lock();
        if (!j || state->done) return ev::undefined();
        if (j->inCallback > 0) {
            return ev::throwError("bro.ear.fit: wait() from inside the same fit's scorer or onProgress");
        }
        ev::Persistent thrown;
        if (serviceAndDrop(j, &thrown)) {
            if (!ev::isUndefined(thrown.get())) ev::throwValue(thrown.get());
            return ev::undefined();
        }
        FitShared& sh = *j->sh;
        std::unique_lock<std::mutex> l(sh.mu);
        sh.cv.wait(l, [&] { return (sh.requested && !sh.taken) || sh.progressPending || sh.finished; });
    }
}

} // namespace

Value launchEarFit(std::unique_ptr<FitRequest> reqIn, JsScoring scoring, Value onDoneV, Value onProgressV) {
    ev::Persistent onDone(onDoneV), onProgress(onProgressV);
    auto job = std::make_shared<FitJob>();
    job->sh = std::make_shared<FitShared>();
    job->req = std::shared_ptr<FitRequest>(std::move(reqIn));
    job->scoring = std::move(scoring);
    job->onDone = onDone;
    job->onProgress = onProgress;

    std::shared_ptr<FitShared> sh = job->sh;
    std::shared_ptr<FitRequest> req = job->req;
    const bool hasScorer = job->scoring.any();
    job->worker = std::thread([sh, req, hasScorer] {
        ear::FitScorer scorer;
        if (hasScorer) {
            scorer = [sh](const std::vector<const ear::Clip*>& clips, std::vector<double>& out) {
                std::unique_lock<std::mutex> l(sh->mu);
                if (sh->cancel.load()) return false;
                sh->clips = &clips;
                sh->out = &out;
                sh->requested = true;
                sh->taken = false;
                sh->answered = false;
                sh->cv.notify_all();
                sh->cv.wait(l, [&] { return sh->answered || (sh->cancel.load() && !sh->taken); });
                const bool ok = sh->answered && sh->answerOk;
                sh->requested = sh->taken = sh->answered = false;
                sh->clips = nullptr;
                sh->out = nullptr;
                return ok;
            };
        }
        auto progress = [sh](const ear::FitProgress& p) {
            {
                std::lock_guard<std::mutex> l(sh->mu);
                sh->progress = p;
                sh->progress.bestValues = nullptr;
                sh->progress.params = nullptr;
                sh->progressValues = *p.bestValues;
                if (sh->params.empty()) sh->params = *p.params;
                sh->progressPending = true;
            }
            sh->cv.notify_all();
            return !sh->cancel.load();
        };
        ear::FitResult r;
        std::string err;
        try {
            r = ear::fit(req->graph, req->options, scorer, progress);
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (sh->cancel.load() && r.stop == "scorer") r.stop = "cancelled";
        {
            std::lock_guard<std::mutex> l(sh->mu);
            sh->result = std::move(r);
            sh->error = err;
            sh->finished = true;
        }
        sh->cv.notify_all();
    });
    jobs().push_back(job);

    std::shared_ptr<HandleState> state = job->state;
    std::weak_ptr<FitJob> weak = job;
    ObjectBuilder h;
    h.accessor("done", [state](Value, std::span<const Value>) { return ev::fromBool(state->done); });
    h.accessor("cancelled", [state](Value, std::span<const Value>) { return ev::fromBool(state->cancelled); });
    h.def("cancel", 0, [state, weak](Value, std::span<const Value>) {
        if (state->done) return ev::undefined();
        state->cancelled = true;
        if (std::shared_ptr<FitJob> j = weak.lock()) cancelJob(*j);
        return ev::undefined();
    });
    h.def("wait", 0, [state, weak](Value, std::span<const Value>) { return waitFn(state, weak); });
    return h.get();
}

void tickEarFitJobs() {
    auto& v = jobs();
    if (v.empty()) return;
    // A snapshot: callbacks may launch jobs onto the list or settle others.
    const std::vector<std::shared_ptr<FitJob>> snapshot = v;
    for (const auto& j : snapshot) serviceAndDrop(j, nullptr);
}

void shutdownEarFitJobs() {
    auto& v = jobs();
    std::vector<std::shared_ptr<FitJob>> all;
    all.swap(v);
    for (auto& j : all) cancelJob(*j);
    // ~FitJob joins; onDone never runs — the realm is going away.
    all.clear();
}

} // namespace broaudio::api
