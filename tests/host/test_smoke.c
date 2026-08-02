/*
 * test_smoke.c - smallest possible harness build/run check.
 *
 * Default build: passes. Acts as the "the harness itself links and runs"
 * canary in `make test`.
 *
 * Build with -DRUN_FAILING_TEST to flip test_failure_demo into a deliberately
 * failing assertion. The Makefile target `test-fail-demo` does this and
 * verifies the binary exits nonzero, proving the harness does not silently
 * swallow assertion failures.
 */
#include "test_framework.h"

static void test_always_passes(void) {
    TEST_ASSERT(1 == 1, "identity holds");
    TEST_ASSERT_EQ_INT(2, 1 + 1);
}

static void test_failure_demo(void) {
#ifdef RUN_FAILING_TEST
    /*
     * Deliberate failure: must cause nonzero exit when this macro is
     * defined. The Makefile `test-fail-demo` target asserts this.
     */
    TEST_ASSERT(0, "deliberate failure for harness verification");
#else
    /*
     * Default build path: keep the suite passing so `make test` is green.
     */
    TEST_ASSERT(1, "RUN_FAILING_TEST not defined - no-op pass");
#endif
}

int main(void) {
    test_framework_init();
    RUN_TEST(test_always_passes);
    RUN_TEST(test_failure_demo);
    return test_framework_finish();
}
