/*
 * expert_force.c - Pure-C force/quiesce/restore lifecycle for expert I/O.
 *
 * Tracks per-service displacement state for force=true operations.
 * No SDK deps. Host-testable.
 */
#include "expert_force.h"

typedef struct {
	expert_force_state_t state;
	bool                 cancellable;
} expert_force_entry_t;

static expert_force_entry_t g_services[EXPERT_FORCE_SVC_COUNT];

void expert_force_init(void)
{
	for (uint8_t i = 0; i < EXPERT_FORCE_SVC_COUNT; i++) {
		g_services[i].state       = EXPERT_FORCE_STATE_FREE;
		g_services[i].cancellable = true;
	}
}

expert_force_result_t expert_force_request(expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return EXPERT_FORCE_ERR_INVALID_PARAM;

	expert_force_entry_t *e = &g_services[svc];

	if (e->state == EXPERT_FORCE_STATE_DISPLACED ||
	    e->state == EXPERT_FORCE_STATE_QUIESCING)
		return EXPERT_FORCE_ERR_ALREADY_DISPLACED;

	if (e->state == EXPERT_FORCE_STATE_RESTORING)
		return EXPERT_FORCE_ERR_WRONG_STATE;

	if (e->state == EXPERT_FORCE_STATE_FAULT)
		return EXPERT_FORCE_ERR_WRONG_STATE;

	if (!e->cancellable)
		return EXPERT_FORCE_ERR_NOT_CANCELLABLE;

	e->state = EXPERT_FORCE_STATE_QUIESCING;
	return EXPERT_FORCE_OK;
}

expert_force_result_t expert_force_quiesce_complete(
	expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return EXPERT_FORCE_ERR_INVALID_PARAM;

	expert_force_entry_t *e = &g_services[svc];

	if (e->state != EXPERT_FORCE_STATE_QUIESCING)
		return EXPERT_FORCE_ERR_WRONG_STATE;

	e->state = EXPERT_FORCE_STATE_DISPLACED;
	return EXPERT_FORCE_OK;
}

expert_force_result_t expert_force_release(expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return EXPERT_FORCE_ERR_INVALID_PARAM;

	expert_force_entry_t *e = &g_services[svc];

	if (e->state != EXPERT_FORCE_STATE_DISPLACED)
		return EXPERT_FORCE_ERR_NOT_DISPLACED;

	e->state = EXPERT_FORCE_STATE_RESTORING;
	return EXPERT_FORCE_OK;
}

expert_force_result_t expert_force_restore_complete(
	expert_force_service_t svc, bool success)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return EXPERT_FORCE_ERR_INVALID_PARAM;

	expert_force_entry_t *e = &g_services[svc];

	if (e->state != EXPERT_FORCE_STATE_RESTORING)
		return EXPERT_FORCE_ERR_WRONG_STATE;

	e->state = success ? EXPERT_FORCE_STATE_FREE
	                   : EXPERT_FORCE_STATE_FAULT;
	return EXPERT_FORCE_OK;
}

expert_force_result_t expert_force_clear_fault(
	expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return EXPERT_FORCE_ERR_INVALID_PARAM;

	expert_force_entry_t *e = &g_services[svc];

	if (e->state != EXPERT_FORCE_STATE_FAULT)
		return EXPERT_FORCE_ERR_WRONG_STATE;

	e->state = EXPERT_FORCE_STATE_FREE;
	return EXPERT_FORCE_OK;
}

expert_force_state_t expert_force_get_state(expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return EXPERT_FORCE_STATE_FREE;
	return g_services[svc].state;
}

bool expert_force_is_displaced(expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return false;
	return g_services[svc].state == EXPERT_FORCE_STATE_DISPLACED;
}

bool expert_force_has_fault(void)
{
	for (uint8_t i = 0; i < EXPERT_FORCE_SVC_COUNT; i++) {
		if (g_services[i].state == EXPERT_FORCE_STATE_FAULT)
			return true;
	}
	return false;
}

int32_t expert_force_get_fault_service(void)
{
	for (uint8_t i = 0; i < EXPERT_FORCE_SVC_COUNT; i++) {
		if (g_services[i].state == EXPERT_FORCE_STATE_FAULT)
			return (int32_t)i;
	}
	return -1;
}

void expert_force_set_cancellable(expert_force_service_t svc, bool cancellable)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return;
	g_services[svc].cancellable = cancellable;
}

bool expert_force_is_cancellable(expert_force_service_t svc)
{
	if ((uint32_t)svc >= EXPERT_FORCE_SVC_COUNT)
		return true;
	return g_services[svc].cancellable;
}

uint32_t expert_force_displaced_mask(void)
{
	uint32_t mask = 0;
	for (uint8_t i = 0; i < EXPERT_FORCE_SVC_COUNT; i++) {
		if (g_services[i].state == EXPERT_FORCE_STATE_DISPLACED)
			mask |= (1u << i);
	}
	return mask;
}
