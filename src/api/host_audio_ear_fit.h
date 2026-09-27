#pragma once

// bro.ear.fit internals, shared by the options / result half
// (host_audio_ear_fit.cpp) and the scoring + async half
// (host_audio_ear_fit_job.cpp).

#include "host_audio_ear.h"

#include <broaudio/ear/fit.h>

#include <memory>
#include <string>
#include <vector>

namespace broaudio::api {

// The JS side of the external scorer: a function over a batch of clips, a
// CLAP model scoring the clips against prompts, or both. Lives on the JS
// thread only.
struct JsScoring {
    ev::Persistent fn;         // fn(clips) -> number[] (distances)
    double fnWeight = 1.0;
    ev::Persistent clapModel;  // has score(clip, prompts, opts?)
    ev::Persistent clapPrompts;  // what score() is handed: embeddings (or strings)
    ev::Persistent clapOptions;
    int clapIndex = 0;
    double clapWeight = 1.0;
    bool hasFn = false;
    bool hasClap = false;
    bool any() const { return hasFn || hasClap; }
};

// Scores `clips` on the JS thread: fnWeight * fn's distance + clapWeight *
// (1 - the CLAP score of prompt clapIndex), per clip. False on failure, with
// `error` set and `thrown` holding what the script threw (undefined when the
// failure is the binding's own, e.g. a return value of the wrong shape).
bool scoreBatchOnJsThread(JsScoring& s, const std::vector<const broaudio::ear::Clip*>& clips,
                          std::vector<double>& out, std::string& error, ev::Persistent& thrown);

// Everything fit needs that is not a JS value: owned by the worker in the
// async form.
struct FitRequest {
    std::shared_ptr<const SynthGraph> graph;
    broaudio::ear::FitOptions options;
    std::unique_ptr<broaudio::ear::Clip> reference;  // options.reference points here
    std::vector<broaudio::ear::FitParamInfo> plan;
};

// The result object bro.ear.fit returns (or hands to onDone).
Value fitResultValue(const broaudio::ear::FitResult& r, const FitRequest& req);
// onProgress's argument.
Value fitProgressValue(const broaudio::ear::FitProgress& p, const std::vector<double>& values,
                       const std::vector<broaudio::ear::FitParamInfo>& params);

// The async form: starts the fit on a native thread and returns its
// AsyncHandle ({done, cancelled, cancel(), wait()}). onDone(result,
// {cancelled, error?}) and onProgress(progress) run on this thread from
// tickAsyncJobs or inside wait(); so does every scoring call.
Value launchEarFit(std::unique_ptr<FitRequest> req, JsScoring scoring, Value onDone, Value onProgress);

} // namespace broaudio::api
