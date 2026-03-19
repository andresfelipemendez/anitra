/* In-engine tests — run automatically on hot-reload */
#include "test_framework.h"
#include "arena.h"
#include "math3d.h"
#include <export.h>

/* --- Arena tests --- */

static int test_arena_init(void) {
    uint8_t buffer[1024];
    arena a;
    arena_init(&a, buffer, sizeof(buffer));
    TEST_ASSERT(a.base == buffer);
    TEST_ASSERT(a.capacity == sizeof(buffer));
    TEST_ASSERT(a.used == 0);
    TEST_ASSERT(a.record_count == 0);
    return 0;
}

static int test_arena_alloc(void) {
    uint8_t buffer[256];
    arena a;
    arena_init(&a, buffer, sizeof(buffer));
    int *ptr = (int*)arena_alloc(&a, sizeof(int), 4, "test");
    TEST_ASSERT(ptr != NULL);
    TEST_ASSERT((size_t)ptr >= (size_t)buffer);
    TEST_ASSERT((size_t)ptr < (size_t)(buffer + sizeof(buffer)));
    TEST_ASSERT(((size_t)ptr % 4) == 0);
    return 0;
}

static int test_arena_multiple_allocs(void) {
    uint8_t buffer[128];
    arena a;
    arena_init(&a, buffer, sizeof(buffer));
    int *p1 = (int*)arena_alloc(&a, 4, 4, "test1");
    int *p2 = (int*)arena_alloc(&a, 4, 4, "test2");
    int *p3 = (int*)arena_alloc(&a, 4, 4, "test3");
    TEST_ASSERT(p1 != NULL);
    TEST_ASSERT(p2 != NULL);
    TEST_ASSERT(p3 != NULL);
    TEST_ASSERT((size_t)p1 < (size_t)p2);
    TEST_ASSERT((size_t)p2 < (size_t)p3);
    return 0;
}

static int test_arena_subarena(void) {
    uint8_t buffer[512];
    arena parent;
    arena_init(&parent, buffer, sizeof(buffer));
    arena *child = arena_alloc_subarena(&parent, 256, 16, "child");
    TEST_ASSERT(child != NULL);
    TEST_ASSERT(child->capacity == 256);
    int *ptr = (int*)arena_alloc(child, 4, 4, "test");
    TEST_ASSERT(ptr != NULL);
    return 0;
}

/* --- Math tests --- */

static int test_vec3_add(void) {
    Vec3 a = VEC3(1.0f, 2.0f, 3.0f);
    Vec3 b = VEC3(4.0f, 5.0f, 6.0f);
    Vec3 r = vec3_add(a, b);
    TEST_ASSERT_EQUAL_FLOAT(r.x, 5.0f, 0.001f);
    TEST_ASSERT_EQUAL_FLOAT(r.y, 7.0f, 0.001f);
    TEST_ASSERT_EQUAL_FLOAT(r.z, 9.0f, 0.001f);
    return 0;
}

static int test_vec3_dot(void) {
    Vec3 a = VEC3(1.0f, 0.0f, 0.0f);
    Vec3 b = VEC3(0.0f, 1.0f, 0.0f);
    float dot = vec3_dot(a, b);
    TEST_ASSERT_EQUAL_FLOAT(dot, 0.0f, 0.001f);
    Vec3 c = VEC3(1.0f, 1.0f, 0.0f);
    dot = vec3_dot(a, c);
    TEST_ASSERT_EQUAL_FLOAT(dot, 1.0f, 0.001f);
    return 0;
}

static int test_quat_identity(void) {
    Quat q = quat_identity();
    TEST_ASSERT_EQUAL_FLOAT(q.x, 0.0f, 0.001f);
    TEST_ASSERT_EQUAL_FLOAT(q.y, 0.0f, 0.001f);
    TEST_ASSERT_EQUAL_FLOAT(q.z, 0.0f, 0.001f);
    TEST_ASSERT_EQUAL_FLOAT(q.w, 1.0f, 0.001f);
    return 0;
}

/* --- Test runner (called by core.c after hot-reload) --- */

EXPORT int run_tests(void) {
    TestSuite suite;
    test_suite_init(&suite);

    test_register(&suite, "arena_init",           test_arena_init);
    test_register(&suite, "arena_alloc",          test_arena_alloc);
    test_register(&suite, "arena_multiple_allocs", test_arena_multiple_allocs);
    test_register(&suite, "arena_subarena",       test_arena_subarena);
    test_register(&suite, "vec3_add",             test_vec3_add);
    test_register(&suite, "vec3_dot",             test_vec3_dot);
    test_register(&suite, "quat_identity",        test_quat_identity);

    int result = test_suite_run(&suite);
    test_suite_cleanup(&suite);
    return result;
}
