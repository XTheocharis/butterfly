/*
 * calib_eval.h - Pure-C calibration/profile/runtime record validation.
 *
 * Defines a sub-protocol within QSPI journal payloads (Todo 29) for
 * persisting sensor calibration (IMU/MAG), remote profiles, and the
 * runtime-mode store.  All logic is host-testable with no SDK deps.
 *
 * The journal record payload carries one calib sub-record.  The journal
 * header/payload CRC32 (ISO-HDLC) protects integrity end-to-end.  This
 * layer adds semantic validation: record kind, sensor/profile identity,
 * and data sanity before application.
 *
 * Precedence for runtime mode (highest first):
 *   1. GPREGRET2 one-shot (runtime.h)
 *   2. Adopted-QSPI runtime record (this layer, via store_read)
 *   3. Raw-WHAD default
 *
 * Non-adopted storage: every store function returns UNAVAILABLE and
 * every load function returns no data.  The caller is responsible for
 * checking adoption before issuing store calls.
 */
#ifndef CALIB_EVAL_H
#define CALIB_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sub-record header (6 bytes, prepended to the calib blob) ---------
 *
 * [0]      magic  (CALIB_MAGIC = 0xBC)
 * [1]      version (CALIB_VERSION = 1)
 * [2]      kind    (calib_kind_t)
 * [3]      ref_id  (sensor_id or profile_id, 0 for runtime)
 * [4..6)   blob_len u16 LE (length of the calib blob that follows)
 *
 * The journal payload CRC32 covers this header + the blob.  The
 * calib_eval layer additionally validates that blob_len is within
 * the expected range for the given kind.
 */

#define CALIB_MAGIC             0xBCu
#define CALIB_VERSION           1u
#define CALIB_HDR_SIZE          6u

/* Maximum blob sizes per kind (from imu.h/mag.h/profiles_eval.h). */
#define CALIB_BLOB_MAX          128u  /* fits PB_BYTES_ARRAY_T(128) */
#define CALIB_IMU_BLOB_SIZE     40u   /* IMU_CALIB_SER_SIZE */
#define CALIB_MAG_BLOB_SIZE     60u   /* MAG_CALIB_SER_SIZE */
#define CALIB_PROFILE_BLOB_MAX  112u  /* prof_profile_t packed */
#define CALIB_RUNTIME_BLOB_SIZE 1u    /* single byte: runtime_mode_t */

/* Total max payload = header + blob.  Must fit journal max payload. */
#define CALIB_PAYLOAD_MAX       (CALIB_HDR_SIZE + CALIB_BLOB_MAX)

/* ---- Record kinds ---------------------------------------------------- */

typedef enum {
	CALIB_KIND_NONE    = 0,
	CALIB_KIND_IMU     = 1,
	CALIB_KIND_MAG     = 2,
	CALIB_KIND_PROFILE = 3,
	CALIB_KIND_RUNTIME = 4,
} calib_kind_t;

#define CALIB_KIND_MIN  CALIB_KIND_IMU
#define CALIB_KIND_MAX  CALIB_KIND_RUNTIME

/* ---- In-memory sub-record ------------------------------------------- */

typedef struct {
	uint8_t  magic;
	uint8_t  version;
	uint8_t  kind;
	uint8_t  ref_id;
	uint16_t blob_len;
	uint8_t  blob[CALIB_BLOB_MAX];
} calib_record_t;

/* ---- Validation result ---------------------------------------------- */

typedef enum {
	CALIB_OK             = 0,
	CALIB_ERR_BAD_MAGIC  = 1,
	CALIB_ERR_BAD_VERSION= 2,
	CALIB_ERR_BAD_KIND   = 3,
	CALIB_ERR_BAD_LEN    = 4,
	CALIB_ERR_BAD_CRC    = 5,
	CALIB_ERR_BAD_VALUE  = 6,
	CALIB_ERR_WRONG_SENSOR = 7,
} calib_result_t;

/* ---- Adopted flag --------------------------------------------------- */

/* calib_eval needs to know whether storage is adopted.  The caller
 * sets this via calib_eval_set_adopted().  When not adopted, all
 * store functions return UNAVAILABLE (mapped from CALIB_ERR_BAD_VALUE
 * at the firmware layer) and load functions return no data. */
void calib_eval_set_adopted(bool adopted);
bool calib_eval_is_adopted(void);

/* ---- Init / serialize / deserialize --------------------------------- */

void calib_rec_init(calib_record_t *rec, uint8_t kind, uint8_t ref_id,
		    const uint8_t *blob, uint16_t blob_len);

/* Serialize to a flat buffer (header + blob).  Returns bytes written,
 * 0 if buffer too small or record invalid. */
uint16_t calib_rec_serialize(const calib_record_t *rec,
			     uint8_t *out, uint16_t out_size);

/* Deserialize from a flat buffer.  Returns true on success. */
bool calib_rec_deserialize(const uint8_t *buf, uint16_t buf_len,
			   calib_record_t *rec);

/* ---- Validation ----------------------------------------------------- */

/* Validate header structure + blob length for the given kind. */
calib_result_t calib_rec_validate(const calib_record_t *rec);

/* Validate IMU blob: check version byte and bias value ranges.
 * IMU blob format (from imu.h): version(1)+variant(1)+pad(2)+
 * gyro_bias[3]*4+accel_bias[3]*4+stationary(1)+pad(3)+crc32(4) = 40 bytes.
 * Biases must be within ±200000 (±200 dps / ±200g) — if any bias
 * exceeds this, the calibration is corrupt. */
calib_result_t calib_eval_validate_imu(const uint8_t *blob, uint16_t len);

/* Validate MAG blob: check version byte, hard_iron and soft_iron ranges.
 * MAG blob format (from mag.h): version(1)+status(1)+pad(2)+
 * hard_iron[3]*4+soft_q16[3]*4+pad(4)+crc32(4) = 40 bytes.
 * soft_iron_q16 must be in [16384, 262144] (0.25x to 4x). */
calib_result_t calib_eval_validate_mag(const uint8_t *blob, uint16_t len);

/* Validate profile blob: check it's non-empty and within size.
 * Profile blob is a packed prof_profile_t — firmware validates
 * structure when applying. */
calib_result_t calib_eval_validate_profile(const uint8_t *blob, uint16_t len);

/* Validate runtime blob: single byte, must be 0 (RAW_WHAD) or 1 (BLE_HID). */
calib_result_t calib_eval_validate_runtime(const uint8_t *blob, uint16_t len);

/* ---- Record selection (fault recovery) ------------------------------
 *
 * Given two records of the same kind+ref_id, pick the one with the
 * higher journal sequence.  If the preferred record fails validation,
 * fall back to the previous one.  This implements the "corrupted
 * header CRC → previous record selected" recovery.
 *
 * Returns true if *out_rec was set to a valid record.
 */
bool calib_eval_select_latest(const calib_record_t *new_rec,
			      uint64_t new_seq,
			      const calib_record_t *old_rec,
			      uint64_t old_seq,
			      calib_record_t *out_rec,
			      uint64_t *out_seq);

/* ---- Runtime mode helpers ------------------------------------------- */

/* Pack/unpack runtime mode into the 1-byte runtime blob.
 * mode: 0=RAW_WHAD, 1=BLE_HID (matches runtime.h runtime_mode_t). */
void calib_runtime_pack(uint8_t *blob, uint8_t mode);
bool calib_runtime_unpack(const uint8_t *blob, uint16_t len, uint8_t *out_mode);

/* ---- Expected blob length for kind ---------------------------------- */

uint16_t calib_expected_blob_len(uint8_t kind);

#ifdef __cplusplus
}
#endif

#endif /* CALIB_EVAL_H */
