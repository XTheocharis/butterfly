/*
 * apds9960.cpp - APDS-9960 pure-logic implementation.
 *
 * No SDK dependencies.  All functions are host-testable.  The firmware
 * wrapper layer issues async i2cBus transfers and calls these functions
 * to validate registers, parse data, and decode gestures.
 */
#include "apds9960.h"

#include <string.h>
#include <stdlib.h>

/* ---- Compile-time register assertions -------------------------------- */

static_assert(APDS9960_ADDR == 0x39u, "APDS-9960 address must be 0x39");
static_assert(APDS9960_ID_VALUE == 0xABu, "APDS-9960 ID must be 0xAB");
static_assert(APDS9960_GOLDEN_ENABLE == 0x27u, "golden ENABLE");
static_assert(APDS9960_GOLDEN_ATIME == 0xFFu, "golden ATIME");
static_assert(APDS9960_GOLDEN_WTIME == 0xFFu, "golden WTIME");
static_assert(APDS9960_GOLDEN_PPULSE == 0xC9u, "golden PPULSE");
static_assert(APDS9960_GOLDEN_CONTROL == 0x20u, "golden CONTROL");
static_assert(APDS9960_GOLDEN_CONFIG2 == 0x00u, "golden CONFIG2");
static_assert(APDS9960_GOLDEN_GPENTH == 40u, "golden GPENTH");
static_assert(APDS9960_GOLDEN_GEXTH == 30u, "golden GEXTH");
static_assert(APDS9960_GOLDEN_GFIFOTH == 4u, "golden GFIFOTH");

/* ---- Mutual exclusion invariant -------------------------------------- */
/*
 * AEN (ALS/RGBC enable) and GEN (gesture enable) must NEVER be set
 * simultaneously.  This is the core hardware constraint: the gesture
 * engine and the RGBC ADC share the same analog frontend.
 */

static_assert((APDS9960_ENABLE_AEN & APDS9960_ENABLE_GEN) == 0,
	"AEN and GEN must be distinct bits");

bool apds9960_is_mutually_exclusive_violated(uint8_t enable_reg)
{
	bool aen = (enable_reg & APDS9960_ENABLE_AEN) != 0;
	bool gen = (enable_reg & APDS9960_ENABLE_GEN) != 0;
	return aen && gen;
}

/* ---- Mode transition state machine ----------------------------------- */

void apds9960_mode_init(apds9960_mode_state_t *st)
{
	if (st == NULL) {
		return;
	}
	st->current = APDS9960_MODE_OFF;
	st->target = APDS9960_MODE_OFF;
	st->transition_pending = false;
}

apds9960_transition_result_t apds9960_request_mode(
	apds9960_mode_state_t *st, apds9960_mode_t target)
{
	if (st == NULL) {
		return APDS9960_TRANSITION_BUSY;
	}

	/* Already in target mode — no-op. */
	if (st->current == target && !st->transition_pending) {
		return APDS9960_TRANSITION_SAME;
	}

	/* A transition is already in flight. */
	if (st->transition_pending) {
		/* If the pending target matches, allow (idempotent).
		 * Otherwise reject. */
		if (st->target == target) {
			return APDS9960_TRANSITION_SAME;
		}
		return APDS9960_TRANSITION_BUSY;
	}

	/* OFF → any target is immediate (no quiesce needed). */
	if (st->current == APDS9960_MODE_OFF) {
		st->current = target;
		st->target = target;
		st->transition_pending = false;
		return APDS9960_TRANSITION_OK;
	}

	/* Same mode — already handled above. */

	/* Optical ↔ Gesture: must quiesce first. */
	st->target = target;
	st->transition_pending = true;
	st->current = APDS9960_MODE_QUIESCING;
	return APDS9960_TRANSITION_OK;
}

apds9960_mode_t apds9960_complete_quiesce(apds9960_mode_state_t *st)
{
	if (st == NULL) {
		return APDS9960_MODE_OFF;
	}

	if (st->transition_pending) {
		st->current = st->target;
		st->transition_pending = false;
	}

	return st->current;
}

/* ---- Register config validation ------------------------------------- */

static bool check_golden(uint8_t reg, uint8_t value)
{
	switch (reg) {
	case APDS9960_REG_ENABLE:
		return value == APDS9960_GOLDEN_ENABLE;
	case APDS9960_REG_ATIME:
		return value == APDS9960_GOLDEN_ATIME;
	case APDS9960_REG_WTIME:
		return value == APDS9960_GOLDEN_WTIME;
	case APDS9960_REG_PPULSE:
		return value == APDS9960_GOLDEN_PPULSE;
	case APDS9960_REG_CONTROL:
		return value == APDS9960_GOLDEN_CONTROL;
	case APDS9960_REG_CONFIG2:
		return value == APDS9960_GOLDEN_CONFIG2;
	case APDS9960_REG_GPENTH:
		return value == APDS9960_GOLDEN_GPENTH;
	case APDS9960_REG_GEXTH:
		return value == APDS9960_GOLDEN_GEXTH;
	default:
		return false;
	}
}

bool apds9960_validate_optical_config(
	const apds9960_reg_val_t *config, size_t count)
{
	if (config == NULL || count == 0) {
		return false;
	}

	for (size_t i = 0; i < count; i++) {
		if (!check_golden(config[i].reg, config[i].value)) {
			return false;
		}
	}
	return true;
}

bool apds9960_is_optical_enabled(uint8_t enable_reg)
{
	bool pon = (enable_reg & APDS9960_ENABLE_PON) != 0;
	bool aen = (enable_reg & APDS9960_ENABLE_AEN) != 0;
	bool pen = (enable_reg & APDS9960_ENABLE_PEN) != 0;
	bool gen = (enable_reg & APDS9960_ENABLE_GEN) != 0;
	return pon && aen && pen && !gen;
}

bool apds9960_is_gesture_enabled(uint8_t enable_reg)
{
	bool pon = (enable_reg & APDS9960_ENABLE_PON) != 0;
	bool gen = (enable_reg & APDS9960_ENABLE_GEN) != 0;
	bool aen = (enable_reg & APDS9960_ENABLE_AEN) != 0;
	return pon && gen && !aen;
}

/* ---- RGBC parsing --------------------------------------------------- */

void apds9960_parse_rgbc(const uint8_t burst[APDS9960_RGBC_LEN],
                         apds9960_optical_sample_t *out)
{
	if (burst == NULL || out == NULL) {
		return;
	}

	out->clear = ((uint32_t)burst[APDS9960_C_OFFSET + 1] << 8)
	           | burst[APDS9960_C_OFFSET];
	out->red   = ((uint32_t)burst[APDS9960_R_OFFSET + 1] << 8)
	           | burst[APDS9960_R_OFFSET];
	out->green = ((uint32_t)burst[APDS9960_G_OFFSET + 1] << 8)
	           | burst[APDS9960_G_OFFSET];
	out->blue  = ((uint32_t)burst[APDS9960_B_OFFSET + 1] << 8)
	           | burst[APDS9960_B_OFFSET];
	out->proximity = 0;
}

/* ---- Gesture FIFO decode -------------------------------------------- */

void apds9960_gesture_dec_init(apds9960_gesture_decoder_t *dec)
{
	if (dec == NULL) {
		return;
	}
	memset(dec, 0, sizeof(*dec));
}

void apds9960_gesture_dec_feed(apds9960_gesture_decoder_t *dec,
                               uint8_t u, uint8_t d,
                               uint8_t l, uint8_t r)
{
	if (dec == NULL) {
		return;
	}

	int32_t iu = (int32_t)u;
	int32_t id = (int32_t)d;
	int32_t il = (int32_t)l;
	int32_t ir = (int32_t)r;

	dec->ud_delta += (iu - id);
	dec->lr_delta += (il - ir);

	/* Track absolute magnitudes for Near/Far detection. */
	int32_t au = iu;
	if (au < 0) au = -au;
	int32_t ad = id;
	if (ad < 0) ad = -ad;
	dec->ud_abs_sum += au + ad;

	int32_t al = il;
	if (al < 0) al = -al;
	int32_t ar = ir;
	if (ar < 0) ar = -ar;
	dec->lr_abs_sum += al + ar;

	dec->datasets++;
}

apds9960_gesture_t apds9960_gesture_dec_result(
	const apds9960_gesture_decoder_t *dec)
{
	if (dec == NULL || dec->datasets == 0) {
		return APDS9960_GESTURE_NONE;
	}

	int32_t ud_abs = dec->ud_delta;
	if (ud_abs < 0) ud_abs = -ud_abs;
	int32_t lr_abs = dec->lr_delta;
	if (lr_abs < 0) lr_abs = -lr_abs;

	/* Need enough signal to distinguish from noise. */
	if (ud_abs < 10 && lr_abs < 10) {
		return APDS9960_GESTURE_NONE;
	}

	/* Near/Far: both opposing pairs increase/decrease together. */
	if (ud_abs < 15 && lr_abs < 15) {
		/* If total magnitude is high but deltas are near-zero,
		 * both pairs rose together → Near. */
		int32_t total = dec->ud_abs_sum + dec->lr_abs_sum;
		if (total > (int32_t)(dec->datasets * 200)) {
			return APDS9960_GESTURE_NEAR;
		}
		if (total < (int32_t)(dec->datasets * 20)) {
			return APDS9960_GESTURE_FAR;
		}
		return APDS9960_GESTURE_NONE;
	}

	/* Dominant axis determines direction. */
	if (ud_abs > lr_abs) {
		/* Vertical gesture. */
		if (dec->ud_delta > 0) {
			return APDS9960_GESTURE_UP;
		}
		return APDS9960_GESTURE_DOWN;
	}

	/* Horizontal gesture. */
	if (dec->lr_delta > 0) {
		return APDS9960_GESTURE_LEFT;
	}
	return APDS9960_GESTURE_RIGHT;
}

apds9960_gesture_t apds9960_decode_fifo(
	const uint8_t *fifo_u, const uint8_t *fifo_d,
	const uint8_t *fifo_l, const uint8_t *fifo_r,
	uint32_t count)
{
	if (count == 0 || count > APDS9960_FIFO_DEPTH) {
		return APDS9960_GESTURE_NONE;
	}
	if (fifo_u == NULL || fifo_d == NULL ||
	    fifo_l == NULL || fifo_r == NULL) {
		return APDS9960_GESTURE_NONE;
	}

	apds9960_gesture_decoder_t dec;
	apds9960_gesture_dec_init(&dec);

	for (uint32_t i = 0; i < count; i++) {
		apds9960_gesture_dec_feed(&dec,
			fifo_u[i], fifo_d[i], fifo_l[i], fifo_r[i]);
	}

	return apds9960_gesture_dec_result(&dec);
}

/* ---- Absent device detection ---------------------------------------- */

bool apds9960_check_id(uint8_t id_reg_value)
{
	return id_reg_value == APDS9960_ID_VALUE;
}

/* ---- Gesture to proto mapping --------------------------------------- */

uint32_t apds9960_gesture_to_proto(apds9960_gesture_t g)
{
	/* Internal enum values match board.proto Gesture enum exactly:
	 * UNKNOWN=0, UP=1, DOWN=2, LEFT=3, RIGHT=4, NEAR=5, FAR=6 */
	return (uint32_t)g;
}

/* ---- FIFO overflow detection ---------------------------------------- */

bool apds9960_fifo_overflow(uint8_t gflvl)
{
	return gflvl > APDS9960_FIFO_DEPTH;
}
