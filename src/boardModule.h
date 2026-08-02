/*
 * boardModule.h - Board domain dispatch for CLUE.
 *
 * Pure-C evaluation API lives in boardModuleC.h (safe for host tests).
 * The BoardModule C++ class wraps the eval functions with firmware I/O
 * (message pool, Core TX queue).
 */
#ifndef BOARD_MODULE_H
#define BOARD_MODULE_H

#include "boardModuleC.h"
#include "boardMotionEval.h"

#ifdef __cplusplus

/* ---- BoardModule class (firmware dispatch handler) ------------------- */

#include <whad.h>

#ifdef BOARD_CLUE
#include "motion/motion_manager.h"
#include "ble/profiles_eval.h"
#endif

class Core;
class ProfileManager;

class BoardModule {
public:
    explicit BoardModule(Core *core);

    /* Entry point called from Core::processInputMessage. */
    void processMessage(whad::board::BoardMsg &msg);

    /* === MOTION STREAM (Todo 19) ===
     * Called by Core::loop() to emit SensorSample events for
     * active streams. Uses request_id=0 (unsolicited event). */
    void emitStreamSamples(void);

    /* === MOTION TICK (Todo 9) ===
     * Called by Core::loop() each iteration. Drives the MotionManager
     * (fusion / air-mouse / tilt / rotation gesture). Emits BoardStatus
     * events for rotation-gesture state changes. */
    void tick(void);

    /* === MOTION DATA INJECTION (Todo 9) ===
     * Called by i2cBus completion callbacks / GPIO ISRs to feed real
     * sensor data into the motion subsystem. */
    void injectImu(const imu_sample_t *imu, uint64_t now_us);
    void injectMag(const mag_sample_t *mag, bool healthy, uint64_t now_us);
    void injectApdsGesture(apds9960_gesture_t g);
    void injectButtons(bool button_a, bool button_b);

    /* Wire ProfileManager (set by BleRuntime once instantiated). */
    void setProfileManager(ProfileManager *profiles);

#ifdef BOARD_CLUE
    /* Direct access for tests and the runtime-switch callback. */
    MotionManager &motion() { return m_motion; }
#endif

private:
    Core *m_core;

    /* Individual command handlers. Each parses the request, builds
     * the response, and pushes it to Core's TX queue. */
    void handleGetBoardInfo(uint32_t requestId);
    void handleGetRuntimeConfig(uint32_t requestId);
    void handleSetRuntimeMode(uint32_t requestId,
                              const board_SetRuntimeModeRequest &req);
    void handleSetRuntimeConfig(uint32_t requestId,
                                const board_SetRuntimeConfigRequest &req);

    /* === INPUT/PROFILE HANDLERS (Todo 23) === */
    void handleGetInputState(uint32_t requestId);
    void handleConfigureInput(uint32_t requestId,
                              const board_ConfigureInputRequest &req);
    void handleRemoteProfileGet(uint32_t requestId,
                                const board_RemoteProfileGetRequest &req);
    void handleRemoteProfileSet(uint32_t requestId,
                                const board_RemoteProfileSetRequest &req);

    /* === AUDIO HANDLERS (Todo 26) === */
    void handleAudioConfigure(uint32_t requestId,
                              const board_AudioConfigureRequest &req);
    void handleRawPcmDiagnostics(uint32_t requestId,
                                 const board_RawPcmDiagnosticsRequest &req);

    /* === OUTPUT HANDLERS (Todo 27) === */
    void handleSetOutput(uint32_t requestId,
                         const board_SetOutputRequest &req);

    /* === EXPERT I/O HANDLERS (Todo 32) === */
    void handleI2cTransfer(uint32_t requestId,
                           const board_I2cTransferRequest &req);
    void handleSpiTransfer(uint32_t requestId,
                           const board_SpiTransferRequest &req);
    void handleReleasePin(uint32_t requestId,
                          const board_ReleasePinRequest &req);

    /* === MOTION HANDLERS (Todo 19) === */
    void handleListSensors(uint32_t requestId,
                           const board_ListSensorsRequest &req);
    void handleReadSensor(uint32_t requestId,
                          const board_ReadSensorRequest &req);
    void handleConfigureStream(uint32_t requestId,
                               const board_ConfigureStreamRequest &req);
    void handleStopStream(uint32_t requestId,
                          const board_StopStreamRequest &req);
    void handleCalibrate(uint32_t requestId,
                         const board_CalibrateRequest &req);
    void handleGetCalibration(uint32_t requestId,
                              const board_GetCalibrationRequest &req);

    /* === MOTION STATE (Todo 19) === */
    board_motion_stream_state_t m_streamState;
    board_motion_calib_state_t  m_calibState;

#ifdef BOARD_CLUE
    /* === MOTION SUBSYSTEM (Todo 9) === */
    MotionManager m_motion;
    ProfileManager *m_profiles;
    bool m_dashboardRegistered;
    uint32_t m_streamSeq;
    uint32_t m_streamOverflow;
    uint32_t m_lastStreamEmitMs[BOARD_MOTION_MAX_STREAMS];

    /* Async calibration driver state */
    uint32_t m_calibRequestId;
    uint32_t m_calibSensorId;
    uint32_t m_calibEmitMs;
#endif

    /* === AUDIO STATE (Todo 26) === */
    uint8_t m_audioGainReg;
    bool    m_audioEnabled;
    uint32_t m_rawPcmRequestId;

    /* Helper: populate a board_SensorDescriptor from eval table. */
    void populateDescriptor(board_SensorDescriptor *out,
                            const board_motion_sensor_info_t *info);

    /* Helper: emit a SensorSample message with request_id=0. */
    void emitSensorSample(uint32_t sensor_id, uint32_t sequence,
                          uint64_t timestamp_us, uint32_t status,
                          const int32_t *values, uint32_t value_count);

#ifdef BOARD_CLUE
    /* Register dashboard pages for runtime-mode display (Todo 9, item j).
     * Creates one page per active runtime mode showing sensor readouts.
     * Idempotent — guarded by m_dashboardRegistered. */
    void registerDashboardPages(void);

    /* Push the rotation gesture state to the display (called on event). */
    void publishRotationState(rotg_event_t evt);

    /* Dispatch a decoded APDS gesture to the ProfileManager (if wired). */
    void dispatchApdsToProfiles(apds9960_gesture_t g);
#endif

    /* Send a CommandResult response with the given code. */
    void sendCommandResult(uint32_t requestId,
                           board_BoardCommand command,
                           board_BoardResultCode result);
};

#endif /* __cplusplus */

#endif /* BOARD_MODULE_H */
