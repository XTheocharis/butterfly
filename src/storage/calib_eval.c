/*
 * calib_eval.c - Pure-C calibration/profile/runtime record validation.
 *
 * No SDK deps. Included from calib.cpp (firmware) and test_calib.cpp (host).
 */
#include "calib_eval.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Adopted flag --------------------------------------------------- */

static bool s_adopted = false;

void calib_eval_set_adopted(bool adopted) { s_adopted = adopted; }
bool calib_eval_is_adopted(void) { return s_adopted; }

/* ---- Init / serialize / deserialize --------------------------------- */

void calib_rec_init(calib_record_t *rec, uint8_t kind, uint8_t ref_id,
		    const uint8_t *blob, uint16_t blob_len)
{
	memset(rec, 0, sizeof(*rec));
	rec->magic = CALIB_MAGIC;
	rec->version = CALIB_VERSION;
	rec->kind = kind;
	rec->ref_id = ref_id;
	rec->blob_len = blob_len;
	if (blob != NULL && blob_len > 0) {
		uint16_t copy = blob_len;
		if (copy > CALIB_BLOB_MAX) copy = CALIB_BLOB_MAX;
		memcpy(rec->blob, blob, copy);
		rec->blob_len = copy;
	}
}

uint16_t calib_rec_serialize(const calib_record_t *rec,
			     uint8_t *out, uint16_t out_size)
{
	uint16_t need = CALIB_HDR_SIZE + rec->blob_len;
	if (out == NULL || out_size < need || need > CALIB_PAYLOAD_MAX) {
		return 0;
	}
	out[0] = rec->magic;
	out[1] = rec->version;
	out[2] = rec->kind;
	out[3] = rec->ref_id;
	out[4] = (uint8_t)(rec->blob_len & 0xFF);
	out[5] = (uint8_t)((rec->blob_len >> 8) & 0xFF);
	if (rec->blob_len > 0) {
		memcpy(&out[CALIB_HDR_SIZE], rec->blob, rec->blob_len);
	}
	return need;
}

bool calib_rec_deserialize(const uint8_t *buf, uint16_t buf_len,
			   calib_record_t *rec)
{
	if (buf == NULL || rec == NULL || buf_len < CALIB_HDR_SIZE) {
		return false;
	}
	memset(rec, 0, sizeof(*rec));
	rec->magic = buf[0];
	rec->version = buf[1];
	rec->kind = buf[2];
	rec->ref_id = buf[3];
	rec->blob_len = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
	if (rec->blob_len > CALIB_BLOB_MAX) {
		return false;
	}
	if (buf_len < (uint16_t)(CALIB_HDR_SIZE + rec->blob_len)) {
		return false;
	}
	if (rec->blob_len > 0) {
		memcpy(rec->blob, &buf[CALIB_HDR_SIZE], rec->blob_len);
	}
	return true;
}

/* ---- Expected blob length ------------------------------------------- */

uint16_t calib_expected_blob_len(uint8_t kind)
{
	switch (kind) {
	case CALIB_KIND_IMU:     return CALIB_IMU_BLOB_SIZE;
	case CALIB_KIND_MAG:     return CALIB_MAG_BLOB_SIZE;
	case CALIB_KIND_PROFILE: return 0; /* variable, 1..CALIB_PROFILE_BLOB_MAX */
	case CALIB_KIND_RUNTIME: return CALIB_RUNTIME_BLOB_SIZE;
	default:                 return 0;
	}
}

/* ---- Validation ----------------------------------------------------- */

calib_result_t calib_rec_validate(const calib_record_t *rec)
{
	if (rec == NULL) return CALIB_ERR_BAD_VALUE;
	if (rec->magic != CALIB_MAGIC) return CALIB_ERR_BAD_MAGIC;
	if (rec->version != CALIB_VERSION) return CALIB_ERR_BAD_VERSION;
	if (rec->kind < CALIB_KIND_MIN || rec->kind > CALIB_KIND_MAX) {
		return CALIB_ERR_BAD_KIND;
	}
	if (rec->blob_len > CALIB_BLOB_MAX) return CALIB_ERR_BAD_LEN;

	uint16_t expected = calib_expected_blob_len(rec->kind);
	if (rec->kind != CALIB_KIND_PROFILE) {
		if (rec->blob_len != expected) return CALIB_ERR_BAD_LEN;
	} else {
		if (rec->blob_len == 0 || rec->blob_len > CALIB_PROFILE_BLOB_MAX) {
			return CALIB_ERR_BAD_LEN;
		}
	}

	/* Kind-specific semantic validation. */
	switch (rec->kind) {
	case CALIB_KIND_IMU:
		return calib_eval_validate_imu(rec->blob, rec->blob_len);
	case CALIB_KIND_MAG:
		return calib_eval_validate_mag(rec->blob, rec->blob_len);
	case CALIB_KIND_PROFILE:
		return calib_eval_validate_profile(rec->blob, rec->blob_len);
	case CALIB_KIND_RUNTIME:
		return calib_eval_validate_runtime(rec->blob, rec->blob_len);
	default:
		return CALIB_OK;
	}
}

/* IMU blob layout (40 bytes):
 *   [0] version (must be 1)
 *   [1] variant (0=unknown, 1=LSM6DS33, 2=LSM6DS3TRC)
 *   [2..4) pad
 *   [4..16) gyro_bias[3] i32 LE (mdps)
 *   [16..28) accel_bias[3] i32 LE (mg)
 *   [28] stationary
 *   [29..32) pad
 *   [32..36) crc32 LE
 *   [36..40) pad
 */
calib_result_t calib_eval_validate_imu(const uint8_t *blob, uint16_t len)
{
	if (blob == NULL || len != CALIB_IMU_BLOB_SIZE) {
		return CALIB_ERR_BAD_LEN;
	}
	if (blob[0] != 1) return CALIB_ERR_BAD_VERSION; /* IMU_CALIB_VERSION */
	uint8_t variant = blob[1];
	if (variant > 2) return CALIB_ERR_BAD_VALUE;

	/* Check bias ranges: each i32 must be within ±200000.
	 * Gyro biases at [4..16), accel biases at [16..28). */
	for (int i = 0; i < 6; i++) {
		int32_t val = (int32_t)((uint32_t)blob[4 + i*4] |
			        ((uint32_t)blob[5 + i*4] << 8) |
			        ((uint32_t)blob[6 + i*4] << 16) |
			        ((uint32_t)blob[7 + i*4] << 24));
		if (val > 200000 || val < -200000) {
			return CALIB_ERR_BAD_VALUE;
		}
	}
	return CALIB_OK;
}

/* MAG blob layout (60 bytes):
 *   [0]    version (must be 1)
 *   [1]    status (0=idle, 1=collect, 2=done)
 *   [2..4) pad
 *   [4..16)   hard_iron[3] i32 LE (milligauss)
 *   [16..52)  soft_iron_q16[3][3] i32 LE (row-major)
 *   [52..56)  pad
 *   [56..60)  crc32 LE
 */
calib_result_t calib_eval_validate_mag(const uint8_t *blob, uint16_t len)
{
	if (blob == NULL || len != CALIB_MAG_BLOB_SIZE) {
		return CALIB_ERR_BAD_LEN;
	}
	if (blob[0] != 1) return CALIB_ERR_BAD_VERSION;
	if (blob[1] > 2) return CALIB_ERR_BAD_VALUE;

	/* hard_iron must be within ±500000 milligauss (±500 gauss). */
	for (int i = 0; i < 3; i++) {
		int32_t val = (int32_t)((uint32_t)blob[4 + i*4] |
			        ((uint32_t)blob[5 + i*4] << 8) |
			        ((uint32_t)blob[6 + i*4] << 16) |
			        ((uint32_t)blob[7 + i*4] << 24));
		if (val > 500000 || val < -500000) {
			return CALIB_ERR_BAD_VALUE;
		}
	}

	/* soft_iron_q16 3x3 matrix:
	 *   - diagonal (i==j): must be in [16384, 262144] (0.25x..4x)
	 *   - off-diagonal: 0 allowed (no fit yet) or in same range */
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			uint16_t off = 16 + (uint16_t)(i * 3 + j) * 4;
			int32_t val = (int32_t)((uint32_t)blob[off] |
				        ((uint32_t)blob[off + 1] << 8) |
				        ((uint32_t)blob[off + 2] << 16) |
				        ((uint32_t)blob[off + 3] << 24));
			if (i != j && val == 0) continue;
			if (val < 16384 || val > 262144) {
				return CALIB_ERR_BAD_VALUE;
			}
		}
	}
	return CALIB_OK;
}

calib_result_t calib_eval_validate_profile(const uint8_t *blob, uint16_t len)
{
	if (blob == NULL || len == 0 || len > CALIB_PROFILE_BLOB_MAX) {
		return CALIB_ERR_BAD_LEN;
	}
	/* Profile blob is opaque packed struct — firmware validates structure
	 * when deserializing.  Here we only check size bounds. */
	return CALIB_OK;
}

calib_result_t calib_eval_validate_runtime(const uint8_t *blob, uint16_t len)
{
	if (blob == NULL || len != CALIB_RUNTIME_BLOB_SIZE) {
		return CALIB_ERR_BAD_LEN;
	}
	uint8_t mode = blob[0];
	if (mode > 1) return CALIB_ERR_BAD_VALUE;
	return CALIB_OK;
}

/* ---- Record selection (fault recovery) ------------------------------ */

bool calib_eval_select_latest(const calib_record_t *new_rec,
			      uint64_t new_seq,
			      const calib_record_t *old_rec,
			      uint64_t old_seq,
			      calib_record_t *out_rec,
			      uint64_t *out_seq)
{
	if (out_rec == NULL) return false;

	bool new_ok = (new_rec != NULL &&
		       calib_rec_validate(new_rec) == CALIB_OK);
	bool old_ok = (old_rec != NULL &&
		       calib_rec_validate(old_rec) == CALIB_OK);

	if (new_ok && old_ok) {
		/* Pick the one with the higher sequence. */
		if (new_seq >= old_seq) {
			*out_rec = *new_rec;
			if (out_seq) *out_seq = new_seq;
		} else {
			*out_rec = *old_rec;
			if (out_seq) *out_seq = old_seq;
		}
		return true;
	}
	if (new_ok) {
		*out_rec = *new_rec;
		if (out_seq) *out_seq = new_seq;
		return true;
	}
	if (old_ok) {
		*out_rec = *old_rec;
		if (out_seq) *out_seq = old_seq;
		return true;
	}
	return false;
}

/* ---- Runtime mode helpers ------------------------------------------- */

void calib_runtime_pack(uint8_t *blob, uint8_t mode)
{
	if (blob != NULL) {
		blob[0] = mode;
	}
}

bool calib_runtime_unpack(const uint8_t *blob, uint16_t len, uint8_t *out_mode)
{
	if (blob == NULL || len < 1 || out_mode == NULL) {
		return false;
	}
	uint8_t mode = blob[0];
	if (mode > 1) return false;
	*out_mode = mode;
	return true;
}

#ifdef __cplusplus
}
#endif
