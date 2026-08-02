/*
 * i2cBus.h - asynchronous serialized TWIM1 bus manager for CLUE sensors.
 *
 * Manages all five onboard I2C sensors on a single shared TWIM1 bus at
 * 400 kHz. Transfers are serialized through a bounded FIFO: one bus
 * operation at a time, each with a 10 ms bus-operation timeout, an
 * ownership token for cancellation, and exactly-once completion.
 *
 * Sensor conversion delays are NOT bus timeouts. The bus is free during
 * conversion. Callers schedule their own delay between write and read.
 *
 * Bus recovery: if a transfer NACKs or errors, the manager releases SDA,
 * clocks SCL up to nine times via open-drain GPIO (checking SDA after
 * each clock), generates a valid STOP, reinitializes TWIM, and reprobes
 * affected devices. SDA/SCL are never driven push-pull during recovery.
 *
 * Device identification is non-destructive: WHO_AM_I/ID register reads
 * for LSM6DS33/LSM6DS3TR-C, LIS3MDL, APDS9960, BMP280. SHT31-D has no
 * ID register; its presence is confirmed by issuing the nonpersistent
 * single-shot measurement command and verifying CRC-valid response.
 *
 * Pure logic: the TWIM hardware is behind a pluggable backend struct
 * of function pointers. No SDK deps. Compiles on host for unit testing.
 *
 * TWIM1 group lease from pinRegistry (Todo 7) is acquired by the caller
 * before init and released on shutdown.
 */
#ifndef I2CBUS_H
#define I2CBUS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Frozen 7-bit onboard address table -------------------------------
 * Single source of truth for the five CLUE onboard sensors on TWIM1. */

#define I2CBUS_ADDR_LSM6DS33  0x6Au  /* LSM6DS33 or LSM6DS3TR-C */
#define I2CBUS_ADDR_LIS3MDL   0x1Cu
#define I2CBUS_ADDR_APDS9960  0x39u
#define I2CBUS_ADDR_SHT31D    0x44u
#define I2CBUS_ADDR_BMP280    0x77u

#define I2CBUS_ONBOARD_COUNT  5
extern const uint8_t I2CBUS_ONBOARD_ADDRESSES[I2CBUS_ONBOARD_COUNT];

#define I2CBUS_FREQUENCY_HZ   400000u
#define I2CBUS_IRQ_PRIORITY   6u

/* ---- Result codes ---------------------------------------------------- */

typedef enum {
	I2CBUS_OK              = 0,
	I2CBUS_ERR_NACK        = 1,  /* slave did not ACK address or data */
	I2CBUS_ERR_BUS         = 2,  /* TWIM bus error (OVERRUN, etc.)    */
	I2CBUS_ERR_TIMEOUT     = 3,  /* 10 ms bus-operation timeout       */
	I2CBUS_ERR_CANCELLED   = 4,  /* caller requested cancellation     */
	I2CBUS_ERR_QUEUE_FULL  = 5,  /* enqueue rejected, FIFO full       */
	I2CBUS_ERR_NO_DEVICE   = 6,  /* device not present (probe failed) */
	I2CBUS_ERR_RECOVERY    = 7,  /* recovery failed, bus still locked */
	I2CBUS_ERR_NOT_READY   = 8,  /* manager not initialized           */
} i2cbus_result_t;

/* ---- Manager state --------------------------------------------------- */

typedef enum {
	I2CBUS_STATE_UNINIT   = 0,
	I2CBUS_STATE_IDLE     = 1,
	I2CBUS_STATE_BUSY     = 2,  /* TWIM transfer in progress          */
	I2CBUS_STATE_CONVERT  = 3,  /* bus free, sensor converting         */
	I2CBUS_STATE_RECOVERY = 4,  /* bus recovery in progress            */
} i2cbus_state_t;

/* ---- Transfer descriptor --------------------------------------------- */

typedef void (*i2cbus_completion_fn)(i2cbus_result_t result, void *user);

typedef struct {
	uint8_t              addr;       /* 7-bit slave address             */
	const uint8_t       *write_buf;  /* TX (NULL if write_len=0)        */
	size_t               write_len;
	uint8_t             *read_buf;   /* RX (NULL if read_len=0)         */
	size_t               read_len;
	i2cbus_completion_fn completion; /* called exactly once on finish   */
	void                *user;       /* opaque context for callback     */
} i2cbus_transfer_t;

typedef uint32_t i2cbus_token_t;
#define I2CBUS_TOKEN_INVALID 0u

/* ---- Backend (hardware abstraction) ---------------------------------- */

/* Start a write-then-read TWIM transfer. Return 0 if started, !0 if the
 * hardware rejected (e.g. bus not idle). Completion MUST be reported
 * via i2cbus_report_xfer_complete() from the ISR or backend timer. */
typedef int  (*i2cbus_start_xfer_fn)(uint8_t addr,
                                     const uint8_t *write_buf, size_t write_len,
                                     uint8_t *read_buf, size_t read_len);

/* Recovery primitives: open-drain GPIO, never push-pull. */
typedef void (*i2cbus_release_sda_fn)(void);
typedef void (*i2cbus_toggle_scl_fn)(void);
typedef int  (*i2cbus_read_sda_fn)(void);    /* 0=low, 1=high */
typedef void (*i2cbus_gen_stop_fn)(void);
typedef void (*i2cbus_reinit_twim_fn)(void);

typedef struct {
	i2cbus_start_xfer_fn  start_xfer;
	i2cbus_release_sda_fn release_sda;
	i2cbus_toggle_scl_fn  toggle_scl;
	i2cbus_read_sda_fn    read_sda;
	i2cbus_gen_stop_fn    gen_stop;
	i2cbus_reinit_twim_fn reinit_twim;
	uint64_t            (*now_us)(void);
} i2cbus_backend_t;

/* ---- Public API ------------------------------------------------------ */

/* group_lease: PINREG_TOKEN_INVALID to skip lease tracking (tests),
 * or a real pinreg_acquire_group token from the caller. */
void            i2cbus_init(const i2cbus_backend_t *backend,
                            uint32_t group_lease);
i2cbus_token_t  i2cbus_enqueue(const i2cbus_transfer_t *xfer);
int             i2cbus_cancel(i2cbus_token_t token);
void            i2cbus_tick(void);
void            i2cbus_report_xfer_complete(i2cbus_result_t result);
i2cbus_state_t  i2cbus_get_state(void);

/* Probe / identify */
typedef enum {
	I2CBUS_DEV_UNKNOWN   = 0,
	I2CBUS_DEV_LSM6DS33  = 1,
	I2CBUS_DEV_LSM6DS3TRC = 2,
	I2CBUS_DEV_LIS3MDL   = 3,
	I2CBUS_DEV_APDS9960  = 4,
	I2CBUS_DEV_BMP280    = 5,
	I2CBUS_DEV_SHT31D    = 6,
} i2cbus_device_id_t;

/* bit 0=LSM, 1=LIS, 2=APDS, 3=SHT, 4=BMP */
#define I2CBUS_PRESENCE_LSM  (1u << 0)
#define I2CBUS_PRESENCE_LIS  (1u << 1)
#define I2CBUS_PRESENCE_APDS (1u << 2)
#define I2CBUS_PRESENCE_SHT  (1u << 3)
#define I2CBUS_PRESENCE_BMP  (1u << 4)
#define I2CBUS_PRESENCE_ALL  0x1Fu

void     i2cbus_probe_all(void);
uint32_t i2cbus_get_presence(void);
bool     i2cbus_is_present(uint8_t addr);
i2cbus_device_id_t i2cbus_get_device_id(uint8_t addr);

void     i2cbus_shutdown(void);

/* SHT31-D CRC-8 (polynomial 0x31, init 0xFF) — public for test access */
uint8_t  i2cbus_sht31_crc(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* I2CBUS_H */
