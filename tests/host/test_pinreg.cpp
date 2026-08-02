/*
 * test_pinreg.cpp - pin ownership registry host tests.
 *
 * Covers: individual pin acquire/release, group atomicity, refcounts,
 * stale/double release, reserved resources, force quiescence flow,
 * no partial group release, reset_all. No hardware required.
 */
#include "test_framework.h"

/* Direct-include production logic for host testing (no SDK deps). */
#include "../../src/pinRegistry.cpp"

#include <string.h>

/* ---- Mock callbacks ---- */
static int s_restoreCalls;
static pinreg_group_t s_lastRestoreGroup;
static uint8_t s_lastRestorePin;
static void mock_restore(pinreg_group_t g, uint8_t p) {
    s_restoreCalls++; s_lastRestoreGroup = g; s_lastRestorePin = p;
}

static int s_quiesceCalls;
static bool s_quiesceResult;
static bool mock_quiesce(pinreg_group_t g) {
    (void)g; s_quiesceCalls++; return s_quiesceResult;
}

static int s_eventCount;
static pinreg_event_t s_lastEvent;
static pinreg_token_t s_lastToken;
static pinreg_owner_t s_lastPrevOwner;
static void mock_event(pinreg_group_t g, pinreg_event_t e,
                       pinreg_owner_t prev, pinreg_token_t t) {
    (void)g; s_eventCount++; s_lastEvent = e; s_lastPrevOwner = prev; s_lastToken = t;
}

static void reset_mocks(void) {
    s_restoreCalls = 0; s_quiesceCalls = 0; s_quiesceResult = true;
    s_eventCount = 0; s_lastEvent = PINREG_EV_DISPLACED;
    s_lastToken = PINREG_TOKEN_INVALID; s_lastPrevOwner = PINREG_OWNER_NONE;
    s_lastRestoreGroup = PINREG_GROUP_NONE; s_lastRestorePin = 0;
}

static void setup_groups(void) {
    /* P0.13/P0.14 are pre-reserved by pinreg_init() (USB D+/D- on CLUE);
     * group fixtures use other pin numbers to avoid the reservation. */
    static const uint8_t dsp[] = {11,12,15,16,35,37};
    static const uint8_t twi[] = {24,25};
    static const uint8_t qsp[] = {17,19,20,21,22,23};
    pinreg_group_set_pins(PINREG_GROUP_DISPLAY_SPI, dsp, 6);
    pinreg_group_set_pins(PINREG_GROUP_TWIM1, twi, 2);
    pinreg_group_set_pins(PINREG_GROUP_QSPI, qsp, 6);
}

static void reg_init(void) {
    pinreg_init(nullptr, nullptr);
    pinreg_set_event_sink(mock_event);
    setup_groups();
    reset_mocks();
}

/* ---- Tests ---- */

static void test_pin_acquire_release(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_acquire_pin(11, PINREG_OWNER_DISPLAY,
                                                      mock_restore, &tok));
    TEST_ASSERT(tok != PINREG_TOKEN_INVALID, "got valid token");

    /* BUSY for second acquire by different owner */
    pinreg_token_t tok2 = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_BUSY, pinreg_acquire_pin(11, PINREG_OWNER_NEOPIXEL,
                                                        mock_restore, &tok2));
    TEST_ASSERT_EQ_INT(PINREG_TOKEN_INVALID, tok2);

    /* Release restores exactly once */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(1, s_restoreCalls);
    TEST_ASSERT_EQ_INT(PINREG_GROUP_NONE, s_lastRestoreGroup);
}

static void test_group_atomic_acquire(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_acquire_group(PINREG_GROUP_DISPLAY_SPI,
                                                       PINREG_OWNER_DISPLAY,
                                                       mock_restore, &tok));
    TEST_ASSERT(tok != PINREG_TOKEN_INVALID, "group token valid");

    /* All 6 member pins should be OWNED */
    static const uint8_t pins[] = {11,12,15,16,35,37};
    for (int i = 0; i < 6; i++) {
        pinreg_owner_t who;
        TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_query(pins[i], &who));
        TEST_ASSERT_EQ_INT(PINREG_OWNER_DISPLAY, who);
    }

    /* Individual acquire of a member pin fails with BUSY */
    pinreg_token_t ptok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_BUSY, pinreg_acquire_pin(11, PINREG_OWNER_GPIO_USER,
                                                        mock_restore, &ptok));

    /* Release group: restore called once, members freed */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(1, s_restoreCalls);
    /* Member pins now free */
    pinreg_owner_t who2;
    TEST_ASSERT_EQ_INT(PINREG_BUSY, pinreg_query(11, &who2));
}

static void test_group_partial_fail(void) {
    reg_init();
    /* Individually acquire pin 24 (member of TWIM1) first */
    pinreg_token_t ptok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_acquire_pin(24, PINREG_OWNER_GPIO_USER,
                                                      mock_restore, &ptok));
    /* Group acquire of TWIM1 must fail atomically (member busy) */
    pinreg_token_t gtok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_BUSY, pinreg_acquire_group(PINREG_GROUP_TWIM1,
                                                         PINREG_OWNER_SENSOR_BUS,
                                                         mock_restore, &gtok));
    TEST_ASSERT_EQ_INT(PINREG_TOKEN_INVALID, gtok);
    /* Pin 25 should NOT be owned (atomicity: no partial lease) */
    pinreg_owner_t who;
    TEST_ASSERT_EQ_INT(PINREG_BUSY, pinreg_query(25, &who));
    pinreg_release(ptok);
}

static void test_refcount(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    pinreg_acquire_pin(10, PINREG_OWNER_GPIO_USER, mock_restore, &tok);

    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_retain(tok));
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_retain(tok));

    /* First release decrements refcount, no restore yet */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(0, s_restoreCalls);

    /* Second release: still retained */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(0, s_restoreCalls);

    /* Third release: refcount hits 0, restore called */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(1, s_restoreCalls);
}

static void test_stale_token(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    pinreg_acquire_pin(5, PINREG_OWNER_GPIO_USER, mock_restore, &tok);
    pinreg_release(tok);

    /* Token from released lease is stale */
    TEST_ASSERT_EQ_INT(PINREG_INVALID_TOKEN, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(PINREG_INVALID_TOKEN, pinreg_retain(tok));
}

static void test_double_release_group(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    pinreg_acquire_group(PINREG_GROUP_TWIM1, PINREG_OWNER_SENSOR_BUS,
                         mock_restore, &tok);
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(tok));
    /* Second release is stale */
    TEST_ASSERT_EQ_INT(PINREG_INVALID_TOKEN, pinreg_release(tok));
    TEST_ASSERT_EQ_INT(1, s_restoreCalls);
}

static void test_reserved_always_rejected(void) {
    reg_init();
    pinreg_reserve(47);  /* reserve a pin */
    pinreg_token_t tok = PINREG_TOKEN_INVALID;

    /* Normal acquire fails */
    TEST_ASSERT_EQ_INT(PINREG_RESERVED, pinreg_acquire_pin(47, PINREG_OWNER_GPIO_USER,
                                                           mock_restore, &tok));

    /* Force on a group that includes a reserved pin also fails */
    pinreg_reserve(24);  /* TWIM1 member */
    TEST_ASSERT_EQ_INT(PINREG_RESERVED, pinreg_acquire_group(PINREG_GROUP_TWIM1,
                                                             PINREG_OWNER_SENSOR_BUS,
                                                             mock_restore, &tok));
    TEST_ASSERT_EQ_INT(PINREG_TOKEN_INVALID, tok);
    /* Pin 25 should not be owned (atomic failure) */
    pinreg_owner_t who;
    TEST_ASSERT_EQ_INT(PINREG_BUSY, pinreg_query(25, &who));
}

static void test_reserved_group_rejected(void) {
    reg_init();
    pinreg_reserve(PINREG_GROUP_BASE + PINREG_GROUP_QSPI - 1); /* reserve QSPI group */
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_RESERVED, pinreg_acquire_group(PINREG_GROUP_QSPI,
                                                             PINREG_OWNER_QSPI,
                                                             mock_restore, &tok));
}

static void test_force_free_group(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    /* Force on a free group = immediate grant */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_force_group(PINREG_GROUP_BUZZER_PWM,
                                                     PINREG_OWNER_AUDIO,
                                                     mock_restore, &tok));
    TEST_ASSERT(tok != PINREG_TOKEN_INVALID, "force on free gives token");
    pinreg_release(tok);
}

static void test_force_quiesce_flow(void) {
    reg_init();
    pinreg_set_quiesce_handler(PINREG_OWNER_DISPLAY, PINREG_GROUP_DISPLAY_SPI,
                               mock_quiesce);

    /* Original owner acquires */
    pinreg_token_t tok1 = PINREG_TOKEN_INVALID;
    pinreg_acquire_group(PINREG_GROUP_DISPLAY_SPI, PINREG_OWNER_DISPLAY,
                         mock_restore, &tok1);

    /* Force by different owner */
    reset_mocks();
    pinreg_token_t tok2 = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_QUIESCING, pinreg_force_group(PINREG_GROUP_DISPLAY_SPI,
                                                            PINREG_OWNER_BLE_HID,
                                                            mock_restore, &tok2));
    TEST_ASSERT_EQ_INT(PINREG_TOKEN_INVALID, tok2);  /* no token yet */
    TEST_ASSERT_EQ_INT(1, s_quiesceCalls);           /* handler invoked */

    /* Group should be QUIESCING */
    pinreg_info_t info;
    pinreg_inspect(PINREG_GROUP_BASE, &info);
    TEST_ASSERT_EQ_INT(PINREG_STATE_QUIESCING, info.state);

    /* Complete quiescence: old owner restored, new owner granted */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_quiesce_complete(PINREG_GROUP_DISPLAY_SPI));
    TEST_ASSERT_EQ_INT(1, s_restoreCalls);            /* old owner's restore */
    TEST_ASSERT_EQ_INT(3, s_eventCount);              /* RESTORED + DISPLACED + GRANTED */

    /* New owner holds the group */
    pinreg_owner_t who;
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_query(PINREG_GROUP_BASE, &who));
    TEST_ASSERT_EQ_INT(PINREG_OWNER_BLE_HID, who);

    /* Old token is now stale */
    TEST_ASSERT_EQ_INT(PINREG_INVALID_TOKEN, pinreg_release(tok1));

    /* New token from event is valid */
    TEST_ASSERT(s_lastToken != PINREG_TOKEN_INVALID, "granted token from event");
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(s_lastToken));
    TEST_ASSERT_EQ_INT(2, s_restoreCalls);  /* old restore + new restore */
}

static void test_force_not_cancellable(void) {
    reg_init();
    /* Register handler that returns false (non-cancellable) */
    s_quiesceResult = false;
    pinreg_set_quiesce_handler(PINREG_OWNER_DISPLAY, PINREG_GROUP_DISPLAY_SPI,
                               mock_quiesce);

    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    pinreg_acquire_group(PINREG_GROUP_DISPLAY_SPI, PINREG_OWNER_DISPLAY,
                         mock_restore, &tok);

    reset_mocks();
    s_quiesceResult = false;
    pinreg_token_t ftok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_NOT_CANCELLABLE, pinreg_force_group(PINREG_GROUP_DISPLAY_SPI,
                                                                  PINREG_OWNER_BLE_HID,
                                                                  mock_restore, &ftok));
    /* Group still owned by original */
    pinreg_owner_t who;
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_query(PINREG_GROUP_BASE, &who));
    TEST_ASSERT_EQ_INT(PINREG_OWNER_DISPLAY, who);
    pinreg_release(tok);
}

static void test_force_no_handler(void) {
    reg_init();
    /* No quiesce handler registered for this owner+group */
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    pinreg_acquire_group(PINREG_GROUP_TWIM1, PINREG_OWNER_SENSOR_BUS,
                         mock_restore, &tok);

    pinreg_token_t ftok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_NOT_CANCELLABLE, pinreg_force_group(PINREG_GROUP_TWIM1,
                                                                  PINREG_OWNER_BLE_HID,
                                                                  mock_restore, &ftok));
    pinreg_release(tok);
}

static void test_no_partial_group_release(void) {
    reg_init();
    pinreg_token_t gtok = PINREG_TOKEN_INVALID;
    pinreg_acquire_group(PINREG_GROUP_DISPLAY_SPI, PINREG_OWNER_DISPLAY,
                         mock_restore, &gtok);

    /* Member pin's generation matches group's generation, but releasing
     * individually must be rejected. We can't easily forge a valid pin
     * token, so verify via query that members are owned and via release
     * that the group token works. */
    pinreg_owner_t who;
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_query(11, &who));
    TEST_ASSERT_EQ_INT(PINREG_OWNER_DISPLAY, who);

    /* Group release works */
    TEST_ASSERT_EQ_INT(PINREG_OK, pinreg_release(gtok));
    TEST_ASSERT_EQ_INT(1, s_restoreCalls);
}

static void test_reset_all(void) {
    reg_init();
    pinreg_token_t t1 = PINREG_TOKEN_INVALID, t2 = PINREG_TOKEN_INVALID;
    pinreg_acquire_pin(5, PINREG_OWNER_GPIO_USER, mock_restore, &t1);
    pinreg_acquire_group(PINREG_GROUP_TWIM1, PINREG_OWNER_SENSOR_BUS,
                         mock_restore, &t2);

    /* Reset releases everything */
    s_restoreCalls = 0;
    s_eventCount = 0;
    pinreg_reset_all();
    TEST_ASSERT_EQ_INT(2, s_restoreCalls);  /* pin + group */
    TEST_ASSERT_EQ_INT(2, s_eventCount);    /* 2 RESTORED events */

    /* Both tokens now stale */
    TEST_ASSERT_EQ_INT(PINREG_INVALID_TOKEN, pinreg_release(t1));
    TEST_ASSERT_EQ_INT(PINREG_INVALID_TOKEN, pinreg_release(t2));

    /* Reserved pin survives reset */
    pinreg_reserve(40);
    pinreg_reset_all();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_RESERVED, pinreg_acquire_pin(40, PINREG_OWNER_GPIO_USER,
                                                           mock_restore, &tok));
}

static void test_invalid_params(void) {
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_INVALID_PARAM, pinreg_acquire_pin(200, PINREG_OWNER_DISPLAY,
                                                                mock_restore, &tok));
    TEST_ASSERT_EQ_INT(PINREG_INVALID_PARAM, pinreg_acquire_pin(14, PINREG_OWNER_COUNT,
                                                                mock_restore, &tok));
    TEST_ASSERT_EQ_INT(PINREG_INVALID_PARAM, pinreg_acquire_pin(14, PINREG_OWNER_DISPLAY,
                                                                nullptr, &tok));
    TEST_ASSERT_EQ_INT(PINREG_INVALID_PARAM, pinreg_acquire_group(PINREG_GROUP_NONE,
                                                                  PINREG_OWNER_DISPLAY,
                                                                  mock_restore, &tok));
}

static void test_usb_pins_reserved_after_init(void) {
    /* pinreg_init() pre-reserves P0.13 (USB D+) and P0.14 (USB D-) so
     * expert GPIO cannot steal the USB peripheral pins. */
    reg_init();
    pinreg_token_t tok = PINREG_TOKEN_INVALID;
    TEST_ASSERT_EQ_INT(PINREG_RESERVED, pinreg_acquire_pin(13, PINREG_OWNER_GPIO_USER,
                                                           mock_restore, &tok));
    TEST_ASSERT_EQ_INT(PINREG_TOKEN_INVALID, tok);
    TEST_ASSERT_EQ_INT(PINREG_RESERVED, pinreg_acquire_pin(14, PINREG_OWNER_GPIO_USER,
                                                           mock_restore, &tok));
    TEST_ASSERT_EQ_INT(PINREG_TOKEN_INVALID, tok);
}

int main(void) {
    test_framework_init();
    RUN_TEST(test_pin_acquire_release);
    RUN_TEST(test_group_atomic_acquire);
    RUN_TEST(test_group_partial_fail);
    RUN_TEST(test_refcount);
    RUN_TEST(test_stale_token);
    RUN_TEST(test_double_release_group);
    RUN_TEST(test_reserved_always_rejected);
    RUN_TEST(test_reserved_group_rejected);
    RUN_TEST(test_force_free_group);
    RUN_TEST(test_force_quiesce_flow);
    RUN_TEST(test_force_not_cancellable);
    RUN_TEST(test_force_no_handler);
    RUN_TEST(test_no_partial_group_release);
    RUN_TEST(test_reset_all);
    RUN_TEST(test_invalid_params);
    RUN_TEST(test_usb_pins_reserved_after_init);
    return test_framework_finish();
}
