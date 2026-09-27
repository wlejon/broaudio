#pragma once
// Synthesis graphs shared by the synth-graph tests and the cost bench: the
// representative sounds (a gunshot-like noise burst + body, an FM bell, a
// metallic resonator hit) and a graph that uses every node kind and variant.

namespace synthtest {

// Crack: highpassed white noise with a fast envelope. Body, 2 ms later: pink
// noise through a bandpass whose centre sweeps down, enveloped, then driven
// into a tanh.
inline const char* kGunshot = R"({"layers": {
  "crack": {"nodes": {
      "n":  {"type": "noise", "color": "white", "gain": "e"},
      "e":  {"type": "env", "attack": 0.0005, "decay": {"value": 0.03, "jitter": 0.2}, "sustain": 0, "release": 0.01},
      "hp": {"type": "filter", "mode": "highpass", "input": "n", "cutoff": {"value": 1800, "jitter": 0.1}, "q": 0.7}
    }, "output": "hp"},
  "body": {"offset": 0.002, "gain": 0.8, "nodes": {
      "bn": {"type": "noise", "color": "pink"},
      "sw": {"type": "sweep", "from": 1400, "to": 180, "time": 0.12, "curve": "exp"},
      "bp": {"type": "filter", "mode": "bandpass", "input": "bn", "cutoff": "sw", "q": 1.5, "gain": "be"},
      "be": {"type": "env", "attack": 0.001, "decay": 0.25, "sustain": 0, "release": 0.05},
      "sh": {"type": "shaper", "mode": "tanh", "input": "bp", "drive": 2.5}
    }, "output": "sh"}
}})";

// Two-operator FM with an index envelope (the brightness decays faster than
// the level) and a little modulator feedback.
inline const char* kFmBell = R"({"nodes": {
    "bell": {"type": "fm", "freq": {"value": 880, "jitter": 0.01}, "ratio": 3.5, "index": "ie",
             "feedback": 0.2, "gain": "ae"},
    "ie":   {"type": "env", "attack": 0.001, "decay": 1.2, "sustain": 0, "release": 0.1, "peak": 6},
    "ae":   {"type": "env", "attack": 0.002, "decay": 2.5, "sustain": 0, "release": 0.2}
  }, "output": "bell"})";

// A 4 ms noise burst exciting six inharmonic modes (a struck bar).
inline const char* kMetalHit = R"({"nodes": {
    "exc": {"type": "noise", "gain": "ee"},
    "ee":  {"type": "env", "attack": 0.0002, "decay": 0.004, "sustain": 0, "release": 0.001},
    "res": {"type": "resonator", "input": "exc", "freq": {"value": 520, "jitter": 0.02}, "modes": [
        {"ratio": 1, "decay": 1.2, "gain": 0.5}, {"ratio": 2.76, "decay": 0.9, "gain": 0.35},
        {"ratio": 5.40, "decay": 0.6, "gain": 0.25}, {"ratio": 8.93, "decay": 0.4, "gain": 0.15},
        {"ratio": 13.34, "decay": 0.25, "gain": 0.1}, {"ratio": 18.64, "decay": 0.18, "gain": 0.08}]}
  }, "output": "res"})";

// Every node kind and variant: wired and constant frequency, cutoff, q and
// pulse width, phase modulation, a segment envelope with a hold and a gate,
// all three sweep curves, a jittered layer offset, a comb and a duration.
inline const char* kKitchenSink = R"({"layers": {
  "a": {"nodes": {
      "lfo":   {"type": "osc", "wave": "triangle", "freq": 5},
      "vib":   {"type": "mix", "inputs": [220, {"node": "lfo", "weight": 6}]},
      "saw":   {"type": "osc", "wave": "saw", "freq": "vib"},
      "pwm":   {"type": "osc", "wave": "sine", "freq": 0.7, "gain": 0.3},
      "pwc":   {"type": "mix", "inputs": [0.5, "pwm"]},
      "sq":    {"type": "osc", "wave": "square", "freq": 330, "pw": "pwc", "pm": 0.25},
      "sn":    {"type": "osc", "wave": "sine", "freq": {"value": 110, "jitterAbs": 3}},
      "cut":   {"type": "sweep", "from": 4000, "to": 300, "time": 0.4, "delay": 0.05, "curve": "decay"},
      "q":     {"type": "sweep", "from": 0.7, "to": 6, "time": 0.3, "curve": "linear"},
      "lp":    {"type": "filter", "mode": "lowpass", "input": "saw", "cutoff": "cut", "q": "q"},
      "notch": {"type": "filter", "mode": "notch", "input": "sq", "cutoff": 1200, "q": 2},
      "m":     {"type": "mix", "inputs": ["lp", {"node": "notch", "weight": 0.5}, {"node": "sn", "weight": 0.3}]},
      "fold":  {"type": "shaper", "mode": "fold", "input": "m", "drive": 1.7},
      "clip":  {"type": "shaper", "mode": "clip", "input": "fold", "drive": "de"},
      "de":    {"type": "env", "start": 0.5, "segments": [
                   {"time": 0.1, "level": 3, "curve": "exp"}, {"time": 0.2, "level": 1.2},
                   {"time": 0.3, "level": 0, "curve": "decay"}], "hold": 1, "gate": 0.35},
      "fmv":   {"type": "fm", "freq": "vib", "ratio": "q", "index": 2, "gain": 0.2},
      "sum":   {"type": "mix", "inputs": ["clip", "fmv"]},
      "amp":   {"type": "mul", "a": "sum", "b": "de", "gain": 0.4}
    }, "output": "amp"},
  "b": {"offset": {"value": 0.05, "jitter": 0.5}, "gain": 0.5, "nodes": {
      "br": {"type": "noise", "color": "brown", "gain": "be"},
      "be": {"type": "env", "attack": 0.01, "decay": 0.1, "sustain": 0.3, "release": 0.2, "gate": 0.2},
      "cb": {"type": "comb", "input": "br", "freq": {"value": 180, "jitter": 0.05}, "feedback": 0.85, "damp": 0.3},
      "hp": {"type": "filter", "mode": "highpass", "input": "cb", "cutoff": 90}
    }, "output": "hp"}
}, "duration": 1.2})";

} // namespace synthtest
