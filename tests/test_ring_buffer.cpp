#include "test_harness.h"
#include "broaudio/core/ring_buffer.h"
#include <vector>

using namespace broaudio;

TEST(ringbuf_push_pop_single) {
    RingBuffer<int> rb(16);
    ASSERT_TRUE(rb.push(42));
    int val;
    ASSERT_TRUE(rb.pop(val));
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(ringbuf_empty_pop_fails) {
    RingBuffer<int> rb(16);
    int val;
    ASSERT_FALSE(rb.pop(val));
    PASS();
}

TEST(ringbuf_fill_to_capacity) {
    RingBuffer<int> rb(8);  // rounds up to 8
    // SPSC ring holds size-1 elements
    for (int i = 0; i < 7; i++) {
        ASSERT_TRUE(rb.push(i));
    }
    // Should be full
    ASSERT_FALSE(rb.push(999));
    PASS();
}

TEST(ringbuf_wrap_around) {
    RingBuffer<int> rb(8);
    // Fill and drain a few times to force wrap
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 5; i++) {
            ASSERT_TRUE(rb.push(round * 10 + i));
        }
        for (int i = 0; i < 5; i++) {
            int val;
            ASSERT_TRUE(rb.pop(val));
            ASSERT_EQ(val, round * 10 + i);
        }
    }
    ASSERT_TRUE(rb.empty());
    PASS();
}

TEST(ringbuf_bulk_push_pop) {
    RingBuffer<float> rb(64);

    float data[10];
    for (int i = 0; i < 10; i++) data[i] = static_cast<float>(i);

    size_t written = rb.push(data, 10);
    ASSERT_EQ(static_cast<int>(written), 10);

    float out[10] = {};
    size_t read = rb.pop(out, 10);
    ASSERT_EQ(static_cast<int>(read), 10);

    for (int i = 0; i < 10; i++) {
        ASSERT_NEAR(out[i], static_cast<float>(i), 1e-10f);
    }
    PASS();
}

TEST(ringbuf_available_read_write) {
    RingBuffer<int> rb(16);

    ASSERT_EQ(static_cast<int>(rb.availableRead()), 0);
    ASSERT_GT(static_cast<int>(rb.availableWrite()), 0);

    for (int i = 0; i < 5; i++) rb.push(i);
    ASSERT_EQ(static_cast<int>(rb.availableRead()), 5);
    PASS();
}

TEST(ringbuf_reset) {
    RingBuffer<int> rb(16);
    for (int i = 0; i < 5; i++) rb.push(i);
    rb.reset();
    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(static_cast<int>(rb.availableRead()), 0);
    PASS();
}

// --- AnalysisBuffer ---

TEST(analysis_buffer_write_read_latest) {
    AnalysisBuffer ab(32);

    float data[10];
    for (int i = 0; i < 10; i++) data[i] = static_cast<float>(i + 1);
    ab.write(data, 10);

    float out[5];
    ab.readLatest(out, 5);

    // Should get the last 5 values: 6, 7, 8, 9, 10
    for (int i = 0; i < 5; i++) {
        ASSERT_NEAR(out[i], static_cast<float>(i + 6), 1e-6f);
    }
    PASS();
}

TEST(analysis_buffer_wraps) {
    AnalysisBuffer ab(8);

    // Write more than capacity
    float data[20];
    for (int i = 0; i < 20; i++) data[i] = static_cast<float>(i);
    ab.write(data, 20);

    float out[4];
    ab.readLatest(out, 4);

    // Should get last 4: 16, 17, 18, 19
    for (int i = 0; i < 4; i++) {
        ASSERT_NEAR(out[i], static_cast<float>(i + 16), 1e-6f);
    }
    PASS();
}

int main() { return runAllTests(); }
