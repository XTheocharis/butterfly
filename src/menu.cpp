/*
 * menu.cpp - recovery/normal menu with app-time A+B chord recognition.
 *
 * Three layers:
 *   1. Debounce: rejects edges < MENU_DEBOUNCE_US apart per button.
 *   2. Gesture FSM: classifies debounced edges into singles / chord / long.
 *   3. Menu nav: consumes classified gestures for item navigation and
 *      destructive confirmations.
 *
 * No SDK dependencies. Display rendering via Todo 9 widget callback.
 */
#include "menu.h"
#ifdef BOARD_CLUE
#include "custom_board.h"
#endif

/* ---- Item availability ---- */

bool menuItemAvailable(MenuItemId item, const MenuAvailability &avail) {
    switch (item) {
    case MENU_ITEM_STATUS:           return true;
    case MENU_ITEM_RUNTIME_OVERRIDE: return true;
    case MENU_ITEM_PAIR_FORGET:      return avail.ble;
    case MENU_ITEM_IMU_CAL:          return avail.imu;
    case MENU_ITEM_MAG_CAL:          return avail.mag;
    case MENU_ITEM_REMOTE_PROFILE:   return avail.ble;
    case MENU_ITEM_STORAGE:          return avail.qspi;
    case MENU_ITEM_SENSOR_DASHBOARD: return avail.sensors;
    case MENU_ITEM_DFU:              return true;
    case MENU_ITEM_EXIT:             return true;
    default:                         return false;
    }
}

#ifdef BOARD_CLUE
static void menuWidgetRender(DisplayModule &disp, const DisplayRect &clip,
                             void *ctx) {
    (void)disp; (void)clip;
    auto *menu = static_cast<MenuManager *>(ctx);
    if (!menu || !menu->isOpen()) return;
}
#endif

/* ---- MenuManager ---- */

MenuManager::MenuManager()
    : m_fsm(FSM_IDLE)
    , m_pendingBtn(MENU_BTN_NONE), m_pendingStart(0), m_pendingReleased(false)
    , m_chordStart(0)
    , m_trackBtn(MENU_BTN_NONE), m_trackStart(0), m_trackEmitted(false)
    , m_destructiveKind(MENU_CONF_NONE), m_destructiveStart(0)
    , m_menuOpen(false)
    , m_currentItem(MENU_ITEM_STATUS), m_confirmState(MENU_CONF_NONE)
    , m_actHead(0), m_actTail(0), m_actCount(0)
    , m_renderFn(nullptr), m_renderCtx(nullptr) {
    m_stable[0] = m_stable[1] = false;
    m_edgeSeen[0] = m_edgeSeen[1] = false;
    m_lastEdge[0] = m_lastEdge[1] = 0;
}

void MenuManager::init(const MenuAvailability *a, MenuRenderFn fn,
                        void *ctx) {
    m_avail = a ? *a : menuDefaultAvailability();
    m_renderFn = fn;
    m_renderCtx = ctx;
}

void MenuManager::setAction(MenuAction act) {
    if (m_actCount < ACT_Q_SIZE) {
        m_actQ[m_actHead] = act;
        m_actHead = (uint8_t)((m_actHead + 1) % ACT_Q_SIZE);
        m_actCount++;
    }
}

MenuAction MenuManager::pollAction(void) {
    if (m_actCount == 0) return MENU_ACT_NONE;
    MenuAction a = m_actQ[m_actTail];
    m_actTail = (uint8_t)((m_actTail + 1) % ACT_Q_SIZE);
    m_actCount--;
    return a;
}

uint8_t MenuManager::destructivePct(void) const {
    if (m_fsm != FSM_DESTRUCTIVE_HOLD || m_destructiveStart == 0) return 0;
    return 0;
}

static bool isDestructiveWarning(MenuConfirm s) {
    return s == MENU_CONF_FORGET_WARN || s == MENU_CONF_ERASE_WARN
        || s == MENU_CONF_FDS_WARN    || s == MENU_CONF_QSPI_WARN;
}

void MenuManager::feedEdge(MenuButton btn, bool pressed, uint64_t now_us) {
    if (btn == MENU_BTN_NONE) return;
    uint8_t idx = (btn == MENU_BTN_A) ? 0 : 1;

    if (m_edgeSeen[idx] && (now_us - m_lastEdge[idx] < MENU_DEBOUNCE_US)) return;
    if (pressed == m_stable[idx]) return;

    m_stable[idx] = pressed;
    m_lastEdge[idx] = now_us;
    m_edgeSeen[idx] = true;

    if (isDestructiveWarning(m_confirmState)) {
        bool aDown = m_stable[0];
        bool bDown = m_stable[1];

        if (m_confirmState == MENU_CONF_QSPI_WARN) {
            if (aDown && bDown) {
                m_confirmState = MENU_CONF_QSPI_HOLD;
                m_fsm = FSM_DESTRUCTIVE_HOLD;
                m_destructiveKind = MENU_CONF_QSPI_HOLD;
                m_destructiveStart = now_us;
            } else if (pressed && btn == MENU_BTN_B && !aDown) {
                m_confirmState = MENU_CONF_NONE;
                setAction(MENU_ACT_DESTRUCTIVE_CANCEL);
                render();
                return;
            }
        } else {
            if (pressed && btn == MENU_BTN_B) {
                m_confirmState = MENU_CONF_NONE;
                setAction(MENU_ACT_DESTRUCTIVE_CANCEL);
                render();
                return;
            }
            if (pressed && btn == MENU_BTN_A) {
            switch (m_confirmState) {
            case MENU_CONF_FORGET_WARN:
                m_confirmState = MENU_CONF_FORGET_HOLD;
                m_destructiveKind = MENU_CONF_FORGET_HOLD;
                break;
            case MENU_CONF_ERASE_WARN:
                m_confirmState = MENU_CONF_ERASE_HOLD;
                m_destructiveKind = MENU_CONF_ERASE_HOLD;
                break;
            case MENU_CONF_FDS_WARN:
                m_confirmState = MENU_CONF_FDS_HOLD;
                m_destructiveKind = MENU_CONF_FDS_HOLD;
                break;
            default: break;
            }
            m_fsm = FSM_DESTRUCTIVE_HOLD;
            m_destructiveStart = now_us;
            }
        }
        render();
        return;
    }

    processEdge(btn, pressed, now_us);
}

void MenuManager::processEdge(MenuButton btn, bool pressed, uint64_t now_us) {
    switch (m_fsm) {
    case FSM_IDLE:
        if (pressed) {
            m_fsm = FSM_PENDING_SINGLE;
            m_pendingBtn = btn;
            m_pendingStart = now_us;
            m_pendingReleased = false;
        }
        break;

    case FSM_PENDING_SINGLE: {
        bool otherPressed = (btn != m_pendingBtn && pressed);
        bool selfReleased = (btn == m_pendingBtn && !pressed);

        if (otherPressed && !m_pendingReleased) {
            m_fsm = FSM_CHORD_PENDING;
            m_chordStart = now_us;
        } else if (otherPressed && m_pendingReleased) {
            if (!m_menuOpen) {
                setAction(m_pendingBtn == MENU_BTN_A
                              ? MENU_ACT_SINGLE_A_PRESS
                              : MENU_ACT_SINGLE_A_RELEASE);
                setAction(m_pendingBtn == MENU_BTN_B
                              ? MENU_ACT_SINGLE_B_PRESS
                              : MENU_ACT_SINGLE_B_RELEASE);
            }
            m_pendingBtn = btn;
            m_pendingStart = now_us;
            m_pendingReleased = false;
        } else if (selfReleased) {
            m_pendingReleased = true;
        }
        break;
    }

    case FSM_CHORD_PENDING:
        if (!pressed) m_fsm = FSM_SUPPRESSED;
        break;

    case FSM_CHORD_FIRED:
        if (!pressed && !m_stable[0] && !m_stable[1]) {
            enterMenu(now_us);
            m_fsm = FSM_IDLE;
        }
        break;

    case FSM_SINGLE_TRACKING:
        if (!pressed && btn == m_trackBtn) {
            if (m_menuOpen) {
                if (!m_trackEmitted && m_trackBtn == MENU_BTN_B) {
                    m_currentItem = (MenuItemId)((m_currentItem + 1) %
                                   MENU_ITEM_COUNT);
                    setAction(MENU_ACT_NAV_NEXT);
                }
            } else {
                setAction(m_trackBtn == MENU_BTN_A
                              ? MENU_ACT_SINGLE_A_RELEASE
                              : MENU_ACT_SINGLE_B_RELEASE);
            }
            m_fsm = FSM_IDLE;
        }
        break;

    case FSM_LONG_PRESS:
        if (!pressed && btn == m_trackBtn) m_fsm = FSM_IDLE;
        break;

    case FSM_SUPPRESSED:
        if (!m_stable[0] && !m_stable[1]) m_fsm = FSM_IDLE;
        break;

    case FSM_DESTRUCTIVE_HOLD:
        if (!pressed) {
            bool aReq = (m_destructiveKind == MENU_CONF_FDS_HOLD
                      || m_destructiveKind == MENU_CONF_FORGET_HOLD
                      || m_destructiveKind == MENU_CONF_ERASE_HOLD);
            bool abReq = (m_destructiveKind == MENU_CONF_QSPI_HOLD);
            if ((aReq && btn == MENU_BTN_A)
             || (abReq && (btn == MENU_BTN_A || btn == MENU_BTN_B))) {
                setAction(MENU_ACT_DESTRUCTIVE_CANCEL);
                switch (m_destructiveKind) {
                case MENU_CONF_FDS_HOLD:    m_confirmState = MENU_CONF_FDS_WARN;    break;
                case MENU_CONF_QSPI_HOLD:   m_confirmState = MENU_CONF_QSPI_WARN;   break;
                case MENU_CONF_FORGET_HOLD: m_confirmState = MENU_CONF_FORGET_WARN; break;
                case MENU_CONF_ERASE_HOLD:  m_confirmState = MENU_CONF_ERASE_WARN;  break;
                default: break;
                }
                m_fsm = FSM_IDLE;
            }
        }
        break;
    }
    render();
}

void MenuManager::processTick(uint64_t now_us) {
    switch (m_fsm) {
    case FSM_PENDING_SINGLE:
        if (now_us - m_pendingStart >= MENU_CHORD_WINDOW_US) {
            if (m_pendingReleased) {
                if (!m_menuOpen) {
                    MenuAction p = (m_pendingBtn == MENU_BTN_A)
                                       ? MENU_ACT_SINGLE_A_PRESS
                                       : MENU_ACT_SINGLE_B_PRESS;
                    MenuAction r = (m_pendingBtn == MENU_BTN_A)
                                       ? MENU_ACT_SINGLE_A_RELEASE
                                       : MENU_ACT_SINGLE_B_RELEASE;
                    setAction(p); setAction(r);
                } else {
                    if (m_pendingBtn == MENU_BTN_A)
                        handleMenuSelect(m_currentItem, now_us);
                    else {
                        m_currentItem = (MenuItemId)((m_currentItem + 1) %
                                       MENU_ITEM_COUNT);
                        setAction(MENU_ACT_NAV_NEXT);
                    }
                }
                m_fsm = FSM_IDLE;
            } else {
                m_fsm = FSM_SINGLE_TRACKING;
                m_trackBtn = m_pendingBtn;
                m_trackStart = m_pendingStart;
                m_trackEmitted = false;
                if (!m_menuOpen) {
                    setAction(m_trackBtn == MENU_BTN_A
                                  ? MENU_ACT_SINGLE_A_PRESS
                                  : MENU_ACT_SINGLE_B_PRESS);
                    m_trackEmitted = true;
                } else if (m_trackBtn == MENU_BTN_A) {
                    handleMenuSelect(m_currentItem, now_us);
                    m_trackEmitted = true;
                }
            }
        }
        break;

    case FSM_CHORD_PENDING:
        if (now_us - m_chordStart >= MENU_CHORD_HOLD_US)
            m_fsm = FSM_CHORD_FIRED;
        break;

    case FSM_SINGLE_TRACKING:
        if (m_menuOpen && m_trackBtn == MENU_BTN_B && !m_trackEmitted) {
            if (now_us - m_trackStart >= MENU_LONG_PRESS_US) {
                if (m_confirmState != MENU_CONF_NONE)
                    m_confirmState = MENU_CONF_NONE;
                setAction(MENU_ACT_NAV_BACK);
                m_trackEmitted = true;
                m_fsm = FSM_LONG_PRESS;
            }
        }
        break;

    case FSM_DESTRUCTIVE_HOLD:
        if (now_us - m_destructiveStart >= MENU_DESTRUCTIVE_HOLD_US) {
            switch (m_destructiveKind) {
            case MENU_CONF_FDS_HOLD:    setAction(MENU_ACT_DESTRUCTIVE_FDS);    break;
            case MENU_CONF_QSPI_HOLD:   setAction(MENU_ACT_DESTRUCTIVE_QSPI);   break;
            case MENU_CONF_FORGET_HOLD: setAction(MENU_ACT_DESTRUCTIVE_FORGET); break;
            case MENU_CONF_ERASE_HOLD:  setAction(MENU_ACT_DESTRUCTIVE_ERASE);  break;
            default: break;
            }
            m_confirmState = MENU_CONF_NONE;
            m_fsm = FSM_IDLE;
        }
        break;

    default: break;
    }
    render();
}

void MenuManager::tick(uint64_t now_us) { processTick(now_us); }

void MenuManager::enterMenu(uint64_t now_us) {
    (void)now_us;
    m_menuOpen = true;
    m_currentItem = MENU_ITEM_STATUS;
    m_confirmState = MENU_CONF_NONE;
    setAction(MENU_ACT_ENTER_MENU);
    render();
}

void MenuManager::open(uint64_t now_us) { enterMenu(now_us); }

void MenuManager::close(void) {
    m_menuOpen = false;
    m_confirmState = MENU_CONF_NONE;
    m_fsm = FSM_IDLE;
    setAction(MENU_ACT_EXIT_MENU);
    render();
}

void MenuManager::handleMenuSelect(MenuItemId id, uint64_t now_us) {
    (void)now_us;
    switch (id) {
    case MENU_ITEM_EXIT:
        close();
        break;
    case MENU_ITEM_PAIR_FORGET:
        if (m_avail.ble) m_confirmState = MENU_CONF_FORGET_WARN;
        break;
    case MENU_ITEM_STORAGE:
        if (m_avail.qspi) m_confirmState = MENU_CONF_QSPI_WARN;
        break;
    default: break;
    }
    setAction(MENU_ACT_NAV_SELECT);
    render();
}

void MenuManager::render(void) {
    if (m_renderFn) m_renderFn(m_renderCtx);
}

void MenuManager::registerOnDisplay(DisplayModule &disp, uint8_t page) {
#ifdef BOARD_CLUE
    DisplayWidgetConfig cfg = {};
    cfg.bounds = {0, 0, CLUE_TFT_WIDTH, CLUE_TFT_HEIGHT};
    cfg.render = menuWidgetRender;
    cfg.context = this;
    cfg.z = 100;
    cfg.page = page;
    WidgetHandle h = disp.registerWidget(cfg);
    if (h != DISPLAY_WIDGET_INVALID) disp.addWidgetToPage(page, h);
#else
    (void)disp; (void)page;
#endif
}
