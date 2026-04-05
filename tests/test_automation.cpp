#include "test_harness.h"
#include "broaudio/sequencer/automation.h"
#include "broaudio/sequencer/sequence.h"
#include "broaudio/synth/voice_allocator.h"
#include "broaudio/engine.h"

using namespace broaudio;

// --- AutomationLane unit tests ---

TEST(empty_lane_no_op) {
    float captured = -999.0f;
    AutomationLane lane([&](float v) { captured = v; });
    lane.apply(0.0);
    // Should not call apply when empty
    ASSERT_NEAR(captured, -999.0f, 1e-9);
    PASS();
}

TEST(single_point_returns_value) {
    AutomationLane lane([](float) {});
    lane.addPoint(2.0, 0.75f);
    ASSERT_NEAR(lane.evaluate(0.0), 0.75f, 1e-6);
    ASSERT_NEAR(lane.evaluate(2.0), 0.75f, 1e-6);
    ASSERT_NEAR(lane.evaluate(10.0), 0.75f, 1e-6);
    PASS();
}

TEST(linear_midpoint) {
    AutomationLane lane([](float) {});
    lane.setInterpMode(InterpMode::Linear);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    ASSERT_NEAR(lane.evaluate(2.0), 0.5f, 1e-6);
    PASS();
}

TEST(linear_quarter_points) {
    AutomationLane lane([](float) {});
    lane.setInterpMode(InterpMode::Linear);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    ASSERT_NEAR(lane.evaluate(1.0), 0.25f, 1e-6);
    ASSERT_NEAR(lane.evaluate(3.0), 0.75f, 1e-6);
    PASS();
}

TEST(step_holds_previous) {
    AutomationLane lane([](float) {});
    lane.setInterpMode(InterpMode::Step);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    ASSERT_NEAR(lane.evaluate(0.0), 0.0f, 1e-6);
    ASSERT_NEAR(lane.evaluate(2.0), 0.0f, 1e-6);
    ASSERT_NEAR(lane.evaluate(3.99), 0.0f, 1e-6);
    ASSERT_NEAR(lane.evaluate(4.0), 1.0f, 1e-6);
    PASS();
}

TEST(smooth_midpoint_between_endpoints) {
    AutomationLane lane([](float) {});
    lane.setInterpMode(InterpMode::Smooth);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    float mid = lane.evaluate(2.0);
    // Cosine interp at t=0.5 should give exactly 0.5
    ASSERT_NEAR(mid, 0.5f, 1e-4);
    // Quarter point should be between 0 and 0.5 (ease-in)
    float quarter = lane.evaluate(1.0);
    ASSERT_GT(quarter, 0.0f);
    ASSERT_LT(quarter, 0.5f);
    PASS();
}

TEST(clamp_before_first_point) {
    AutomationLane lane([](float) {});
    lane.addPoint(2.0, 0.5f);
    lane.addPoint(4.0, 1.0f);
    ASSERT_NEAR(lane.evaluate(0.0), 0.5f, 1e-6);
    ASSERT_NEAR(lane.evaluate(1.0), 0.5f, 1e-6);
    PASS();
}

TEST(clamp_after_last_point) {
    AutomationLane lane([](float) {});
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 0.8f);
    ASSERT_NEAR(lane.evaluate(5.0), 0.8f, 1e-6);
    ASSERT_NEAR(lane.evaluate(100.0), 0.8f, 1e-6);
    PASS();
}

TEST(points_sorted_after_add) {
    AutomationLane lane([](float) {});
    lane.addPoint(4.0, 1.0f);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 0.5f);
    ASSERT_NEAR(lane.point(0).beat, 0.0, 1e-9);
    ASSERT_NEAR(lane.point(1).beat, 2.0, 1e-9);
    ASSERT_NEAR(lane.point(2).beat, 4.0, 1e-9);
    PASS();
}

TEST(remove_point) {
    AutomationLane lane([](float) {});
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 0.5f);
    lane.addPoint(4.0, 1.0f);
    lane.removePoint(1);
    ASSERT_EQ(lane.pointCount(), 2);
    ASSERT_NEAR(lane.point(1).beat, 4.0, 1e-9);
    PASS();
}

TEST(set_point_value) {
    AutomationLane lane([](float) {});
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    lane.setPointValue(0, 0.25f);
    ASSERT_NEAR(lane.point(0).value, 0.25f, 1e-6);
    PASS();
}

TEST(clear_points) {
    AutomationLane lane([](float) {});
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    lane.clearPoints();
    ASSERT_EQ(lane.pointCount(), 0);
    PASS();
}

TEST(apply_calls_callback) {
    float captured = -1.0f;
    AutomationLane lane([&](float v) { captured = v; });
    lane.setInterpMode(InterpMode::Linear);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(4.0, 1.0f);
    lane.apply(2.0);
    ASSERT_NEAR(captured, 0.5f, 1e-6);
    PASS();
}

TEST(three_segment_linear) {
    AutomationLane lane([](float) {});
    lane.setInterpMode(InterpMode::Linear);
    lane.addPoint(0.0, 0.0f);
    lane.addPoint(2.0, 1.0f);
    lane.addPoint(4.0, 0.0f);
    ASSERT_NEAR(lane.evaluate(1.0), 0.5f, 1e-6);
    ASSERT_NEAR(lane.evaluate(2.0), 1.0f, 1e-6);
    ASSERT_NEAR(lane.evaluate(3.0), 0.5f, 1e-6);
    PASS();
}

// --- Sequence integration tests ---

struct TestFixture {
    Engine engine;
    VoiceAllocator allocator;
    Sequence sequence;

    TestFixture()
        : allocator(engine, 8)
        , sequence(allocator)
    {}
};

TEST(sequence_automation_fires_during_update) {
    TestFixture f;
    f.sequence.setBPM(120.0);

    float captured = -1.0f;
    int lane = f.sequence.addAutomationLane([&](float v) { captured = v; });
    f.sequence.automationLane(lane).setInterpMode(InterpMode::Linear);
    f.sequence.automationLane(lane).addPoint(0.0, 0.0f);
    f.sequence.automationLane(lane).addPoint(2.0, 1.0f);

    f.sequence.play(0.0);
    // At 120 BPM, 0.25s = beat 0.5 -> linear interp = 0.25
    f.sequence.update(0.25);
    ASSERT_NEAR(captured, 0.25f, 1e-4);
    PASS();
}

TEST(sequence_automation_not_playing) {
    TestFixture f;
    float captured = -999.0f;
    int lane = f.sequence.addAutomationLane([&](float v) { captured = v; });
    f.sequence.automationLane(lane).addPoint(0.0, 0.5f);

    // Not playing — update should be no-op
    f.sequence.update(0.1);
    ASSERT_NEAR(captured, -999.0f, 1e-9);
    PASS();
}

TEST(sequence_automation_paused) {
    TestFixture f;
    f.sequence.setBPM(120.0);

    float captured = -1.0f;
    int lane = f.sequence.addAutomationLane([&](float v) { captured = v; });
    f.sequence.automationLane(lane).setInterpMode(InterpMode::Linear);
    f.sequence.automationLane(lane).addPoint(0.0, 0.0f);
    f.sequence.automationLane(lane).addPoint(4.0, 1.0f);

    f.sequence.play(0.0);
    f.sequence.update(0.5);  // beat 1.0 -> value 0.25
    ASSERT_NEAR(captured, 0.25f, 1e-4);

    f.sequence.pause(0.5);
    captured = -1.0f;
    f.sequence.update(5.0);  // paused — no update
    ASSERT_NEAR(captured, -1.0f, 1e-9);
    PASS();
}

TEST(sequence_automation_loops) {
    TestFixture f;
    f.sequence.setBPM(120.0);

    float captured = -1.0f;
    int lane = f.sequence.addAutomationLane([&](float v) { captured = v; });
    f.sequence.automationLane(lane).setInterpMode(InterpMode::Linear);
    f.sequence.automationLane(lane).addPoint(0.0, 0.0f);
    f.sequence.automationLane(lane).addPoint(2.0, 1.0f);

    f.sequence.setLoopEnabled(true);
    f.sequence.setLoopRange(0.0, 2.0);  // loop every 2 beats (1.0s at 120 BPM)
    f.sequence.play(0.0);

    // Beat 1.0 -> value 0.5
    f.sequence.update(0.5);
    ASSERT_NEAR(captured, 0.5f, 1e-4);

    // Hit loop boundary at 1.0s (beat 2.0) — wraps back
    f.sequence.update(1.0);

    // After wrap, update at 1.25s — beat should be ~0.5 -> value 0.25
    f.sequence.update(1.25);
    ASSERT_NEAR(captured, 0.25f, 1e-4);
    PASS();
}

TEST(sequence_multiple_lanes) {
    TestFixture f;
    f.sequence.setBPM(120.0);

    float capturedA = -1.0f, capturedB = -1.0f;
    int a = f.sequence.addAutomationLane([&](float v) { capturedA = v; });
    int b = f.sequence.addAutomationLane([&](float v) { capturedB = v; });

    f.sequence.automationLane(a).addPoint(0.0, 0.0f);
    f.sequence.automationLane(a).addPoint(4.0, 1.0f);

    f.sequence.automationLane(b).addPoint(0.0, 100.0f);
    f.sequence.automationLane(b).addPoint(4.0, 200.0f);

    f.sequence.play(0.0);
    f.sequence.update(0.5);  // beat 1.0

    ASSERT_NEAR(capturedA, 0.25f, 1e-4);
    ASSERT_NEAR(capturedB, 125.0f, 0.1f);
    PASS();
}

TEST(sequence_remove_lane) {
    TestFixture f;
    ASSERT_EQ(f.sequence.automationLaneCount(), 0);

    f.sequence.addAutomationLane([](float) {});
    f.sequence.addAutomationLane([](float) {});
    ASSERT_EQ(f.sequence.automationLaneCount(), 2);

    f.sequence.removeAutomationLane(0);
    ASSERT_EQ(f.sequence.automationLaneCount(), 1);

    f.sequence.clearAutomationLanes();
    ASSERT_EQ(f.sequence.automationLaneCount(), 0);
    PASS();
}

int main() { return runAllTests(); }
