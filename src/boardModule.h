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
#include "storage/qspi.h"
#include "storage/qspi_journal.h"
#include "storage/calib.h"
#include "audio/pdm.h"
#include "sensors/sensor_drivers.h"
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
    void handleGpioConfigure(uint32_t requestId,
                             const board_GpioConfigureRequest &req);
    void handleGpioRead(uint32_t requestId,
                        const board_GpioReadRequest &req);
    void handleGpioWrite(uint32_t requestId,
                         const board_GpioWriteRequest &req);
    void handleAdcRead(uint32_t requestId,
                        const board_AdcReadRequest &req);

    /* === STORAGE HANDLERS (Todo 28) === */
    void handleStorageInfo(uint32_t requestId);
    void handleStorageAdopt(uint32_t requestId,
                            const board_StorageAdoptRequest &req);
    void handleStorageReadLog(uint32_t requestId,
                              const board_StorageReadLogRequest &req);
    void handleStorageEraseLog(uint32_t requestId,
                               const board_StorageEraseLogRequest &req);

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

    /* === STORAGE SUBSYSTEM (Todo 28) ===
     * QSPI NOR manager + journal + calibration/runtime persistence.
     * Construction is unconditional; init() is conditional on a
     * PINREG_GROUP_QSPI lease (granted by main.cpp's pinreg_init). */
    QspiManager   m_qspi;
    QspiJournal   m_journal;
    CalibManager  m_calib;

    /* === PDM MICROPHONE (Todo 9 — RawPcmDiagnostics capture) ===
     * Owns the nrfx_pdm driver + metric window + PCM capture ring.
     * init() is called from the BoardModule constructor; DMA sampling
     * is started on first PCM request (or audio enable) and stopped
     * when both go inactive. */
    PdmMicrophone m_pdm;

    /* === I2C SENSOR DRIVERS (T20/T21) ===
     * Aggregates the 5 onboard sensor wrappers (IMU, mag, BMP280,
     * SHT31D, APDS9960). begin() is called from the BoardModule
     * constructor after the I2C bus was initialized in main.cpp;
     * tick() is driven from BoardModule::tick() each iteration. The
     * motion-sensor wrappers (IMU/mag/APDS-gesture) feed parsed
     * samples into BoardModule::inject* which forward to
     * MotionManager; the env/color/proximity wrappers cache their
     * latest sample for handleReadSensor. */
    SensorDrivers m_sensors;
#endif

    /* === AUDIO STATE (Todo 26) === */
    uint8_t m_audioGainReg;
    bool    m_audioEnabled;
    uint32_t m_rawPcmRequestId;
    uint32_t m_rawPcmSequence;

    /* === INPUT MODE STATE (D9 — ConfigureInput handler) ===
     * Cached ConfigureInput parameters. m_inputMode gates APDS dispatch. */
    board_InputMode m_inputMode;
    uint32_t m_inputFlags;
    uint32_t m_inputDwellMs;
    uint32_t m_inputDeadzone;

    /* === EVENT EMISSION STATE (D4/D5) ===
     * Monotonic sequence counters for unsolicited InputEvent/GestureEvent
     * and last-known button states for edge detection. */
    uint32_t m_inputEventSeq;
    uint32_t m_gestureEventSeq;
    bool     m_lastBtnA;
    bool     m_lastBtnB;
    uint32_t m_btnPollNextMs;

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

    /* Serialize the just-completed calibration for the given target and
     * persist it via CalibManager. No-op when storage is not adopted —
     * CalibManager handles that gracefully. */
    void persistCalibrationResult(MotionManager::CalibTarget target);
#endif

    /* Send a CommandResult response with the given code. */
    void sendCommandResult(uint32_t requestId,
                           board_BoardCommand command,
                           board_BoardResultCode result);

    /* Send an unsolicited BoardStatus event (request_id=0). Used to
     * surface progress/fault/runtime-switching notifications from the
     * motion and storage subsystems. Supplements CommandResult, which
     * remains the path for synchronous request/response. */
    void sendBoardStatus(board_BoardStatusCode code,
                         board_BoardResultCode result = board_BoardResultCode_SUCCESS,
                         board_ResourceKind resource = board_ResourceKind_RESOURCE_UNKNOWN,
                         uint32_t instance = 0,
                         uint32_t progress_per_mille = 0,
                         bool terminal = false,
                         const char *detail = nullptr);

    /* Send an unsolicited InputEvent for a button transition. */
    void sendInputEvent(board_InputSource source,
                        board_InputAction action,
                        int32_t value = 0);

    /* Send an unsolicited GestureEvent for a decoded APDS gesture. */
    void sendGestureEvent(board_Gesture gesture);
};

#endif /* __cplusplus */

#endif /* BOARD_MODULE_H */
