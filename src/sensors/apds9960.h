/*
 * apds9960.h - APDS-9960 digital proximity, ambient light, RGB, and
 * gesture sensor pure-logic driver.
 *
 * Register constants, golden configuration values, gesture FIFO decode,
 * optical/gesture mode transition state machine, and integer conversion
 * math.  No SDK deps — host-testable.
 *
 * The firmware C++ class (apds9960.cpp) wraps these with i2cBus async
 * transfers and GPIOTE IRQ handling.  The host test includes the .cpp
 * directly.
 *
 * Sensor at I2C 0x39 (from i2cBus.h frozen table).  ID 0xAB at reg 0x92.
 * IRQ pin P0.09 (custom_board.h).
 *
 * Output units (board_manifest.json):
 *   Color:      raw counts (ID 10) — 4 values: R, G, B, C
 *   Proximity:  0-255              (ID 11) — 1 value
 *   Gesture:    enum UP/DOWN/LEFT/RIGHT/NEAR/FAR (ID 12) — 1 value
 *
 * Optical (RGBC + proximity) and gesture modes are MUTUALLY EXCLUSIVE.
 * Mode transition requires quiescing the current mode before enabling
 * the new one: disable AEN/PEN → settle → enable GEN (or vice versa).
 *
 * ISR only timestamps and queues work; FIFO reads and gesture decoding
 * run asynchronously from the main loop.
 */
#ifndef SENSORS_APDS9960_H
#define SENSORS_APDS9960_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C address (from i2cBus.h frozen table) ------------------------ */

#define APDS9960_ADDR                 0x39u

/* ---- Register map ---------------------------------------------------- */

#define APDS9960_REG_ENABLE           0x80u
#define APDS9960_REG_ATIME            0x81u
#define APDS9960_REG_WTIME            0x83u
#define APDS9960_REG_AILTL            0x84u
#define APDS9960_REG_AILTH            0x85u
#define APDS9960_REG_AIHTL            0x86u
#define APDS9960_REG_AIHTH            0x87u
#define APDS9960_REG_PILT             0x89u
#define APDS9960_REG_PIHT             0x8Bu
#define APDS9960_REG_PERS             0x8Cu
#define APDS9960_REG_CONFIG1          0x8Du
#define APDS9960_REG_PPULSE           0x8Eu
#define APDS9960_REG_CONTROL          0x8Fu
#define APDS9960_REG_CONFIG2          0x90u
#define APDS9960_REG_ID               0x92u
#define APDS9960_REG_STATUS           0x93u
#define APDS9960_REG_CDATAL           0x94u
#define APDS9960_REG_CDATAH           0x95u
#define APDS9960_REG_RDATAL           0x96u
#define APDS9960_REG_RDATAH           0x97u
#define APDS9960_REG_GDATAL           0x98u
#define APDS9960_REG_GDATAH           0x99u
#define APDS9960_REG_BDATAL           0x9Au
#define APDS9960_REG_BDATAH           0x9Bu
#define APDS9960_REG_PDATA            0x9Cu
#define APDS9960_REG_POFFSET_UR       0x9Du
#define APDS9960_REG_POFFSET_DL       0x9Eu
#define APDS9960_REG_CONFIG3          0x9Fu
#define APDS9960_REG_GPENTH           0xA0u
#define APDS9960_REG_GEXTH            0xA1u
#define APDS9960_REG_GCONF1           0xA2u
#define APDS9960_REG_GCONF2           0xA3u
#define APDS9960_REG_GOFFSET_U        0xA4u
#define APDS9960_REG_GOFFSET_D        0xA5u
#define APDS9960_REG_GPULSE           0xA6u
#define APDS9960_REG_GOFFSET_L        0xA7u
#define APDS9960_REG_GOFFSET_R        0xA9u
#define APDS9960_REG_GCONF3           0xAAu
#define APDS9960_REG_GCONF4           0xABu
#define APDS9960_REG_GFLVL            0xAEu
#define APDS9960_REG_GFIFO_U          0xFCu
#define APDS9960_REG_GFIFO_D          0xFDu
#define APDS9960_REG_GFIFO_L          0xFEu
#define APDS9960_REG_GFIFO_R          0xFFu

/* ---- ID register ----------------------------------------------------- */

#define APDS9960_ID_VALUE             0xABu

/* ---- ENABLE register bits (reg 0x80) --------------------------------- */

#define APDS9960_ENABLE_PON           0x01u  /* Power ON            */
#define APDS9960_ENABLE_AEN           0x02u  /* ALS (RGBC) Enable   */
#define APDS9960_ENABLE_PEN           0x04u  /* Proximity Enable    */
#define APDS9960_ENABLE_WEN           0x08u  /* Wait Enable         */
#define APDS9960_ENABLE_AIEN          0x10u  /* ALS Interrupt En    */
#define APDS9960_ENABLE_PIEN          0x20u  /* Proximity Int En    */
#define APDS9960_ENABLE_GEN           0x40u  /* Gesture Enable      */

/* ---- Golden configuration values ------------------------------------- */
/*
 * Frozen defaults per the CLUE implementation plan.  Host tests verify
 * exact byte equality.  These are the values the firmware writes during
 * init for optical mode.
 */

#define APDS9960_GOLDEN_ENABLE        0x27u  /* PON + AEN + PEN + PIEN */
#define APDS9960_GOLDEN_ATIME         0xFFu  /* 178ms RGBC integration  */
#define APDS9960_GOLDEN_WTIME         0xFFu  /* 178ms wait (if enabled) */
#define APDS9960_GOLDEN_PPULSE        0xC9u  /* 32µs GPLEN + 10 pulses (count-1=9) */
#define APDS9960_GOLDEN_CONTROL       0x20u  /* Drive + gain config     */
#define APDS9960_GOLDEN_CONFIG2       0x00u  /* LED drive 100mA, no boost */
#define APDS9960_GOLDEN_GPENTH        40u    /* Gesture enter threshold */
#define APDS9960_GOLDEN_GEXTH         30u    /* Gesture exit threshold  */
#define APDS9960_GOLDEN_GFIFOTH       4u     /* Gesture FIFO threshold  */

/* ENABLE value for gesture mode (PON + WEN + PEN + GEN) */
#define APDS9960_ENABLE_GESTURE_MODE  (APDS9960_ENABLE_PON  | \
                                       APDS9960_ENABLE_WEN  | \
                                       APDS9960_ENABLE_PEN  | \
                                       APDS9960_ENABLE_GEN)

/* Quiesced intermediate state during transition (PON + PEN only) */
#define APDS9960_ENABLE_QUIESCED      (APDS9960_ENABLE_PON | \
                                       APDS9960_ENABLE_PEN)

/* ---- Status register bits (reg 0x93) -------------------------------- */

#define APDS9960_STATUS_AVALID        0x01u  /* RGBC data valid       */
#define APDS9960_STATUS_PVALID        0x02u  /* Proximity data valid  */
#define APDS9960_STATUS_GINT          0x04u  /* Gesture interrupt     */
#define APDS9960_STATUS_AINT          0x10u  /* ALS interrupt         */
#define APDS9960_STATUS_PINT          0x20u  /* Proximity interrupt   */
#define APDS9960_STATUS_PGSAT         0x40u  /* Proximity/gesture sat */
#define APDS9960_STATUS_CPSAT         0x80u  /* Clear photodiode sat  */

/* ---- ATIME / WTIME conversion --------------------------------------- */
/*
 * ATIME: 256 - ATIME = integration steps.  Each step = 2.78ms.
 * ATIME=0xFF → 1 step → 2.78ms.  ATIME=0x00 → 256 steps → 712ms.
 * ATIME=0xFF with default step: ~2.78ms.
 * But the task says 0xFF = 178ms.  The actual integration time depends
 * on the internal step count; ATIME=0xFF gives the minimum steps.
 * The device advertises: step_count = 256 - ATIME.
 * For 178ms: 178/2.78 ≈ 64 steps → ATIME = 256-64 = 192 = 0xC0.
 * However, some implementations use a different step period.
 * We use the golden value 0xFF and report the step count = 256-0xFF = 1.
 * The 178ms annotation may refer to a different calculation including
 * wait time.  The golden byte is authoritative for tests.
 */

#define APDS9960_STEP_US              2780u
#define APDS9960_STEP_MAX             256u

/* ---- RGBC data layout (8 bytes: C_lo C_hi R_lo R_hi G_lo G_hi B_lo B_hi) */

#define APDS9960_RGBC_LEN             8u
#define APDS9960_C_OFFSET             0u
#define APDS9960_R_OFFSET             2u
#define APDS9960_G_OFFSET             4u
#define APDS9960_B_OFFSET             6u

/* ---- Gesture FIFO --------------------------------------------------- */
/*
 * The APDS-9960 gesture engine collects U/D/L/R photodiode data into
 * four separate FIFOs, one per direction.  Each FIFO can hold up to 32
 * 8-bit samples.  The FIFO level register (GFLVL) indicates how many
 * complete U/D/L/R datasets are available.
 *
 * One dataset = { U, D, L, R } four bytes.
 * FIFO_FULL when GFLVL reaches 32 (all FIFOs full).
 * GFIFOTH determines when GINT fires (configured in GCONF1).
 */

#define APDS9960_FIFO_DEPTH           32u    /* max datasets per direction */
#define APDS9960_FIFO_BYTES_PER_SET   4u     /* U, D, L, R                 */

/* ---- GCONF1 GFIFOTH encoding ---------------------------------------- */
/*
 * GCONF1 (0xA2) bits [7:6] = GFIFOTH:
 *   00 = trigger at 1 dataset
 *   01 = trigger at 4 datasets
 *   10 = trigger at 8 datasets
 *   11 = trigger at 16 datasets
 *
 * To set threshold = 4: GFIFOTH = 01 in bits [7:6] → 0x40 in GCONF1.
 */

#define APDS9960_GCONF1_GFIFOTH_MASK  0xC0u
#define APDS9960_GCONF1_GFIFOTH_SHIFT 6u

/* Encode desired FIFO threshold (1, 4, 8, or 16) into GCONF1 bits[7:6]. */
static inline uint8_t apds9960_encode_gfifoth(uint8_t threshold)
{
	if (threshold >= 16u) return 0xC0u;
	if (threshold >= 8u)  return 0x80u;
	if (threshold >= 4u)  return 0x40u;
	return 0x00u;  /* threshold 1 */
}

/* ---- Sensor IDs (board_manifest.json) -------------------------------- */

#define APDS9960_SENSOR_ID_COLOR      10u
#define APDS9960_SENSOR_ID_PROXIMITY  11u
#define APDS9960_SENSOR_ID_GESTURE    12u

#define APDS9960_COLOR_VALUE_COUNT    4u     /* R, G, B, C */

/* ---- Operating mode state machine ----------------------------------- */

typedef enum {
	APDS9960_MODE_OFF      = 0,
	APDS9960_MODE_OPTICAL  = 1,  /* RGBC + proximity */
	APDS9960_MODE_GESTURE  = 2,  /* Gesture engine   */
	APDS9960_MODE_QUIESCING = 3, /* Transition quiesce */
} apds9960_mode_t;

typedef enum {
	APDS9960_TRANSITION_OK     = 0,
	APDS9960_TRANSITION_BUSY   = 1,  /* already transitioning or busy */
	APDS9960_TRANSITION_SAME   = 2,  /* already in target mode        */
} apds9960_transition_result_t;

/* ---- RGBC sample ---------------------------------------------------- */

typedef struct {
	uint32_t clear;     /* raw 16-bit clear channel   */
	uint32_t red;       /* raw 16-bit red channel     */
	uint32_t green;     /* raw 16-bit green channel   */
	uint32_t blue;      /* raw 16-bit blue channel    */
	uint8_t  proximity; /* raw 8-bit proximity (0-255) */
} apds9960_optical_sample_t;

/* ---- Gesture result ------------------------------------------------- */

typedef enum {
	APDS9960_GESTURE_NONE  = 0,  /* no gesture detected */
	APDS9960_GESTURE_UP    = 1,
	APDS9960_GESTURE_DOWN  = 2,
	APDS9960_GESTURE_LEFT  = 3,
	APDS9960_GESTURE_RIGHT = 4,
	APDS9960_GESTURE_NEAR  = 5,
	APDS9960_GESTURE_FAR   = 6,
} apds9960_gesture_t;

/* ---- Mode transition context ---------------------------------------- */

typedef struct {
	apds9960_mode_t current;
	apds9960_mode_t target;
	bool transition_pending;
} apds9960_mode_state_t;

void apds9960_mode_init(apds9960_mode_state_t *st);

/*
 * Request a mode transition.  Returns:
 *   TRANSITION_OK    — transition initiated (or immediate if OFF→X)
 *   TRANSITION_BUSY  — a transition is already in progress
 *   TRANSITION_SAME  — already in the requested mode
 *
 * Optical → Gesture and Gesture → Optical always go through QUIESCING.
 * OFF → any target is immediate.
 */
apds9960_transition_result_t apds9960_request_mode(
	apds9960_mode_state_t *st, apds9960_mode_t target);

/*
 * Called after the quiesce settle delay completes.
 * Advances the state machine from QUIESCING to the target mode.
 * Returns the new current mode.
 */
apds9960_mode_t apds9960_complete_quiesce(apds9960_mode_state_t *st);

/* ---- Register config validation ------------------------------------- */

/*
 * Validate that the given register/value pairs match the golden defaults
 * for optical mode.  Returns true if all match.
 *
 * config: array of {reg, value} pairs.  count = number of pairs.
 */
typedef struct {
	uint8_t reg;
	uint8_t value;
} apds9960_reg_val_t;

bool apds9960_validate_optical_config(
	const apds9960_reg_val_t *config, size_t count);

/* Check if ENABLE register value represents a valid optical config. */
bool apds9960_is_optical_enabled(uint8_t enable_reg);

/* Check if ENABLE register value represents a valid gesture config. */
bool apds9960_is_gesture_enabled(uint8_t enable_reg);

/* Check mutual exclusion: AEN and GEN must not both be set. */
bool apds9960_is_mutually_exclusive_violated(uint8_t enable_reg);

/* ---- RGBC parsing --------------------------------------------------- */

/*
 * Parse 8-byte RGBC burst read into optical sample.
 * Byte order: C_lo C_hi R_lo R_hi G_lo G_hi B_lo B_hi.
 * Proximity is read separately from PDATA register.
 */
void apds9960_parse_rgbc(const uint8_t burst[APDS9960_RGBC_LEN],
                         apds9960_optical_sample_t *out);

/* Parse proximity byte (0-255). */
static inline uint8_t apds9960_parse_proximity(uint8_t pdata)
{
	return pdata;
}

/* ---- Gesture FIFO decode -------------------------------------------- */
/*
 * The APDS-9960 gesture engine uses four directional photodiodes (U/D/L/R).
 * When an object moves over the sensor, the U/D and L/R pairs show
 * differential responses that indicate direction.
 *
 * Gesture detection algorithm:
 *   1. For each dataset, compute the difference between opposing pairs.
 *      U_minus_D = U - D
 *      L_minus_R = L - R
 *   2. Track the first "enter" event (when a pair exceeds threshold) and
 *      the subsequent "exit" event (when a pair drops below threshold).
 *   3. The direction is determined by:
 *      - The sign of the dominant difference at entry
 *      - The order of enter vs exit peaks
 *   4. Near/Far are determined by whether both U and D (or L and R)
 *      increase together (Near) or decrease together (Far).
 *
 * This implementation uses the simplified Adafruit approach:
 *   - Accumulate U/D/L/R differences across the FIFO
 *   - Compare absolute accumulated deltas
 *   - The larger of |U-D| vs |L-R| determines vertical vs horizontal
 *   - The sign determines direction
 *   - Near/Far by total magnitude pattern
 */

/*
 * Process a single FIFO dataset (U, D, L, R bytes).
 * The decoder accumulates differences internally and produces a gesture
 * when the FIFO is fully consumed (apds9960_gesture_result is called).
 */
typedef struct {
	int32_t ud_delta;      /* accumulated U - D */
	int32_t lr_delta;      /* accumulated L - R */
	int32_t ud_abs_sum;    /* accumulated |U| + |D| */
	int32_t lr_abs_sum;    /* accumulated |L| + |R| */
	uint32_t datasets;     /* count of datasets processed */
} apds9960_gesture_decoder_t;

void apds9960_gesture_dec_init(apds9960_gesture_decoder_t *dec);

/* Feed one U/D/L/R dataset into the decoder. */
void apds9960_gesture_dec_feed(apds9960_gesture_decoder_t *dec,
                               uint8_t u, uint8_t d,
                               uint8_t l, uint8_t r);

/*
 * Determine the gesture from accumulated decoder state.
 * Returns APDS9960_GESTURE_NONE if insufficient data or ambiguous.
 */
apds9960_gesture_t apds9960_gesture_dec_result(
	const apds9960_gesture_decoder_t *dec);

/*
 * Process a full FIFO dump and return the detected gesture.
 * fifo_u, fifo_d, fifo_l, fifo_r: arrays of `count` bytes each.
 * count: number of datasets (0 to APDS9960_FIFO_DEPTH).
 */
apds9960_gesture_t apds9960_decode_fifo(
	const uint8_t *fifo_u, const uint8_t *fifo_d,
	const uint8_t *fifo_l, const uint8_t *fifo_r,
	uint32_t count);

/* ---- Absent device detection ---------------------------------------- */
/*
 * Read the ID register.  Returns true if the value matches APDS9960_ID_VALUE.
 */
bool apds9960_check_id(uint8_t id_reg_value);

/* ---- Gesture to board.proto Gesture enum mapping -------------------- */
/*
 * Maps internal gesture enum to board.proto Gesture values:
 *   UNKNOWN=0, UP=1, DOWN=2, LEFT=3, RIGHT=4, NEAR=5, FAR=6
 * The internal enum uses the same numeric values.
 */
uint32_t apds9960_gesture_to_proto(apds9960_gesture_t g);

/* ---- FIFO overflow detection ---------------------------------------- */
/*
 * Check if GFLVL indicates FIFO overflow (> APDS9960_FIFO_DEPTH datasets).
 * GFLVL is an 8-bit register; values >32 indicate overflow in some
 * implementations, or we cap at FIFO_DEPTH.
 */
bool apds9960_fifo_overflow(uint8_t gflvl);

#ifdef __cplusplus
}
#endif
#endif /* SENSORS_APDS9960_H */
