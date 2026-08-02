/*
 * expert_eval.h - Pure-C GPIO/SAADC expert API evaluation (host-testable).
 *
 * Contains ALL decision logic for the CLUE edge-connector expert APIs:
 *   - Pin alias table: D-pin ↔ A-pin ↔ P0.NN resource ↔ SAADC AIN channel
 *   - GPIO config validation (direction/pull/drive/sense enum ranges)
 *   - SAADC raw-to-millivolt conversion (12-bit, 0.6V ref, gain 1/6)
 *   - SAADC 4x oversampling averaging
 *   - Expert lease table: session + owner validation on opaque tokens,
 *     idempotent release for the immediately-preceding release.
 *
 * CRITICAL: the A-number to AIN mapping is NON-TRIVIAL. A0 maps to AIN7,
 * A1 to AIN5, etc. Never infer AIN from the Arduino A-number. Use the
 * frozen lookup table. Software CANNOT enforce the 3.3V electrical
 * maximum — the SAADC full-scale is 3.6V but CLUE pins are NOT
 * 5V-tolerant and must not exceed VDD.
 *
 * No SDK deps. Compiled from both the firmware wrappers (gpio.cpp,
 * adc.cpp) and host tests (test_expert.cpp).
 */
#ifndef EXPERT_EVAL_H
#define EXPERT_EVAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pin alias table (compile-time frozen) ----------------------------------
 *
 * Maps the 8 CLUE edge-connector GPIOs that are analog-capable.
 * Resource IDs follow pinRegistry: P0.0–P0.31 = 0–31.
 *
 *   D-pin  A-pin  nRF pin  SAADC AIN
 *   D0     A2     P0.04    AIN2
 *   D1     A3     P0.05    AIN3
 *   D2     A4     P0.03    AIN1
 *   D3     A5     P0.28    AIN4
 *   D4     A6     P0.02    AIN0
 *   D10    A7     P0.30    AIN6
 *   D12    A0     P0.31    AIN7
 *   D16    A1     P0.29    AIN5
 */
#define EXPERT_ALIAS_COUNT 8

typedef struct {
	uint8_t d_pin;       /* Arduino D number                            */
	uint8_t a_pin;       /* Arduino A number                            */
	uint8_t resource_id; /* pinRegistry resource (P0.NN = NN)           */
	uint8_t ain;         /* SAADC AIN channel (0-7)                     */
} expert_alias_t;

extern const expert_alias_t EXPERT_ALIASES[EXPERT_ALIAS_COUNT];

/* Lookup by each key. Returns NULL if not found. */
const expert_alias_t *expert_eval_alias_by_d(uint8_t d_pin);
const expert_alias_t *expert_eval_alias_by_a(uint8_t a_pin);
const expert_alias_t *expert_eval_alias_by_resource(uint8_t resource_id);

/* Convenience: resolve D-pin to AIN. Returns false if D-pin is not analog. */
bool expert_eval_d_to_ain(uint8_t d_pin, uint8_t *out_ain);

/* Check if a resource ID is an analog-capable pin. */
bool expert_eval_is_analog_pin(uint8_t resource_id);

/* ---- GPIO config validation ---- */

typedef enum {
	EXPERT_GPIO_DIR_INPUT  = 0,
	EXPERT_GPIO_DIR_OUTPUT = 1,
} expert_gpio_dir_t;

#define EXPERT_GPIO_DIR_COUNT 2

typedef enum {
	EXPERT_GPIO_PULL_NONE     = 0,
	EXPERT_GPIO_PULL_PULLDOWN = 1,
	EXPERT_GPIO_PULL_PULLUP   = 2,
} expert_gpio_pull_t;

#define EXPERT_GPIO_PULL_COUNT 3

/* Drive strength configuration (maps to nRF GPIO PIN_CNF.DRIVE). */
typedef enum {
	EXPERT_GPIO_DRIVE_S0S1 = 0, /* Standard 0, Standard 1   */
	EXPERT_GPIO_DRIVE_H0S1 = 1, /* High-drive 0, Standard 1 */
	EXPERT_GPIO_DRIVE_S0H1 = 2, /* Standard 0, High-drive 1 */
	EXPERT_GPIO_DRIVE_H0H1 = 3, /* High-drive 0, High-drive 1 */
	EXPERT_GPIO_DRIVE_D0S1 = 4, /* Disconnect 0, Standard 1 */
	EXPERT_GPIO_DRIVE_D0H1 = 5, /* Disconnect 0, High-drive 1 */
	EXPERT_GPIO_DRIVE_S0D1 = 6, /* Standard 0, Disconnect 1 */
	EXPERT_GPIO_DRIVE_H0D1 = 7, /* High-drive 0, Disconnect 1 */
} expert_gpio_drive_t;

#define EXPERT_GPIO_DRIVE_COUNT 8

typedef enum {
	EXPERT_GPIO_SENSE_NONE = 0,
	EXPERT_GPIO_SENSE_HIGH = 1,
	EXPERT_GPIO_SENSE_LOW  = 2,
} expert_gpio_sense_t;

#define EXPERT_GPIO_SENSE_COUNT 3

typedef struct {
	expert_gpio_dir_t   dir;
	expert_gpio_pull_t  pull;
	expert_gpio_drive_t drive;
	expert_gpio_sense_t sense;
} expert_gpio_config_t;

/* Validate all enum fields are in range. Returns true if valid. */
bool expert_eval_gpio_config_valid(const expert_gpio_config_t *cfg);

/* ---- SAADC conversion ----
 *
 * nRF52840 SAADC: 12-bit (0-4095), internal 0.6V reference, gain 1/6.
 * Full-scale input = 0.6V / (1/6) = 3.6V.
 *
 * WARNING: CLUE pins are 3.3V-only (powered from VDD). The SAADC can
 * measure up to 3.6V, but any input above VDD (3.3V typical) is
 * electrically out of spec and may damage the pin. Software CANNOT
 * protect against this — the caller must ensure the input stays within
 * 0–VDD range. The SATURATED flag indicates the measurement exceeded
 * the nominal VDD; it is a diagnostic, not protection.
 */

#define EXPERT_ADC_RESOLUTION_BITS 12u
#define EXPERT_ADC_MAX_RAW         4095u
#define EXPERT_ADC_REF_MV          600u    /* 0.6 V internal reference */
#define EXPERT_ADC_FS_MV           3600u   /* REF / (1/6) = 600 * 6   */
#define EXPERT_VDD_NOMINAL_MV      3300u

typedef enum {
	EXPERT_ADC_OK        = 0,
	EXPERT_ADC_SATURATED = 1, /* measured > VDD (3.3 V): clamped, flag set */
} expert_adc_status_t;

/* Convert raw 12-bit reading to millivolts.
 * mV = raw * 3600 / 4095.
 * Saturates and clamps to 3300 mV if result > VDD nominal. */
expert_adc_status_t expert_eval_adc_raw_to_mv(uint16_t raw, uint16_t *out_mv);

/* 4x oversampling: average 4 samples, right-shift by 2.
 * Effective resolution improvement: +1 bit (noise averaging). */
#define EXPERT_ADC_OVERSAMPLE_COUNT 4u
uint16_t expert_eval_adc_oversample_4x(const uint16_t samples[EXPERT_ADC_OVERSAMPLE_COUNT]);

/* Calibration status. Pure eval: always returns true (no-op).
 * Firmware side triggers nrfx_saadc calibration. */
typedef enum {
	EXPERT_ADC_CAL_DONE = 0,
} expert_adc_cal_status_t;
expert_adc_cal_status_t expert_eval_adc_calibrate_result(void);

/* ---- Expert lease table ----
 *
 * Session-scoped, owner-checked opaque tokens wrapping pinRegistry.
 * Adds two invariants that pinRegistry alone does not enforce:
 *   1. Wrong-owner rejection (pinreg allows any caller to release any token).
 *   2. Cross-session rejection (new expert_eval_init invalidates all old tokens).
 *   3. Idempotent release: the immediately-preceding release can be repeated
 *      without error (network retry tolerance).
 */

#define EXPERT_MAX_LEASES    12u
typedef uint32_t expert_token_t;
#define EXPERT_TOKEN_INVALID 0u

typedef enum {
	EXPERT_OK               = 0,
	EXPERT_ERR_INVALID_PARAM  = 1,
	EXPERT_ERR_NOT_ANALOG     = 2,
	EXPERT_ERR_TABLE_FULL     = 3,
	EXPERT_ERR_INVALID_TOKEN  = 4,
	EXPERT_ERR_WRONG_OWNER    = 5,
	EXPERT_ERR_WRONG_SESSION  = 6,
} expert_result_t;

/* Start a new session. Invalidates all outstanding tokens.
 * Session counter wraps at 0xFFFF → 1 (never 0). */
void expert_eval_init(void);

/* Current session ID (for diagnostics). */
uint16_t expert_eval_get_session(void);

/* Issue a lease for a resource + owner pair.
 * For ADC: set require_analog=true to reject non-AIN pins.
 * Returns EXPERT_OK + token on success. */
expert_result_t expert_eval_lease_issue(uint8_t resource_id, uint8_t owner,
                                        bool require_analog,
                                        expert_token_t *out_token);

/* Validate a token: must be active, same session, same owner.
 * Returns EXPERT_OK if the token is usable by this owner in this session. */
expert_result_t expert_eval_lease_validate(expert_token_t token, uint8_t owner);

/* Release a lease.
 * - Owner must match the owner recorded at issue time; mismatched owners
 *   get EXPERT_ERR_WRONG_OWNER (defence against one client releasing
 *   another client's lease).
 * - Valid active token + matching owner → OK, token deactivated.
 * - Immediately repeating the same release (same caller, same owner) →
 *   OK (idempotent, cached for network-retry tolerance).
 * - Any other stale/wrong-session token → error.
 * After all releases, pinreg_release must also be called by the firmware
 * wrapper — this function only manages the expert-level table. */
expert_result_t expert_eval_lease_release(expert_token_t token, uint8_t owner);

/* Release all active leases. Called on shutdown / runtime switch. */
void expert_eval_lease_release_all(void);

/* Query a lease. Returns false if token is not active in current session. */
bool expert_eval_lease_query(expert_token_t token,
                             uint8_t *out_resource, uint8_t *out_owner);

#ifdef __cplusplus
}
#endif
#endif /* EXPERT_EVAL_H */
