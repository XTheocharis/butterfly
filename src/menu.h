/*
 * menu.h - recovery/normal menu with app-time A+B chord recognition.
 *
 * Design:
 *   - Three layers: debounce filter, gesture FSM, menu navigation.
 *   - App-time ONLY chord (bootloader consumes A=DFU, A+B=OTA at reset).
 *   - FSM states: IDLE → PENDING_SINGLE → CHORD_PENDING → CHORD_FIRED,
 *     plus SINGLE_TRACKING / LONG_PRESS / SUPPRESSED / DESTRUCTIVE_HOLD.
 *   - Singles deferred 120ms (chord window, > 50ms debounce).
 *   - In-menu B deferred until release or long-press disqualification.
 *   - Destructive holds: 3s A (FDS), 3s A+B (QSPI). Separate confirmation
 *     states per destructive op — never shared.
 *   - Optional hardware appears unavailable, not blocking.
 *
 * Pure logic: no SDK deps. Display interaction via Todo 9 widget callback.
 * Compiles on host for unit testing.
 */
#ifndef MENU_H
#define MENU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
#include "display.h"

/* ---- Timing constants (microseconds via Todo 7 timebase) ---- */

/* CLUE_BUTTON_DEBOUNCE_TICKS = 50000UL at 1 MHz = 50 ms. */
#define MENU_DEBOUNCE_US           50000ULL
/* Chord window: must exceed debounce. Singles deferred this long. */
#define MENU_CHORD_WINDOW_US       120000ULL
/* A+B held this long fires the chord → arms menu entry. */
#define MENU_CHORD_HOLD_US         1500000ULL
/* In-menu long press threshold for B=back. */
#define MENU_LONG_PRESS_US         600000ULL
/* Destructive confirmation hold duration. */
#define MENU_DESTRUCTIVE_HOLD_US   3000000ULL

/* ---- Button identifiers ---- */
enum MenuButton : uint8_t {
    MENU_BTN_NONE = 0,
    MENU_BTN_A    = 1,
    MENU_BTN_B    = 2,
};

/* ---- Actions emitted by the FSM ---- */
enum MenuAction : uint8_t {
    MENU_ACT_NONE = 0,
    MENU_ACT_SINGLE_A_PRESS,      /* classified single A (normal mode) */
    MENU_ACT_SINGLE_A_RELEASE,
    MENU_ACT_SINGLE_B_PRESS,      /* classified single B (normal mode) */
    MENU_ACT_SINGLE_B_RELEASE,
    MENU_ACT_ENTER_MENU,          /* chord released → open recovery menu */
    MENU_ACT_NAV_NEXT,            /* B short in menu → next item */
    MENU_ACT_NAV_SELECT,          /* A short in menu → select item */
    MENU_ACT_NAV_BACK,            /* B long in menu → back / exit submenu */
    MENU_ACT_EXIT_MENU,           /* menu closed */
    MENU_ACT_DESTRUCTIVE_FDS,     /* 3s A hold → clear unknown FDS data */
    MENU_ACT_DESTRUCTIVE_QSPI,    /* 3s A+B hold → adopt QSPI */
    MENU_ACT_DESTRUCTIVE_FORGET,  /* forget bonds confirmed */
    MENU_ACT_DESTRUCTIVE_ERASE,   /* erase log confirmed */
    MENU_ACT_DESTRUCTIVE_CANCEL,  /* destructive hold aborted */
};

/* ---- Menu items ---- */
enum MenuItemId : uint8_t {
    MENU_ITEM_STATUS = 0,
    MENU_ITEM_RUNTIME_OVERRIDE,
    MENU_ITEM_PAIR_FORGET,
    MENU_ITEM_IMU_CAL,
    MENU_ITEM_MAG_CAL,
    MENU_ITEM_REMOTE_PROFILE,
    MENU_ITEM_STORAGE,
    MENU_ITEM_SENSOR_DASHBOARD,
    MENU_ITEM_DFU,
    MENU_ITEM_EXIT,
    MENU_ITEM_COUNT,
};

/* ---- Destructive confirmation states (each unique — never shared) ---- */
enum MenuConfirm : uint8_t {
    MENU_CONF_NONE = 0,
    MENU_CONF_FORGET_WARN,        /* warning screen, awaiting confirm */
    MENU_CONF_FORGET_HOLD,        /* hold in progress */
    MENU_CONF_ERASE_WARN,
    MENU_CONF_ERASE_HOLD,
    MENU_CONF_FDS_WARN,
    MENU_CONF_FDS_HOLD,           /* 3s A hold */
    MENU_CONF_QSPI_WARN,
    MENU_CONF_QSPI_HOLD,          /* 3s A+B hold */
};

/* ---- Optional hardware availability ---- */
struct MenuAvailability {
    bool ble;
    bool imu;
    bool mag;
    bool qspi;
    bool sensors;
    bool audio;
};

/* Get default availability: everything absent (sensors checked at runtime). */
static inline MenuAvailability menuDefaultAvailability(void) {
    MenuAvailability a = {};
    return a;
}

/* ---- Render callback: invoked when display state changes ---- */
typedef void (*MenuRenderFn)(void *ctx);

/* Check whether a menu item is available given hardware flags. */
bool menuItemAvailable(MenuItemId item, const MenuAvailability &avail);

class MenuManager {
public:
    MenuManager();

    /* Configure availability and optional render callback. */
    void init(const MenuAvailability *avail, MenuRenderFn render_fn,
              void *render_ctx);

    /* Feed a raw button edge (from GPIOTE or polling). */
    void feedEdge(MenuButton btn, bool pressed, uint64_t now_us);

    /* Periodic tick — processes chord-window expiry, chord-hold timer,
     * long-press timer, and destructive-hold timer.
     * Call at least every 10 ms. */
    void tick(uint64_t now_us);

    /* Poll for the next emitted action (NONE if queue empty). */
    MenuAction pollAction(void);

    /* ---- Menu state queries (for rendering) ---- */
    bool isOpen(void) const { return m_menuOpen; }
    MenuItemId item(void) const { return m_currentItem; }
    MenuConfirm confirm(void) const { return m_confirmState; }
    uint8_t destructivePct(void) const; /* 0-100 */
    const MenuAvailability &avail(void) const { return m_avail; }

    /* Register the menu render widget on a DisplayModule (firmware only). */
    void registerOnDisplay(DisplayModule &disp, uint8_t page);

    /* Force-close the menu (from Exit item, DFU reboot, etc.). */
    void close(void);

    /* Force-open the menu (for testing or external trigger). */
    void open(uint64_t now_us);

private:
    /* ---- Gesture FSM states ---- */
    enum FsmState : uint8_t {
        FSM_IDLE,
        FSM_PENDING_SINGLE,     /* one button down, in chord window */
        FSM_CHORD_PENDING,      /* both down, in 1.5s hold */
        FSM_CHORD_FIRED,        /* 1.5s reached, armed for release */
        FSM_SINGLE_TRACKING,    /* classified single, awaiting release/long */
        FSM_LONG_PRESS,         /* long press fired, awaiting release */
        FSM_SUPPRESSED,         /* failed chord, suppressing until all released */
        FSM_DESTRUCTIVE_HOLD,   /* destructive confirmation in progress */
    };

    void setAction(MenuAction act);
    void processEdge(MenuButton btn, bool pressed, uint64_t now_us);
    void processTick(uint64_t now_us);
    void enterMenu(uint64_t now_us);
    void handleMenuSelect(MenuItemId id, uint64_t now_us);
    void render(void);

    /* Debounce (per-button) */
    bool m_stable[2];           /* debounced button state */
    bool m_edgeSeen[2];         /* true once any edge accepted */
    uint64_t m_lastEdge[2];     /* last accepted edge timestamp */

    /* Gesture FSM */
    FsmState m_fsm;
    MenuButton m_pendingBtn;    /* button in PENDING_SINGLE */
    uint64_t m_pendingStart;    /* timestamp of pending press */
    bool m_pendingReleased;     /* pending button released during window */
    uint64_t m_chordStart;      /* both-pressed timestamp */
    MenuButton m_trackBtn;      /* button in SINGLE_TRACKING/LONG_PRESS */
    uint64_t m_trackStart;      /* tracked button press timestamp */
    bool m_trackEmitted;        /* action already emitted for this track */

    /* Destructive hold */
    MenuConfirm m_destructiveKind;
    uint64_t m_destructiveStart;

    /* Menu navigation */
    bool m_menuOpen;
    MenuItemId m_currentItem;
    MenuConfirm m_confirmState;

    /* Action queue (small ring) */
    static constexpr uint8_t ACT_Q_SIZE = 4;
    MenuAction m_actQ[ACT_Q_SIZE];
    uint8_t m_actHead, m_actTail, m_actCount;

    /* Config */
    MenuAvailability m_avail;
    MenuRenderFn m_renderFn;
    void *m_renderCtx;
};

#endif /* __cplusplus */
#endif /* MENU_H */
