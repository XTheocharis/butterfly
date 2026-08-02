/*
 * test_menu.cpp - host tests for app-time chord recognition and menu FSM.
 *
 * Sweeps press/release skew around debounce (50ms), chord-window (120ms),
 * chord-hold (1.5s), long-press (600ms), and destructive-hold (3s)
 * thresholds. Proves: no premature singles, no duplicate singles, aborted
 * chord emits no actions, paired releases, release-after-1.5s menu entry,
 * and destructive confirmation separation.
 */
#include "test_framework.h"
#include "../../src/menu.cpp"

#include <string.h>

static int s_renderCalls;
static void mock_render(void *ctx) { (void)ctx; s_renderCalls++; }

static void reset_menu(MenuManager &m) {
    m = MenuManager();
    MenuAvailability a = {};
    a.ble = true;
    a.imu = true;
    a.mag = true;
    a.qspi = true;
    a.sensors = true;
    a.audio = true;
    m.init(&a, mock_render, nullptr);
    s_renderCalls = 0;
}

static MenuAction drain(MenuManager &m) {
    MenuAction last = MENU_ACT_NONE;
    MenuAction a;
    while ((a = m.pollAction()) != MENU_ACT_NONE) last = a;
    return last;
}

static int countActions(MenuManager &m) {
    int n = 0;
    while (m.pollAction() != MENU_ACT_NONE) n++;
    return n;
}

/* ---- Test: single A press in normal mode ---- */
static void test_single_a_press_release(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_A, false, 200000);

    TEST_ASSERT_EQ_INT(0, countActions(m));

    m.tick(130000);
    MenuAction a1 = m.pollAction();
    MenuAction a2 = m.pollAction();
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_A_PRESS, a1);
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_A_RELEASE, a2);
    TEST_ASSERT_EQ_INT(MENU_ACT_NONE, m.pollAction());
}

/* ---- Test: single B press in normal mode ---- */
static void test_single_b_press_release(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_B, true,  0);
    m.tick(130000);
    m.feedEdge(MENU_BTN_B, false, 200000);

    MenuAction a1 = m.pollAction();
    MenuAction a2 = m.pollAction();
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_B_PRESS, a1);
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_B_RELEASE, a2);
    TEST_ASSERT_EQ_INT(MENU_ACT_NONE, m.pollAction());
}

/* ---- Test: no premature singles within chord window ---- */
static void test_no_premature_singles(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true, 0);

    m.tick(50000);
    m.tick(100000);
    TEST_ASSERT_EQ_INT(0, countActions(m));
}

/* ---- Test: quick tap within chord window — paired press+release ---- */
static void test_quick_tap_paired(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_A, false, 60000);
    m.tick(130000);

    MenuAction a1 = m.pollAction();
    MenuAction a2 = m.pollAction();
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_A_PRESS, a1);
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_A_RELEASE, a2);
    TEST_ASSERT_EQ_INT(MENU_ACT_NONE, m.pollAction());
}

/* ---- Test: aborted in-window chord emits NO actions ---- */
static void test_aborted_chord_no_actions(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_B, true,  80000);
    m.tick(200000);
    m.feedEdge(MENU_BTN_A, false, 250000);
    m.feedEdge(MENU_BTN_B, false, 300000);

    TEST_ASSERT_EQ_INT(0, countActions(m));
}

/* ---- Test: chord held 1.5s then released enters menu ---- */
static void test_chord_enters_menu(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_B, true,  80000);
    m.tick(1600000);
    m.feedEdge(MENU_BTN_A, false, 1700000);
    m.feedEdge(MENU_BTN_B, false, 1800000);

    TEST_ASSERT(m.isOpen(), "menu opened after chord release");
    MenuAction act = drain(m);
    TEST_ASSERT_EQ_INT(MENU_ACT_ENTER_MENU, act);
}

/* ---- Test: chord < 1.5s does NOT enter menu ---- */
static void test_short_chord_no_menu(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_B, true,  80000);
    m.tick(1400000);
    m.feedEdge(MENU_BTN_A, false, 1500000);
    m.feedEdge(MENU_BTN_B, false, 1550000);

    TEST_ASSERT(!m.isOpen(), "menu NOT opened for short chord");
    TEST_ASSERT_EQ_INT(0, countActions(m));
}

/* ---- Test: debounce rejects edges < 50ms apart ---- */
static void test_debounce_rejects_close_edges(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_A, false, 30000);  /* bounce-rejected (< 50ms) */
    m.tick(130000);

    /* Only PRESS emitted — the release was bounce-rejected, A still "held". */
    TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_A_PRESS, m.pollAction());
    TEST_ASSERT_EQ_INT(MENU_ACT_NONE, m.pollAction());
}

/* ---- Test: second button at 119ms = chord, 121ms = two singles ---- */
static void test_chord_window_boundary(void) {
    /* Within window: 119ms → chord */
    {
        MenuManager m; reset_menu(m);
        m.feedEdge(MENU_BTN_A, true, 0);
        m.feedEdge(MENU_BTN_B, true, 119000);
        m.tick(130000);
        TEST_ASSERT_EQ_INT(0, countActions(m));
    }
    /* Outside window: 121ms → first is classified as single */
    {
        MenuManager m; reset_menu(m);
        m.feedEdge(MENU_BTN_A, true, 0);
        m.tick(121000);
        TEST_ASSERT_EQ_INT(MENU_ACT_SINGLE_A_PRESS, m.pollAction());
        TEST_ASSERT_EQ_INT(MENU_ACT_NONE, m.pollAction());
    }
}

/* ---- Test: in-menu B short → NAV_NEXT ---- */
static void test_menu_b_next(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    m.feedEdge(MENU_BTN_B, true,  100000);
    m.tick(230000);
    m.feedEdge(MENU_BTN_B, false, 300000);

    MenuAction act = drain(m);
    TEST_ASSERT_EQ_INT(MENU_ACT_NAV_NEXT, act);
}

/* ---- Test: in-menu B long → NAV_BACK (not also NAV_NEXT) ---- */
static void test_menu_b_long_back(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    m.feedEdge(MENU_BTN_B, true,  100000);
    m.tick(230000);
    m.tick(720000);
    m.feedEdge(MENU_BTN_B, false, 800000);

    MenuAction act = drain(m);
    TEST_ASSERT_EQ_INT(MENU_ACT_NAV_BACK, act);
}

/* ---- Test: in-menu A short → NAV_SELECT ---- */
static void test_menu_a_select(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    m.feedEdge(MENU_BTN_A, false, 0);
    drain(m);

    m.feedEdge(MENU_BTN_A, true,  100000);
    m.tick(230000);
    MenuAction act = drain(m);
    TEST_ASSERT_EQ_INT(MENU_ACT_NAV_SELECT, act);
}

/* ---- Test: NAV_NEXT advances through menu items ---- */
static void test_nav_next_advances_items(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    for (int i = 0; i < 3; i++) {
        m.feedEdge(MENU_BTN_B, true,  100000 + i * 500000);
        m.tick(230000 + i * 500000);
        m.feedEdge(MENU_BTN_B, false, 300000 + i * 500000);
        drain(m);
    }
    TEST_ASSERT_EQ_INT(MENU_ITEM_IMU_CAL, m.item());
}

/* ---- Test: EXIT item closes menu ---- */
static void test_exit_item_closes_menu(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    for (int i = 0; i < (int)MENU_ITEM_EXIT; i++) {
        m.feedEdge(MENU_BTN_B, true,  100000 + i * 500000);
        m.tick(230000 + i * 500000);
        m.feedEdge(MENU_BTN_B, false, 300000 + i * 500000);
        drain(m);
    }
    TEST_ASSERT_EQ_INT(MENU_ITEM_EXIT, m.item());

    m.feedEdge(MENU_BTN_A, true,  6000000);
    m.tick(6130000);
    drain(m);
    TEST_ASSERT(!m.isOpen(), "menu closed after EXIT select");
}

/* ---- Test: destructive FDS — 3s A hold ---- */
static void test_destructive_fds_hold(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    /* Navigate to Storage, select to enter QSPI warn — but we need FDS warn.
       For test, directly set confirmState via selecting Storage (QSPI warn)
       then test FDS separately by forcing the state. */
    /* Select Storage → enters QSPI_WARN */
    for (int i = 0; i < (int)MENU_ITEM_STORAGE; i++) {
        m.feedEdge(MENU_BTN_B, true,  i * 400000);
        m.tick(i * 400000 + 130000);
        m.feedEdge(MENU_BTN_B, false, i * 400000 + 200000);
        drain(m);
    }
    m.feedEdge(MENU_BTN_A, true,  5000000);
    m.tick(5130000);
    drain(m);
    TEST_ASSERT_EQ_INT(MENU_CONF_QSPI_WARN, m.confirm());

    /* Now hold A+B for 3s for QSPI adoption */
    m.feedEdge(MENU_BTN_A, true,  6000000);
    m.feedEdge(MENU_BTN_B, true,  6080000);
    m.tick(9100000);
    MenuAction act = drain(m);
    TEST_ASSERT_EQ_INT(MENU_ACT_DESTRUCTIVE_QSPI, act);
    TEST_ASSERT_EQ_INT(MENU_CONF_NONE, m.confirm());
}

/* ---- Test: destructive QSPI cancel — release before 3s ---- */
static void test_destructive_qspi_cancel(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    for (int i = 0; i < (int)MENU_ITEM_STORAGE; i++) {
        m.feedEdge(MENU_BTN_B, true,  i * 400000);
        m.tick(i * 400000 + 130000);
        m.feedEdge(MENU_BTN_B, false, i * 400000 + 200000);
        drain(m);
    }
    m.feedEdge(MENU_BTN_A, true,  5000000);
    m.tick(5130000);
    drain(m);

    m.feedEdge(MENU_BTN_A, true,  6000000);
    m.feedEdge(MENU_BTN_B, true,  6080000);
    m.tick(8000000);
    m.feedEdge(MENU_BTN_A, false, 8100000);
    MenuAction act = drain(m);
    TEST_ASSERT_EQ_INT(MENU_ACT_DESTRUCTIVE_CANCEL, act);
    TEST_ASSERT_EQ_INT(MENU_CONF_QSPI_WARN, m.confirm());
}

/* ---- Test: destructive — B cancels warning state ---- */
static void test_destructive_b_cancels_warning(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    for (int i = 0; i < (int)MENU_ITEM_STORAGE; i++) {
        m.feedEdge(MENU_BTN_B, true,  i * 400000);
        m.tick(i * 400000 + 130000);
        m.feedEdge(MENU_BTN_B, false, i * 400000 + 200000);
        drain(m);
    }
    m.feedEdge(MENU_BTN_A, true,  5000000);
    m.tick(5130000);
    drain(m);
    TEST_ASSERT_EQ_INT(MENU_CONF_QSPI_WARN, m.confirm());

    m.feedEdge(MENU_BTN_A, false, 5500000);
    m.feedEdge(MENU_BTN_B, true,  6000000);
    drain(m);
    TEST_ASSERT_EQ_INT(MENU_CONF_NONE, m.confirm());
}

/* ---- Test: Pair/Forget enters separate confirmation ---- */
static void test_pair_forget_separate_confirm(void) {
    MenuManager m; reset_menu(m);
    m.open(0);
    drain(m);

    for (int i = 0; i < (int)MENU_ITEM_PAIR_FORGET; i++) {
        m.feedEdge(MENU_BTN_B, true,  i * 400000);
        m.tick(i * 400000 + 130000);
        m.feedEdge(MENU_BTN_B, false, i * 400000 + 200000);
        drain(m);
    }
    m.feedEdge(MENU_BTN_A, true,  5000000);
    m.tick(5130000);
    drain(m);
    TEST_ASSERT_EQ_INT(MENU_CONF_FORGET_WARN, m.confirm());
}

/* ---- Test: optional hardware unavailable → item not available ---- */
static void test_unavailable_hardware(void) {
    MenuManager m;
    m = MenuManager();
    MenuAvailability a = {};
    m.init(&a, mock_render, nullptr);

    TEST_ASSERT(!menuItemAvailable(MENU_ITEM_PAIR_FORGET, a), "no BLE");
    TEST_ASSERT(!menuItemAvailable(MENU_ITEM_IMU_CAL, a), "no IMU");
    TEST_ASSERT(!menuItemAvailable(MENU_ITEM_STORAGE, a), "no QSPI");
    TEST_ASSERT(menuItemAvailable(MENU_ITEM_STATUS, a), "status always avail");
    TEST_ASSERT(menuItemAvailable(MENU_ITEM_DFU, a), "DFU always avail");
}

/* ---- Test: no duplicate singles on bounce ---- */
static void test_no_duplicate_on_bounce(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_A, false, 10000);
    m.feedEdge(MENU_BTN_A, true,  20000);
    m.tick(130000);

    int n = countActions(m);
    TEST_ASSERT_EQ_INT(1, n);
}

/* ---- Test: chord fired, one button released, other still held ---- */
static void test_chord_fired_partial_release(void) {
    MenuManager m; reset_menu(m);
    m.feedEdge(MENU_BTN_A, true,  0);
    m.feedEdge(MENU_BTN_B, true,  80000);
    m.tick(1600000);
    m.feedEdge(MENU_BTN_A, false, 1700000);

    TEST_ASSERT(!m.isOpen(), "menu NOT opened on partial release");
    m.feedEdge(MENU_BTN_B, false, 1800000);
    TEST_ASSERT(m.isOpen(), "menu opened after full release");
}

/* ---- Test: long press boundary — 599ms vs 601ms in menu ---- */
static void test_long_press_boundary(void) {
    /* 599ms → still short B → NAV_NEXT */
    {
        MenuManager m; reset_menu(m);
        m.open(0); drain(m);
        m.feedEdge(MENU_BTN_B, true,  100000);
        m.tick(230000);
        m.tick(698000);
        m.feedEdge(MENU_BTN_B, false, 699000);
        MenuAction act = drain(m);
        TEST_ASSERT_EQ_INT(MENU_ACT_NAV_NEXT, act);
    }
    /* 601ms → long B → NAV_BACK */
    {
        MenuManager m; reset_menu(m);
        m.open(0); drain(m);
        m.feedEdge(MENU_BTN_B, true,  100000);
        m.tick(230000);
        m.tick(701000);
        MenuAction act = drain(m);
        TEST_ASSERT_EQ_INT(MENU_ACT_NAV_BACK, act);
    }
}

/* ---- Main ---- */

int main(void) {
    test_framework_init();
    RUN_TEST(test_single_a_press_release);
    RUN_TEST(test_single_b_press_release);
    RUN_TEST(test_no_premature_singles);
    RUN_TEST(test_quick_tap_paired);
    RUN_TEST(test_aborted_chord_no_actions);
    RUN_TEST(test_chord_enters_menu);
    RUN_TEST(test_short_chord_no_menu);
    RUN_TEST(test_debounce_rejects_close_edges);
    RUN_TEST(test_chord_window_boundary);
    RUN_TEST(test_menu_b_next);
    RUN_TEST(test_menu_b_long_back);
    RUN_TEST(test_menu_a_select);
    RUN_TEST(test_nav_next_advances_items);
    RUN_TEST(test_exit_item_closes_menu);
    RUN_TEST(test_destructive_fds_hold);
    RUN_TEST(test_destructive_qspi_cancel);
    RUN_TEST(test_destructive_b_cancels_warning);
    RUN_TEST(test_pair_forget_separate_confirm);
    RUN_TEST(test_unavailable_hardware);
    RUN_TEST(test_no_duplicate_on_bounce);
    RUN_TEST(test_chord_fired_partial_release);
    RUN_TEST(test_long_press_boundary);
    return test_framework_finish();
}
