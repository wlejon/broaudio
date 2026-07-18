#include "test_harness.h"
#include "broaudio/engine.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace broaudio;

static constexpr float PI = std::numbers::pi_v<float>;

static std::vector<float> sineMono(int frames, float freq, int sr) {
    std::vector<float> v(frames);
    for (int i = 0; i < frames; i++)
        v[i] = std::sin(2.0f * PI * freq * i / sr);
    return v;
}

// ---------------------------------------------------------------------------
// Bus deletion
// ---------------------------------------------------------------------------

TEST(create_and_delete_bus) {
    Engine e; e.initHeadless();
    int b = e.createBus();
    ASSERT_TRUE(b > 0);
    e.deleteBus(b);
    // Bus actions on deleted bus should be no-ops (don't crash)
    e.setBusGain(b, 0.5f);
    e.setBusMuted(b, true);
    PASS();
}

TEST(delete_master_bus_is_noop) {
    Engine e; e.initHeadless();
    e.deleteBus(Engine::MASTER_BUS_ID);
    // Master should still work
    e.setBusGain(Engine::MASTER_BUS_ID, 0.7f);
    e.setMasterGain(1.0f);
    PASS();
}

TEST(set_bus_muted_silences_output) {
    Engine e; e.initHeadless();
    int b = e.createBus();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceBus(v, b);
    e.startVoice(v, 0.0);

    e.renderBlock(4096);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);

    e.setBusMuted(b, true);
    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);

    e.setBusMuted(b, false);
    PASS();
}

// ---------------------------------------------------------------------------
// Bus solo
// ---------------------------------------------------------------------------

// Helper: loud sine voice routed to `bus`.
static int makeVoiceOnBus(broaudio::Engine& e, int bus) {
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceBus(v, bus);
    e.startVoice(v, 0.0);
    return v;
}

TEST(solo_silences_non_soloed_buses) {
    Engine e; e.initHeadless();
    int a = e.createBus();
    int b = e.createBus();
    makeVoiceOnBus(e, a);           // only bus A carries signal

    e.renderBlock(4096);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);

    // Solo the SILENT bus B: A must stop reaching master.
    e.setBusSolo(b, true);
    ASSERT_TRUE(e.getBusSolo(b));
    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);

    // Releasing solo restores A.
    e.setBusSolo(b, false);
    ASSERT_FALSE(e.getBusSolo(b));
    e.renderBlock(4096);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

TEST(soloed_bus_stays_audible) {
    Engine e; e.initHeadless();
    int a = e.createBus();
    makeVoiceOnBus(e, a);

    e.setBusSolo(a, true);
    e.renderBlock(4096);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

TEST(mute_wins_over_solo) {
    Engine e; e.initHeadless();
    int a = e.createBus();
    makeVoiceOnBus(e, a);

    e.setBusSolo(a, true);
    e.setBusMuted(a, true);
    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);
    PASS();
}

TEST(delete_soloed_bus_releases_solo) {
    Engine e; e.initHeadless();
    int a = e.createBus();
    int b = e.createBus();
    makeVoiceOnBus(e, a);

    e.setBusSolo(b, true);          // silences A
    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);

    e.deleteBus(b);                 // deleting the soloed bus lifts solo mode
    e.renderBlock(4096);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

TEST(solo_gates_aux_sends) {
    Engine e; e.initHeadless();
    int a = e.createBus();          // signal bus, sends into fx
    int fx = e.createBus();
    int other = e.createBus();
    makeVoiceOnBus(e, a);
    e.setBusSend(a, fx, 1.0f);

    e.setBusSolo(other, true);      // solo an unrelated silent bus
    e.renderBlock(4096);
    e.renderBlock(2048);
    // Neither A's direct path nor its send may reach master.
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);
    PASS();
}

TEST(set_bus_solo_invalid_id_is_safe) {
    Engine e; e.initHeadless();
    e.setBusSolo(4242, true);
    ASSERT_FALSE(e.getBusSolo(4242));
    int a = e.createBus();
    makeVoiceOnBus(e, a);
    e.renderBlock(4096);
    // No stray solo count was accumulated — audio still flows.
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    PASS();
}

// ---------------------------------------------------------------------------
// Voice removal
// ---------------------------------------------------------------------------

TEST(remove_voice_silences) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.startVoice(v, 0.0);
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);

    e.removeVoice(v);
    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_LT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.05f);
    PASS();
}

TEST(remove_voice_invalid_id_safe) {
    Engine e; e.initHeadless();
    e.removeVoice(9999);
    PASS();
}

// ---------------------------------------------------------------------------
// Sends
// ---------------------------------------------------------------------------

TEST(voice_send_to_other_bus_adds_to_send_bus) {
    Engine e; e.initHeadless();
    int sendBus = e.createBus();

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSend(v, sendBus, 1.0f);  // full send to sendBus
    e.startVoice(v, 0.0);

    e.renderBlock(4096);
    e.renderBlock(2048);
    // Both master AND send bus should have signal
    ASSERT_GT(e.getBusPeakL(Engine::MASTER_BUS_ID), 0.01f);
    ASSERT_GT(e.getBusPeakL(sendBus), 0.01f);
    PASS();
}

TEST(voice_send_amount_clamps_to_unit) {
    Engine e; e.initHeadless();
    int sendBus = e.createBus();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceSend(v, sendBus, 99.0f);  // clamp to 1.0
    e.setVoiceSend(v, sendBus, -5.0f);  // clamp to 0
    e.setVoiceSend(v, sendBus, 0.5f);
    e.startVoice(v, 0.0);
    e.renderBlock(2048);
    PASS();
}

TEST(playback_send_to_other_bus) {
    Engine e; e.initHeadless();
    int sendBus = e.createBus();
    auto sine = sineMono(e.sampleRate(), 440.0f, e.sampleRate());
    int cid = e.createClip(sine.data(), (int)sine.size(), 1);
    int pid = e.playClip(cid, 1.0f, true);

    e.setPlaybackSend(pid, sendBus, 1.0f);
    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(sendBus), 0.01f);
    PASS();
}

TEST(bus_send_routes_to_send_bus) {
    Engine e; e.initHeadless();
    int srcBus = e.createBus();
    int sendBus = e.createBus();

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);
    e.setVoiceBus(v, srcBus);
    e.startVoice(v, 0.0);

    e.setBusSend(srcBus, sendBus, 1.0f);

    e.renderBlock(4096);
    e.renderBlock(2048);
    ASSERT_GT(e.getBusPeakL(sendBus), 0.01f);
    PASS();
}

TEST(invalid_send_ids_dont_crash) {
    Engine e; e.initHeadless();
    e.setVoiceSend(9999, 0, 0.5f);
    e.setPlaybackSend(9999, 0, 0.5f);
    e.setBusSend(9999, 0, 0.5f);
    PASS();
}

// ---------------------------------------------------------------------------
// Effect order
// ---------------------------------------------------------------------------

TEST(set_bus_effect_order_default_chain) {
    Engine e; e.initHeadless();
    EffectSlot order[] = {
        EffectSlot::Reverb, EffectSlot::Compressor, EffectSlot::Filter,
        EffectSlot::Delay,  EffectSlot::Chorus, EffectSlot::Distortion,
        EffectSlot::Equalizer
    };
    e.setBusEffectOrder(Engine::MASTER_BUS_ID, order, 7);

    // Enable one effect to actually traverse the chain
    e.setBusReverbEnabled(Engine::MASTER_BUS_ID, true);

    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 0.5f);
    e.startVoice(v, 0.0);
    e.renderBlock(4096);
    PASS();
}

TEST(set_bus_effect_order_invalid_bus_is_safe) {
    Engine e; e.initHeadless();
    EffectSlot order[] = {EffectSlot::Reverb};
    e.setBusEffectOrder(9999, order, 1);
    PASS();
}

// ---------------------------------------------------------------------------
// Scheduled events
// ---------------------------------------------------------------------------

TEST(schedule_note_on_off_produces_audio) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 440.0f);
    e.setGain(v, 1.0f);

    // Schedule events in the near future
    e.scheduleNoteOn(v, 0.01);
    e.scheduleNoteOff(v, 0.05);

    // Render ~0.1s and confirm we got non-zero output at some point
    e.renderBlock(e.sampleRate() / 10);  // 0.1s
    float peakHistory = e.getBusPeakL(Engine::MASTER_BUS_ID);
    // The peak window decays — check that *something* was emitted
    e.renderBlock(2048);
    ASSERT_TRUE(peakHistory > 0.0f || e.getBusPeakL(Engine::MASTER_BUS_ID) > 0.0f);
    PASS();
}

TEST(schedule_overflow_does_not_crash) {
    Engine e; e.initHeadless();
    int v = e.createVoice();
    e.setWaveform(v, Waveform::Sine);
    e.setFrequency(v, 220.0f);
    e.setGain(v, 0.1f);

    // Spam events to test ring buffer full behaviour
    for (int i = 0; i < 5000; i++) {
        e.scheduleNoteOn(v, i * 0.001);
        e.scheduleNoteOff(v, i * 0.001 + 0.0005);
    }
    e.renderBlock(1024);
    PASS();
}

int main() { return runAllTests(); }
