/*
 * boardMotionEval.c - Pure-logic motion Board domain evaluation.
 *
 * No SDK deps. Included from boardModule.cpp and test_motion_board.cpp.
 */
#include "boardMotionEval.h"

/* Pull in BoardResultCode enum values (via board.pb.h → boardModuleC.h chain
 * is NOT available here; define the ones we return as locals). */
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local copies of BoardResultCode values we use (avoids pulling board.pb.h
 * into the host test path; the firmware side validates these match via
 * static_assert in boardModule.cpp). */
#define MOTION_EVAL_SUCCESS          0u
#define MOTION_EVAL_INVALID_ARG      1u
#define MOTION_EVAL_BUSY             3u
#define MOTION_EVAL_NOT_FOUND        4u
#define MOTION_EVAL_SENSOR_FAULT     7u

/* ---- Static sensor component names ----------------------------------- */

static const char * const s_accel_components[] = {"X", "Y", "Z"};
static const char * const s_gyro_components[]  = {"X", "Y", "Z"};
static const char * const s_mag_components[]   = {"X", "Y", "Z"};
static const char * const s_quat_components[]  = {"W", "X", "Y", "Z"};
static const char * const s_orient_components[] = {"Yaw", "Pitch", "Roll"};
static const char * const s_airmouse_components[] = {"dX", "dY", "Wheel", "Buttons"};

/* Environment sensor component names (Todo 24) */
static const char * const s_pressure_components[] = {"Pa"};
static const char * const s_bmp_temp_components[] = {"milli_celsius"};
static const char * const s_humidity_components[] = {"milli_percent_rh"};
static const char * const s_sht_temp_components[] = {"milli_celsius"};

/* APDS-9960 sensor component names (Todo 25) */
static const char * const s_color_components[] = {"R", "G", "B", "C"};
static const char * const s_proximity_components[] = {"counts"};
static const char * const s_gesture_components[] = {"gesture"};

/* Audio sensor component names (Todo 26) */
static const char * const s_audio_components[] = {"q15_dbfs_x1000"};

/* ---- Static sensor descriptor table ---------------------------------- */

static const board_motion_sensor_info_t s_sensor_table[] = {
	{
		BOARD_MOTION_SENSOR_ACCEL,
		"acceleration",
		"mg",
		3,
		s_accel_components,
		104000,
		BOARD_MOTION_RATE_IMU_MAX_MHZ,
		{10000, 25000, 50000, 104000},
		4,
	},
	{
		BOARD_MOTION_SENSOR_GYRO,
		"gyroscope",
		"mdps",
		3,
		s_gyro_components,
		104000,
		BOARD_MOTION_RATE_IMU_MAX_MHZ,
		{10000, 25000, 50000, 104000},
		4,
	},
	{
		BOARD_MOTION_SENSOR_MAG,
		"magnetic",
		"milligauss",
		3,
		s_mag_components,
		40000,
		BOARD_MOTION_RATE_MAG_MAX_MHZ,
		{10000, 20000, 40000, 0},
		3,
	},
	{
		BOARD_MOTION_SENSOR_QUATERNION,
		"quaternion",
		"q30",
		4,
		s_quat_components,
		104000,
		BOARD_MOTION_RATE_IMU_MAX_MHZ,
		{10000, 25000, 50000, 104000},
		4,
	},
	{
		BOARD_MOTION_SENSOR_ORIENTATION,
		"orientation",
		"millidegrees",
		3,
		s_orient_components,
		104000,
		BOARD_MOTION_RATE_IMU_MAX_MHZ,
		{10000, 25000, 50000, 104000},
		4,
	},
	{
		BOARD_MOTION_SENSOR_AIR_MOUSE,
		"air_mouse",
		"counts",
		4,
		s_airmouse_components,
		60000,
		BOARD_MOTION_RATE_AIR_MOUSE_MAX_MHZ,
		{10000, 30000, 60000, 0},
		3,
	},
	/* === ENVIRONMENT HANDLERS (Todo 25) — sensor table entries === */
	{
		BOARD_MOTION_SENSOR_PRESSURE,
		"pressure",
		"pa",
		1,
		s_pressure_components,
		13000,
		BOARD_MOTION_RATE_BMP_MAX_MHZ,
		{1000, 5000, 13000, 0},
		3,
	},
	{
		BOARD_MOTION_SENSOR_BMP_TEMP,
		"bmp_temperature",
		"milli_celsius",
		1,
		s_bmp_temp_components,
		13000,
		BOARD_MOTION_RATE_BMP_MAX_MHZ,
		{1000, 5000, 13000, 0},
		3,
	},
	{
		BOARD_MOTION_SENSOR_HUMIDITY,
		"humidity",
		"milli_percent_rh",
		1,
		s_humidity_components,
		1000,
		BOARD_MOTION_RATE_SHT_MAX_MHZ,
		{1000, 0, 0, 0},
		1,
	},
	{
		BOARD_MOTION_SENSOR_SHT_TEMP,
		"sht_temperature",
		"milli_celsius",
		1,
		s_sht_temp_components,
		1000,
		BOARD_MOTION_RATE_SHT_MAX_MHZ,
		{1000, 0, 0, 0},
		1,
	},
	{
		BOARD_MOTION_SENSOR_COLOR,
		"color",
		"counts",
		4,
		s_color_components,
		5000,
		BOARD_MOTION_RATE_APDS_MAX_MHZ,
		{1000, 5000, 0, 0},
		2,
	},
	{
		BOARD_MOTION_SENSOR_PROXIMITY,
		"proximity",
		"0_255",
		1,
		s_proximity_components,
		5000,
		BOARD_MOTION_RATE_APDS_MAX_MHZ,
		{1000, 5000, 0, 0},
		2,
	},
	{
		BOARD_MOTION_SENSOR_GESTURE,
		"gesture",
		"enum",
		1,
		s_gesture_components,
		5000,
		BOARD_MOTION_RATE_APDS_MAX_MHZ,
		{1000, 5000, 0, 0},
		2,
	},
	/* === AUDIO HANDLERS (Todo 26) — sensor table entry === */
	{
		BOARD_MOTION_SENSOR_AUDIO,
		"audio_level",
		"q15_dbfs_x1000",
		1,
		s_audio_components,
		BOARD_MOTION_RATE_AUDIO_MAX_MHZ,
		BOARD_MOTION_RATE_AUDIO_MAX_MHZ,
		{5000, 10000, 20000, 0},
		3,
	},
};

#define TABLE_LEN (sizeof(s_sensor_table) / sizeof(s_sensor_table[0]))

/* ---- Table access ---------------------------------------------------- */

const board_motion_sensor_info_t *board_motion_get_sensor_table(
	uint32_t *out_count)
{
	if (out_count != NULL) {
		*out_count = (uint32_t)TABLE_LEN;
	}
	return s_sensor_table;
}

const board_motion_sensor_info_t *board_motion_lookup_sensor(
	uint32_t sensor_id)
{
	for (uint32_t i = 0; i < TABLE_LEN; i++) {
		if (s_sensor_table[i].sensor_id == sensor_id) {
			return &s_sensor_table[i];
		}
	}
	return NULL;
}

const board_motion_sensor_info_t *board_motion_get_by_cursor(
	uint32_t cursor, uint32_t *out_next_cursor, bool *out_eof)
{
	if (cursor >= TABLE_LEN) {
		if (out_next_cursor != NULL) {
			*out_next_cursor = cursor;
		}
		if (out_eof != NULL) {
			*out_eof = true;
		}
		return NULL;
	}

	if (out_eof != NULL) {
		*out_eof = (cursor + 1u >= TABLE_LEN);
	}
	if (out_next_cursor != NULL) {
		*out_next_cursor = cursor + 1u;
	}
	return &s_sensor_table[cursor];
}

uint32_t board_motion_clamp_rate(uint32_t sensor_id,
                                 uint32_t requested_millihz)
{
	if (requested_millihz == 0u) {
		return 0u;
	}

	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(sensor_id);
	if (info == NULL) {
		return 0u;
	}

	if (requested_millihz > info->max_rate_millihz) {
		return info->max_rate_millihz;
	}
	if (requested_millihz < BOARD_MOTION_RATE_MIN_MHZ) {
		return BOARD_MOTION_RATE_MIN_MHZ;
	}
	return requested_millihz;
}

bool board_motion_is_valid_sensor(uint32_t sensor_id)
{
	return board_motion_lookup_sensor(sensor_id) != NULL;
}

/* ---- Stream state ---------------------------------------------------- */

void board_motion_stream_init(board_motion_stream_state_t *st)
{
	if (st == NULL) {
		return;
	}
	memset(st, 0, sizeof(*st));
}

uint32_t board_motion_stream_configure(
	board_motion_stream_state_t *st,
	uint32_t sensor_id, uint32_t requested_millihz,
	uint32_t *out_code)
{
	if (st == NULL || out_code == NULL) {
		return 0u;
	}

	*out_code = MOTION_EVAL_SUCCESS;

	if (!board_motion_is_valid_sensor(sensor_id)) {
		*out_code = MOTION_EVAL_INVALID_ARG;
		return 0u;
	}

	uint32_t actual = board_motion_clamp_rate(sensor_id, requested_millihz);
	if (actual == 0u) {
		*out_code = MOTION_EVAL_INVALID_ARG;
		return 0u;
	}

	/* Find existing slot or a free one. */
	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		if (st->slots[i].active && st->slots[i].sensor_id == sensor_id) {
			st->slots[i].rate_millihz = actual;
			return actual;
		}
	}

	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		if (!st->slots[i].active) {
			st->slots[i].sensor_id = sensor_id;
			st->slots[i].rate_millihz = actual;
			st->slots[i].active = true;
			return actual;
		}
	}

	*out_code = MOTION_EVAL_BUSY;
	return 0u;
}

uint32_t board_motion_stream_stop(
	board_motion_stream_state_t *st,
	uint32_t sensor_id)
{
	if (st == NULL) {
		return MOTION_EVAL_SUCCESS;
	}

	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		if (st->slots[i].active && st->slots[i].sensor_id == sensor_id) {
			st->slots[i].active = false;
			st->slots[i].rate_millihz = 0u;
			st->slots[i].sensor_id = 0u;
			break;
		}
	}
	return MOTION_EVAL_SUCCESS;
}

bool board_motion_stream_is_active(
	const board_motion_stream_state_t *st,
	uint32_t sensor_id)
{
	if (st == NULL) {
		return false;
	}

	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		if (st->slots[i].active && st->slots[i].sensor_id == sensor_id) {
			return true;
		}
	}
	return false;
}

uint32_t board_motion_stream_get_rate(
	const board_motion_stream_state_t *st,
	uint32_t sensor_id)
{
	if (st == NULL) {
		return 0u;
	}

	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		if (st->slots[i].active && st->slots[i].sensor_id == sensor_id) {
			return st->slots[i].rate_millihz;
		}
	}
	return 0u;
}

/* ---- Calibration ownership ------------------------------------------- */

void board_motion_calib_init(board_motion_calib_state_t *st)
{
	if (st == NULL) {
		return;
	}
	memset(st, 0, sizeof(*st));
}

uint32_t board_motion_calib_start(
	board_motion_calib_state_t *st,
	uint32_t sensor_id)
{
	if (st == NULL) {
		return MOTION_EVAL_INVALID_ARG;
	}

	/* IMU calibration targets sensor_id 1 (accel) or 2 (gyro).
	 * Mag calibration targets sensor_id 3. Both IMU IDs are treated
	 * as the same calibration target (one IMU calibration calibrates
	 * both accel and gyro). */
	bool is_imu = (sensor_id == BOARD_MOTION_SENSOR_ACCEL ||
	               sensor_id == BOARD_MOTION_SENSOR_GYRO);
	bool is_mag = (sensor_id == BOARD_MOTION_SENSOR_MAG);

	if (!is_imu && !is_mag) {
		return MOTION_EVAL_INVALID_ARG;
	}

	if (st->in_progress) {
		/* If same target already calibrating, still BUSY. */
		return MOTION_EVAL_BUSY;
	}

	if (is_imu) {
		st->active_sensor_id = BOARD_MOTION_SENSOR_ACCEL;
	} else {
		st->active_sensor_id = BOARD_MOTION_SENSOR_MAG;
	}
	st->in_progress = true;
	return MOTION_EVAL_SUCCESS;
}

void board_motion_calib_complete(board_motion_calib_state_t *st)
{
	if (st == NULL) {
		return;
	}
	st->active_sensor_id = 0u;
	st->in_progress = false;
}

bool board_motion_calib_is_busy(const board_motion_calib_state_t *st)
{
	if (st == NULL) {
		return false;
	}
	return st->in_progress;
}

#ifdef __cplusplus
}
#endif
