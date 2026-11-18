// An OfflineAudioContext's node graph and its render quanta
// (host_audio_offline.h). The graph is read from the JS objects at the start
// of each render segment (the context's `_nodes`, each node's `_targets` and
// targetPorts, its params, buffers and curves), so whatever a script changed
// while the context was suspended is what the next quanta render. Nodes that
// reach the destination are processed in dependency order; a DelayNode in a
// cycle reads its line before the quantum's input is written (at least a
// quantum of delay), and a cycle without one is silent, as Web Audio says.

#include "host_audio_offline.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace broaudio::api {

// ---- Channel mixing (Web Audio, "Channel Up-Mixing and Down-Mixing") ----------

void mixInto(const AudioBus& in, AudioBus& out, bool discrete) {
    const int ni = in.channels, no = out.channels;
    auto add = [&](int oc, int ic, float g) {
        float* d = out.channel(oc);
        const float* s = in.channel(ic);
        for (int i = 0; i < kRenderQuantum; ++i) d[i] += s[i] * g;
    };
    if (ni == no) {
        for (int c = 0; c < ni; ++c) add(c, c, 1.0f);
        return;
    }
    const float r = static_cast<float>(std::sqrt(0.5));
    if (!discrete) {
        if (ni == 1 && no == 2) { add(0, 0, 1.0f); add(1, 0, 1.0f); return; }
        if (ni == 1 && no == 4) { add(0, 0, 1.0f); add(1, 0, 1.0f); return; }
        if (ni == 1 && no == 6) { add(2, 0, 1.0f); return; }
        if (ni == 2 && (no == 4 || no == 6)) { add(0, 0, 1.0f); add(1, 1, 1.0f); return; }
        if (ni == 4 && no == 6) { add(0, 0, 1.0f); add(1, 1, 1.0f); add(4, 2, 1.0f); add(5, 3, 1.0f); return; }
        if (ni == 2 && no == 1) { add(0, 0, 0.5f); add(0, 1, 0.5f); return; }
        if (ni == 4 && no == 1) { for (int c = 0; c < 4; ++c) add(0, c, 0.25f); return; }
        if (ni == 6 && no == 1) { add(0, 0, r); add(0, 1, r); add(0, 2, 1.0f); add(0, 4, 0.5f); add(0, 5, 0.5f); return; }
        if (ni == 4 && no == 2) { add(0, 0, 0.5f); add(0, 2, 0.5f); add(1, 1, 0.5f); add(1, 3, 0.5f); return; }
        if (ni == 6 && no == 2) { add(0, 0, 1.0f); add(0, 2, r); add(0, 4, r); add(1, 1, 1.0f); add(1, 2, r); add(1, 5, r); return; }
        if (ni == 6 && no == 4) { add(0, 0, 1.0f); add(0, 2, r); add(1, 1, 1.0f); add(1, 2, r); add(2, 4, 1.0f); add(3, 5, 1.0f); return; }
    }
    for (int c = 0; c < std::min(ni, no); ++c) add(c, c, 1.0f);
}

namespace {

constexpr uint32_t kMaxNodes = 1u << 16;

struct Edge {
    int src = -1;        // node index
    int output = 0;      // the source's output
};

struct ParamSlot {
    ParamRef param;
    std::vector<Edge> inputs;  // audio connected to the param
};

struct GraphNode {
    HostAudioNode* host = nullptr;
    std::vector<std::vector<Edge>> inputs;  // per input
    std::vector<ParamSlot> params;          // in the kind's order (see paramsOf)
    std::vector<AudioBus> outputs;
    // Read at segment start: a buffer source's buffer, a convolver's
    // impulse, and their rates.
    std::vector<std::vector<float>> data;
    double dataRate = 0.0;
    bool inCycle = false;     // a delay in a cycle: at least a quantum of delay
    bool muted = false;       // in a cycle without a delay
    int state = 0;            // DFS: 0 new, 1 on stack, 2 done
};

int numInputs(const HostAudioNode& n) {
    switch (n.nodeType) {
    case AudioNodeType::Oscillator:
    case AudioNodeType::BufferSource:
    case AudioNodeType::ConstantSource:
    case AudioNodeType::MediaStreamSource:
        return 0;
    case AudioNodeType::ChannelMerger:
        return reinterpret_cast<const HostChannelMergerNode&>(n).numberOfInputs;
    default:
        return 1;
    }
}

int numOutputs(const HostAudioNode& n) {
    switch (n.nodeType) {
    case AudioNodeType::Destination:
        return 0;
    case AudioNodeType::ChannelSplitter:
        return reinterpret_cast<const HostChannelSplitterNode&>(n).numberOfOutputs;
    default:
        return 1;
    }
}

// The node's params, in the order the processing below reads them.
std::vector<ParamRef> paramsOf(HostAudioNode& n) {
    switch (n.nodeType) {
    case AudioNodeType::Gain:
        return {reinterpret_cast<HostGainNode&>(n).gainParam};
    case AudioNodeType::Oscillator: {
        auto& o = reinterpret_cast<HostOscillatorNode&>(n);
        return {o.frequencyParam, o.detuneParam};
    }
    case AudioNodeType::ConstantSource:
        return {reinterpret_cast<HostConstantSourceNode&>(n).offsetParam};
    case AudioNodeType::BufferSource: {
        auto& b = reinterpret_cast<HostAudioBufferSourceNode&>(n);
        return {b.playbackRateParam, b.detuneParam};
    }
    case AudioNodeType::BiquadFilter: {
        auto& b = reinterpret_cast<HostBiquadFilterNode&>(n);
        return {b.frequencyParam, b.detuneParam, b.qParam, b.gainParam};
    }
    case AudioNodeType::Delay:
        return {reinterpret_cast<HostDelayNode&>(n).delayTimeParam};
    case AudioNodeType::StereoPanner:
        return {reinterpret_cast<HostStereoPannerNode&>(n).panParam};
    case AudioNodeType::Panner: {
        auto& p = reinterpret_cast<HostPannerNode&>(n);
        return {p.positionParams[0], p.positionParams[1], p.positionParams[2],
                p.orientationParams[0], p.orientationParams[1], p.orientationParams[2]};
    }
    case AudioNodeType::DynamicsCompressor: {
        auto& c = reinterpret_cast<HostDynamicsCompressorNode&>(n);
        return {c.thresholdParam, c.kneeParam, c.ratioParam, c.attackParam, c.releaseParam};
    }
    default:
        return {};
    }
}

// A buffer's channels as the script last left them: a getChannelData view
// is the channel's live storage.
void readBuffer(Value bufferObj, std::vector<std::vector<float>>& out, double& rate) {
    out.clear();
    HostAudioBuffer* b = hostAudioBufferOf(bufferObj);
    if (!b) return;
    rate = b->sampleRate;
    out.resize(static_cast<size_t>(b->numberOfChannels));
    for (int c = 0; c < b->numberOfChannels; ++c) {
        auto& ch = out[static_cast<size_t>(c)];
        Value view = audioBufferChannelView(bufferObj, c);
        ev::TypedArrayInfo info = ev::isTypedArray(view) ? ev::typedArrayInfo(view) : ev::TypedArrayInfo{};
        if (info && info.data) {
            const float* p = reinterpret_cast<const float*>(info.data);
            ch.assign(p, p + std::min<size_t>(info.elementCount, static_cast<size_t>(b->length)));
        } else if (c < static_cast<int>(b->channels.size())) {
            ch = b->channels[static_cast<size_t>(c)];
        }
        ch.resize(static_cast<size_t>(b->length), 0.0f);
    }
}

struct Graph {
    std::vector<GraphNode> nodes;
    int destination = -1;
    std::vector<int> order;        // processing order
    ListenerState listener;
    std::vector<ParamRef> listenerParams;  // position, forward, up (9)
};

// Depth-first from `root` over inputs and param inputs, appending to `order`
// in dependency order. An edge back to a node on the stack closes a cycle:
// through a DelayNode it reads the previous quantum; without one, the cycle's nodes
// are muted and the walk must start again (returns true).
bool visit(Graph& g, int root, std::vector<int>& stack) {
    g.nodes[static_cast<size_t>(root)].state = 1;
    stack.push_back(root);
    auto walk = [&](Edge& e) {
        GraphNode& src = g.nodes[static_cast<size_t>(e.src)];
        if (src.state == 2) return false;
        if (src.state == 0) return visit(g, e.src, stack);
        // On the stack: the cycle is the stack from it.
        auto from = std::find(stack.begin(), stack.end(), e.src);
        bool delay = false;
        for (auto it = from; it != stack.end(); ++it)
            delay = delay || g.nodes[static_cast<size_t>(*it)].host->nodeType == AudioNodeType::Delay;
        if (delay) {
            // The source is ordered after this reader, so the edge carries
            // the source's previous quantum.
            for (auto it = from; it != stack.end(); ++it) g.nodes[static_cast<size_t>(*it)].inCycle = true;
            return false;
        }
        for (auto it = from; it != stack.end(); ++it) g.nodes[static_cast<size_t>(*it)].muted = true;
        return true;
    };
    GraphNode& n = g.nodes[static_cast<size_t>(root)];
    if (!n.muted) {
        for (auto& in : n.inputs)
            for (Edge& e : in)
                if (walk(e)) return true;
        for (auto& p : n.params)
            for (Edge& e : p.inputs)
                if (walk(e)) return true;
    }
    stack.pop_back();
    g.nodes[static_cast<size_t>(root)].state = 2;
    g.order.push_back(root);
    return false;
}

void computeOrder(Graph& g) {
    for (;;) {
        g.order.clear();
        for (auto& n : g.nodes) {
            n.state = 0;
            n.inCycle = false;
        }
        std::vector<int> stack;
        if (!visit(g, g.destination, stack)) break;
    }
}

const AudioBus* busOf(const std::vector<GraphNode>& all, const Edge& e) {
    const GraphNode& s = all[static_cast<size_t>(e.src)];
    return e.output < static_cast<int>(s.outputs.size()) ? &s.outputs[static_cast<size_t>(e.output)] : nullptr;
}

// The defaults Web Audio gives each kind's channel mixing, set when an
// offline node is made (host_audio_offline.cpp) — here only the
// computed count.
int computedChannels(const GraphNode& n, const std::vector<GraphNode>& all, int input) {
    const HostAudioNode& h = *n.host;
    int maxIn = 0;
    for (const Edge& e : n.inputs[static_cast<size_t>(input)])
        if (const AudioBus* b = busOf(all, e)) maxIn = std::max(maxIn, b->channels);
    if (maxIn == 0) maxIn = 1;
    const int count = static_cast<int>(std::clamp<uint32_t>(h.channelCount, 1, 32));
    switch (h.channelCountMode) {
    case HostAudioNode::kModeExplicit: return count;
    case HostAudioNode::kModeClampedMax: return std::min(maxIn, count);
    default: return maxIn;
    }
}

void mixInput(const GraphNode& n, const std::vector<GraphNode>& all, int input, AudioBus& bus) {
    bus.setSilent(computedChannels(n, all, input));
    for (const Edge& e : n.inputs[static_cast<size_t>(input)])
        if (const AudioBus* b = busOf(all, e)) mixInto(*b, bus, n.host->discrete);
}

void paramValues(const GraphNode& n, const std::vector<GraphNode>& all, size_t i, const KernelContext& k,
                 float* out) {
    const ParamSlot& p = n.params[i];
    if (!p.param) {
        std::fill(out, out + kRenderQuantum, 0.0f);
        return;
    }
    if (p.inputs.empty()) {
        computeParam(*p.param, nullptr, k.time0, k.sampleRate, out);
        return;
    }
    AudioBus in;
    in.setSilent(1);
    for (const Edge& e : p.inputs) {
        if (const AudioBus* b = busOf(all, e)) {
            AudioBus mono;
            mono.setSilent(1);
            mixInto(*b, mono, false);
            mixInto(mono, in, false);
        }
    }
    computeParam(*p.param, &in, k.time0, k.sampleRate, out);
}

bool automated(const ParamSlot& p) { return p.param && (!p.param->timeline.empty() || !p.inputs.empty()); }

void processNode(Graph& g, int idx, const KernelContext& k) {
    GraphNode& n = g.nodes[static_cast<size_t>(idx)];
    HostAudioNode& h = *n.host;
    const int nOut = numOutputs(h);
    n.outputs.resize(static_cast<size_t>(std::max(nOut, 1)));
    if (n.muted) {
        for (auto& o : n.outputs) o.setSilent(1);
        return;
    }
    float p[6][kRenderQuantum];
    for (size_t i = 0; i < n.params.size() && i < 6; ++i) paramValues(n, g.nodes, i, k, p[i]);
    AudioBus in;
    if (!n.inputs.empty() && h.nodeType != AudioNodeType::ChannelMerger) mixInput(n, g.nodes, 0, in);
    AudioBus& out = n.outputs[0];
    switch (h.nodeType) {
    case AudioNodeType::Gain:
        processGain(in, p[0], out);
        break;
    case AudioNodeType::Oscillator:
        processOscillator(reinterpret_cast<HostOscillatorNode&>(h), p[0], p[1], k, out);
        break;
    case AudioNodeType::ConstantSource:
        processConstantSource(reinterpret_cast<HostConstantSourceNode&>(h), p[0], k, out);
        break;
    case AudioNodeType::BufferSource:
        processBufferSource(reinterpret_cast<HostAudioBufferSourceNode&>(h), n.data, n.dataRate, p[0][0], p[1][0], k, out);
        break;
    case AudioNodeType::BiquadFilter: {
        const bool perSample = automated(n.params[0]) || automated(n.params[1]) || automated(n.params[2]) ||
                               automated(n.params[3]);
        processBiquad(reinterpret_cast<HostBiquadFilterNode&>(h), in, p[0], p[1], p[2], p[3], perSample, k, out);
        break;
    }
    case AudioNodeType::IIRFilter:
        processIIR(reinterpret_cast<HostIIRFilterNode&>(h), in, out);
        break;
    case AudioNodeType::Delay: {
        auto& d = reinterpret_cast<HostDelayNode&>(h);
        delayWrite(d, in, k);
        delayRead(d, p[0], n.inCycle, k, out);
        break;
    }
    case AudioNodeType::StereoPanner:
        processStereoPanner(in, p[0], out);
        break;
    case AudioNodeType::Panner:
        processPanner(reinterpret_cast<HostPannerNode&>(h), in, g.listener, k, out);
        break;
    case AudioNodeType::WaveShaper:
        processWaveShaper(reinterpret_cast<HostWaveShaperNode&>(h), in, out);
        break;
    case AudioNodeType::Convolver:
        processConvolver(reinterpret_cast<HostConvolverNode&>(h), n.data, n.dataRate, in, out);
        break;
    case AudioNodeType::DynamicsCompressor:
        processCompressor(reinterpret_cast<HostDynamicsCompressorNode&>(h), in, k, out);
        break;
    case AudioNodeType::ChannelSplitter:
        for (int o = 0; o < nOut; ++o) {
            AudioBus& b = n.outputs[static_cast<size_t>(o)];
            b.setSilent(1);
            if (o < in.channels) std::copy(in.channel(o), in.channel(o) + kRenderQuantum, b.channel(0));
        }
        break;
    case AudioNodeType::ChannelMerger: {
        const int nIn = static_cast<int>(n.inputs.size());
        out.setSilent(nIn);
        for (int i = 0; i < nIn; ++i) {
            AudioBus one;
            mixInput(n, g.nodes, i, one);
            AudioBus mono;
            mono.setSilent(1);
            mixInto(one, mono, false);
            std::copy(mono.channel(0), mono.channel(0) + kRenderQuantum, out.channel(i));
        }
        break;
    }
    case AudioNodeType::Destination:
        out = std::move(in);
        break;
    default:  // the analyser, and anything else, passes its input through
        out = std::move(in);
        break;
    }
}

Graph buildGraph(Value context, double sampleRate) {
    Graph g;
    ev::Persistent ctx(context);
    std::unordered_map<HostAudioNode*, int> index;
    std::unordered_map<HostAudioParam*, std::pair<int, size_t>> paramIndex;
    std::vector<ev::Persistent> objects;
    auto addNode = [&](Value obj) {
        HostAudioNode* h = hostAudioNodeOf(obj);
        if (!h || index.count(h) || g.nodes.size() >= kMaxNodes) return;
        index[h] = static_cast<int>(g.nodes.size());
        GraphNode n;
        n.host = h;
        n.inputs.resize(static_cast<size_t>(numInputs(*h)));
        for (ParamRef& p : paramsOf(*h)) {
            ParamSlot slot;
            slot.param = p;
            n.params.push_back(std::move(slot));
        }
        g.nodes.push_back(std::move(n));
        objects.emplace_back(obj);
    };
    addNode(ev::getProperty(ctx.get(), "destination"));
    g.destination = 0;
    {
        ev::Persistent list(ev::getProperty(ctx.get(), "_nodes"));
        Value lenV = ev::isObject(list.get()) ? ev::getProperty(list.get(), "length") : ev::undefined();
        const uint32_t len = ev::isNumber(lenV) ? std::min<uint32_t>(saturateU32(ev::toDouble(lenV)), kMaxNodes) : 0;
        for (uint32_t i = 0; i < len; ++i) addNode(ev::getElement(list.get(), i));
    }
    for (size_t i = 0; i < g.nodes.size(); ++i)
        for (size_t j = 0; j < g.nodes[i].params.size(); ++j)
            if (g.nodes[i].params[j].param) paramIndex[g.nodes[i].params[j].param.get()] = {static_cast<int>(i), j};

    // Edges, and what each node reads at the segment's start.
    for (size_t i = 0; i < objects.size(); ++i) {
        HostAudioNode* h = g.nodes[i].host;
        std::vector<ev::Persistent> targets = connectTargetsOf(objects[i].get());
        for (size_t t = 0; t < targets.size(); ++t) {
            const uint32_t port = t < h->targetPorts.size() ? h->targetPorts[t] : 0;
            const Edge e{static_cast<int>(i), static_cast<int>(port >> 16)};
            if (HostAudioNode* dst = hostAudioNodeOf(targets[t].get())) {
                auto it = index.find(dst);
                if (it == index.end()) continue;
                auto& ins = g.nodes[static_cast<size_t>(it->second)].inputs;
                const size_t input = port & 0xffff;
                if (input < ins.size()) ins[input].push_back(e);
            } else if (HostAudioParam* p = hostAudioParamOf(targets[t].get())) {
                auto it = paramIndex.find(p);
                if (it != paramIndex.end())
                    g.nodes[static_cast<size_t>(it->second.first)].params[it->second.second].inputs.push_back(e);
            }
        }
        if (h->nodeType == AudioNodeType::BufferSource || h->nodeType == AudioNodeType::Convolver) {
            ev::Persistent buf(ev::getProperty(objects[i].get(), "_buffer"));
            readBuffer(buf.get(), g.nodes[i].data, g.nodes[i].dataRate);
        }
    }

    // The listener: its params, evaluated at each quantum's start.
    ev::Persistent listener(ev::getProperty(ctx.get(), "listener"));
    if (ev::isObject(listener.get())) {
        for (const char* name : {"positionX", "positionY", "positionZ", "forwardX", "forwardY", "forwardZ", "upX",
                                 "upY", "upZ"}) {
            g.listenerParams.push_back(hostAudioParamRef(ev::getProperty(listener.get(), name)));
        }
    }
    computeOrder(g);
    (void)sampleRate;
    return g;
}

}  // namespace

void renderOfflineGraph(Value context, OfflineRender& render, int64_t endFrame) {
    Graph g = buildGraph(context, render.sampleRate);
    if (g.destination < 0) return;
    KernelContext k;
    k.sampleRate = render.sampleRate;
    while (render.frame < endFrame) {
        k.frame0 = render.frame;
        k.time0 = static_cast<double>(render.frame) / render.sampleRate;
        render.clock->time = k.time0;
        if (g.listenerParams.size() == 9) {
            double* fields[3] = {g.listener.pos, g.listener.forward, g.listener.up};
            for (int i = 0; i < 9; ++i)
                if (g.listenerParams[static_cast<size_t>(i)])
                    fields[i / 3][i % 3] = g.listenerParams[static_cast<size_t>(i)]->evaluate(k.time0);
        }
        for (int idx : g.order) processNode(g, idx, k);
        // The destination's input, mixed to the context's channels.
        GraphNode& dest = g.nodes[static_cast<size_t>(g.destination)];
        AudioBus out;
        out.setSilent(render.channels);
        if (!dest.outputs.empty()) mixInto(dest.outputs[0], out, dest.host->discrete);
        const int64_t n = std::min<int64_t>(kRenderQuantum, render.length - render.frame);
        for (int c = 0; c < render.channels; ++c)
            std::copy(out.channel(c), out.channel(c) + n, render.output[static_cast<size_t>(c)].begin() + render.frame);
        render.frame += n;
    }
    render.clock->time = static_cast<double>(render.frame) / render.sampleRate;
}

}  // namespace broaudio::api
