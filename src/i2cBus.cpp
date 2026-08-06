/*
 * i2cBus.cpp - pure-logic async TWIM1 bus manager.
 *
 * No SDK deps. Hardware is behind i2cbus_backend_t function pointers.
 * Compiles on host for unit testing and on firmware for CLUE sensors.
 *
 * allow: SIZE_OK — indivisible state machine. The queue, active transfer,
 * recovery FSM, and probe FSM all share the same state variables and
 * invariants. Splitting would scatter transitions across files (same
 * exception as pinRegistry.cpp 276 LOC and menu.cpp 338 LOC).
 */
#include "i2cBus.h"
#include <string.h>

/* ---- Frozen address table -------------------------------------------- */
const uint8_t I2CBUS_ONBOARD_ADDRESSES[I2CBUS_ONBOARD_COUNT] = {
	I2CBUS_ADDR_LSM6DS33, I2CBUS_ADDR_LIS3MDL, I2CBUS_ADDR_APDS9960,
	I2CBUS_ADDR_SHT31D,   I2CBUS_ADDR_BMP280,
};

/* ---- Constants ------------------------------------------------------- */
#define QUEUE_SIZE        4
#define TIMEOUT_US        10000u
#define SHT31_CONVERT_US  15000u
#define MAX_SCL_CLOCKS    9

struct IdEntry { uint8_t addr, id_reg, expect, alt; bool is_sht; };
static const IdEntry ID_TABLE[I2CBUS_ONBOARD_COUNT] = {
	{ 0x6A, 0x0F, 0x69, 0x6A, false },
	{ 0x1C, 0x0F, 0x3D, 0xFF, false },
	{ 0x39, 0x92, 0xAB, 0xFF, false },
	{ 0x44, 0x00, 0x00, 0xFF, true  },
	{ 0x77, 0xD0, 0x58, 0xFF, false },
};

/* ---- Pending transfer ------------------------------------------------ */
struct Pending {
	i2cbus_transfer_t xfer;
	i2cbus_token_t    token;
	uint64_t          start_us;
	bool              cancelled;
	bool              completed;
};

/* ---- Manager state --------------------------------------------------- */
static i2cbus_backend_t g_backend;
static bool             g_ready;
static i2cbus_state_t   g_state;
static uint32_t         g_groupLease;

static Pending g_queue[QUEUE_SIZE];
static uint8_t g_qHead, g_qTail, g_qCount;
static Pending g_active;
static bool    g_hasActive;

static uint8_t g_recClocks;
static uint8_t g_recPhase; /* 0=release 1=clock 2=stop 3=reinit 4=done */
static uint32_t g_presence;
static i2cbus_device_id_t g_devIds[I2CBUS_ONBOARD_COUNT];

static uint8_t  g_probeIdx;    /* 0=idle, 1..5 = device being probed */
static bool     g_probeConvert;
static uint64_t g_convertDeadline;
static uint8_t  g_probeWriteBuf[2];
static uint8_t  g_probeReadBuf[6];

/* ---- Helpers --------------------------------------------------------- */
static i2cbus_token_t allocToken(void) {
	static uint32_t s = 0;
	uint32_t t = ++s;
	return t ? t : ++s;
}

static uint8_t addrToBit(uint8_t addr) {
	for (uint8_t i = 0; i < I2CBUS_ONBOARD_COUNT; i++)
		if (I2CBUS_ONBOARD_ADDRESSES[i] == addr) return (uint8_t)(1u << i);
	return 0;
}

static void fireCompletion(Pending *p, i2cbus_result_t r) {
	if (p->completed) return;
	p->completed = true;
	if (p->xfer.completion) p->xfer.completion(r, p->xfer.user);
}

static i2cbus_device_id_t idxToDevId(uint8_t idx, uint8_t who) {
	switch (idx) {
	case 0: return (who == 0x69) ? I2CBUS_DEV_LSM6DS33 : I2CBUS_DEV_LSM6DS3TRC;
	case 1: return I2CBUS_DEV_LIS3MDL;
	case 2: return I2CBUS_DEV_APDS9960;
	case 3: return I2CBUS_DEV_SHT31D;
	case 4: return I2CBUS_DEV_BMP280;
	default: return I2CBUS_DEV_UNKNOWN;
	}
}

/* ---- Probe FSM ------------------------------------------------------- */
static void probeStartNext(void);

static void probeCompletion(i2cbus_result_t result, void * /*user*/) {
	uint8_t idx = g_probeIdx - 1;
	if (result == I2CBUS_OK && idx < I2CBUS_ONBOARD_COUNT) {
		const IdEntry *e = &ID_TABLE[idx];
		if (e->is_sht) {
			if (i2cbus_sht31_crc(g_probeReadBuf, 2) == g_probeReadBuf[2] &&
			    i2cbus_sht31_crc(g_probeReadBuf + 3, 2) == g_probeReadBuf[5]) {
				g_presence |= addrToBit(e->addr);
				g_devIds[idx] = I2CBUS_DEV_SHT31D;
			}
		} else {
			uint8_t v = g_probeReadBuf[0];
			if (v == e->expect || v == e->alt) {
				g_presence |= addrToBit(e->addr);
				g_devIds[idx] = idxToDevId(idx, v);
			}
		}
	}
	g_probeIdx++;
	g_probeConvert = false;
	probeStartNext();
}

static void probeShtWriteDone(i2cbus_result_t result, void * /*user*/) {
	if (result != I2CBUS_OK) { probeCompletion(result, NULL); return; }
	g_probeConvert = true;
	g_convertDeadline = g_backend.now_us() + SHT31_CONVERT_US;
}

static void probeStartNext(void) {
	if (g_probeIdx == 0 || g_probeIdx > I2CBUS_ONBOARD_COUNT) {
		g_probeIdx = 0;
		return;
	}
	uint8_t idx = g_probeIdx - 1;
	const IdEntry *e = &ID_TABLE[idx];
	i2cbus_transfer_t xfer;
	memset(&xfer, 0, sizeof xfer);
	xfer.addr = e->addr;

	if (e->is_sht) {
		g_probeWriteBuf[0] = 0x24;
		g_probeWriteBuf[1] = 0x00;
		xfer.write_buf = g_probeWriteBuf;
		xfer.write_len = 2;
		xfer.completion = probeShtWriteDone;
	} else {
		g_probeWriteBuf[0] = e->id_reg;
		xfer.write_buf = g_probeWriteBuf;
		xfer.write_len = 1;
		xfer.read_buf = g_probeReadBuf;
		xfer.read_len = 1;
		xfer.completion = probeCompletion;
	}
	i2cbus_enqueue(&xfer);
}

/* ---- Recovery -------------------------------------------------------- */
static void startRecovery(void) {
	g_state = I2CBUS_STATE_RECOVERY;
	g_recPhase = 0;
	g_recClocks = 0;
}

/* Advance recovery one step. Returns true when finished (reinit done). */
static bool driveRecovery(void) {
	switch (g_recPhase) {
	case 0:
		if (g_backend.release_sda) g_backend.release_sda();
		g_recPhase = 1;
		g_recClocks = 0;
		return false;
	case 1:
		if (g_backend.toggle_scl) g_backend.toggle_scl();
		g_recClocks++;
		if (g_recClocks >= MAX_SCL_CLOCKS ||
		    (g_backend.read_sda && g_backend.read_sda()))
			g_recPhase = 2;
		return false;
	case 2:
		if (g_backend.gen_stop) g_backend.gen_stop();
		g_recPhase = 3;
		return false;
	case 3:
		if (g_backend.reinit_twim) g_backend.reinit_twim();
		g_recPhase = 4;
		g_state = I2CBUS_STATE_IDLE;
		i2cbus_probe_all();
		return true;
	default:
		g_state = I2CBUS_STATE_IDLE;
		return true;
	}
}

/* ---- SHT31 CRC-8 ----------------------------------------------------- */
uint8_t i2cbus_sht31_crc(const uint8_t *data, size_t len) {
	uint8_t crc = 0xFF;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
			                   : (uint8_t)(crc << 1);
	}
	return crc;
}

/* ---- Public API ------------------------------------------------------ */

void i2cbus_init(const i2cbus_backend_t *backend, uint32_t group_lease) {
	g_backend = *backend;
	g_groupLease = group_lease;
	g_ready = true;
	g_state = I2CBUS_STATE_IDLE;
	g_qHead = g_qTail = g_qCount = 0;
	g_hasActive = false;
	g_recPhase = 0;
	g_recClocks = 0;
	g_presence = 0;
	g_probeIdx = 0;
	g_probeConvert = false;
	memset(g_devIds, 0, sizeof g_devIds);
}

i2cbus_token_t i2cbus_enqueue(const i2cbus_transfer_t *xfer) {
	if (!g_ready) return I2CBUS_TOKEN_INVALID;
	if (g_qCount >= QUEUE_SIZE) return I2CBUS_TOKEN_INVALID;
	Pending *p = &g_queue[g_qTail];
	memset(p, 0, sizeof *p);
	p->xfer = *xfer;
	p->token = allocToken();
	g_qTail = (uint8_t)((g_qTail + 1) % QUEUE_SIZE);
	g_qCount++;
	return p->token;
}

int i2cbus_cancel(i2cbus_token_t token) {
	if (!g_ready || token == I2CBUS_TOKEN_INVALID) return -1;
	if (g_hasActive && g_active.token == token) {
		g_active.cancelled = true;
		return 0;
	}
	for (uint8_t i = 0; i < g_qCount; i++) {
		uint8_t idx = (uint8_t)((g_qHead + i) % QUEUE_SIZE);
		if (g_queue[idx].token == token) {
			g_queue[idx].cancelled = true;
			return 0;
		}
	}
	return -1;
}

void i2cbus_report_xfer_complete(i2cbus_result_t result) {
	if (!g_ready || !g_hasActive) return;
	g_hasActive = false;

	if (g_active.cancelled) {
		fireCompletion(&g_active, I2CBUS_ERR_CANCELLED);
		g_state = I2CBUS_STATE_IDLE;
	} else if (result == I2CBUS_OK) {
		fireCompletion(&g_active, I2CBUS_OK);
		g_state = I2CBUS_STATE_IDLE;
	} else if (result == I2CBUS_ERR_BUS) {
		fireCompletion(&g_active, result);
		startRecovery();
	} else {
		/* NACK: slave absent or busy — bus is fine, no recovery */
		fireCompletion(&g_active, result);
		g_state = I2CBUS_STATE_IDLE;
	}
}

void i2cbus_tick(void) {
	if (!g_ready) return;
	uint64_t now = g_backend.now_us ? g_backend.now_us() : 0;

	/* Probe conversion delay: bus is free, just waiting. */
	if (g_probeConvert && now >= g_convertDeadline) {
		g_probeConvert = false;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr = I2CBUS_ADDR_SHT31D;
		xfer.read_buf = g_probeReadBuf;
		xfer.read_len = 6;
		xfer.completion = probeCompletion;
		i2cbus_enqueue(&xfer);
	}

	/* Recovery FSM */
	if (g_state == I2CBUS_STATE_RECOVERY) {
		driveRecovery();
		return;
	}

	/* BUSY: check cancellation and timeout */
	if (g_state == I2CBUS_STATE_BUSY && g_hasActive) {
		if (g_active.cancelled) {
			g_hasActive = false;
			fireCompletion(&g_active, I2CBUS_ERR_CANCELLED);
			g_state = I2CBUS_STATE_IDLE;
		} else if (now - g_active.start_us >= TIMEOUT_US) {
			g_hasActive = false;
			fireCompletion(&g_active, I2CBUS_ERR_TIMEOUT);
			startRecovery();
		}
		return;
	}

	/* IDLE: serve queue */
	if (g_state == I2CBUS_STATE_IDLE) {
		while (g_qCount > 0) {
			Pending *p = &g_queue[g_qHead];
			g_qHead = (uint8_t)((g_qHead + 1) % QUEUE_SIZE);
			g_qCount--;
			if (p->cancelled) {
				fireCompletion(p, I2CBUS_ERR_CANCELLED);
				continue;
			}
			g_active = *p;
			g_hasActive = true;
			g_active.start_us = now;
			g_state = I2CBUS_STATE_BUSY;
			if (g_backend.start_xfer)
				g_backend.start_xfer(g_active.xfer.addr,
				                     g_active.xfer.write_buf,
				                     g_active.xfer.write_len,
				                     g_active.xfer.read_buf,
				                     g_active.xfer.read_len);
			break;
		}
	}
}

i2cbus_state_t i2cbus_get_state(void) { return g_state; }

void i2cbus_probe_all(void) {
	if (!g_ready) return;
	g_presence = 0;
	memset(g_devIds, 0, sizeof g_devIds);
	g_probeIdx = 1;
	g_probeConvert = false;
	probeStartNext();
}

bool i2cbus_probe_busy(void) {
	return g_probeIdx != 0;
}

void i2cbus_force_presence(uint8_t addr) {
	g_presence |= addrToBit(addr);
}

uint32_t i2cbus_get_presence(void) { return g_presence; }

bool i2cbus_is_present(uint8_t addr) {
	return (g_presence & addrToBit(addr)) != 0;
}

i2cbus_device_id_t i2cbus_get_device_id(uint8_t addr) {
	for (uint8_t i = 0; i < I2CBUS_ONBOARD_COUNT; i++)
		if (I2CBUS_ONBOARD_ADDRESSES[i] == addr) return g_devIds[i];
	return I2CBUS_DEV_UNKNOWN;
}

void i2cbus_shutdown(void) {
	g_ready = false;
	g_state = I2CBUS_STATE_UNINIT;
	g_hasActive = false;
	g_qHead = g_qTail = g_qCount = 0;
	g_probeIdx = 0;
	g_probeConvert = false;
	/* Caller releases the pinRegistry group lease — the manager does
	 * not call pinreg_release directly to stay decoupled from the
	 * registry on host test builds. */
	(void)g_groupLease;
}
