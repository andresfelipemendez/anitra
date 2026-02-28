/* Unit test framework for Anitra Engine */
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test result types */
typedef enum {
    TEST_PASSED,
    TEST_FAILED,
    TEST_ERROR
} TestResult;

/* Test structure */
typedef struct {
    const char *name;
    int (*test_func)(void);
    TestResult last_result;
} TestCase;

/* Global test state */
typedef struct {
    TestCase *tests;
    int test_count;
    int max_tests;
    int passed;
    int failed;
    int errors;
} TestSuite;

/* Initialize a test suite */
static inline void test_suite_init(TestSuite *suite) {
    suite->tests = NULL;
    suite->test_count = 0;
    suite->max_tests = 128;
    suite->passed = 0;
    suite->failed = 0;
    suite->errors = 0;
    
    /* Allocate test array */
    suite->tests = (TestCase*)malloc(sizeof(TestCase) * suite->max_tests);
}

/* Cleanup test suite */
static inline void test_suite_cleanup(TestSuite *suite) {
    if (suite->tests) free(suite->tests);
    suite->tests = NULL;
    suite->test_count = 0;
}

/* Register a test case */
static inline int test_register(TestSuite *suite, const char *name, int (*test_func)(void)) {
    if (suite->test_count >= suite->max_tests) {
        fprintf(stderr, "Test suite full! Max tests: %d\n", suite->max_tests);
        return -1;
    }
    
    suite->tests[suite->test_count].name = name;
    suite->tests[suite->test_count].test_func = test_func;
    suite->tests[suite->test_count].last_result = TEST_ERROR;
    suite->test_count++;
    
    return 0;
}

/* Run a single test */
static inline TestResult test_run(TestCase *test) {
    printf("  Running: %s... ", test->name);
    fflush(stdout);
    
    int result = test->test_func();
    
    if (result == 0) {
        printf("PASSED\n");
        return TEST_PASSED;
    } else {
        printf("FAILED (error code: %d)\n", result);
        return TEST_FAILED;
    }
}

/* Run all tests in suite */
static inline int test_suite_run(TestSuite *suite) {
    printf("\n=== Running %d Tests ===\n\n", suite->test_count);
    
    for (int i = 0; i < suite->test_count; i++) {
        TestResult result = test_run(&suite->tests[i]);
        
        switch(result) {
            case TEST_PASSED:
                suite->passed++;
                break;
            case TEST_FAILED:
                suite->failed++;
                break;
            default:
                suite->errors++;
                break;
        }
    }
    
    printf("\n=== Test Results ===\n");
    printf("Passed: %d\n", suite->passed);
    printf("Failed: %d\n", suite->failed);
    printf("Errors: %d\n", suite->errors);
    printf("Total:  %d\n", suite->test_count);
    
    return suite->failed > 0 || suite->errors > 0;
}

/* Test assertions */
#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "Assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return -1; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
    do { \
        int _expected = (expected); \
        int _actual = (actual); \
        if (_expected != _actual) { \
            fprintf(stderr, "Assertion failed: %s == %s (%d != %d) at %s:%d\n", \
                    #expected, #actual, _expected, _actual, __FILE__, __LINE__); \
            return -1; \
        } \
    } while(0)

#define TEST_ASSERT_EQUAL_FLOAT(expected, actual, tolerance) \
    do { \
        float _expected = (expected); \
        float _actual = (actual); \
        float _diff = (_expected > _actual) ? (_expected - _actual) : (_actual - _expected); \
        if (_diff > (tolerance)) { \
            fprintf(stderr, "Assertion failed: %s == %s (%.4f != %.4f) at %s:%d\n", \
                    #expected, #actual, _expected, _actual, __FILE__, __LINE__); \
            return -1; \
        } \
    } while(0)

/* Test runner main function */
#define TEST_RUNNER(suite_name) \
int main(void) { \
    TestSuite suite; \
    test_suite_init(&suite); \
    \
    /* Register tests */ \
    suite_name(&suite); \
    \
    int result = test_suite_run(&suite); \
    \
    test_suite_cleanup(&suite); \
    return result; \
}

#endif /* TEST_FRAMEWORK_H */
