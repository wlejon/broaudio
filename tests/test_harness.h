#pragma once

// Minimal test harness — no dependencies, returns 0 on success, 1 on failure.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) static void test_##name(); \
    static struct Register_##name { \
        Register_##name() { testRegistry().push_back({#name, test_##name}); } \
    } register_##name; \
    static void test_##name()

struct TestEntry { const char* name; void (*fn)(); };
static std::vector<TestEntry>& testRegistry() {
    static std::vector<TestEntry> reg;
    return reg;
}

static int runAllTests() {
    for (auto& t : testRegistry()) {
        t.fn();
    }
    std::printf("\n%d passed, %d failed\n", g_testsPassed, g_testsFailed);
    return g_testsFailed > 0 ? 1 : 0;
}

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        g_testsFailed++; return; \
    } } while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::printf("  FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
        g_testsFailed++; return; \
    } } while(0)

#define ASSERT_NEAR(a, b, tol) do { \
    float _a = static_cast<float>(a); float _b = static_cast<float>(b); \
    if (std::fabs(_a - _b) > static_cast<float>(tol)) { \
        std::printf("  FAIL %s:%d: %s=%f != %s=%f (tol=%f)\n", \
            __FILE__, __LINE__, #a, _a, #b, _b, static_cast<float>(tol)); \
        g_testsFailed++; return; \
    } } while(0)

#define ASSERT_GT(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (!(_a > _b)) { \
        std::printf("  FAIL %s:%d: %s (%f) not > %s (%f)\n", \
            __FILE__, __LINE__, #a, (double)_a, #b, (double)_b); \
        g_testsFailed++; return; \
    } } while(0)

#define ASSERT_LT(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (!(_a < _b)) { \
        std::printf("  FAIL %s:%d: %s (%f) not < %s (%f)\n", \
            __FILE__, __LINE__, #a, (double)_a, #b, (double)_b); \
        g_testsFailed++; return; \
    } } while(0)

#define PASS() do { g_testsPassed++; } while(0)
