/*
 * expert_eval.c - Pure-C GPIO/SAADC expert API evaluation (host-testable).
 *
 * No SDK deps. All decision logic for the CLUE edge-connector expert APIs.
 * Compiled from both firmware wrappers (gpio.cpp, adc.cpp) and host tests.
 *
 * allow: SIZE_OK — the alias table, GPIO validation, ADC math, and lease
 * table are one cohesive module. Splitting would force callers to know
 * which sub-module owns which function, and the lease table references
 * the alias table for analog checks. Same exception as qspi_eval.c.
 */
#include "expert_eval.h"

/* ---- Frozen alias table -----------------------------------------------
 *
 * nRF52840 SAADC analog inputs: AIN0= P0.02, AIN1= P0.03,
 * AIN2= P0.04, AIN3= P0.05, AIN4= P0.28, AIN5= P0.29,
 * AIN6= P0.30, AIN7= P0.31.
 *
 * CLUE edge connector maps D-pins to P0.NN in a non-sequential order,
 * and the Arduino A-number does NOT correlate with AIN:
 *   A0 → AIN7, A1 → AIN5, A2 → AIN2, A3 → AIN3,
 *   A4 → AIN1, A5 → AIN4, A6 → AIN0, A7 → AIN6.
 */
const expert_alias_t EXPERT_ALIASES[EXPERT_ALIAS_COUNT] = {
	{  0, 2,  4, 2 },  /* D0  / A2 : P0.04 → AIN2 */
	{  1, 3,  5, 3 },  /* D1  / A3 : P0.05 → AIN3 */
	{  2, 4,  3, 1 },  /* D2  / A4 : P0.03 → AIN1 */
	{  3, 5, 28, 4 },  /* D3  / A5 : P0.28 → AIN4 */
	{  4, 6,  2, 0 },  /* D4  / A6 : P0.02 → AIN0 */
	{ 10, 7, 30, 6 },  /* D10 / A7 : P0.30 → AIN6 */
	{ 12, 0, 31, 7 },  /* D12 / A0 : P0.31 → AIN7 */
	{ 16, 1, 29, 5 },  /* D16 / A1 : P0.29 → AIN5 */
};

const expert_alias_t *expert_eval_alias_by_d(uint8_t d_pin)
{
	for (uint8_t i = 0; i < EXPERT_ALIAS_COUNT; i++) {
		if (EXPERT_ALIASES[i].d_pin == d_pin)
			return &EXPERT_ALIASES[i];
	}
	return NULL;
}

const expert_alias_t *expert_eval_alias_by_a(uint8_t a_pin)
{
	for (uint8_t i = 0; i < EXPERT_ALIAS_COUNT; i++) {
		if (EXPERT_ALIASES[i].a_pin == a_pin)
			return &EXPERT_ALIASES[i];
	}
	return NULL;
}

const expert_alias_t *expert_eval_alias_by_resource(uint8_t resource_id)
{
	for (uint8_t i = 0; i < EXPERT_ALIAS_COUNT; i++) {
		if (EXPERT_ALIASES[i].resource_id == resource_id)
			return &EXPERT_ALIASES[i];
	}
	return NULL;
}

bool expert_eval_d_to_ain(uint8_t d_pin, uint8_t *out_ain)
{
	const expert_alias_t *a = expert_eval_alias_by_d(d_pin);
	if (!a) return false;
	*out_ain = a->ain;
	return true;
}

bool expert_eval_is_analog_pin(uint8_t resource_id)
{
	return expert_eval_alias_by_resource(resource_id) != NULL;
}

/* ---- GPIO config validation ----------------------------------------- */

bool expert_eval_gpio_config_valid(const expert_gpio_config_t *cfg)
{
	if (!cfg) return false;
	if ((unsigned)cfg->dir   >= EXPERT_GPIO_DIR_COUNT)   return false;
	if ((unsigned)cfg->pull  >= EXPERT_GPIO_PULL_COUNT)  return false;
	if ((unsigned)cfg->drive >= EXPERT_GPIO_DRIVE_COUNT) return false;
	if ((unsigned)cfg->sense >= EXPERT_GPIO_SENSE_COUNT) return false;
	return true;
}

/* ---- SAADC conversion ----------------------------------------------- */

expert_adc_status_t expert_eval_adc_raw_to_mv(uint16_t raw, uint16_t *out_mv)
{
	/* mV = raw * 3600 / 4095. Integer math, no float.
	 * 3600 * 4095 = 14,742,000 — fits in uint32_t. */
	uint32_t mv = ((uint32_t)raw * EXPERT_ADC_FS_MV) / EXPERT_ADC_MAX_RAW;

	if (mv > EXPERT_VDD_NOMINAL_MV) {
		*out_mv = EXPERT_VDD_NOMINAL_MV;
		return EXPERT_ADC_SATURATED;
	}
	*out_mv = (uint16_t)mv;
	return EXPERT_ADC_OK;
}

uint16_t expert_eval_adc_oversample_4x(const uint16_t samples[EXPERT_ADC_OVERSAMPLE_COUNT])
{
	uint32_t sum = 0;
	for (uint8_t i = 0; i < EXPERT_ADC_OVERSAMPLE_COUNT; i++) {
		sum += samples[i];
	}
	/* Right-shift by 2 (divide by 4). */
	return (uint16_t)(sum >> 2);
}

expert_adc_cal_status_t expert_eval_adc_calibrate_result(void)
{
	return EXPERT_ADC_CAL_DONE;
}

/* ---- Expert lease table --------------------------------------------- */

typedef struct {
	expert_token_t token;      /* 1-based, never 0 (invalid)              */
	uint8_t  resource_id;
	uint8_t  owner;
	uint16_t session;
	bool     active;
} expert_lease_entry_t;

static expert_lease_entry_t g_leases[EXPERT_MAX_LEASES];
static uint16_t             g_session;
static expert_token_t       g_nextToken;
static expert_token_t       g_lastReleasedToken;  /* for idempotent release */
static uint16_t             g_lastReleasedSession;

void expert_eval_init(void)
{
	/* Session wraps 0xFFFF → 1 (never 0 to avoid collision with initial). */
	g_session++;
	if (g_session == 0) g_session = 1;

	g_nextToken = 0;
	g_lastReleasedToken = EXPERT_TOKEN_INVALID;
	g_lastReleasedSession = 0;

	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++) {
		g_leases[i].active = false;
	}
}

uint16_t expert_eval_get_session(void)
{
	return g_session;
}

static expert_lease_entry_t *find_by_token(expert_token_t token)
{
	if (token == EXPERT_TOKEN_INVALID) return NULL;
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++) {
		if (g_leases[i].active && g_leases[i].token == token)
			return &g_leases[i];
	}
	return NULL;
}

static expert_lease_entry_t *find_free_slot(void)
{
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++) {
		if (!g_leases[i].active)
			return &g_leases[i];
	}
	return NULL;
}

static expert_token_t alloc_token(void)
{
	expert_token_t t = ++g_nextToken;
	return (t != EXPERT_TOKEN_INVALID) ? t : ++g_nextToken;
}

expert_result_t expert_eval_lease_issue(uint8_t resource_id, uint8_t owner,
                                        bool require_analog,
                                        expert_token_t *out_token)
{
	if (!out_token) return EXPERT_ERR_INVALID_PARAM;
	*out_token = EXPERT_TOKEN_INVALID;

	/* Owner 0 is PINREG_OWNER_NONE — invalid for a lease. */
	if (owner == 0) return EXPERT_ERR_INVALID_PARAM;

	if (require_analog && !expert_eval_is_analog_pin(resource_id))
		return EXPERT_ERR_NOT_ANALOG;

	expert_lease_entry_t *slot = find_free_slot();
	if (!slot) return EXPERT_ERR_TABLE_FULL;

	slot->token       = alloc_token();
	slot->resource_id = resource_id;
	slot->owner       = owner;
	slot->session     = g_session;
	slot->active      = true;

	*out_token = slot->token;
	return EXPERT_OK;
}

expert_result_t expert_eval_lease_validate(expert_token_t token, uint8_t owner)
{
	/* Idempotent release cache: a just-released token is NOT valid for
	 * new operations — it's just valid for a repeat release call. */
	expert_lease_entry_t *entry = find_by_token(token);
	if (!entry) return EXPERT_ERR_INVALID_TOKEN;

	if (entry->session != g_session)
		return EXPERT_ERR_WRONG_SESSION;

	if (entry->owner != owner)
		return EXPERT_ERR_WRONG_OWNER;

	return EXPERT_OK;
}

expert_result_t expert_eval_lease_release(expert_token_t token, uint8_t owner)
{
	/* Idempotent: if this exact token was released in this session
	 * as the immediately preceding release, return OK (no-op). */
	if (token == g_lastReleasedToken && g_lastReleasedSession == g_session) {
		return EXPERT_OK;
	}

	expert_lease_entry_t *entry = find_by_token(token);
	if (!entry) {
		/* Not active and not the last-released → genuinely invalid. */
		return EXPERT_ERR_INVALID_TOKEN;
	}

	if (entry->session != g_session)
		return EXPERT_ERR_WRONG_SESSION;

	/* Owner check: only the original holder may release. Wrong owner
	 * gets rejected before any state change. The idempotent cache
	 * above still lets a retrying owner repeat its own release. */
	if (entry->owner != owner)
		return EXPERT_ERR_WRONG_OWNER;

	entry->active = false;

	/* Cache for idempotent repeat. */
	g_lastReleasedToken  = token;
	g_lastReleasedSession = g_session;

	return EXPERT_OK;
}

void expert_eval_lease_release_all(void)
{
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++) {
		g_leases[i].active = false;
	}
	g_lastReleasedToken   = EXPERT_TOKEN_INVALID;
	g_lastReleasedSession = 0;
}

bool expert_eval_lease_query(expert_token_t token,
                             uint8_t *out_resource, uint8_t *out_owner)
{
	expert_lease_entry_t *entry = find_by_token(token);
	if (!entry || entry->session != g_session)
		return false;
	if (out_resource) *out_resource = entry->resource_id;
	if (out_owner)    *out_owner    = entry->owner;
	return true;
}
