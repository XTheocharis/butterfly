/*
 * boardMotionEval.h - Pure-logic motion Board domain evaluation.
 *
 * No SDK deps. Included from boardModule.cpp (firmware) and
 * test_motion_board.cpp (host test).
 *
 * Provides: static sensor descriptor table, cursor pagination,
 * rate clamping, bounded stream state, single-owner calibration.
 */
#ifndef BOARD_MOTION_EVAL_H
#define BOARD_MOTION_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sensor IDs (board_manifest.json) -------------------------------- */

#define BOARD_MOTION_SENSOR_ACCEL         1u
#define BOARD_MOTION_SENSOR_GYRO          2u
#define BOARD_MOTION_SENSOR_MAG           3u
#define BOARD_MOTION_SENSOR_QUATERNION    4u
#define BOARD_MOTION_SENSOR_ORIENTATION   5u
#define BOARD_MOTION_SENSOR_AIR_MOUSE    14u

/* Environment sensors (Todo 24: BMP280 + SHT31-D) */
#define BOARD_MOTION_SENSOR_PRESSURE      6u
#define BOARD_MOTION_SENSOR_BMP_TEMP      7u
#define BOARD_MOTION_SENSOR_HUMIDITY      8u
#define BOARD_MOTION_SENSOR_SHT_TEMP      9u

/* Optical/gesture sensors (Todo 25: APDS-9960) */
#define BOARD_MOTION_SENSOR_COLOR        10u
#define BOARD_MOTION_SENSOR_PROXIMITY    11u
#define BOARD_MOTION_SENSOR_GESTURE      12u

/* Audio sensor (Todo 26: PDM microphone metrics) */
#define BOARD_MOTION_SENSOR_AUDIO        13u
#define BOARD_MOTION_RATE_AUDIO_MAX_MHZ  20000u  /* 20 Hz */

#define BOARD_MOTION_SENSOR_COUNT        14u

/* ---- Max stream rates in millihz ------------------------------------- */

#define BOARD_MOTION_RATE_IMU_MAX_MHZ       104000u  /* 104 Hz */
#define BOARD_MOTION_RATE_MAG_MAX_MHZ        40000u  /* 40 Hz  */
#define BOARD_MOTION_RATE_AIR_MOUSE_MAX_MHZ  60000u  /* 60 Hz  */
#define BOARD_MOTION_RATE_BMP_MAX_MHZ        13000u  /* 13 Hz  */
#define BOARD_MOTION_RATE_SHT_MAX_MHZ         1000u  /* 1 Hz   */
#define BOARD_MOTION_RATE_APDS_MAX_MHZ        5000u  /* ~5 Hz  */
#define BOARD_MOTION_RATE_MIN_MHZ                1u  /* 1 mHz floor */

/* ---- Stream / calibration bounds ------------------------------------- */

#define BOARD_MOTION_MAX_STREAMS    4u
#define BOARD_MOTION_MAX_RATES      4u

/* ---- Sensor descriptor (eval-level, protobuf-agnostic) --------------- */

typedef struct {
	uint32_t sensor_id;
	const char *name;
	const char *unit;
	uint32_t value_count;
	const char * const *component_names;
	uint32_t default_rate_millihz;
	uint32_t max_rate_millihz;
	uint32_t supported_rates[BOARD_MOTION_MAX_RATES];
	uint32_t supported_rate_count;
} board_motion_sensor_info_t;

/* Returns pointer to the static sensor table. *out_count = entry count. */
const board_motion_sensor_info_t *board_motion_get_sensor_table(
	uint32_t *out_count);

/* Look up sensor by ID. Returns NULL if not a motion sensor. */
const board_motion_sensor_info_t *board_motion_lookup_sensor(
	uint32_t sensor_id);

/* Cursor pagination for ListSensors.
 * Returns descriptor at cursor index, or NULL if out of range.
 * Sets *out_next_cursor and *out_eof. */
const board_motion_sensor_info_t *board_motion_get_by_cursor(
	uint32_t cursor, uint32_t *out_next_cursor, bool *out_eof);

/* Clamp requested rate to sensor max. Returns 0 for invalid sensor.
 * A requested rate of 0 means "stop" and returns 0. */
uint32_t board_motion_clamp_rate(uint32_t sensor_id,
                                 uint32_t requested_millihz);

/* Check if sensor_id is a valid motion sensor. */
bool board_motion_is_valid_sensor(uint32_t sensor_id);

/* ---- Stream state (bounded, host-testable) -------------------------- */

typedef struct {
	uint32_t sensor_id;
	uint32_t rate_millihz;
	bool active;
} board_motion_stream_slot_t;

typedef struct {
	board_motion_stream_slot_t slots[BOARD_MOTION_MAX_STREAMS];
} board_motion_stream_state_t;

void board_motion_stream_init(board_motion_stream_state_t *st);

/* Configure or reconfigure a stream.
 * Returns actual clamped rate (>0 on success), 0 on error.
 * Sets *out_code to BoardResultCode (SUCCESS / INVALID_ARGUMENT / SENSOR_FAULT). */
uint32_t board_motion_stream_configure(
	board_motion_stream_state_t *st,
	uint32_t sensor_id, uint32_t requested_millihz,
	uint32_t *out_code);

/* Stop a stream. Idempotent. Returns SUCCESS. */
uint32_t board_motion_stream_stop(
	board_motion_stream_state_t *st,
	uint32_t sensor_id);

/* Check if a sensor is currently streaming. */
bool board_motion_stream_is_active(
	const board_motion_stream_state_t *st,
	uint32_t sensor_id);

/* Get the configured rate for a streaming sensor. Returns 0 if inactive. */
uint32_t board_motion_stream_get_rate(
	const board_motion_stream_state_t *st,
	uint32_t sensor_id);

/* ---- Calibration ownership (single-owner) --------------------------- */

typedef struct {
	uint32_t active_sensor_id;  /* 0 = idle */
	bool in_progress;
} board_motion_calib_state_t;

void board_motion_calib_init(board_motion_calib_state_t *st);

/* Try to start calibration for sensor_id.
 * Returns SUCCESS if started, BUSY if another calibration is in progress. */
uint32_t board_motion_calib_start(
	board_motion_calib_state_t *st,
	uint32_t sensor_id);

/* Complete calibration (success or fail). Clears the owner. */
void board_motion_calib_complete(board_motion_calib_state_t *st);

/* Check if any calibration is in progress. */
bool board_motion_calib_is_busy(const board_motion_calib_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_MOTION_EVAL_H */
