#include "test_harness.h"
#include "broaudio/types.h"
#include "broaudio/mix/bus.h"

using namespace broaudio;

// --- Default order ---

TEST(default_order_matches_expected) {
    Bus bus;
    ASSERT_EQ(bus.effectOrder[0].load(), static_cast<uint8_t>(EffectSlot::Filter));
    ASSERT_EQ(bus.effectOrder[1].load(), static_cast<uint8_t>(EffectSlot::Delay));
    ASSERT_EQ(bus.effectOrder[2].load(), static_cast<uint8_t>(EffectSlot::Compressor));
    ASSERT_EQ(bus.effectOrder[3].load(), static_cast<uint8_t>(EffectSlot::Chorus));
    ASSERT_EQ(bus.effectOrder[4].load(), static_cast<uint8_t>(EffectSlot::Reverb));
    PASS();
}

TEST(default_cache_matches_order) {
    Bus bus;
    ASSERT_EQ(bus.effectOrderCache[0], 0);
    ASSERT_EQ(bus.effectOrderCache[1], 1);
    ASSERT_EQ(bus.effectOrderCache[2], 2);
    ASSERT_EQ(bus.effectOrderCache[3], 3);
    ASSERT_EQ(bus.effectOrderCache[4], 4);
    ASSERT_EQ(bus.effectOrderCache[5], 5);
    PASS();
}

// --- EffectSlot enum ---

TEST(effect_slot_count_is_six) {
    ASSERT_EQ(static_cast<int>(EffectSlot::Count), 6);
    PASS();
}

TEST(num_effect_slots_matches_count) {
    ASSERT_EQ(Bus::NUM_EFFECT_SLOTS, static_cast<int>(EffectSlot::Count));
    PASS();
}

// --- Reordering ---

TEST(set_custom_order) {
    Bus bus;
    // Reverb first, then filter, delay, compressor, chorus, eq
    bus.effectOrder[0].store(static_cast<uint8_t>(EffectSlot::Reverb));
    bus.effectOrder[1].store(static_cast<uint8_t>(EffectSlot::Filter));
    bus.effectOrder[2].store(static_cast<uint8_t>(EffectSlot::Delay));
    bus.effectOrder[3].store(static_cast<uint8_t>(EffectSlot::Compressor));
    bus.effectOrder[4].store(static_cast<uint8_t>(EffectSlot::Chorus));
    bus.effectOrder[5].store(static_cast<uint8_t>(EffectSlot::Equalizer));
    bus.effectOrderVersion.fetch_add(1);

    ASSERT_EQ(bus.effectOrder[0].load(), static_cast<uint8_t>(EffectSlot::Reverb));
    ASSERT_EQ(bus.effectOrder[1].load(), static_cast<uint8_t>(EffectSlot::Filter));
    ASSERT_GT(bus.effectOrderVersion.load(), 0u);
    PASS();
}

TEST(version_counter_increments) {
    Bus bus;
    uint32_t v0 = bus.effectOrderVersion.load();
    bus.effectOrderVersion.fetch_add(1);
    uint32_t v1 = bus.effectOrderVersion.load();
    ASSERT_EQ(v1, v0 + 1);
    PASS();
}

// --- Init state ---

TEST(init_audio_state_preserves_order) {
    Bus bus;
    bus.effectOrder[0].store(static_cast<uint8_t>(EffectSlot::Chorus));
    bus.initAudioState(44100, 512);
    // initAudioState should not reset the effect order
    ASSERT_EQ(bus.effectOrder[0].load(), static_cast<uint8_t>(EffectSlot::Chorus));
    PASS();
}

int main() { return runAllTests(); }
