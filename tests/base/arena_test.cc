// =============================================================================
// tests/base/arena_test.cc
// =============================================================================

#include "tests/test-framework.h"

#include "base/arena.h"

using namespace v12;
using namespace v12_test;

TEST(Arena, Allocate) {
    Arena a;
    void* p1 = a.Allocate(100);
    void* p2 = a.Allocate(200);
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
    EXPECT_GE(a.total_allocated(), 300u);
}

TEST(Arena, Alignment) {
    Arena a;
    void* p1 = a.Allocate(7, 8);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p1) % 8, 0u);

    void* p2 = a.Allocate(7, 16);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p2) % 16, 0u);

    void* p3 = a.Allocate(7, 64);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p3) % 64, 0u);
}

TEST(Arena, New) {
    Arena a;
    struct Foo {
        int x;
        double y;
        Foo(int x_, double y_) : x(x_), y(y_) {}
    };
    Foo* f = a.New<Foo>(42, 3.14);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->x, 42);
    EXPECT_DOUBLE_EQ(f->y, 3.14);
}

TEST(Arena, NewArray) {
    Arena a;
    int* arr = a.NewArray<int>(100);
    ASSERT_NE(arr, nullptr);
    for (int i = 0; i < 100; ++i) {
        arr[i] = i;
    }
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(arr[i], i);
    }
}

TEST(Arena, LargeAllocation) {
    Arena a(1024);
    void* p = a.Allocate(4096);
    EXPECT_NE(p, nullptr);
    EXPECT_GE(a.total_allocated(), 4096u);
}

TEST(Arena, ReleaseAll) {
    Arena a;
    a.Allocate(1000);
    EXPECT_GE(a.total_allocated(), 1000u);
    a.ReleaseAll();
    EXPECT_EQ(a.total_allocated(), 0u);
}
