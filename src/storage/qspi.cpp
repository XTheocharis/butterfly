/*
 * qspi.cpp - QSPI flash storage manager firmware implementation.
 *
 * Wraps qspi_eval.{h,c} with nrfx_qspi hardware operations.
 * All decision logic is in qspi_eval.c; this file translates between
 * hardware and the pure functions.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifdef BOARD_CLUE

#include "qspi.h"
#include "platformRuntime.h"
#include "custom_board.h"

#include "nrf.h"
#include "nrf_gpio.h"
#include "nrfx_qspi.h"
#include "nrf_delay.h"
#include "app_util_platform.h"
#include "nrf_crypto_rng.h"

/* Include the eval implementation so all logic functions are available
 * without a separate compilation unit in the firmware build. */
extern "C" {
#include "qspi_eval.c"
}

#define QSPI_ERASE_TIMEOUT_MS  400
#define QSPI_PAGE_PROGRAM_TIMEOUT_MS 5

static bool g_qspi_ready = false;

static bool qspi_wait_ready(uint32_t timeout_ms)
{
	for (uint32_t i = 0; i < timeout_ms; i++) {
		nrf_qspi_event_t ev = NRF_QSPI_EVENT_READY;
		if (nrf_qspi_event_check(NRF_QSPI, ev)) {
			return true;
		}
		nrf_delay_ms(1);
	}
	return false;
}

QspiManager::QspiManager()
    : m_initialized(false)
    , m_present(false)
    , m_adoptState(QSPI_ST_UNADOPTED)
    , m_groupLease(PINREG_TOKEN_INVALID)
    , m_progressSector(0)
{
	qspi_eval_sb_init(&m_pendingSb);
	qspi_eval_nonce_init(&m_nonce);
}

bool QspiManager::sendJedecRdid(uint8_t rdid[3])
{
	if (!g_qspi_ready) return false;

	nrf_qspi_cinstr_conf_t cinstr = {
		.opcode    = QSPI_OP_JEDEC_ID,
		.length    = NRF_QSPI_CINSTR_LEN_4B,
		.io2_level = false,
		.io3_level = false,
		.wipwait   = true,
		.wren      = false,
	};

	uint8_t rx[3] = {0};
	nrfx_err_t err = nrfx_qspi_cinstr_xfer(&cinstr, NULL, rx);
	if (err != NRFX_SUCCESS) return false;

	rdid[0] = rx[0];
	rdid[1] = rx[1];
	rdid[2] = rx[2];
	return true;
}

bool QspiManager::init(pinreg_token_t group_lease)
{
	m_groupLease = group_lease;

	nrfx_qspi_config_t config = {
		.xip_offset = 0x12000000,
		.pins = {
			.sck_pin = CLUE_QSPI_SCK,
			.csn_pin = CLUE_QSPI_CSN,
			.io0_pin = CLUE_QSPI_IO0,
			.io1_pin = CLUE_QSPI_IO1,
			.io2_pin = CLUE_QSPI_IO2,
			.io3_pin = CLUE_QSPI_IO3,
		},
		.prot_if = {
			.readoc = (nrf_qspi_readoc_t)NRFX_QSPI_CONFIG_READOC,
			.writeoc = (nrf_qspi_writeoc_t)NRFX_QSPI_CONFIG_WRITEOC,
			.addrmode = NRF_QSPI_ADDRMODE_24BIT,
			.dpmconfig = false,
		},
		.phy_if = {
			.sck_delay = 5,
			.dpmen = false,
			.spi_mode = NRF_QSPI_MODE_0,
			.sck_freq = NRF_QSPI_FREQ_32MDIV5,
		},
		.irq_priority = 6,
	};

	nrfx_err_t err = nrfx_qspi_init(&config, NULL, NULL);
	if (err != NRFX_SUCCESS) {
		return false;
	}
	g_qspi_ready = true;

	m_adoptState = qspi_eval_adopt_transition(
		m_adoptState, QSPI_EVT_PROBE_OK);

	uint8_t rdid[3];
	if (!sendJedecRdid(rdid)) {
		m_present = false;
		m_adoptState = QSPI_ST_UNADOPTED;
		m_initialized = true;
		return true;
	}

	if (qspi_eval_jedec_match(rdid)) {
		m_present = true;
		m_adoptState = qspi_eval_adopt_transition(
			m_adoptState, QSPI_EVT_ID_MATCH);
	} else {
		m_present = false;
		m_adoptState = qspi_eval_adopt_transition(
			m_adoptState, QSPI_EVT_ID_MISMATCH);
	}

	m_initialized = true;
	return true;
}

void QspiManager::getStorageInfo(uint8_t jedec_id[3], uint32_t *capacity,
                                 qspi_state_t *storage_state,
                                 uint8_t nonce_out[QSPI_NONCE_SIZE])
{
	if (m_present) {
		jedec_id[0] = QSPI_JEDEC_MANUFACTURER;
		jedec_id[1] = QSPI_JEDEC_TYPE;
		jedec_id[2] = QSPI_JEDEC_CAPACITY;
		*capacity = QSPI_FLASH_SIZE_BYTES;
	} else {
		jedec_id[0] = jedec_id[1] = jedec_id[2] = 0;
		*capacity = 0;
	}
	*storage_state = m_adoptState;

	uint8_t nonce[QSPI_NONCE_SIZE];
	generateNonce(nonce);
	uint64_t now = timebase_now_us();
	qspi_eval_nonce_issue(&m_nonce, nonce, now);
	memcpy(nonce_out, nonce, QSPI_NONCE_SIZE);
}

bool QspiManager::generateNonce(uint8_t out[QSPI_NONCE_SIZE])
{
	if (platform_runtime_softdevice_active()) {
		nrf_crypto_rng_vector_generate(out, QSPI_NONCE_SIZE);
	} else {
		NRF_RNG->TASKS_START = 1;
		NRF_RNG->CONFIG = RNG_CONFIG_DERCEN_Msk;
		for (uint8_t i = 0; i < QSPI_NONCE_SIZE; i++) {
			while (!NRF_RNG->EVENTS_VALRDY) {}
			out[i] = (uint8_t)NRF_RNG->VALUE;
			NRF_RNG->EVENTS_VALRDY = 0;
		}
		NRF_RNG->TASKS_STOP = 1;
	}
	return true;
}

bool QspiManager::beginAdoption(const uint8_t *candidate_nonce,
                                size_t nonce_len,
                                bool confirm_menu, bool confirm_cli)
{
	if (!m_present) return false;
	if (!confirm_menu && !confirm_cli) return false;

	uint64_t now = timebase_now_us();
	qspi_nonce_result_t nr = qspi_eval_nonce_check(
		&m_nonce, candidate_nonce, nonce_len, now);

	if (nr != QSPI_NONCE_MATCH_OK) return false;

	m_adoptState = qspi_eval_adopt_transition(
		m_adoptState, QSPI_EVT_CONFIRMED);
	/* Sectors 0..1 (the superblock) are erased during CONFIRMED. Start the
	 * ERASING_SUPERBLOCK loop at sector 2 to avoid re-erasing them. */
	m_progressSector = QSPI_SUPERBLOCK_SECTORS;
	qspi_eval_sb_init(&m_pendingSb);
	m_pendingSb.crc32 = qspi_eval_sb_compute_crc(&m_pendingSb);
	return true;
}

bool QspiManager::eraseSectorByIndex(uint32_t sector_idx)
{
	if (!g_qspi_ready) return false;
	uint32_t addr = sector_idx * QSPI_SECTOR_SIZE;
	nrfx_err_t err = nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, addr);
	if (err != NRFX_SUCCESS) return false;
	return qspi_wait_ready(QSPI_ERASE_TIMEOUT_MS);
}

bool QspiManager::verifySectorErased(uint32_t sector_idx)
{
	uint8_t buf[256];
	uint32_t addr = sector_idx * QSPI_SECTOR_SIZE;
	for (uint32_t off = 0; off < QSPI_SECTOR_SIZE; off += sizeof(buf)) {
		nrfx_err_t err = nrfx_qspi_read(buf, sizeof(buf), addr + off);
		if (err != NRFX_SUCCESS) return false;
		if (!qspi_wait_ready(10)) return false;
		for (size_t i = 0; i < sizeof(buf); i++) {
			if (buf[i] != 0xFF) return false;
		}
	}
	return true;
}

bool QspiManager::writeSuperblockHeader(const uint8_t buf[QSPI_SB_HEADER_SIZE])
{
	nrfx_err_t err = nrfx_qspi_write(buf, QSPI_SB_COMMIT_OFFSET, 0);
	if (err != NRFX_SUCCESS) return false;
	if (!qspi_wait_ready(QSPI_PAGE_PROGRAM_TIMEOUT_MS)) return false;

	/* Read back the just-written region and confirm bit-for-bit equality.
	 * A mismatch means the sector was not actually erased, the device is
	 * faulty, or the program operation silently dropped bytes. Aborting
	 * here prevents the commit marker from being written on top of a
	 * corrupted header. */
	uint8_t readback[QSPI_SB_COMMIT_OFFSET];
	err = nrfx_qspi_read(readback, sizeof(readback), 0);
	if (err != NRFX_SUCCESS) return false;
	if (!qspi_wait_ready(QSPI_PAGE_PROGRAM_TIMEOUT_MS)) return false;
	if (memcmp(readback, buf, QSPI_SB_COMMIT_OFFSET) != 0) return false;

	return true;
}

bool QspiManager::writeCommitMarker(void)
{
	uint8_t commit = QSPI_SB_COMMIT_VALUE;
	nrfx_err_t err = nrfx_qspi_write(&commit, 1, QSPI_SB_COMMIT_OFFSET);
	if (err != NRFX_SUCCESS) return false;
	return qspi_wait_ready(QSPI_PAGE_PROGRAM_TIMEOUT_MS);
}

bool QspiManager::tick(void)
{
	switch (m_adoptState) {
	case QSPI_ST_CONFIRMED:
		if (eraseSectorByIndex(0) && verifySectorErased(0) &&
		    eraseSectorByIndex(1) && verifySectorErased(1)) {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_ERASE_SB_OK);
		} else {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_ERASE_SB_FAIL);
		}
		break;

	case QSPI_ST_ERASING_SUPERBLOCK:
		if (m_progressSector < QSPI_SECTOR_COUNT) {
			bool ok = eraseSectorByIndex(m_progressSector);
			if (!ok || !verifySectorErased(m_progressSector)) {
				m_adoptState = qspi_eval_adopt_transition(
					m_adoptState, QSPI_EVT_ERASE_REM_FAIL);
				break;
			}
			m_progressSector++;
		} else {
			/* Erase+verify of all remaining sectors complete. Reset the
			 * counter so ERASING_REMAINING can do a final full pass. */
			m_progressSector = QSPI_SUPERBLOCK_SECTORS;
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_ERASE_REM_OK);
		}
		break;

	case QSPI_ST_ERASING_REMAINING:
		/* Final verification pass: re-read every data sector and confirm
		 * it is fully erased. Catches any erase that appeared to succeed
		 * but did not stick. One sector per tick to keep the main loop
		 * responsive; on any non-erased sector, transition to fault. */
		if (m_progressSector < QSPI_SECTOR_COUNT) {
			if (!verifySectorErased(m_progressSector)) {
				m_adoptState = qspi_eval_adopt_transition(
					m_adoptState, QSPI_EVT_VERIFY_FAIL);
				break;
			}
			m_progressSector++;
		} else {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_VERIFY_OK);
		}
		break;

	case QSPI_ST_VERIFYING: {
		uint8_t buf[QSPI_SB_HEADER_SIZE];
		qspi_eval_sb_serialize(&m_pendingSb, buf);
		if (writeSuperblockHeader(buf)) {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_WRITE_SB_OK);
		} else {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_WRITE_SB_FAIL);
		}
		break;
	}

	case QSPI_ST_WRITING_SUPERBLOCK:
		if (writeCommitMarker()) {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_COMMIT_OK);
		} else {
			m_adoptState = qspi_eval_adopt_transition(
				m_adoptState, QSPI_EVT_COMMIT_FAIL);
		}
		break;

	case QSPI_ST_COMMITTING:
		m_adoptState = qspi_eval_adopt_transition(
			m_adoptState, QSPI_EVT_COMMIT_OK);
		break;

	case QSPI_ST_ADOPTED:
	case QSPI_ST_UNADOPTED:
	default:
		return false;
	}

	return m_adoptState != QSPI_ST_ADOPTED &&
	       m_adoptState != QSPI_ST_UNADOPTED;
}

#endif /* BOARD_CLUE */
