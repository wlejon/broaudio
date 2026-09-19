// `audioContext.destination` is a real AudioDestinationNode, not a bare
// object: it carries the AudioDestinationNode prototype, which inherits
// AudioNode's, so `instanceof` answers for both and connect/disconnect are
// still the single base methods every node shares.
//
// No engine is installed — nothing here renders audio, it only reads the
// prototype chain and connects a node to the destination.

#include "api.h"
#include "embed/embed.h"
#include "eval/eval.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace ev = bronze::embed;

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::exit(1); \
    } \
} while (0)

static void runScript(const char* label, const char* body) {
    std::cout << "  " << label << "..." << std::endl;
    std::string script = std::string("(function() {") + body + "\n})()";
    ev::CallResult res = bronze::eval::evalScript(script);
    if (res.thrown) {
        std::cerr << label << " threw: " << ev::toUtf8(res.value) << std::endl;
        std::exit(1);
    }
    std::string out = ev::toUtf8(res.value);
    if (out != "SUCCESS") {
        std::cerr << label << " returned: " << out << std::endl;
        std::exit(1);
    }
}

static void test_destination_is_a_node() {
    runScript("destination prototype chain", R"JS(
        if (typeof AudioDestinationNode !== "function")
            throw new Error("AudioDestinationNode is not a global constructor");

        const ctx = new AudioContext();
        const dest = ctx.destination;

        if (typeof dest !== "object" || dest === null)
            throw new Error("destination is not an object");
        if (!(dest instanceof AudioDestinationNode))
            throw new Error("destination is not an AudioDestinationNode");
        if (!(dest instanceof AudioNode))
            throw new Error("destination is not an AudioNode");

        // The chain is AudioDestinationNode.prototype -> AudioNode.prototype,
        // not a flattened copy of either.
        if (Object.getPrototypeOf(dest) !== AudioDestinationNode.prototype)
            throw new Error("destination's prototype is not AudioDestinationNode.prototype");
        if (Object.getPrototypeOf(AudioDestinationNode.prototype) !== AudioNode.prototype)
            throw new Error("AudioDestinationNode.prototype does not inherit AudioNode.prototype");

        // maxChannelCount came back with the class, and lives on the
        // prototype rather than on the instance.
        if (dest.maxChannelCount !== 2)
            throw new Error("maxChannelCount: " + dest.maxChannelCount);
        if (Object.prototype.hasOwnProperty.call(dest, "maxChannelCount"))
            throw new Error("maxChannelCount should not be an own property");

        // AudioNode's base surface reads through.
        if (dest.numberOfOutputs !== 0)
            throw new Error("destination numberOfOutputs: " + dest.numberOfOutputs);
        if (dest.numberOfInputs !== 1)
            throw new Error("destination numberOfInputs: " + dest.numberOfInputs);
        if (dest.channelCount !== 2)
            throw new Error("destination channelCount: " + dest.channelCount);

        // connect/disconnect are the one pair on AudioNode.prototype, shared
        // with every other node (bro's bronze_host `class` check pins this).
        const osc = ctx.createOscillator();
        if (dest.connect !== osc.connect)
            throw new Error("destination.connect is not the AudioNode.prototype method");
        if (dest.disconnect !== osc.disconnect)
            throw new Error("destination.disconnect is not the AudioNode.prototype method");

        return "SUCCESS";
    )JS");
}

static void test_connect_to_destination() {
    runScript("connect to destination", R"JS(
        const ctx = new AudioContext();

        // The Web Audio spelling: connect returns its argument, so it chains.
        const osc = ctx.createOscillator();
        if (osc.connect(ctx.destination) !== ctx.destination)
            throw new Error("osc.connect(ctx.destination) did not return the destination");
        osc.disconnect();

        // Through a gain, the shape apps actually write.
        const gain = ctx.createGain();
        gain.gain.value = 0.25;
        if (osc.connect(gain) !== gain)
            throw new Error("osc.connect(gain) did not return the gain");
        if (gain.connect(ctx.destination) !== ctx.destination)
            throw new Error("gain.connect(ctx.destination) did not return the destination");
        gain.disconnect();
        osc.disconnect();

        // Every context hands out its own destination instance.
        const other = new AudioContext();
        if (other.destination === ctx.destination)
            throw new Error("two contexts share one destination object");
        if (!(other.destination instanceof AudioDestinationNode))
            throw new Error("second context's destination is not an AudioDestinationNode");

        return "SUCCESS";
    )JS");
}

int main() {
    std::cout << "Running broaudio AudioDestinationNode test..." << std::endl;

    ev::Realm* realm = ev::createRealm();
    {
        ev::RealmScope scope(realm);
        broaudio::api::installAudio();
        test_destination_is_a_node();
        test_connect_to_destination();
    }
    ev::destroyRealm(realm);
    broaudio::api::shutdownAudio();

    std::cout << "All broaudio AudioDestinationNode tests passed!" << std::endl;
    return 0;
}
