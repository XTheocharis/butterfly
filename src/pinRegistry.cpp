/*
 * pinRegistry.cpp - pure-logic pin/group ownership registry.
 * No SDK deps. Caller supplies crit-section hooks, restore callbacks,
 * and quiesce handlers at init. Compiles on host and firmware.
 *
 * allow: SIZE_OK — indivisible state machine; all 13 functions share the
 * same Resource array and generation/refcount invariants. Splitting would
 * scatter acquire/release/force/quiesce transitions across files.
 */
#include "pinRegistry.h"
#include <string.h>

#define PINREG_RESOURCE_COUNT (PINREG_PIN_COUNT + PINREG_GROUP_NUM_VALID)

struct Resource {
    uint8_t state, owner, boundGroup, leaseGroup;
    uint16_t generation;
    uint8_t refcount;
    pinreg_restore_fn restore;
    uint8_t memberPins[PINREG_MAX_PINS_PER_GROUP], memberCount;
    uint8_t pendingOwner;
    pinreg_restore_fn pendingRestore;
};

static Resource g_res[PINREG_RESOURCE_COUNT];
static pinreg_critical_fn g_critEnter, g_critExit;
static pinreg_event_fn g_eventSink;
static pinreg_quiesce_fn g_quiesce[PINREG_OWNER_COUNT][PINREG_GROUP_NUM_VALID];

/* RAII critical-section guard: enter on construct, exit on destruct. */
struct CritGuard {
    CritGuard() { if (g_critEnter) g_critEnter(); }
    ~CritGuard() { if (g_critExit) g_critExit(); }
};

static inline uint8_t groupToResId(pinreg_group_t g) {
    return (uint8_t)(PINREG_GROUP_BASE + (g - 1));
}
static inline pinreg_group_t resIdToGroup(uint8_t r) {
    return (pinreg_group_t)(r - PINREG_GROUP_BASE + 1);
}
static inline bool isGroupRes(uint8_t r) { return r >= PINREG_GROUP_BASE; }
static inline uint16_t nextGen(uint16_t c) {
    return (c == 0xFFFFu) ? 1u : (uint16_t)(c + 1);
}
static inline pinreg_token_t makeToken(uint8_t id, uint16_t gen) {
    return ((uint32_t)gen << 8) | (uint32_t)(id + 1);
}
static inline uint8_t tokenId(pinreg_token_t t) { return (uint8_t)((t & 0xFFu) - 1u); }
static inline uint16_t tokenGen(pinreg_token_t t) { return (uint16_t)(t >> 8); }
static void emit(pinreg_group_t g, pinreg_event_t e, pinreg_owner_t p, pinreg_token_t t) {
    if (g_eventSink) g_eventSink(g, e, p, t);
}

static pinreg_result_t checkMembers(const Resource &gr) {
    for (uint8_t i = 0; i < gr.memberCount; i++) {
        uint8_t s = g_res[gr.memberPins[i]].state;
        if (s == PINREG_STATE_RESERVED) return PINREG_RESERVED;
        if (s == PINREG_STATE_OWNED || s == PINREG_STATE_QUIESCING) return PINREG_BUSY;
    }
    return PINREG_OK;
}

static void grantGroup(uint8_t gid, pinreg_owner_t owner, pinreg_restore_fn restore) {
    Resource &gr = g_res[gid];
    gr.state = PINREG_STATE_OWNED;
    gr.owner = (uint8_t)owner;
    gr.generation = nextGen(gr.generation);
    gr.refcount = 1;
    gr.restore = restore;
    gr.leaseGroup = gr.boundGroup;
    for (uint8_t i = 0; i < gr.memberCount; i++) {
        Resource &p = g_res[gr.memberPins[i]];
        p.state = PINREG_STATE_OWNED;
        p.owner = (uint8_t)owner;
        p.generation = gr.generation;
        p.leaseGroup = gr.boundGroup;
    }
}

static void freeMembers(Resource &gr) {
    for (uint8_t i = 0; i < gr.memberCount; i++) {
        Resource &p = g_res[gr.memberPins[i]];
        p.state = PINREG_STATE_FREE;
        p.owner = PINREG_OWNER_NONE;
        p.leaseGroup = PINREG_GROUP_NONE;
    }
}

/* ---- Public API ---- */

void pinreg_init(pinreg_critical_fn enter, pinreg_critical_fn exit) {
    memset(g_res, 0, sizeof(g_res));
    memset(g_quiesce, 0, sizeof(g_quiesce));
    g_critEnter = enter;
    g_critExit = exit;
    g_eventSink = nullptr;

    /* Reserve fixed-function pins that must never be allocated via the
     * expert GPIO/I2C API regardless of force. On nRF52840:
     *   - USB D+ is on P0.13, USB D- on P0.14 (USB peripheral claim;
     *     also overlapped by CLUE_TFT_DC/SCK when USB is disabled).
     *   - SWDIO/SWCLK are dedicated debug bondouts, not in P0.x; the
     *     registry only covers port 0/1 GPIO so they need no entry. */
    pinreg_reserve(13);
    pinreg_reserve(14);
}

void pinreg_reserve(uint8_t resourceId) {
    if (resourceId >= PINREG_RESOURCE_COUNT) return;
    CritGuard cg;
    g_res[resourceId].state = PINREG_STATE_RESERVED;
}

void pinreg_group_set_pins(pinreg_group_t group, const uint8_t *pins, uint8_t count) {
    if (group < 1 || group > PINREG_GROUP_NUM_VALID || !pins ||
        count > PINREG_MAX_PINS_PER_GROUP) return;
    CritGuard cg;
    Resource &gr = g_res[groupToResId(group)];
    memcpy(gr.memberPins, pins, count);
    gr.memberCount = count;
    gr.boundGroup = (uint8_t)group;
    for (uint8_t i = 0; i < count; i++) g_res[pins[i]].boundGroup = (uint8_t)group;
}

void pinreg_set_event_sink(pinreg_event_fn fn) {
    CritGuard cg;
    g_eventSink = fn;
}

void pinreg_set_quiesce_handler(pinreg_owner_t owner, pinreg_group_t group, pinreg_quiesce_fn fn) {
    if (owner >= PINREG_OWNER_COUNT || group >= PINREG_GROUP_NUM_VALID) return;
    CritGuard cg;
    g_quiesce[owner][group] = fn;
}

pinreg_result_t pinreg_acquire_pin(uint8_t pin, pinreg_owner_t owner,
                                   pinreg_restore_fn restore, pinreg_token_t *outToken) {
    if (pin >= PINREG_PIN_COUNT || owner >= PINREG_OWNER_COUNT || !restore || !outToken)
        return PINREG_INVALID_PARAM;
    CritGuard cg;
    Resource &r = g_res[pin];
    if (r.state == PINREG_STATE_RESERVED) return PINREG_RESERVED;
    if (r.state != PINREG_STATE_FREE) return PINREG_BUSY;
    r.state = PINREG_STATE_OWNED;
    r.owner = (uint8_t)owner;
    r.generation = nextGen(r.generation);
    r.refcount = 1;
    r.restore = restore;
    r.leaseGroup = PINREG_GROUP_NONE;
    *outToken = makeToken(pin, r.generation);
    return PINREG_OK;
}

pinreg_result_t pinreg_acquire_group(pinreg_group_t group, pinreg_owner_t owner,
                                     pinreg_restore_fn restore, pinreg_token_t *outToken) {
    if (group < 1 || group > PINREG_GROUP_NUM_VALID ||
        owner >= PINREG_OWNER_COUNT || !restore || !outToken)
        return PINREG_INVALID_PARAM;
    uint8_t gid = groupToResId(group);
    CritGuard cg;
    Resource &gr = g_res[gid];
    if (gr.state == PINREG_STATE_RESERVED) return PINREG_RESERVED;
    if (gr.state != PINREG_STATE_FREE) return PINREG_BUSY;
    pinreg_result_t mr = checkMembers(gr);
    if (mr != PINREG_OK) return mr;
    grantGroup(gid, owner, restore);
    *outToken = makeToken(gid, gr.generation);
    return PINREG_OK;
}

pinreg_result_t pinreg_force_group(pinreg_group_t group, pinreg_owner_t owner,
                                   pinreg_restore_fn restore, pinreg_token_t *outToken) {
    if (group < 1 || group > PINREG_GROUP_NUM_VALID ||
        owner >= PINREG_OWNER_COUNT || !restore || !outToken)
        return PINREG_INVALID_PARAM;
    uint8_t gid = groupToResId(group);
    CritGuard cg;
    Resource &gr = g_res[gid];
    if (gr.state == PINREG_STATE_RESERVED) return PINREG_RESERVED;
    if (gr.state == PINREG_STATE_FREE) {
        grantGroup(gid, owner, restore);
        *outToken = makeToken(gid, gr.generation);
        return PINREG_OK;
    }
    if (gr.state == PINREG_STATE_QUIESCING) return PINREG_BUSY;
    /* OWNED: request async quiescence from current owner. */
    pinreg_quiesce_fn handler = g_quiesce[gr.owner][group];
    if (!handler || !handler(group)) return PINREG_NOT_CANCELLABLE;
    gr.state = PINREG_STATE_QUIESCING;
    gr.pendingOwner = (uint8_t)owner;
    gr.pendingRestore = restore;
    for (uint8_t i = 0; i < gr.memberCount; i++)
        g_res[gr.memberPins[i]].state = PINREG_STATE_QUIESCING;
    return PINREG_QUIESCING;
}

pinreg_result_t pinreg_release(pinreg_token_t token) {
    if (token == PINREG_TOKEN_INVALID) return PINREG_INVALID_TOKEN;
    uint8_t id = tokenId(token);
    if (id >= PINREG_RESOURCE_COUNT) return PINREG_INVALID_TOKEN;
    CritGuard cg;
    Resource &r = g_res[id];
    if (r.state != PINREG_STATE_OWNED || r.generation != tokenGen(token))
        return PINREG_INVALID_TOKEN;
    if (r.refcount > 1) { r.refcount--; return PINREG_OK; }
    if (isGroupRes(id)) {
        if (r.leaseGroup == PINREG_GROUP_NONE) return PINREG_INVALID_TOKEN;
        pinreg_restore_fn restore = r.restore;
        pinreg_owner_t prev = (pinreg_owner_t)r.owner;
        pinreg_group_t grp = resIdToGroup(id);
        r.state = PINREG_STATE_FREE;
        r.owner = PINREG_OWNER_NONE;
        r.refcount = 0;
        r.restore = nullptr;
        r.leaseGroup = PINREG_GROUP_NONE;
        freeMembers(r);
        if (restore) restore(grp, 0xFF);
        emit(grp, PINREG_EV_RESTORED, prev, PINREG_TOKEN_INVALID);
        return PINREG_OK;
    }
    /* Individual pin: reject if acquired via group (no partial release). */
    if (r.leaseGroup != PINREG_GROUP_NONE) return PINREG_INVALID_TOKEN;
    pinreg_restore_fn restore = r.restore;
    pinreg_owner_t prev = (pinreg_owner_t)r.owner;
    r.state = PINREG_STATE_FREE;
    r.owner = PINREG_OWNER_NONE;
    r.refcount = 0;
    r.restore = nullptr;
    if (restore) restore(PINREG_GROUP_NONE, id);
    emit(PINREG_GROUP_NONE, PINREG_EV_RESTORED, prev, PINREG_TOKEN_INVALID);
    return PINREG_OK;
}

pinreg_result_t pinreg_retain(pinreg_token_t token) {
    if (token == PINREG_TOKEN_INVALID) return PINREG_INVALID_TOKEN;
    uint8_t id = tokenId(token);
    if (id >= PINREG_RESOURCE_COUNT) return PINREG_INVALID_TOKEN;
    CritGuard cg;
    Resource &r = g_res[id];
    if (r.state != PINREG_STATE_OWNED || r.generation != tokenGen(token))
        return PINREG_INVALID_TOKEN;
    if (r.refcount >= 255) return PINREG_INVALID_PARAM;
    r.refcount++;
    return PINREG_OK;
}

pinreg_result_t pinreg_quiesce_complete(pinreg_group_t group) {
    if (group < 1 || group > PINREG_GROUP_NUM_VALID) return PINREG_INVALID_PARAM;
    uint8_t gid = groupToResId(group);
    CritGuard cg;
    Resource &gr = g_res[gid];
    if (gr.state != PINREG_STATE_QUIESCING) return PINREG_INVALID_TOKEN;
    /* Restore old owner's pins, emit displacement + restoration. */
    pinreg_restore_fn oldRestore = gr.restore;
    pinreg_owner_t displaced = (pinreg_owner_t)gr.owner;
    if (oldRestore) oldRestore(group, 0xFF);
    emit(group, PINREG_EV_RESTORED, displaced, PINREG_TOKEN_INVALID);
    emit(group, PINREG_EV_DISPLACED, displaced, PINREG_TOKEN_INVALID);
    /* Grant to pending owner. */
    gr.state = PINREG_STATE_OWNED;
    gr.owner = gr.pendingOwner;
    gr.restore = gr.pendingRestore;
    gr.generation = nextGen(gr.generation);
    gr.refcount = 1;
    gr.leaseGroup = (uint8_t)group;
    gr.pendingOwner = PINREG_OWNER_NONE;
    gr.pendingRestore = nullptr;
    for (uint8_t i = 0; i < gr.memberCount; i++) {
        Resource &p = g_res[gr.memberPins[i]];
        p.state = PINREG_STATE_OWNED;
        p.owner = gr.owner;
        p.generation = gr.generation;
        p.leaseGroup = (uint8_t)group;
    }
    emit(group, PINREG_EV_GRANTED, displaced, makeToken(gid, gr.generation));
    return PINREG_OK;
}

pinreg_result_t pinreg_query(uint8_t resourceId, pinreg_owner_t *outOwner) {
    if (resourceId >= PINREG_RESOURCE_COUNT) return PINREG_INVALID_PARAM;
    CritGuard cg;
    Resource &r = g_res[resourceId];
    if (r.state == PINREG_STATE_OWNED || r.state == PINREG_STATE_QUIESCING) {
        if (outOwner) *outOwner = (pinreg_owner_t)r.owner;
        return PINREG_OK;
    }
    return PINREG_BUSY;
}

void pinreg_reset_all(void) {
    CritGuard cg;
    for (uint8_t i = 0; i < PINREG_RESOURCE_COUNT; i++) {
        Resource &r = g_res[i];
        if (r.state != PINREG_STATE_OWNED && r.state != PINREG_STATE_QUIESCING)
            continue;
        /* Skip group member pins — restored via the group resource. */
        if (!isGroupRes(i) && r.leaseGroup != PINREG_GROUP_NONE)
            continue;
        pinreg_restore_fn restore = r.restore;
        pinreg_owner_t prev = (pinreg_owner_t)r.owner;
        bool isGrp = isGroupRes(i);
        pinreg_group_t grp = isGrp ? resIdToGroup(i) : PINREG_GROUP_NONE;
        r.state = PINREG_STATE_FREE;
        r.owner = PINREG_OWNER_NONE;
        r.refcount = 0;
        r.restore = nullptr;
        r.leaseGroup = PINREG_GROUP_NONE;
        r.pendingOwner = PINREG_OWNER_NONE;
        r.pendingRestore = nullptr;
        if (isGrp) freeMembers(r);
        if (restore) restore(grp, isGrp ? 0xFF : i);
        emit(grp, PINREG_EV_RESTORED, prev, PINREG_TOKEN_INVALID);
    }
}

void pinreg_inspect(uint8_t resourceId, pinreg_info_t *out) {
    if (!out || resourceId >= PINREG_RESOURCE_COUNT) return;
    CritGuard cg;
    Resource &r = g_res[resourceId];
    out->state = (pinreg_state_t)r.state;
    out->owner = (pinreg_owner_t)r.owner;
    out->generation = r.generation;
    out->refcount = r.refcount;
    out->memberCount = isGroupRes(resourceId) ? r.memberCount : 0;
}
