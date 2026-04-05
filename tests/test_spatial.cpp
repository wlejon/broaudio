#include "test_harness.h"
#include "broaudio/spatial/listener.h"
#include <cmath>

using namespace broaudio;

// Helper: set up a listener at origin facing -Z, up +Y
static void initDefaultListener(Listener& l) {
    l.posX.store(0.0f); l.posY.store(0.0f); l.posZ.store(0.0f);
    l.fwdX.store(0.0f); l.fwdY.store(0.0f); l.fwdZ.store(-1.0f);
    l.upX.store(0.0f); l.upY.store(1.0f); l.upZ.store(0.0f);
}

static void initSource(SpatialSource& s, float x, float y, float z, DistanceModel model = DistanceModel::Inverse) {
    s.spatialEnabled.store(true);
    s.posX.store(x); s.posY.store(y); s.posZ.store(z);
    s.refDistance.store(1.0f);
    s.maxDistance.store(100.0f);
    s.rolloff.store(1.0f);
    s.distanceModel.store(static_cast<int>(model));
}

// --- Distance attenuation: Inverse model ---

TEST(inverse_at_ref_distance) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -1.0f);  // 1 unit in front
    float pan;
    float gain = computeSpatial(l, s, pan);
    ASSERT_NEAR(gain, 1.0f, 0.01f);
    PASS();
}

TEST(inverse_at_double_ref_distance) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -2.0f);
    float pan;
    float gain = computeSpatial(l, s, pan);
    // Inverse: ref / (ref + rolloff * (d - ref)) = 1 / (1 + 1*(2-1)) = 0.5
    ASSERT_NEAR(gain, 0.5f, 0.01f);
    PASS();
}

TEST(inverse_at_large_distance) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -50.0f);
    float pan;
    float gain = computeSpatial(l, s, pan);
    ASSERT_LT(gain, 0.05f);
    PASS();
}

// --- Distance attenuation: Linear model ---

TEST(linear_at_ref_distance) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -1.0f, DistanceModel::Linear);
    float pan;
    float gain = computeSpatial(l, s, pan);
    ASSERT_NEAR(gain, 1.0f, 0.01f);
    PASS();
}

TEST(linear_at_max_distance) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -100.0f, DistanceModel::Linear);
    float pan;
    float gain = computeSpatial(l, s, pan);
    ASSERT_NEAR(gain, 0.0f, 0.01f);
    PASS();
}

TEST(linear_halfway) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -50.5f, DistanceModel::Linear);
    float pan;
    float gain = computeSpatial(l, s, pan);
    ASSERT_NEAR(gain, 0.5f, 0.02f);
    PASS();
}

// --- Distance attenuation: Exponential model ---

TEST(exponential_at_ref_distance) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -1.0f, DistanceModel::Exponential);
    float pan;
    float gain = computeSpatial(l, s, pan);
    ASSERT_NEAR(gain, 1.0f, 0.01f);
    PASS();
}

TEST(exponential_at_double_ref) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -2.0f, DistanceModel::Exponential);
    float pan;
    float gain = computeSpatial(l, s, pan);
    // (d/ref)^-rolloff = (2/1)^-1 = 0.5
    ASSERT_NEAR(gain, 0.5f, 0.01f);
    PASS();
}

// --- Panning ---

TEST(source_directly_ahead_pans_center) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, -5.0f);
    float pan;
    computeSpatial(l, s, pan);
    ASSERT_NEAR(pan, 0.0f, 0.01f);
    PASS();
}

TEST(source_to_right_pans_right) {
    Listener l; initDefaultListener(l);
    // Listener faces -Z, so right is +X
    SpatialSource s; initSource(s, 5.0f, 0.0f, 0.0f);
    float pan;
    computeSpatial(l, s, pan);
    ASSERT_GT(pan, 0.9f);
    PASS();
}

TEST(source_to_left_pans_left) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, -5.0f, 0.0f, 0.0f);
    float pan;
    computeSpatial(l, s, pan);
    ASSERT_LT(pan, -0.9f);
    PASS();
}

TEST(source_behind_pans_near_zero) {
    Listener l; initDefaultListener(l);
    SpatialSource s; initSource(s, 0.0f, 0.0f, 5.0f);  // behind
    float pan;
    computeSpatial(l, s, pan);
    ASSERT_NEAR(pan, 0.0f, 0.05f);
    PASS();
}

// --- Vec3 ---

TEST(vec3_subtract) {
    Vec3 a{3.0f, 2.0f, 1.0f};
    Vec3 b{1.0f, 1.0f, 1.0f};
    Vec3 c = a - b;
    ASSERT_NEAR(c.x, 2.0f, 1e-6f);
    ASSERT_NEAR(c.y, 1.0f, 1e-6f);
    ASSERT_NEAR(c.z, 0.0f, 1e-6f);
    PASS();
}

TEST(vec3_dot) {
    Vec3 a{1.0f, 0.0f, 0.0f};
    Vec3 b{0.0f, 1.0f, 0.0f};
    ASSERT_NEAR(a.dot(b), 0.0f, 1e-6f);
    ASSERT_NEAR(a.dot(a), 1.0f, 1e-6f);
    PASS();
}

TEST(vec3_cross) {
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 z = x.cross(y);
    ASSERT_NEAR(z.x, 0.0f, 1e-6f);
    ASSERT_NEAR(z.y, 0.0f, 1e-6f);
    ASSERT_NEAR(z.z, 1.0f, 1e-6f);
    PASS();
}

TEST(vec3_length) {
    Vec3 v{3.0f, 4.0f, 0.0f};
    ASSERT_NEAR(v.length(), 5.0f, 1e-5f);
    PASS();
}

int main() { return runAllTests(); }
