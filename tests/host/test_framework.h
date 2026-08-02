/*
 * test_framework.h - repository-local host test harness for Butterfly CLUE.
 *
 * Compiles with host GCC or Clang only. NEVER arm-none-eabi-gcc.
 * No external dependencies. License: same as Butterfly source (source/LICENSE).
 *
 * Design:
 *   - Each test file owns its own main() and explicitly RUN_TEST()'s each
 *     suite function. No constructor magic, no linker sections, no surprises.
 *   - Assertion failures longjmp back to the runner so a single failing
 *     assertion does not abort the whole binary; it aborts only the
 *     offending test and records a failure.
 *   - Deterministic xorshift32 PRNG seeded by -DTEST_SEED=<value> so
 *     every CI run and every local run reproduce the same sequence
 *     unless the operator overrides the seed on the make command line.
 *
 * Usage:
 *
 *     #include "test_framework.h"
 *
 *     static void my_behavior(void) {
 *         TEST_ASSERT(1 + 1 == 2, "arithmetic works");
 *         TEST_ASSERT_EQ_INT(4, 2 * 2);
 *     }
 *
 *     int main(void) {
 *         test_framework_init();
 *         RUN_TEST(my_behavior);
 *         return test_framework_finish();
 *     }
 *
 * Assertion failure -> nonzero exit. test_framework_finish() returns the
 * process exit code (0 if no failures, 1 otherwise).
 */
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public state (read-only for tests). */
extern int      g_tests_run;
extern int      g_tests_failed;
extern jmp_buf  g_test_jmp;
extern int      g_test_jmp_valid;

/* Lifecycle. */
void test_framework_init(void);    /* prints seed, resets counters */
int  test_framework_finish(void);  /* prints summary, returns exit code */

/* Run one named test function; isolates failures via setjmp. */
void test_framework_run__(const char *name, void (*fn)(void));

/* Deterministic PRNG, seeded from TEST_SEED at init time. */
void     test_framework_srand(uint32_t seed);
uint32_t test_framework_rand(void);

/* Backends for the assertion macros. Do not call directly. */
void test_framework_assert__(int cond, const char *msg,
                             const char *file, int line);
void test_framework_assert_eq_int__(long long expected, long long actual,
                                    const char *file, int line,
                                    const char *expr);
void test_framework_fail__(const char *msg, const char *file, int line);

/*
 * User-facing macros. All honor __FILE__ / __LINE__ for diagnostic output.
 */
#define TEST_ASSERT(cond, msg) \
    test_framework_assert__((cond) ? 1 : 0, (msg), __FILE__, __LINE__)

#define TEST_ASSERT_EQ_INT(expected, actual) \
    test_framework_assert_eq_int__( \
        (long long)(expected), (long long)(actual), \
        __FILE__, __LINE__, \
        #expected " == " #actual)

#define TEST_FAIL(msg) \
    test_framework_fail__((msg), __FILE__, __LINE__)

#define RUN_TEST(fn) \
    test_framework_run__(#fn, (fn))

#ifdef __cplusplus
}
#endif

#endif /* TEST_FRAMEWORK_H */
