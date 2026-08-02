/*
 * expert_force.h - Pure-C force/quiesce/restore lifecycle for expert I/O.
 *
 * Tracks per-service displacement state for force=true operations.
 * When an expert transfer needs to access onboard resources (e.g.
 * onboard I2C), the entire sensor bus group must be quiesced first.
 * This module tracks the lifecycle:
 *
 *   FREE → QUIESCING → DISPLACED → RESTORING → FREE/FAULT
 *
 * Non-cancellable operations (e.g., active streaming) are rejected
 * with NOT_CANCELLABLE rather than being torn down.
 *
 * Restore failure for any service must be reported as a BoardStatus
 * fault event — the caller is responsible for emitting that event.
 *
 * No SDK deps. Host-testable.
 */
#ifndef EXPERT_FORCE_H
#define EXPERT_FORCE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Services -------------------------------------------------------- */

typedef enum {
	EXPERT_FORCE_SVC_DISPLAY = 0,
	EXPERT_FORCE_SVC_SENSOR_BUS,
	EXPERT_FORCE_SVC_QSPI,
	EXPERT_FORCE_SVC_PDM,
	EXPERT_FORCE_SVC_NEOPIXEL,
	EXPERT_FORCE_SVC_BUZZER,
	EXPERT_FORCE_SVC_COUNT
} expert_force_service_t;

#define EXPERT_FORCE_SVC_COUNT_VAL ((uint32_t)EXPERT_FORCE_SVC_COUNT)

/* ---- States ---------------------------------------------------------- */

typedef enum {
	EXPERT_FORCE_STATE_FREE = 0,
	EXPERT_FORCE_STATE_QUIESCING,
	EXPERT_FORCE_STATE_DISPLACED,
	EXPERT_FORCE_STATE_RESTORING,
	EXPERT_FORCE_STATE_FAULT,
} expert_force_state_t;

/* ---- Results --------------------------------------------------------- */

typedef enum {
	EXPERT_FORCE_OK = 0,
	EXPERT_FORCE_ERR_BUSY,            /* service in use, not quiesceable */
	EXPERT_FORCE_ERR_NOT_CANCELLABLE, /* active streaming, cannot quiesce */
	EXPERT_FORCE_ERR_INVALID_PARAM,
	EXPERT_FORCE_ERR_ALREADY_DISPLACED,
	EXPERT_FORCE_ERR_NOT_DISPLACED,
	EXPERT_FORCE_ERR_WRONG_STATE,
} expert_force_result_t;

/* ---- Lifecycle ------------------------------------------------------- */

/* Initialize / reset all services to FREE. Call at startup. */
void expert_force_init(void);

/* Request force-quiesce of a service.
 * If the service has been marked non-cancellable (e.g. active stream),
 * returns NOT_CANCELLABLE.
 * If the service is already displaced, returns ALREADY_DISPLACED.
 * On success, transitions to QUIESCING — the caller must call
 * expert_force_quiesce_complete() when the quiesce handler finishes. */
expert_force_result_t expert_force_request(expert_force_service_t svc);

/* Mark quiesce complete — the service is now DISPLACED and the
 * resource is available for the expert caller. */
expert_force_result_t expert_force_quiesce_complete(expert_force_service_t svc);

/* Release a displaced service — begins the restore sequence.
 * Transitions DISPLACED → RESTORING. Caller must call
 * expert_force_restore_complete() when the restore handler finishes. */
expert_force_result_t expert_force_release(expert_force_service_t svc);

/* Mark restore complete. If success=true → FREE. If success=false → FAULT.
 * A FAULT state means the service could not be restored and must be
 * reported via BoardStatus fault event. The service stays in FAULT
 * until expert_force_init() resets it or expert_force_clear_fault(). */
expert_force_result_t expert_force_restore_complete(
	expert_force_service_t svc, bool success);

/* Clear a fault (after the BoardStatus event has been emitted). */
expert_force_result_t expert_force_clear_fault(expert_force_service_t svc);

/* ---- Queries --------------------------------------------------------- */

expert_force_state_t expert_force_get_state(expert_force_service_t svc);

/* Is the service currently displaced (owned by expert)? */
bool expert_force_is_displaced(expert_force_service_t svc);

/* Is any service in FAULT state? (Caller should emit BoardStatus.) */
bool expert_force_has_fault(void);

/* Get the first service in FAULT state, or -1 if none. */
int32_t expert_force_get_fault_service(void);

/* ---- Cancellable control --------------------------------------------- */

/* Mark a service as non-cancellable (e.g. when streaming starts).
 * force_request will reject with NOT_CANCELLABLE while this is set. */
void expert_force_set_cancellable(expert_force_service_t svc,
                                  bool cancellable);

bool expert_force_is_cancellable(expert_force_service_t svc);

/* ---- Bitmap helpers -------------------------------------------------- */

/* Get a bitmask of all currently displaced services. */
uint32_t expert_force_displaced_mask(void);

#ifdef __cplusplus
}
#endif
#endif /* EXPERT_FORCE_H */
