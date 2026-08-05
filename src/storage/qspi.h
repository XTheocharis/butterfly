/*
 * qspi.h - QSPI flash storage manager (firmware wrapper for CLUE).
 *
 * Wraps qspi_eval.{h,c} (pure-logic) with nrfx_qspi hardware operations.
 * Provides: JEDEC RDID probe, StorageInfo + nonce, and the async
 * adoption transaction state machine.
 *
 * Pre-adoption safety: the QSPI peripheral is initialized in RDID-only
 * mode. No write/erase commands are issued until the adoption
 * transaction is fully confirmed (exact nonce + CLI flag or 3s A+B).
 *
 * The async adoption FSM runs long erase/verify work across multiple
 * tick() calls, feeding the watchdog between steps. BoardModule emits
 * BOARD_STATUS_STORAGE_PROGRESS events using progressCurrent() /
 * progressTotal() at the start and completion of each transaction.
 *
 * Compiled only under BOARD_CLUE. Host tests exercise qspi_eval.c
 * directly without this wrapper.
 */
#ifndef QSPI_H
#define QSPI_H

#ifdef BOARD_CLUE

#include <stdint.h>
#include <stdbool.h>

#include "qspi_eval.h"
#include "pinRegistry.h"
#include "timebase.h"

#ifdef __cplusplus

class QspiManager {
public:
    QspiManager();

    /* Initialize with backend hooks. Acquires PINREG_GROUP_QSPI.
     * Performs JEDEC RDID probe. Does NOT auto-adopt. */
    bool init(pinreg_token_t group_lease);

    /* Device presence + adoption state. */
    bool isPresent(void) const { return m_present; }
    qspi_state_t state(void) const { return m_adoptState; }
    bool isAdopted(void) const
        { return qspi_eval_state_is_adopted(m_adoptState); }

    /* StorageInfo response data. Generates a new nonce on each call
     * (invalidating any previous nonce). */
    void getStorageInfo(uint8_t jedec_id[3], uint32_t *capacity,
                        qspi_state_t *storage_state,
                        uint8_t nonce_out[QSPI_NONCE_SIZE]);

    /* Start adoption. Returns false if conditions not met.
     * confirm_menu = true for 3s A+B hold path.
     * confirm_cli = true for --yes-really-adopt-and-erase path.
     * At least one must be true AND the nonce must match. */
    bool beginAdoption(const uint8_t *candidate_nonce, size_t nonce_len,
                       bool confirm_menu, bool confirm_cli);

    /* Advance the async adoption transaction. Returns true while work
     * remains, false when done (adopted or failed).
     * Call from main loop. Feeds watchdog between steps. */
    bool tick(void);

    /* Progress for BoardStatus events: current sector being processed. */
    uint32_t progressCurrent(void) const { return m_progressSector; }
    uint32_t progressTotal(void) const { return QSPI_SECTOR_COUNT; }

private:
    bool sendJedecRdid(uint8_t rdid[3]);
    bool eraseSectorByIndex(uint32_t sector_idx);
    bool verifySectorErased(uint32_t sector_idx);
    bool writeSuperblockHeader(const uint8_t buf[QSPI_SB_HEADER_SIZE]);
    bool writeCommitMarker(void);
    bool generateNonce(uint8_t out[QSPI_NONCE_SIZE]);

    void setState(qspi_state_t s) { m_adoptState = s; }

    bool              m_initialized;
    bool              m_present;
    qspi_state_t      m_adoptState;
    qspi_nonce_tracker_t m_nonce;
    pinreg_token_t    m_groupLease;
    uint32_t          m_progressSector;
    qspi_superblock_t m_pendingSb;
};

#endif /* __cplusplus */
#endif /* BOARD_CLUE */
#endif /* QSPI_H */
