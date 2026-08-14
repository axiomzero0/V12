// =============================================================================
// tests/test-framework.h
// =============================================================================
// Minimal test framework for environments without GoogleTest.
// Provides TEST(), EXPECT_EQ(), EXPECT_TRUE(), etc.
//
// Why not just use gtest?
//   We can't install it in this environment. This framework provides the
//   subset of gtest functionality we need. When gtest is available, the
//   CMake build uses it instead.

#ifndef V12_TEST_FRAMEWORK_H_
#define V12_TEST_FRAMEWORK_H_

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace v12_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

class TestRegistry {
public:
    static TestRegistry& Instance() {
        static TestRegistry r;
        return r;
    }

    void Register(const std::string& name, std::function<void()> fn) {
        cases_.push_back({name, std::move(fn)});
    }

    int RunAll() {
        int passed = 0;
        int failed = 0;
        for (auto& tc : cases_) {
            current_test_ = tc.name.c_str();
            current_failed_ = false;
            try {
                tc.fn();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[FAIL] %s: exception: %s\n", tc.name.c_str(), e.what());
                current_failed_ = true;
            } catch (...) {
                std::fprintf(stderr, "[FAIL] %s: unknown exception\n", tc.name.c_str());
                current_failed_ = true;
            }
            if (current_failed_) {
                ++failed;
            } else {
                ++passed;
                std::fprintf(stderr, "[PASS] %s\n", tc.name.c_str());
            }
        }
        std::fprintf(stderr, "\n=== Tests: %d passed, %d failed, %d total ===\n",
                     passed, failed, static_cast<int>(cases_.size()));
        return failed == 0 ? 0 : 1;
    }

    void Fail(const char* file, int line, const std::string& msg) {
        std::fprintf(stderr, "[FAIL] %s at %s:%d: %s\n",
                     current_test_, file, line, msg.c_str());
        current_failed_ = true;
    }

private:
    std::vector<TestCase> cases_;
    const char* current_test_ = "";
    bool current_failed_ = false;
};

class TestRegistrar {
public:
    TestRegistrar(const std::string& name, std::function<void()> fn) {
        TestRegistry::Instance().Register(name, std::move(fn));
    }
};

}  // namespace v12_test

#define V12_TEST_NAME_(suite, name) \
    v12_test_##suite##_##name##_Test

#define TEST(suite, name) \
    static void V12_TEST_NAME_(suite, name)(); \
    static v12_test::TestRegistrar \
        v12_test_##suite##_##name##_registrar( \
            #suite "." #name, \
            V12_TEST_NAME_(suite, name)); \
    static void V12_TEST_NAME_(suite, name)()

#define V12_FAIL_MSG_(file, line, msg) \
    v12_test::TestRegistry::Instance().Fail(file, line, msg)

#define EXPECT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_TRUE failed: ") + #cond); \
        } \
    } while (0)

#define EXPECT_FALSE(cond) \
    do { \
        if ((cond)) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_FALSE failed: ") + #cond); \
        } \
    } while (0)

#define EXPECT_EQ(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (!(_a == _b)) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_EQ failed: ") + #a + " != " + #b); \
        } \
    } while (0)

#define EXPECT_NE(a, b) \
    do { \
        auto _a = (a); \
        auto _b = (b); \
        if (_a == _b) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_NE failed: ") + #a + " == " + #b); \
        } \
    } while (0)

#define EXPECT_LT(a, b) \
    do { \
        if (!((a) < (b))) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_LT failed: ") + #a + " >= " + #b); \
        } \
    } while (0)

#define EXPECT_LE(a, b) \
    do { \
        if (!((a) <= (b))) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_LE failed: ") + #a + " > " + #b); \
        } \
    } while (0)

#define EXPECT_GT(a, b) \
    do { \
        if (!((a) > (b))) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_GT failed: ") + #a + " <= " + #b); \
        } \
    } while (0)

#define EXPECT_GE(a, b) \
    do { \
        if (!((a) >= (b))) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_GE failed: ") + #a + " < " + #b); \
        } \
    } while (0)

#define EXPECT_DOUBLE_EQ(a, b) \
    do { \
        double _a = (a); \
        double _b = (b); \
        double _diff = _a - _b; \
        if (_diff < 0) _diff = -_diff; \
        if (_diff > 1e-9) { \
            V12_FAIL_MSG_(__FILE__, __LINE__, \
                std::string("EXPECT_DOUBLE_EQ failed: ") + #a + " != " + #b); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) EXPECT_EQ(a, b)
#define ASSERT_NE(a, b) EXPECT_NE(a, b)
#define ASSERT_TRUE(cond) EXPECT_TRUE(cond)
#define ASSERT_FALSE(cond) EXPECT_FALSE(cond)
#define ASSERT_LT(a, b) EXPECT_LT(a, b)
#define ASSERT_GT(a, b) EXPECT_GT(a, b)
#define ASSERT_LE(a, b) EXPECT_LE(a, b)
#define ASSERT_GE(a, b) EXPECT_GE(a, b)

#define RUN_ALL_TESTS() v12_test::TestRegistry::Instance().RunAll()

#endif  // V12_TEST_FRAMEWORK_H_
