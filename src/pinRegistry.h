/*
 * pinRegistry.h - runtime pin/group ownership registry for CLUE dual-runtime.
 *
 * Tracks individual GPIO pins and atomic multi-pin groups. Leases carry
 * an opaque generation-tagged token preventing stale/cross-session
 * releases. Force=true requests async quiescence via the current owner's
 * quiesce handler; the lease is granted only after completion + restore.
 *
 * Pure logic: no SDK deps. Caller provides restore callbacks and
 * critical-section hooks at init. Compiles on host for unit testing.
 *
 * Resource IDs: 0..47 = pins (P0.0-P0.31=0-31, P1.0-P1.15=32-47),
 *               48..53 = group pseudo-resources.
 * Token = (generation << 8) | (resourceId + 1). Token 0 = invalid.
 */
#ifndef PIN_REGISTRY_H
#define PIN_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PINREG_MAX_PINS_PER_GROUP 6
#define PINREG_PIN_COUNT          48
#define PINREG_GROUP_BASE         PINREG_PIN_COUNT

typedef uint32_t pinreg_token_t;
#define PINREG_TOKEN_INVALID 0u

typedef enum {
    PINREG_OWNER_NONE = 0, PINREG_OWNER_RAW_RADIO, PINREG_OWNER_BLE_HID,
    PINREG_OWNER_DISPLAY, PINREG_OWNER_SENSOR_BUS, PINREG_OWNER_AUDIO,
    PINREG_OWNER_NEOPIXEL, PINREG_OWNER_QSPI, PINREG_OWNER_GPIO_USER,
    PINREG_OWNER_COUNT
} pinreg_owner_t;

typedef enum {
    PINREG_GROUP_NONE = 0,
    PINREG_GROUP_DISPLAY_SPI = 1, PINREG_GROUP_TWIM1, PINREG_GROUP_QSPI,
    PINREG_GROUP_PDM, PINREG_GROUP_NEOPIXEL_PWM, PINREG_GROUP_BUZZER_PWM,
    PINREG_GROUP_NUM_VALID
} pinreg_group_t;

typedef enum {
    PINREG_OK = 0, PINREG_BUSY, PINREG_RESERVED, PINREG_INVALID_TOKEN,
    PINREG_QUIESCING, PINREG_NOT_CANCELLABLE, PINREG_INVALID_PARAM
} pinreg_result_t;

typedef enum {
    PINREG_STATE_FREE = 0, PINREG_STATE_OWNED, PINREG_STATE_RESERVED,
    PINREG_STATE_QUIESCING
} pinreg_state_t;

typedef enum {
    PINREG_EV_DISPLACED = 0, PINREG_EV_RESTORED, PINREG_EV_GRANTED
} pinreg_event_t;

typedef void (*pinreg_restore_fn)(pinreg_group_t group, uint8_t pin);
typedef bool  (*pinreg_quiesce_fn)(pinreg_group_t group);
typedef void  (*pinreg_event_fn)(pinreg_group_t group, pinreg_event_t evt,
                                 pinreg_owner_t prevOwner, pinreg_token_t newToken);
typedef void  (*pinreg_critical_fn)(void);

void pinreg_init(pinreg_critical_fn enter, pinreg_critical_fn exit);
void pinreg_reserve(uint8_t resourceId);
void pinreg_group_set_pins(pinreg_group_t group, const uint8_t *pins, uint8_t count);
void pinreg_set_event_sink(pinreg_event_fn fn);
void pinreg_set_quiesce_handler(pinreg_owner_t owner, pinreg_group_t group,
                                pinreg_quiesce_fn fn);

pinreg_result_t pinreg_acquire_pin(uint8_t pin, pinreg_owner_t owner,
                                   pinreg_restore_fn restore, pinreg_token_t *outToken);
pinreg_result_t pinreg_acquire_group(pinreg_group_t group, pinreg_owner_t owner,
                                     pinreg_restore_fn restore, pinreg_token_t *outToken);
pinreg_result_t pinreg_force_group(pinreg_group_t group, pinreg_owner_t owner,
                                   pinreg_restore_fn restore, pinreg_token_t *outToken);
pinreg_result_t pinreg_release(pinreg_token_t token);
pinreg_result_t pinreg_retain(pinreg_token_t token);
pinreg_result_t pinreg_quiesce_complete(pinreg_group_t group);
pinreg_result_t pinreg_query(uint8_t resourceId, pinreg_owner_t *outOwner);
void pinreg_reset_all(void);

typedef struct {
    pinreg_state_t state; pinreg_owner_t owner;
    uint16_t generation; uint8_t refcount; uint8_t memberCount;
} pinreg_info_t;
void pinreg_inspect(uint8_t resourceId, pinreg_info_t *out);

#ifdef __cplusplus
}
#endif
#endif /* PIN_REGISTRY_H */
