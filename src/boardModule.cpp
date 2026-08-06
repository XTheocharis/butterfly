/*
 * boardModule.cpp - Board domain dispatch handler for CLUE.
 *
 * BoardModule processes GetBoardInfo, GetRuntimeConfig, SetRuntimeMode,
 * and SetRuntimeConfig. Unimplemented commands return
 * BoardResultCode.NOT_IMPLEMENTED (not advertised).
 *
 * Evaluation functions live in boardModuleEval.c (pure C logic,
 * no SDK deps) for host testability.
 */
#include <string.h>
#include <stdio.h>

#include "core.h"
#include "boardModule.h"
#include "capabilities.h"
#include "messagePool.h"
#include "version.h"
#include "nrf.h"
#include "timebase.h"
#include "i2cBus.h"
#include "i2c_twim_backend.h"

#ifdef BOARD_CLUE
#include "ble/profiles.h"
#include "ble/ble_runtime.h"
#include "ble/advertising.h"
#include "ble/bond.h"
#include "ble/profiles_eval.c"
#include "storage/calib.h"
#include "storage/qspi.h"
#include "storage/qspi_journal.h"
#include "output/buzzer.h"
#include "expert/i2c.h"
#include "expert/spi.h"
#include "expert/gpio.h"
#include "expert/adc.h"
#include "sensors/apds9960.h"
#endif

/* Pull in the pure-logic eval functions. */
#include "boardModuleEval.c"
#include "boardMotionEval.c"
#include "audio/pdm_eval.c"
#include "output/buzzer_eval.c"
#include "expert/io_eval.c"
#include "expert/expert_force.c"
#ifdef BOARD_CLUE
#include "motion/rotation_gesture_eval.c"
#endif

#ifdef BOARD_CLUE
/* File-scope buzzer instance shared between handleSetOutput
 * (startTone/stopTone) and BoardModule::tick (tick → auto-stop). */
static Buzzer s_buzzer;
#endif

/* ---- BoardModule class ------------------------------------------------ */

BoardModule::BoardModule(Core *core)
	: m_core(core)
{
	board_motion_stream_init(&m_streamState);
	board_motion_calib_init(&m_calibState);
	m_audioGainReg = PDM_GAIN_DEFAULT;
	m_audioEnabled = false;
	m_rawPcmRequestId = 0;
	m_inputMode = board_InputMode_INPUT_MODE_REMOTE;
	m_inputFlags = 0;
	m_inputDwellMs = 0;
	m_inputDeadzone = 0;
	m_inputEventSeq = 0;
	m_gestureEventSeq = 0;
	m_lastBtnA = false;
	m_lastBtnB = false;
	m_btnPollNextMs = 0;
	m_tickStartMs = 0;
#ifdef BOARD_CLUE
	m_profiles = nullptr;
	m_dashboardRegistered = false;
	m_streamSeq = 0;
	m_streamOverflow = 0;
	memset(m_lastStreamEmitMs, 0, sizeof(m_lastStreamEmitMs));
	m_calibRequestId = 0;
	m_calibSensorId = 0;
	m_calibEmitMs = 0;
	m_motion.init();

	/* Wire the rotation gesture confirm callback to a runtime switch.
	 * The target is the opposite of the currently-selected runtime
	 * (toggle behavior). */
	auto switch_cb = [](void *user) {
		(void)user;
		runtime_mode_t cur = runtime_get_selected();
		runtime_mode_t target = (cur == RUNTIME_RAW_WHAD)
		                      ? RUNTIME_BLE_HID : RUNTIME_RAW_WHAD;
		(void)runtime_request_switch(target);
	};
	m_motion.setRotationSwitchCallback(switch_cb, nullptr);

	/* QSPI, PDM, and sensor init moved to init() — see comment above. */

	/* === I2C SENSOR DRIVERS (T21) ===
	 * main.cpp already initialized the TWIM1 backend + i2cBus and
	 * probed all 5 onboard sensors before constructing Core. Wire
	 * the inject callbacks (IMU/mag/APDS-gesture → MotionManager)
	 * then begin() each present sensor's async config FSM. The
	 * pin-group lease was acquired by main.cpp and is stored inside
	 * i2cbus_init; SensorDrivers::init does not re-acquire it. */
	m_sensors.setListener(this);
#endif
}

void BoardModule::initHardware()
{
#ifdef BOARD_CLUE
	pinreg_token_t qspi_lease = PINREG_TOKEN_INVALID;
	if (pinreg_acquire_group(PINREG_GROUP_QSPI, PINREG_OWNER_QSPI,
	                         nullptr, &qspi_lease) == PINREG_OK) {
		if (m_qspi.init(qspi_lease) && m_qspi.isAdopted()) {
			if (m_journal.init(qspi_lease)) {
				m_calib.init(&m_journal);
				calib_set_global_manager(&m_calib);
			}
		}
	}
	(void)m_pdm.init();

	pinreg_token_t i2c_lease = PINREG_TOKEN_INVALID;
	(void)pinreg_acquire_group(PINREG_GROUP_TWIM1,
	                            PINREG_OWNER_SENSOR_BUS, NULL, &i2c_lease);
	const i2cbus_backend_t *i2c_be = i2c_twim_backend_get();
	if (i2c_be != NULL) {
		i2cbus_init(i2c_be, (uint32_t)i2c_lease);
		i2cbus_probe_all();
	}
	(void)m_sensors.init(timebase_now_us(), 0);
#endif
}

void BoardModule::processMessage(whad::board::BoardMsg &boardMsg)
{
	Message *rawMsg = boardMsg.getRaw();

	switch (boardMsg.getType()) {
	case whad::board::GetBoardInfoMsg: {
		uint32_t requestId = 0;
		board_GetBoardInfoRequest req = {};
		whad_board_get_board_info_parse(rawMsg, &requestId, &req);
		handleGetBoardInfo(requestId);
		break;
	}
	case whad::board::GetRuntimeConfigMsg: {
		uint32_t requestId = 0;
		board_GetRuntimeConfigRequest req = {};
		whad_board_get_runtime_config_parse(rawMsg, &requestId, &req);
		handleGetRuntimeConfig(requestId);
		break;
	}
	case whad::board::SetRuntimeModeMsg: {
		uint32_t requestId = 0;
		board_SetRuntimeModeRequest req = {};
		whad_board_set_runtime_mode_parse(rawMsg, &requestId, &req);
		handleSetRuntimeMode(requestId, req);
		break;
	}
	case whad::board::SetRuntimeConfigMsg: {
		uint32_t requestId = 0;
		board_SetRuntimeConfigRequest req = {};
		whad_board_set_runtime_config_parse(rawMsg, &requestId, &req);
		handleSetRuntimeConfig(requestId, req);
		break;
	}
	/* === MOTION HANDLERS (Todo 19) === */
	case whad::board::ListSensorsMsg: {
		uint32_t requestId = 0;
		board_ListSensorsRequest req = {};
		whad_board_list_sensors_parse(rawMsg, &requestId, &req);
		handleListSensors(requestId, req);
		break;
	}
	case whad::board::ReadSensorMsg: {
		uint32_t requestId = 0;
		board_ReadSensorRequest req = {};
		whad_board_read_sensor_parse(rawMsg, &requestId, &req);
		handleReadSensor(requestId, req);
		break;
	}
	case whad::board::ConfigureStreamMsg: {
		uint32_t requestId = 0;
		board_ConfigureStreamRequest req = {};
		whad_board_configure_stream_parse(rawMsg, &requestId, &req);
		handleConfigureStream(requestId, req);
		break;
	}
	case whad::board::StopStreamMsg: {
		uint32_t requestId = 0;
		board_StopStreamRequest req = {};
		whad_board_stop_stream_parse(rawMsg, &requestId, &req);
		handleStopStream(requestId, req);
		break;
	}
	case whad::board::CalibrateMsg: {
		uint32_t requestId = 0;
		board_CalibrateRequest req = {};
		whad_board_calibrate_parse(rawMsg, &requestId, &req);
		handleCalibrate(requestId, req);
		break;
	}
	case whad::board::GetCalibrationMsg: {
		uint32_t requestId = 0;
		board_GetCalibrationRequest req = {};
		whad_board_get_calibration_parse(rawMsg, &requestId, &req);
		handleGetCalibration(requestId, req);
		break;
	}
	/* === END MOTION HANDLERS (Todo 19) === */

	/* === INPUT/PROFILE HANDLERS (Todo 23) === */
	case whad::board::GetInputStateMsg: {
		uint32_t requestId = 0;
		board_GetInputStateRequest req = {};
		whad_board_get_input_state_parse(rawMsg, &requestId, &req);
		handleGetInputState(requestId);
		break;
	}
	case whad::board::ConfigureInputMsg: {
		uint32_t requestId = 0;
		board_ConfigureInputRequest req = {};
		whad_board_configure_input_parse(rawMsg, &requestId, &req);
		handleConfigureInput(requestId, req);
		break;
	}
	case whad::board::RemoteProfileGetMsg: {
		uint32_t requestId = 0;
		board_RemoteProfileGetRequest req = {};
		whad_board_remote_profile_get_parse(rawMsg, &requestId, &req);
		handleRemoteProfileGet(requestId, req);
		break;
	}
	case whad::board::RemoteProfileSetMsg: {
		uint32_t requestId = 0;
		board_RemoteProfileSetRequest req = {};
		whad_board_remote_profile_set_parse(rawMsg, &requestId, &req);
		handleRemoteProfileSet(requestId, req);
		break;
	}
	/* === END INPUT/PROFILE HANDLERS (Todo 23) === */

	/* === AUDIO HANDLERS (Todo 26) === */
	case whad::board::AudioConfigureMsg: {
		uint32_t requestId = 0;
		board_AudioConfigureRequest req = {};
		whad_board_audio_configure_parse(rawMsg, &requestId, &req);
		handleAudioConfigure(requestId, req);
		break;
	}
	case whad::board::RawPcmDiagnosticsMsg: {
		uint32_t requestId = 0;
		board_RawPcmDiagnosticsRequest req = {};
		whad_board_raw_pcm_diagnostics_parse(rawMsg, &requestId, &req);
		handleRawPcmDiagnostics(requestId, req);
		break;
	}
	/* === END AUDIO HANDLERS (Todo 26) === */

	/* === OUTPUT HANDLERS (Todo 27) === */
	case whad::board::SetOutputMsg: {
		uint32_t requestId = 0;
		board_SetOutputRequest req = {};
		whad_board_set_output_parse(rawMsg, &requestId, &req);
		handleSetOutput(requestId, req);
		break;
	}
	/* === END OUTPUT HANDLERS (Todo 27) === */

	/* === EXPERT I/O HANDLERS (Todo 32) === */
	case whad::board::GpioConfigureMsg: {
		uint32_t requestId = 0;
		board_GpioConfigureRequest req = {};
		whad_board_gpio_configure_parse(rawMsg, &requestId, &req);
		handleGpioConfigure(requestId, req);
		break;
	}
	case whad::board::GpioReadMsg: {
		uint32_t requestId = 0;
		board_GpioReadRequest req = {};
		whad_board_gpio_read_parse(rawMsg, &requestId, &req);
		handleGpioRead(requestId, req);
		break;
	}
	case whad::board::GpioWriteMsg: {
		uint32_t requestId = 0;
		board_GpioWriteRequest req = {};
		whad_board_gpio_write_parse(rawMsg, &requestId, &req);
		handleGpioWrite(requestId, req);
		break;
	}
	case whad::board::I2cTransferMsg: {
		uint32_t requestId = 0;
		board_I2cTransferRequest req = {};
		whad_board_i2c_transfer_parse(rawMsg, &requestId, &req);
		handleI2cTransfer(requestId, req);
		break;
	}
	case whad::board::SpiTransferMsg: {
		uint32_t requestId = 0;
		board_SpiTransferRequest req = {};
		whad_board_spi_transfer_parse(rawMsg, &requestId, &req);
		handleSpiTransfer(requestId, req);
		break;
	}
	case whad::board::ReleasePinMsg: {
		uint32_t requestId = 0;
		board_ReleasePinRequest req = {};
		whad_board_release_pin_parse(rawMsg, &requestId, &req);
		handleReleasePin(requestId, req);
		break;
	}
	case whad::board::AdcReadMsg: {
		uint32_t requestId = 0;
		board_AdcReadRequest req = {};
		whad_board_adc_read_parse(rawMsg, &requestId, &req);
		handleAdcRead(requestId, req);
		break;
	}
	/* === STORAGE HANDLERS (Todo 28) === */
	case whad::board::StorageInfoMsg: {
		uint32_t requestId = 0;
		board_StorageInfoRequest req = {};
		whad_board_storage_info_parse(rawMsg, &requestId, &req);
		handleStorageInfo(requestId);
		break;
	}
	case whad::board::StorageAdoptMsg: {
		uint32_t requestId = 0;
		board_StorageAdoptRequest req = {};
		whad_board_storage_adopt_parse(rawMsg, &requestId, &req);
		handleStorageAdopt(requestId, req);
		break;
	}
	case whad::board::StorageReadLogMsg: {
		uint32_t requestId = 0;
		board_StorageReadLogRequest req = {};
		whad_board_storage_read_log_parse(rawMsg, &requestId, &req);
		handleStorageReadLog(requestId, req);
		break;
	}
	case whad::board::StorageEraseLogMsg: {
		uint32_t requestId = 0;
		board_StorageEraseLogRequest req = {};
		whad_board_storage_erase_log_parse(rawMsg, &requestId, &req);
		handleStorageEraseLog(requestId, req);
		break;
	}
	/* === END STORAGE HANDLERS (Todo 28) === */
	/* === END EXPERT I/O HANDLERS (Todo 32) === */
	default: {
		uint32_t requestId = rawMsg->msg.board.request_id;
		board_BoardCommand cmd = board_BoardCommand_GetBoardInfo;
		(void)whad_board_command_from_message_type(
			(whad_board_msgtype_t)boardMsg.getType(), &cmd);

		sendCommandResult(requestId,
			cmd,
			board_BoardResultCode_NOT_IMPLEMENTED);
		break;
	}
	}
}

void BoardModule::handleGetBoardInfo(uint32_t requestId)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_GetBoardInfoResponse info;
	memset(&info, 0, sizeof(info));

	strncpy(info.board_name, "Adafruit CLUE",
		sizeof(info.board_name) - 1);
	strncpy(info.hardware_revision, "nRF52840",
		sizeof(info.hardware_revision) - 1);
	snprintf(info.firmware_version, sizeof(info.firmware_version),
		"%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION);
	strncpy(info.protocol_variant, "WHAD",
		sizeof(info.protocol_variant) - 1);

	memcpy(info.device_id.bytes,
		(const void *)NRF_FICR->DEVICEID, 8);
	memcpy(info.device_id.bytes + 8,
		(const void *)NRF_FICR->DEVICEADDR, 8);
	info.device_id.size = 16;

	info.active_runtime = static_cast<board_RuntimeMode>(
		boardmodule_rt_to_proto(runtime_get_selected()));
	{
		uint32_t sensor_count = 0;
		(void)board_motion_get_sensor_table(&sensor_count);
		info.implemented_sensor_count = sensor_count;
	}

	whad_board_board_info(resp, requestId, &info);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleGetRuntimeConfig(uint32_t requestId)
{
	board_runtime_state_t state = {};

	state.active_runtime = boardmodule_rt_to_proto(runtime_get_selected());

#ifdef BOARD_CLUE
	/* Persisted runtime mode — backed by QSPI calib store (CLUE only).
	 * Returns RUNTIME_STORE_UNAVAILABLE when storage is not adopted. */
	runtime_mode_t persisted = RUNTIME_RAW_WHAD;
	runtime_store_result_t sr = calib_runtime_store_read(&persisted);
	state.persistence_available = (sr == RUNTIME_STORE_OK);
	state.persisted_runtime = state.persistence_available
		? boardmodule_rt_to_proto(persisted)
		: BOARD_RT_UNKNOWN;

	/* BLE-HID state — meaningful only when BleRuntime is constructed
	 * (RUNTIME_BLE_HID mode). In raw-WHAD mode the pointer is null. */
	BleRuntime *ble = m_core->getBleRuntime();
	if (ble != nullptr) {
		BleRuntimeState bstate = ble->getState();
		state.ble_advertising = (bstate == BleRuntimeState::Advertising);
		state.ble_connected  = (bstate == BleRuntimeState::Connected);
		state.ble_pairable   = (ble->getSecurity() != nullptr);
		/* bond_count stays 0: BondStorage exposes no count accessor yet. */
	}
#endif

	board_RuntimeConfigResponse cfg = {};
	boardmodule_eval_get_runtime_config(&state, &cfg);

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		whad_board_runtime_config(resp, requestId, &cfg);
		m_core->pushMessageToQueue(resp);
	}
}

void BoardModule::handleSetRuntimeMode(uint32_t requestId,
	const board_SetRuntimeModeRequest &req)
{
	uint32_t code = boardmodule_eval_set_runtime_mode(
		req.runtime, req.persist);

	if (code == board_BoardResultCode_SUCCESS && req.reboot) {
		bool valid = false;
		runtime_mode_t target =
			boardmodule_proto_to_rt(req.runtime, &valid);
		if (valid) {
			runtime_request_switch(target);
		}
	}

	sendCommandResult(requestId,
		board_BoardCommand_SetRuntimeMode,
		(board_BoardResultCode)code);
}

void BoardModule::handleSetRuntimeConfig(uint32_t requestId,
	const board_SetRuntimeConfigRequest &req)
{
	bool hasPersistedRuntime = false;

	if (req.which_operation == BOARD_CFG_OP_UPDATE) {
		hasPersistedRuntime = req.operation.update.has_persisted_runtime;
	}

#ifdef BOARD_CLUE
	BleRuntime *ble = m_core->getBleRuntime();
	bool ble_active = (ble != nullptr);

	uint32_t code = boardmodule_eval_set_runtime_config(
		req.which_operation, hasPersistedRuntime, ble_active);

	if (code == board_BoardResultCode_SUCCESS && ble_active) {
		if (req.which_operation == BOARD_CFG_OP_OPEN_PAIRING) {
			AdvertisingManager *adv = ble->getAdvertising();
			if (adv != nullptr) {
				(void)adv->start();
			}
		} else if (req.which_operation == BOARD_CFG_OP_CLEAR_BONDS) {
			BondStorage *bond = ble->getBond();
			if (bond != nullptr) {
				bond->requestForget();
				(void)bond->confirmForget();
			}
		}
	}
#else
	uint32_t code = boardmodule_eval_set_runtime_config(
		req.which_operation, hasPersistedRuntime, false);
#endif

	sendCommandResult(requestId,
		board_BoardCommand_SetRuntimeConfig,
		(board_BoardResultCode)code);
}

/* === MOTION HANDLER IMPLEMENTATIONS (Todo 19) === */

void BoardModule::populateDescriptor(board_SensorDescriptor *out,
                                     const board_motion_sensor_info_t *info)
{
	memset(out, 0, sizeof(*out));
	out->sensor_id = info->sensor_id;
	strncpy(out->name, info->name, sizeof(out->name) - 1);
	strncpy(out->unit, info->unit, sizeof(out->unit) - 1);
	out->value_count = info->value_count;
	out->default_rate_millihz = info->default_rate_millihz;
	out->min_rate_millihz = BOARD_MOTION_RATE_MIN_MHZ;
	out->max_rate_millihz = info->max_rate_millihz;

	uint32_t copy = info->supported_rate_count;
	if (copy > 8) {
		copy = 8;
	}
	out->supported_rates_millihz_count = (pb_size_t)copy;
	for (uint32_t i = 0; i < copy; i++) {
		out->supported_rates_millihz[i] = info->supported_rates[i];
	}

	uint32_t comp = info->value_count;
	if (comp > 10) {
		comp = 10;
	}
	out->component_names_count = (pb_size_t)comp;
	for (uint32_t i = 0; i < comp; i++) {
		strncpy(out->component_names[i], info->component_names[i], 15);
	}
}

void BoardModule::handleListSensors(uint32_t requestId,
                                    const board_ListSensorsRequest &req)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	uint32_t nextCursor = 0;
	bool eof = false;
	const board_motion_sensor_info_t *info =
		board_motion_get_by_cursor(req.cursor, &nextCursor, &eof);

	board_ListSensorsResponse lsr;
	memset(&lsr, 0, sizeof(lsr));

	if (info != NULL) {
		lsr.has_descriptor = true;
		populateDescriptor(&lsr.descriptor, info);
	} else {
		lsr.has_descriptor = false;
	}
	lsr.next_cursor = nextCursor;
	lsr.eof = eof;

	whad_board_sensor_descriptor(resp, requestId, &lsr);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleReadSensor(uint32_t requestId,
                                   const board_ReadSensorRequest &req)
{
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(req.sensor_id);

	if (info == NULL) {
		sendCommandResult(requestId,
			board_BoardCommand_ReadSensor,
			board_BoardResultCode_SENSOR_FAULT);
		return;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_SensorSample sample;
	memset(&sample, 0, sizeof(sample));
	sample.sensor_id = req.sensor_id;
	sample.sequence = 0;
	sample.timestamp_us = 0;

	int32_t values[10] = {0};
	uint32_t status = board_SensorStatusFlag_SENSOR_STATUS_STALE;

#ifdef BOARD_CLUE
	MotionManager::SampleStatus ss = MotionManager::STALE;
	uint32_t n = m_motion.readSensor(req.sensor_id, values, &ss);
	if (n > 0) {
		status = (ss == MotionManager::FRESH)
		       ? board_SensorStatusFlag_SENSOR_STATUS_NONE
		       : board_SensorStatusFlag_SENSOR_STATUS_STALE;
		sample.timestamp_us = timebase_now_us();
	} else {
		/* Non-motion sensors 6-13: pull cached values from the
		 * SensorDrivers wrappers (BMP280 / SHT31D / APDS-9960) or
		 * the PDM microphone driver. MotionManager::readSensor
		 * returns 0 for these IDs. Gesture (12) stays STALE: gesture
		 * mode is not entered by the optical-only APDS wrapper (T20);
		 * injectApdsGesture is never called, so no cached gesture
		 * exists to surface. */
		switch (req.sensor_id) {
		case 6: { /* pressure (Pa) */
			bmp280_sample_t s;
			if (m_sensors.bmp280().getLatest(&s)) {
				values[0] = (int32_t)s.pressure_pa;
				n = 1;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		case 7: { /* BMP280 temperature (milli-degC) */
			bmp280_sample_t s;
			if (m_sensors.bmp280().getLatest(&s)) {
				values[0] = s.temp_milli_c;
				n = 1;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		case 8: { /* humidity (milli-%RH) */
			sht31d_sample_t s;
			if (m_sensors.sht31d().getLatest(&s)) {
				values[0] = (int32_t)s.humidity_milli_rh;
				n = 1;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		case 9: { /* SHT31D temperature (milli-degC) */
			sht31d_sample_t s;
			if (m_sensors.sht31d().getLatest(&s)) {
				values[0] = s.temp_milli_c;
				n = 1;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		case 10: { /* color (clear, red, green, blue counts) */
			apds9960_optical_sample_t s;
			if (m_sensors.apds9960().getLatestOptical(&s)) {
				values[0] = (int32_t)s.clear;
				values[1] = (int32_t)s.red;
				values[2] = (int32_t)s.green;
				values[3] = (int32_t)s.blue;
				n = 4;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		case 11: { /* proximity (0-255) */
			apds9960_optical_sample_t s;
			if (m_sensors.apds9960().getLatestOptical(&s)) {
				values[0] = (int32_t)s.proximity;
				n = 1;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		case 12: /* gesture (enum) — see note above, stays STALE */
			break;
		case 13: { /* audio level (milli-dBFS) — PDM microphone */
			const pdm_metrics_t *m = m_pdm.getLatestMetrics();
			if (m != nullptr) {
				values[0] = m->dbfs_x1000;
				n = 1;
				status = board_SensorStatusFlag_SENSOR_STATUS_NONE;
				sample.timestamp_us = timebase_now_us();
			}
			break;
		}
		default:
			status = board_SensorStatusFlag_SENSOR_STATUS_STALE;
			break;
		}
	}
#else
	/* Host build (no MotionManager): return STALE zeros. */
	uint32_t n = info->value_count;
#endif

	uint32_t copy = n;
	if (copy > 10) copy = 10;
	if (copy > info->value_count) copy = info->value_count;
	sample.values_count = (pb_size_t)copy;
	for (uint32_t i = 0; i < copy; i++) {
		sample.values[i] = values[i];
	}
	sample.status = status;

	whad_board_sensor_sample(resp, requestId, &sample);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleConfigureStream(uint32_t requestId,
                                        const board_ConfigureStreamRequest &req)
{
	uint32_t code = board_BoardResultCode_SUCCESS;
	uint32_t actual = board_motion_stream_configure(
		&m_streamState, req.sensor_id, req.rate_millihz, &code);

	if (code != board_BoardResultCode_SUCCESS) {
		sendCommandResult(requestId,
			board_BoardCommand_ConfigureStream,
			(board_BoardResultCode)code);
		return;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_ConfigureStreamResponse csr;
	memset(&csr, 0, sizeof(csr));
	csr.sensor_id = req.sensor_id;
	csr.actual_rate_millihz = actual;
	csr.flags = req.flags;

	whad_board_stream_configured(resp, requestId, &csr);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleStopStream(uint32_t requestId,
                                   const board_StopStreamRequest &req)
{
	board_motion_stream_stop(&m_streamState, req.sensor_id);

	sendCommandResult(requestId,
		board_BoardCommand_StopStream,
		board_BoardResultCode_SUCCESS);
}

void BoardModule::handleCalibrate(uint32_t requestId,
                                  const board_CalibrateRequest &req)
{
	uint32_t code = board_motion_calib_start(&m_calibState, req.sensor_id);

	if (code != board_BoardResultCode_SUCCESS) {
		sendCommandResult(requestId,
			board_BoardCommand_Calibrate,
			(board_BoardResultCode)code);
		return;
	}

#ifdef BOARD_CLUE
	/* Kick off the async calibration driver. Periodic progress + the
	 * terminal result are emitted from tick() as samples accumulate. */
	MotionManager::CalibTarget target = MotionManager::CALIB_NONE;
	if (req.sensor_id == BOARD_MOTION_SENSOR_ACCEL ||
	    req.sensor_id == BOARD_MOTION_SENSOR_GYRO) {
		target = MotionManager::CALIB_IMU;
	} else if (req.sensor_id == BOARD_MOTION_SENSOR_MAG) {
		target = MotionManager::CALIB_MAG;
	}

	if (target != MotionManager::CALIB_NONE &&
	    m_motion.startCalibration(target)) {
		m_calibRequestId = requestId;
		m_calibSensorId = req.sensor_id;
		m_calibEmitMs = 0;
	} else {
		/* MotionManager busy or unsupported target — release the
		 * ownership slot we just acquired and report BUSY. */
		board_motion_calib_complete(&m_calibState);
		sendCommandResult(requestId,
			board_BoardCommand_Calibrate,
			board_BoardResultCode_BUSY);
		return;
	}
#else
	/* Host build (no MotionManager): complete immediately. */
	board_motion_calib_complete(&m_calibState);
#endif

	/* Nonterminal "accepted" result — client knows calibration started.
	 * Firmware emits progress + terminal CommandResult from tick(). */
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
#ifdef BOARD_CLUE
		board_motion_calib_complete(&m_calibState);
		uint8_t dummy_pct = 0;
		(void)m_motion.tickCalibration(nullptr, nullptr, &dummy_pct);
#endif
		return;
	}

	board_CommandResult cr;
	memset(&cr, 0, sizeof(cr));
	cr.command = board_BoardCommand_Calibrate;
	cr.result = board_BoardResultCode_SUCCESS;
	cr.terminal = false;
	cr.detail[0] = '\0';

	whad_board_command_result(resp, requestId, &cr);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleGetCalibration(uint32_t requestId,
                                       const board_GetCalibrationRequest &req)
{
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(req.sensor_id);

	if (info == NULL) {
		sendCommandResult(requestId,
			board_BoardCommand_GetCalibration,
			board_BoardResultCode_SENSOR_FAULT);
		return;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_CalibrationResponse cal;
	memset(&cal, 0, sizeof(cal));
	cal.sensor_id = req.sensor_id;
	cal.version = 0;
	cal.calibration_data.size = 0;
	cal.persisted = false;
	cal.crc32 = 0;

#ifdef BOARD_CLUE
	/* Try loading from persisted storage if adopted.
	 * Volatile (in-progress) calibration takes precedence — if
	 * none, fall through to persisted. The load validates CRC and
	 * value ranges before returning data. */
	if (calib_eval_is_adopted()) {
		uint8_t blob[CALIB_BLOB_MAX];
		uint16_t blob_len = 0;
		uint8_t kind = 0;
		bool loaded = false;

		if (req.sensor_id == BOARD_MOTION_SENSOR_ACCEL ||
		    req.sensor_id == BOARD_MOTION_SENSOR_GYRO) {
			kind = CALIB_KIND_IMU;
		} else if (req.sensor_id == BOARD_MOTION_SENSOR_MAG) {
			kind = CALIB_KIND_MAG;
		}

		if (kind == CALIB_KIND_IMU) {
			loaded = m_calib.loadImu((uint8_t)req.sensor_id,
			                         blob, &blob_len);
		} else if (kind == CALIB_KIND_MAG) {
			loaded = m_calib.loadMag((uint8_t)req.sensor_id,
			                         blob, &blob_len);
		}

		if (loaded && blob_len > 0) {
			calib_record_t crec;
			calib_rec_init(&crec, kind, req.sensor_id,
				       blob, blob_len);
			if (calib_rec_validate(&crec) == CALIB_OK) {
				uint16_t copy = blob_len;
				if (copy > 128) copy = 128;
				cal.calibration_data.size = (pb_size_t)copy;
				memcpy(cal.calibration_data.bytes, blob, copy);
				cal.version = blob[0];
				cal.persisted = true;
				cal.crc32 = 0; /* journal CRC already verified */
			}
		}
	}
#endif

	whad_board_calibration(resp, requestId, &cal);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::emitSensorSample(uint32_t sensor_id, uint32_t sequence,
                                   uint64_t timestamp_us, uint32_t status,
                                   const int32_t *values, uint32_t value_count)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_SensorSample sample;
	memset(&sample, 0, sizeof(sample));
	sample.sensor_id = sensor_id;
	sample.sequence = sequence;
	sample.timestamp_us = timestamp_us;
	sample.status = status;

	uint32_t copy = value_count;
	if (copy > 10) {
		copy = 10;
	}
	sample.values_count = (pb_size_t)copy;
	for (uint32_t i = 0; i < copy; i++) {
		sample.values[i] = values[i];
	}

	whad_board_sensor_sample(resp, 0u, &sample);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::emitStreamSamples(void)
{
#ifdef BOARD_CLUE
	/* Iterate active streams and emit SensorSample events at their
	 * configured rate. The actual sensor data comes from the
	 * MotionManager. */
	uint64_t now_us = timebase_now_us();
	uint32_t now_ms = (uint32_t)(now_us / 1000ull);

	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		const board_motion_stream_slot_t &slot = m_streamState.slots[i];
		if (!slot.active) continue;

		uint32_t period_ms = 1000u;
		if (slot.rate_millihz > 0) {
			/* 1000 ms * (1000 / millihz) = 1_000_000 / millihz */
			period_ms = 1000000u / slot.rate_millihz;
			if (period_ms == 0) period_ms = 1;
		}

		if ((now_ms - m_lastStreamEmitMs[i]) < period_ms) {
			continue;
		}
		m_lastStreamEmitMs[i] = now_ms;

		int32_t values[10] = {0};
		MotionManager::SampleStatus ss = MotionManager::STALE;
		uint32_t n = m_motion.readSensor(slot.sensor_id, values, &ss);
		if (n == 0) {
			/* Sensor has no fresh data source (env sensors). Skip emit;
			 * leave the stream slot intact so the host can keep polling. */
			continue;
		}

		uint32_t status = (ss == MotionManager::FRESH)
		               ? board_SensorStatusFlag_SENSOR_STATUS_NONE
		               : board_SensorStatusFlag_SENSOR_STATUS_STALE;
	uint32_t seq = m_streamSeq++;

	emitSensorSample(slot.sensor_id, seq, now_us, status, values, n);
	}
#else
	(void)m_streamState;
#endif
}

/* === END MOTION HANDLER IMPLEMENTATIONS (Todo 19) === */

void BoardModule::sendCommandResult(uint32_t requestId,
	board_BoardCommand command, board_BoardResultCode result)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_CommandResult cr;
	memset(&cr, 0, sizeof(cr));
	cr.command = command;
	cr.result = result;
	cr.terminal = true;
	cr.detail[0] = '\0';

	whad_board_command_result(resp, requestId, &cr);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::sendBoardStatus(board_BoardStatusCode code,
                                  board_BoardResultCode result,
                                  board_ResourceKind resource,
                                  uint32_t instance,
                                  uint32_t progress_per_mille,
                                  bool terminal,
                                  const char *detail)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_BoardStatus status;
	memset(&status, 0, sizeof(status));
	status.timestamp_us = (uint64_t)timebase_now_us();
	status.code = code;
	status.result = result;
	status.resource = resource;
	status.instance = instance;
	status.progress_per_mille = progress_per_mille;
	status.terminal = terminal;
	if (detail != nullptr) {
		strncpy(status.detail, detail, sizeof(status.detail) - 1);
		status.detail[sizeof(status.detail) - 1] = '\0';
	} else {
		status.detail[0] = '\0';
	}

	whad_board_board_status(resp, 0u, &status);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::sendInputEvent(board_InputSource source,
                                 board_InputAction action,
                                 int32_t value)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_InputEvent evt;
	memset(&evt, 0, sizeof(evt));
	evt.sequence = m_inputEventSeq++;
	evt.timestamp_us = (uint64_t)timebase_now_us();
	evt.source = source;
	evt.action = action;
	evt.value = value;

	whad_board_input_event(resp, 0u, &evt);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::sendGestureEvent(board_Gesture gesture)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_GestureEvent evt;
	memset(&evt, 0, sizeof(evt));
	evt.sequence = m_gestureEventSeq++;
	evt.timestamp_us = (uint64_t)timebase_now_us();
	evt.gesture = gesture;

	whad_board_gesture_event(resp, 0u, &evt);
	m_core->pushMessageToQueue(resp);
}

/* === INPUT/PROFILE HANDLER IMPLEMENTATIONS (Todo 23) === */

void BoardModule::handleGetInputState(uint32_t requestId)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_InputStateResponse state;
	memset(&state, 0, sizeof(state));
	state.sequence = m_streamSeq;
	state.timestamp_us = timebase_now_us();

#ifdef BOARD_CLUE
	state.buttons = (uint32_t)((m_motion.getButtonA() ? 0x1u : 0u) |
	                           (m_motion.getButtonB() ? 0x2u : 0u));
	rotg_state_t gs = m_motion.rotationGesture().getState();
	board_Gesture mapped = board_Gesture_GESTURE_UNKNOWN;
	switch (gs) {
	case ROTG_STATE_IDLE:            mapped = board_Gesture_GESTURE_UNKNOWN; break;
	case ROTG_STATE_ARMING:          mapped = board_Gesture_GESTURE_UNKNOWN; break;
	case ROTG_STATE_ARMED:           mapped = board_Gesture_GESTURE_UNKNOWN; break;
	case ROTG_STATE_INVERTED:        mapped = board_Gesture_GESTURE_DOWN; break;
	case ROTG_STATE_RETURNED:        mapped = board_Gesture_GESTURE_UP; break;
	case ROTG_STATE_CONFIRM_PENDING: mapped = board_Gesture_GESTURE_UP; break;
	default:                         mapped = board_Gesture_GESTURE_UNKNOWN; break;
	}
	state.gesture = mapped;
	state.microphone_threshold = false;
	state.mode = m_inputMode;
#else
	state.buttons = 0;
	state.gesture = board_Gesture_GESTURE_UNKNOWN;
	state.microphone_threshold = false;
	state.mode = m_inputMode;
#endif

	whad_board_input_state(resp, requestId, &state);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleConfigureInput(uint32_t requestId,
                                       const board_ConfigureInputRequest &req)
{
	board_BoardResultCode code = board_BoardResultCode_SUCCESS;

	if (req.mode == board_InputMode_INPUT_MODE_UNKNOWN) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else if (req.persist) {
		code = board_BoardResultCode_NOT_ADOPTED;
	} else {
		m_inputMode    = req.mode;
		m_inputFlags   = req.flags;
		m_inputDwellMs = req.dwell_ms;
		m_inputDeadzone = req.deadzone;
	}

	sendCommandResult(requestId,
	                  board_BoardCommand_ConfigureInput,
	                  code);
}

void BoardModule::handleRemoteProfileGet(uint32_t requestId,
                                         const board_RemoteProfileGetRequest &req)
{
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_RemoteProfileResponse pr;
	memset(&pr, 0, sizeof(pr));

	if (req.profile_id < (uint32_t)PROF_CUSTOM) {
		const prof_profile_t *src = profiles_get_builtin(
			(profile_id_t)req.profile_id);
		if (src != NULL) {
			pr.has_profile = true;
			pr.profile.profile_id = req.profile_id;
			strncpy(pr.profile.name, src->name,
			        sizeof(pr.profile.name) - 1);
			pr.profile.sensitivity = src->sensitivity;
			pr.profile.deadzone = src->deadzone;
			pr.profile.pointer_mode = src->pointer_mode;
			pr.profile.tilt_mode = src->tilt_mode;
			pr.profile.mappings_count = src->mappings_count;
			for (uint8_t i = 0; i < src->mappings_count && i < 16; i++) {
				pr.profile.mappings[i].source =
				static_cast<board_InputSource>(i);
				pr.profile.mappings[i].action =
					board_InputAction_INPUT_ACTION_PRESS;
				pr.profile.mappings[i].hid_usage_page =
					src->mappings[i].usage_page;
				pr.profile.mappings[i].hid_usage =
					src->mappings[i].usage;
				pr.profile.mappings[i].value = 0;
			}
		}
	}
#ifdef BOARD_CLUE
	else if (req.profile_id == (uint32_t)PROF_CUSTOM && m_profiles != nullptr) {
		if (m_profiles->fillRemoteProfile(PROF_CUSTOM, &pr.profile)) {
			pr.has_profile = true;
		}
	}
#endif

	whad_board_remote_profile(resp, requestId, &pr);
	m_core->pushMessageToQueue(resp);
}

void BoardModule::handleRemoteProfileSet(uint32_t requestId,
                                         const board_RemoteProfileSetRequest &req)
{
#ifdef BOARD_CLUE
	if (m_profiles == nullptr) {
		sendCommandResult(requestId,
		                  board_BoardCommand_RemoteProfileSet,
		                  req.persist
		                      ? board_BoardResultCode_NOT_ADOPTED
		                      : board_BoardResultCode_NOT_IMPLEMENTED);
		return;
	}

	if (req.persist) {
		sendCommandResult(requestId,
		                  board_BoardCommand_RemoteProfileSet,
		                  board_BoardResultCode_NOT_ADOPTED);
		return;
	}

	if (!req.has_profile) {
		sendCommandResult(requestId,
		                  board_BoardCommand_RemoteProfileSet,
		                  board_BoardResultCode_INVALID_ARGUMENT);
		return;
	}

	prof_profile_t candidate;
	prof_result_t load_rc = ProfileManager::loadRemoteProfile(
	    &req.profile, &candidate);
	if (load_rc != PROF_OK) {
		sendCommandResult(requestId,
		                  board_BoardCommand_RemoteProfileSet,
		                  board_BoardResultCode_INVALID_ARGUMENT);
		return;
	}

	prof_result_t rc = m_profiles->applyCustomProfile(&candidate);
	board_BoardResultCode proto_code;
	switch (rc) {
	case PROF_OK:                 proto_code = board_BoardResultCode_SUCCESS; break;
	case PROF_ERR_INVALID_ID:
	case PROF_ERR_INVALID_SOURCE:
	case PROF_ERR_NO_MAPPING:
	case PROF_ERR_INVALID_USAGE:
	case PROF_ERR_PROFILE_FULL:
	case PROF_ERR_IMMUTABLE:      proto_code = board_BoardResultCode_INVALID_ARGUMENT; break;
	default:                      proto_code = board_BoardResultCode_NOT_IMPLEMENTED; break;
	}

	sendCommandResult(requestId,
	                  board_BoardCommand_RemoteProfileSet,
	                  proto_code);
#else
	board_BoardResultCode code = req.persist
	                              ? board_BoardResultCode_NOT_ADOPTED
	                              : board_BoardResultCode_NOT_IMPLEMENTED;
	sendCommandResult(requestId,
	                  board_BoardCommand_RemoteProfileSet,
	                  code);
#endif
}

/* === END INPUT/PROFILE HANDLER IMPLEMENTATIONS (Todo 23) === */

/* === AUDIO HANDLER IMPLEMENTATIONS (Todo 26) === */

void BoardModule::handleAudioConfigure(uint32_t requestId,
                                       const board_AudioConfigureRequest &req)
{
	uint32_t actualRate = 0;
	int32_t actualGainDbX2 = 0;

	uint32_t code = pdm_eval_audio_configure(
		req.enabled, req.sample_rate_hz, req.gain_db_x2, req.flags,
		&actualRate, &actualGainDbX2);

	if (code != board_BoardResultCode_SUCCESS) {
		sendCommandResult(requestId,
			board_BoardCommand_AudioConfigure,
			(board_BoardResultCode)code);
		return;
	}

	m_audioEnabled = req.enabled;
	if (actualGainDbX2 != 0) {
		m_audioGainReg = PDM_GAIN_TO_REG(actualGainDbX2 / 2);
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) {
		return;
	}

	board_AudioConfigureResponse acr;
	memset(&acr, 0, sizeof(acr));
	acr.actual_sample_rate_hz = actualRate;
	acr.actual_gain_db_x2 = actualGainDbX2;
	acr.flags = req.flags;

	whad_board_audio_configured(resp, requestId, &acr);
	m_core->pushMessageToQueue(resp);
}

/* PDM_EVAL_* codes are local numeric copies (pdm_eval.h:72-76) that do NOT
 * match BoardResultCode values — bare casts produce semantic collisions
 * (e.g. PDM_EVAL_BUSY=6 → NOT_ADOPTED=6). This explicit remap keeps the
 * host-visible result codes correct. */
static board_BoardResultCode pdm_eval_to_board_result(uint32_t pdm_code)
{
	switch (pdm_code) {
	case PDM_EVAL_SUCCESS:         return board_BoardResultCode_SUCCESS;
	case PDM_EVAL_INVALID_ARG:     return board_BoardResultCode_INVALID_ARGUMENT;
	case PDM_EVAL_PERMISSION:      return board_BoardResultCode_PERMISSION_DENIED;
	case PDM_EVAL_BUSY:            return board_BoardResultCode_BUSY;
	case PDM_EVAL_NOT_IMPLEMENTED: return board_BoardResultCode_NOT_IMPLEMENTED;
	default:                       return board_BoardResultCode_INTERNAL_ERROR;
	}
}

void BoardModule::handleRawPcmDiagnostics(uint32_t requestId,
                                          const board_RawPcmDiagnosticsRequest &req)
{
	/* Delegate to PdmMicrophone: pdm_pcm_start validates the runtime +
	 * radio_idle precondition and clamps chunk_size; startRawPcm ensures
	 * DMA sampling is running and flushes the capture ring. */
	/* radio_idle=true: PDM and RADIO are independent peripherals on nRF52840
	 * (separate EasyDMA channels), so audio capture can proceed regardless
	 * of radio activity. */
	uint32_t code = m_pdm.startRawPcm(requestId, req.duration_ms,
	                                  req.chunk_size, /*radio_idle=*/true);

	if (code != board_BoardResultCode_SUCCESS) {
		sendCommandResult(requestId,
		                  board_BoardCommand_RawPcmDiagnostics,
		                  pdm_eval_to_board_result(code));
		return;
	}

	m_rawPcmRequestId = requestId;
	m_rawPcmSequence  = 0;

	/* Nonterminal "accepted" ack — the actual audio chunks are drained
	 * from the capture ring and pushed from tick() as DMA buffers fill. */
	Message *accepted = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (accepted != NULL) {
		board_CommandResult cr;
		memset(&cr, 0, sizeof(cr));
		cr.command = board_BoardCommand_RawPcmDiagnostics;
		cr.result = board_BoardResultCode_SUCCESS;
		cr.terminal = false;
		whad_board_command_result(accepted, requestId, &cr);
		m_core->pushMessageToQueue(accepted);
	}
}

/* === END AUDIO HANDLER IMPLEMENTATIONS (Todo 26) === */

/* === OUTPUT HANDLER IMPLEMENTATIONS (Todo 27) === */

void BoardModule::handleSetOutput(uint32_t requestId,
                                  const board_SetOutputRequest &req)
{
	uint32_t code = buzz_eval_set_output(
		req.target, req.value, req.duration_ms, req.stop);

	if (code != board_BoardResultCode_SUCCESS) {
		sendCommandResult(requestId,
			board_BoardCommand_SetOutput,
			(board_BoardResultCode)code);
		return;
	}

#ifdef BOARD_CLUE
	LedModule *led = m_core->getLedModule();
	DisplayModule *disp = m_core->getDisplayModule();

	if (req.stop) {
		switch (req.target) {
		case board_OutputTarget_OUTPUT_BUZZER:
			s_buzzer.stopTone();
			break;
		case board_OutputTarget_OUTPUT_NEOPIXEL:
			if (led != NULL) led->off(LED2);
			break;
		case board_OutputTarget_OUTPUT_WHITE_LED:
			if (led != NULL) led->off(LED3);
			break;
		case board_OutputTarget_OUTPUT_RED_LED:
			if (led != NULL) led->off(LED1);
			break;
		case board_OutputTarget_OUTPUT_BACKLIGHT:
			if (disp != NULL) disp->setBacklight(false);
			break;
		default:
			break;
		}
	}
	else {
		switch (req.target) {
		case board_OutputTarget_OUTPUT_BUZZER: {
			uint32_t bcode = s_buzzer.startTone(req.value, req.duration_ms);
			if (bcode != BUZZ_EVAL_SUCCESS) {
				sendCommandResult(requestId,
					board_BoardCommand_SetOutput,
					(board_BoardResultCode)bcode);
				return;
			}
			break;
		}
		case board_OutputTarget_OUTPUT_NEOPIXEL:
			if (led != NULL) {
				uint8_t r = (uint8_t)((req.value >> 16) & 0xFFu);
				uint8_t g = (uint8_t)((req.value >>  8) & 0xFFu);
				uint8_t b = (uint8_t)( req.value        & 0xFFu);
				LedColor mapped = BLUE;
				if (r > 0 && g == 0 && b == 0)      mapped = RED;
				else if (r == 0 && g > 0 && b == 0) mapped = GREEN;
				else if (r == 0 && g == 0 && b > 0) mapped = BLUE;
				else if (r > 0 && g > 0 && b == 0)  mapped = YELLOW;
				else if (r > 0 && b > 0 && g == 0)  mapped = PURPLE;
				else if (g > 0 && b > 0 && r == 0)  mapped = CYAN;
				led->setColor(mapped);
				led->on(LED2);
			}
			break;
		case board_OutputTarget_OUTPUT_WHITE_LED:
			if (led != NULL) {
				if (req.value > 0) led->on(LED3);
				else               led->off(LED3);
			}
			break;
		case board_OutputTarget_OUTPUT_RED_LED:
			if (led != NULL) {
				if (req.value > 0) led->on(LED1);
				else               led->off(LED1);
			}
			break;
		case board_OutputTarget_OUTPUT_BACKLIGHT:
			if (disp != NULL) disp->setBacklight(req.value > 0);
			break;
		default:
			break;
		}
	}
#else
	(void)req;
#endif /* BOARD_CLUE */

	sendCommandResult(requestId,
		board_BoardCommand_SetOutput,
		board_BoardResultCode_SUCCESS);
}

/* === END OUTPUT HANDLER IMPLEMENTATIONS (Todo 27) === */

/* === EXPERT I/O HANDLER IMPLEMENTATIONS (Todo 32) === */

void BoardModule::handleI2cTransfer(uint32_t requestId,
                                    const board_I2cTransferRequest &req)
{
	board_BoardResultCode code = board_BoardResultCode_SUCCESS;

	if (req.address > 0x7Fu) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else if (req.write_data.size > IO_EVAL_MAX_BUFFER ||
	           req.read_length > IO_EVAL_MAX_BUFFER) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else if (io_eval_is_onboard_i2c((uint8_t)req.address) && !req.force) {
		code = board_BoardResultCode_BUSY;
	}

	if (code != board_BoardResultCode_SUCCESS) {
		Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
		if (resp != NULL) {
			board_I2cTransferResponse r = {};
			r.result = code;
			r.bytes_written = 0;
			r.bytes_read = 0;
			whad_board_i2c_result(resp, requestId, &r);
			m_core->pushMessageToQueue(resp);
		}
		return;
	}

#ifdef BOARD_CLUE
	static uint8_t s_readBuf[IO_EVAL_MAX_BUFFER];

	i2c_expert_transfer_t xfer;
	xfer.address = (uint8_t)req.address;
	xfer.write_buf = (req.write_data.size > 0) ? req.write_data.bytes : NULL;
	xfer.write_len = req.write_data.size;
	xfer.read_buf = (req.read_length > 0) ? s_readBuf : NULL;
	xfer.read_len = req.read_length;
	xfer.repeated_start = req.repeated_start;
	xfer.force = req.force;

	i2c_expert_result_t result;
	i2c_expert_code_t ec = i2c_expert_transfer(&xfer, &result);

	board_BoardResultCode resp_code = board_BoardResultCode_SUCCESS;
	switch (ec) {
	case I2C_EXPERT_OK:              resp_code = board_BoardResultCode_SUCCESS; break;
	case I2C_EXPERT_ERR_INVALID_ARG: resp_code = board_BoardResultCode_INVALID_ARGUMENT; break;
	case I2C_EXPERT_ERR_BUSY:        resp_code = board_BoardResultCode_BUSY; break;
	case I2C_EXPERT_ERR_NACK:        resp_code = board_BoardResultCode_BUSY; break;
	case I2C_EXPERT_ERR_TIMEOUT:     resp_code = board_BoardResultCode_BUSY; break;
	default:                         resp_code = board_BoardResultCode_NOT_IMPLEMENTED; break;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_I2cTransferResponse r = {};
		r.result = resp_code;
		r.bytes_written = result.bytes_written;
		r.bytes_read = result.bytes_read;

		uint32_t copy = result.bytes_read;
		if (copy > IO_EVAL_MAX_BUFFER) copy = IO_EVAL_MAX_BUFFER;
		r.read_data.size = (pb_size_t)copy;
		if (copy > 0)
			memcpy(r.read_data.bytes, s_readBuf, copy);

		whad_board_i2c_result(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#else
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_I2cTransferResponse r = {};
		r.result = board_BoardResultCode_NOT_IMPLEMENTED;
		whad_board_i2c_result(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#endif
}

void BoardModule::handleSpiTransfer(uint32_t requestId,
                                    const board_SpiTransferRequest &req)
{
	board_BoardResultCode code = board_BoardResultCode_SUCCESS;

	io_spi_mode_t spi_mode;
	if (!io_eval_spi_mode_valid((int32_t)req.mode, &spi_mode)) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else if (req.tx_data.size > IO_EVAL_MAX_BUFFER ||
	           req.read_length > IO_EVAL_MAX_BUFFER) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else if (req.frequency_hz > IO_EVAL_SPI_FREQ_MAX) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	}

	if (code != board_BoardResultCode_SUCCESS) {
		Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
		if (resp != NULL) {
			board_SpiTransferResponse r = {};
			r.result = code;
			whad_board_spi_result(resp, requestId, &r);
			m_core->pushMessageToQueue(resp);
		}
		return;
	}

#ifdef BOARD_CLUE
	static uint8_t s_rxBuf[IO_EVAL_MAX_BUFFER];

	spi_expert_transfer_t xfer;
	xfer.cs_pin = req.cs_pin;
	xfer.frequency_hz = req.frequency_hz;
	xfer.mode = spi_mode;
	xfer.tx_buf = (req.tx_data.size > 0) ? req.tx_data.bytes : NULL;
	xfer.tx_len = req.tx_data.size;
	xfer.rx_buf = (req.read_length > 0) ? s_rxBuf : NULL;
	xfer.rx_len = req.read_length;

	spi_expert_result_t result;
	spi_expert_code_t ec = spi_expert_transfer(&xfer, &result);

	board_BoardResultCode resp_code = board_BoardResultCode_SUCCESS;
	switch (ec) {
	case SPI_EXPERT_OK:              resp_code = board_BoardResultCode_SUCCESS; break;
	case SPI_EXPERT_ERR_INVALID_ARG: resp_code = board_BoardResultCode_INVALID_ARGUMENT; break;
	case SPI_EXPERT_ERR_BUSY:        resp_code = board_BoardResultCode_BUSY; break;
	case SPI_EXPERT_ERR_CS_CONFLICT: resp_code = board_BoardResultCode_BUSY; break;
	default:                         resp_code = board_BoardResultCode_NOT_IMPLEMENTED; break;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_SpiTransferResponse r = {};
		r.result = resp_code;
		r.bytes_written = result.bytes_written;
		r.bytes_read = result.bytes_read;

		uint32_t copy = result.bytes_read;
		if (copy > IO_EVAL_MAX_BUFFER) copy = IO_EVAL_MAX_BUFFER;
		r.rx_data.size = (pb_size_t)copy;
		if (copy > 0)
			memcpy(r.rx_data.bytes, s_rxBuf, copy);

		whad_board_spi_result(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#else
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_SpiTransferResponse r = {};
		r.result = board_BoardResultCode_NOT_IMPLEMENTED;
		whad_board_spi_result(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#endif
}

void BoardModule::handleGpioConfigure(uint32_t requestId,
                                      const board_GpioConfigureRequest &req)
{
	board_BoardResultCode code = board_BoardResultCode_SUCCESS;

	/* Validate proto enums (reject UNKNOWN sentinel values). */
	expert_gpio_dir_t dir = EXPERT_GPIO_DIR_INPUT;
	switch (req.direction) {
	case board_GpioDirection_GPIO_INPUT:  dir = EXPERT_GPIO_DIR_INPUT;  break;
	case board_GpioDirection_GPIO_OUTPUT: dir = EXPERT_GPIO_DIR_OUTPUT; break;
	default:
		code = board_BoardResultCode_INVALID_ARGUMENT;
		break;
	}

	expert_gpio_pull_t pull = EXPERT_GPIO_PULL_NONE;
	if (code == board_BoardResultCode_SUCCESS) {
		switch (req.pull) {
		case board_GpioPull_GPIO_PULL_NONE: pull = EXPERT_GPIO_PULL_NONE;     break;
		case board_GpioPull_GPIO_PULL_UP:   pull = EXPERT_GPIO_PULL_PULLUP;   break;
		case board_GpioPull_GPIO_PULL_DOWN: pull = EXPERT_GPIO_PULL_PULLDOWN; break;
		default:
			code = board_BoardResultCode_INVALID_ARGUMENT;
			break;
		}
	}

	/* Validate D-pin is in the analog-capable alias table. */
	if (code == board_BoardResultCode_SUCCESS &&
	    expert_eval_alias_by_d((uint8_t)req.pin) == NULL) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	}

	expert_token_t token = EXPERT_TOKEN_INVALID;
	bool leased = false;

#ifdef BOARD_CLUE
	if (code == board_BoardResultCode_SUCCESS) {
		expert_result_t er = gpio_expert_acquire((uint8_t)req.pin,
		                                          PINREG_OWNER_GPIO_USER,
		                                          &token);
		if (er == EXPERT_OK) {
			leased = true;
			expert_gpio_config_t cfg = {};
			cfg.dir   = dir;
			cfg.pull  = pull;
			cfg.drive = EXPERT_GPIO_DRIVE_S0S1;
			cfg.sense = EXPERT_GPIO_SENSE_NONE;
			er = gpio_expert_configure(token, PINREG_OWNER_GPIO_USER, &cfg);
			if (er == EXPERT_OK && dir == EXPERT_GPIO_DIR_OUTPUT) {
				er = gpio_expert_write(token, PINREG_OWNER_GPIO_USER,
				                       req.initial_value ? 1 : 0);
			}
		}

		code = expert_eval_to_board_result(er);
	}
#else
	if (code == board_BoardResultCode_SUCCESS)
		code = board_BoardResultCode_NOT_IMPLEMENTED;
#endif

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_GpioConfigureResponse r = {};
		r.result = code;
		r.pin    = req.pin;
		if (leased && code == board_BoardResultCode_SUCCESS) {
			r.has_lease       = true;
			r.lease.resource   = board_ResourceKind_RESOURCE_GPIO_PIN;
			r.lease.instance   = req.pin;
			r.lease.generation = (uint32_t)token;
			r.lease.owner      = (uint32_t)PINREG_OWNER_GPIO_USER;
		}
		whad_board_gpio_configured(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
}

void BoardModule::handleGpioRead(uint32_t requestId,
                                 const board_GpioReadRequest &req)
{
	board_BoardResultCode code = board_BoardResultCode_SUCCESS;
	int value = 0;

#ifdef BOARD_CLUE
	if (!req.has_lease) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else {
		expert_token_t token = (expert_token_t)req.lease.generation;
		expert_result_t er = gpio_expert_read(token,
		                                      PINREG_OWNER_GPIO_USER,
		                                      &value);
		code = expert_eval_to_board_result(er);
	}
#else
	code = board_BoardResultCode_NOT_IMPLEMENTED;
#endif

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_GpioReadResponse r = {};
		r.result = code;
		r.pin    = req.pin;
		r.value  = (value != 0);
		whad_board_gpio_value(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
}

void BoardModule::handleGpioWrite(uint32_t requestId,
                                  const board_GpioWriteRequest &req)
{
	board_BoardResultCode code;

#ifdef BOARD_CLUE
	if (!req.has_lease) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else {
		expert_token_t token = (expert_token_t)req.lease.generation;
		expert_result_t er = gpio_expert_write(token,
		                                       PINREG_OWNER_GPIO_USER,
		                                       req.value ? 1 : 0);
		code = expert_eval_to_board_result(er);
	}
#else
	code = board_BoardResultCode_NOT_IMPLEMENTED;
#endif

	/* No GpioWriteResponse in the proto — use generic CommandResult. */
	sendCommandResult(requestId, board_BoardCommand_GpioWrite, code);
}

void BoardModule::handleReleasePin(uint32_t requestId,
                                   const board_ReleasePinRequest &req)
{
	board_BoardResultCode code;

#ifdef BOARD_CLUE
	/* The expert_token_t issued at acquire time is carried in the lease
	 * generation field. Dispatch on the resource kind to the owning
	 * expert subsystem. */
	expert_token_t token = (expert_token_t)req.lease.generation;

	switch (req.resource) {
	case board_ResourceKind_RESOURCE_GPIO_PIN: {
		expert_result_t er = gpio_expert_release(token);
		code = expert_eval_to_board_result(er);
		break;
	}
	case board_ResourceKind_RESOURCE_ADC_CHANNEL: {
		expert_result_t er = adc_expert_release(token);
		code = expert_eval_to_board_result(er);
		break;
	}
	default:
		code = board_BoardResultCode_INVALID_ARGUMENT;
		break;
	}
#else
	(void)req;
	code = board_BoardResultCode_NOT_IMPLEMENTED;
#endif

	sendCommandResult(requestId, board_BoardCommand_ReleasePin, code);
}

void BoardModule::handleAdcRead(uint32_t requestId,
                                const board_AdcReadRequest &req)
{
	/* channel = Arduino D-pin number. Must be analog-capable
	 * (one of the 8 pins in EXPERT_ALIASES). samples is ignored —
	 * adc_expert_read_oneshot does a single conversion. */
	board_BoardResultCode code = board_BoardResultCode_SUCCESS;

	if (req.channel > 0xFFu) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	} else if (expert_eval_alias_by_d((uint8_t)req.channel) == NULL) {
		code = board_BoardResultCode_INVALID_ARGUMENT;
	}

	if (code != board_BoardResultCode_SUCCESS) {
		Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
		if (resp != NULL) {
			board_AdcReadResponse r = {};
			r.result = code;
			r.channel = req.channel;
			whad_board_adc_value(resp, requestId, &r);
			m_core->pushMessageToQueue(resp);
		}
		return;
	}

#ifdef BOARD_CLUE
	uint16_t out_mv = 0;
	expert_adc_status_t adc_stat = EXPERT_ADC_OK;
	expert_result_t er = adc_expert_read_oneshot(
		(uint8_t)req.channel, PINREG_OWNER_GPIO_USER,
		&out_mv, &adc_stat);

	board_BoardResultCode resp_code = expert_eval_to_board_result(er);
	if (er == EXPERT_OK && adc_stat == EXPERT_ADC_SATURATED) {
		resp_code = board_BoardResultCode_OVERFLOW;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_AdcReadResponse r = {};
		r.result = resp_code;
		r.channel = req.channel;
		r.millivolts = (resp_code == board_BoardResultCode_SUCCESS ||
		                resp_code == board_BoardResultCode_OVERFLOW)
			? (int32_t)out_mv : 0;
		/* raw not exposed by the oneshot API; callers needing it must
		 * use the retained acquire/configure/read/release path. */
		r.raw = 0;
		whad_board_adc_value(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#else
	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp != NULL) {
		board_AdcReadResponse r = {};
		r.result = board_BoardResultCode_NOT_IMPLEMENTED;
		r.channel = req.channel;
		whad_board_adc_value(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#endif
}

/* === STORAGE HANDLER IMPLEMENTATIONS (Todo 28) === */

void BoardModule::handleStorageInfo(uint32_t requestId)
{
#ifdef BOARD_CLUE
	uint8_t  jedec[3] = {0, 0, 0};
	uint32_t capacity = 0;
	qspi_state_t qs = QSPI_ST_UNADOPTED;
	uint8_t  nonce[QSPI_NONCE_SIZE];
	m_qspi.getStorageInfo(jedec, &capacity, &qs, nonce);

	const journal_sb_info_t *sb = (m_qspi.isAdopted() && m_journal.isReady())
	                              ? m_journal.sbInfo() : nullptr;

	board_StorageState proto_state;
	switch (qs) {
	case QSPI_ST_ADOPTED:
		proto_state = board_StorageState_STORAGE_ADOPTED; break;
	case QSPI_ST_UNADOPTED:
		proto_state = board_StorageState_STORAGE_UNADOPTED; break;
	case QSPI_ST_PROBING:
	case QSPI_ST_VALIDATED:
	case QSPI_ST_NONCE_ISSUED:
	case QSPI_ST_CONFIRMED:
	case QSPI_ST_ERASING_SUPERBLOCK:
	case QSPI_ST_ERASING_REMAINING:
	case QSPI_ST_VERIFYING:
	case QSPI_ST_WRITING_SUPERBLOCK:
	case QSPI_ST_COMMITTING:
		proto_state = board_StorageState_STORAGE_ADOPTING; break;
	default:
		proto_state = board_StorageState_STORAGE_FAULT; break;
	}

	Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
	if (resp == NULL) return;

	board_StorageInfoResponse r;
	memset(&r, 0, sizeof(r));
	r.state         = proto_state;
	r.capacity_bytes = capacity;
	r.used_bytes     = (sb != nullptr) ? sb->log_write_off : 0u;
	r.log_records    = (sb != nullptr) ? sb->log_record_count : 0u;
	r.erase_size     = 0u;
	memcpy(r.jedec_id, jedec, sizeof(jedec));
	whad_board_storage_status(resp, requestId, &r);
	m_core->pushMessageToQueue(resp);
#else
	sendCommandResult(requestId,
	                  board_BoardCommand_StorageInfo,
	                  board_BoardResultCode_NOT_IMPLEMENTED);
#endif
}

void BoardModule::handleStorageAdopt(uint32_t requestId,
                                     const board_StorageAdoptRequest &req)
{
#ifdef BOARD_CLUE
	/* confirm_cli mirrors the host's --yes-really-adopt-and-erase flag;
	 * the menu path is not driven from this handler. */
	bool confirm_cli = req.force ? true : false;
	const uint8_t *nonce_bytes = (const uint8_t *)&req.confirm_nonce;
	const size_t   nonce_len   = sizeof(req.confirm_nonce);

	bool started = m_qspi.beginAdoption(nonce_bytes, nonce_len,
	                                    /*confirm_menu=*/false,
	                                    confirm_cli);
	board_BoardResultCode code = started
		? board_BoardResultCode_SUCCESS
		: board_BoardResultCode_BUSY;
	sendCommandResult(requestId,
	                  board_BoardCommand_StorageAdopt,
	                  code);
	if (started) {
		sendBoardStatus(
			board_BoardStatusCode_BOARD_STATUS_STORAGE_PROGRESS,
			board_BoardResultCode_SUCCESS,
			board_ResourceKind_RESOURCE_STORAGE, 0u, 0u, false,
			"storage:adopt:start");
	}
#else
	(void)req;
	sendCommandResult(requestId,
	                  board_BoardCommand_StorageAdopt,
	                  board_BoardResultCode_NOT_IMPLEMENTED);
#endif
}

void BoardModule::handleStorageReadLog(uint32_t requestId,
                                       const board_StorageReadLogRequest &req)
{
#ifdef BOARD_CLUE
	if (!m_qspi.isAdopted() || !m_journal.isReady()) {
		Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
		if (resp == NULL) return;
		board_LogChunk r;
		memset(&r, 0, sizeof(r));
		r.result = board_BoardResultCode_NOT_ADOPTED;
		r.cursor = req.cursor;
		r.eof    = true;
		whad_board_log_chunk(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
		return;
	}

	journal_read_cursor_t cur;
	memset(&cur, 0, sizeof(cur));
	cur.read_off = req.cursor;

	/* Bound the response burst to avoid starving the message pool.
	 * Each chunk can carry up to 900 bytes; the host re-requests with
	 * an updated cursor if eof was not reached. */
	const uint32_t MAX_CHUNKS_PER_REQ = 4u;
	const uint16_t chunk_cap = (req.max_bytes > 0 &&
	                            req.max_bytes <= sizeof(board_LogChunk_data_t))
	                           ? (uint16_t)req.max_bytes
	                           : (uint16_t)sizeof(board_LogChunk_data_t);

	uint8_t buf[sizeof(board_LogChunk_data_t)];
	uint32_t seq = 0u;

	for (uint32_t i = 0; i < MAX_CHUNKS_PER_REQ; i++) {
		uint16_t actual = 0;
		bool     eof    = false;
		bool ok = m_journal.readLog(&cur, buf, chunk_cap, &actual, &eof);
		if (!ok || actual == 0u) {
			/* Emit a terminal empty chunk so the host sees a clear
			 * end-of-stream signal even when the journal is empty. */
			Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
			if (resp == NULL) return;
			board_LogChunk r;
			memset(&r, 0, sizeof(r));
			r.sequence = seq++;
			r.cursor   = cur.read_off;
			r.eof      = true;
			r.result   = board_BoardResultCode_SUCCESS;
			whad_board_log_chunk(resp, requestId, &r);
			m_core->pushMessageToQueue(resp);
			break;
		}

		Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
		if (resp == NULL) return;
		board_LogChunk r;
		memset(&r, 0, sizeof(r));
		r.sequence = seq++;
		r.cursor   = req.cursor;
		r.offset   = cur.read_off;
		r.count    = actual;
		r.data.size = actual;
		memcpy(r.data.bytes, buf, actual);
		r.eof      = eof;
		r.total    = (m_journal.sbInfo() != nullptr)
		             ? m_journal.sbInfo()->log_record_count : 0u;
		r.result   = board_BoardResultCode_SUCCESS;
		whad_board_log_chunk(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);

		if (eof) break;
	}
#else
	(void)req;
	sendCommandResult(requestId,
	                  board_BoardCommand_StorageReadLog,
	                  board_BoardResultCode_NOT_IMPLEMENTED);
#endif
}

void BoardModule::handleStorageEraseLog(uint32_t requestId,
                                        const board_StorageEraseLogRequest &req)
{
#ifdef BOARD_CLUE
	(void)req;  /* confirm_nonce retained for future menu-driven flow. */

	if (!m_qspi.isAdopted() || !m_journal.isReady()) {
		sendCommandResult(requestId,
		                  board_BoardCommand_StorageEraseLog,
		                  board_BoardResultCode_NOT_ADOPTED);
		return;
	}

	if (m_journal.eraseState() != JOURNAL_ERASE_IDLE) {
		sendCommandResult(requestId,
		                  board_BoardCommand_StorageEraseLog,
		                  board_BoardResultCode_BUSY);
		return;
	}

	bool started = m_journal.beginEraseLog();
	sendCommandResult(requestId,
	                  board_BoardCommand_StorageEraseLog,
	                  started ? board_BoardResultCode_SUCCESS
	                          : board_BoardResultCode_BUSY);
	if (started) {
		sendBoardStatus(
			board_BoardStatusCode_BOARD_STATUS_STORAGE_PROGRESS,
			board_BoardResultCode_SUCCESS,
			board_ResourceKind_RESOURCE_STORAGE, 0u, 0u, false,
			"storage:erase:start");
	}
#else
	(void)req;
	sendCommandResult(requestId,
	                  board_BoardCommand_StorageEraseLog,
	                  board_BoardResultCode_NOT_IMPLEMENTED);
#endif
}

/* === END STORAGE HANDLER IMPLEMENTATIONS (Todo 28) === */

/* === END EXPERT I/O HANDLER IMPLEMENTATIONS (Todo 32) === */

/* === MOTION SUBSYSTEM WIRING (Todo 9) === */

void BoardModule::injectImu(const imu_sample_t *imu, uint64_t now_us)
{
#ifdef BOARD_CLUE
	m_motion.feedImu(imu, now_us);
#else
	(void)imu; (void)now_us;
#endif
}

void BoardModule::injectMag(const mag_sample_t *mag, bool healthy,
                            uint64_t now_us)
{
#ifdef BOARD_CLUE
	m_motion.feedMag(mag, healthy, now_us);
#else
	(void)mag; (void)healthy; (void)now_us;
#endif
}

void BoardModule::injectApdsGesture(apds9960_gesture_t g)
{
#ifdef BOARD_CLUE
	m_motion.feedApdsGesture(g);
	/* NOTE: The GestureEvent emission below is currently UNREACHABLE in
	 * production. The APDS9960 wrapper operates in optical-only mode
	 * (RGBC color/proximity, Wave 5 T20 design). Gesture mode entry
	 * requires enabling the APDS9960 gesture engine in the wrapper,
	 * which is outside Wave 6 scope. See butterfly/AGENTS.md gap #4.
	 * The code is intentionally retained: once gesture mode is wired
	 * the emission path works with zero changes. The internal
	 * apds9960_gesture_t enum values match board_Gesture 1:1
	 * (UNKNOWN=0, UP=1, DOWN=2, LEFT=3, RIGHT=4, NEAR=5, FAR=6). */
	if (g != APDS9960_GESTURE_NONE) {
		sendGestureEvent(static_cast<board_Gesture>(
			apds9960_gesture_to_proto(g)));
	}
#else
	(void)g;
#endif
}

void BoardModule::injectButtons(bool button_a, bool button_b)
{
#ifdef BOARD_CLUE
	m_motion.feedButtons(button_a, button_b);
	/* Emit InputEvent on each button edge (press/release). The CLUE
	 * exposes two tactile buttons: A (P1.02) and B (P1.10). */
	if (button_a != m_lastBtnA) {
		sendInputEvent(
			board_InputSource_INPUT_SOURCE_BUTTON_A,
			button_a ? board_InputAction_INPUT_ACTION_PRESS
			         : board_InputAction_INPUT_ACTION_RELEASE);
		m_lastBtnA = button_a;
	}
	if (button_b != m_lastBtnB) {
		sendInputEvent(
			board_InputSource_INPUT_SOURCE_BUTTON_B,
			button_b ? board_InputAction_INPUT_ACTION_PRESS
			         : board_InputAction_INPUT_ACTION_RELEASE);
		m_lastBtnB = button_b;
	}
#else
	(void)button_a; (void)button_b;
#endif
}

void BoardModule::setProfileManager(ProfileManager *profiles)
{
#ifdef BOARD_CLUE
	m_profiles = profiles;
#else
	(void)profiles;
#endif
}

void BoardModule::tick(void)
{
#ifdef BOARD_CLUE
	uint64_t now_us = timebase_now_us();

	/* Button poller (P1.02=A, P1.10=B, active-low with pull-up).
	 * Poll every ~5ms; injectButtons handles edge detection.
	 * 500ms boot grace period lets pull-ups settle and USB enumerate
	 * before sampling edges — prevents spurious InputEvents on boot. */
	{
		uint32_t now_ms = (uint32_t)(now_us / 1000ull);
		if (m_tickStartMs == 0) {
			m_tickStartMs = now_ms;
			m_lastBtnA = (nrf_gpio_pin_read(BSP_BUTTON_0) == 0);
			m_lastBtnB = (nrf_gpio_pin_read(BSP_BUTTON_1) == 0);
		}
		if ((now_ms - m_tickStartMs >= 500u) &&
		    now_ms >= m_btnPollNextMs) {
			m_btnPollNextMs = now_ms + 5u;
			bool pressed_a = (nrf_gpio_pin_read(BSP_BUTTON_0) == 0);
			bool pressed_b = (nrf_gpio_pin_read(BSP_BUTTON_1) == 0);
			injectButtons(pressed_a, pressed_b);
		}
	}

	/* Drive the I2C sensor wrappers (IMU, mag, BMP280, SHT31D,
	 * APDS9960). Completion callbacks call injectImu / injectMag /
	 * injectApdsGesture, which feed MotionManager's cache before
	 * the motion tick below consumes it. */
	m_sensors.tick(now_us);

	/* Drive the motion subsystem one step. The rotation-gesture FSM
	 * returns an event when state changes; we surface those to the
	 * dashboard. */
	rotg_event_t evt = m_motion.tick(now_us);
	if (evt != ROTG_EVENT_NONE) {
		publishRotationState(evt);
	}

	/* Drain any pending APDS gesture into the ProfileManager. */
	apds9960_gesture_t g = m_motion.consumeApdsGesture();
	if (g != APDS9960_GESTURE_NONE) {
		dispatchApdsToProfiles(g);
	}

	s_buzzer.tick();

	/* Advance async calibration if active. */
	if (m_motion.isCalibrationBusy()) {
		uint8_t progress = 0;
		/* Calibration is driven by injected IMU/mag samples already
		 * cached inside MotionManager — pass nullptr to use the most
		 * recent cached samples via the manager's own feed path. */
		MotionManager::CalibTarget target =
		    m_motion.getCalibrationTarget();
		MotionManager::CalibResult cr =
		    m_motion.tickCalibration(nullptr, nullptr, &progress);
		uint32_t now_ms = (uint32_t)(now_us / 1000ull);
		bool emit_progress = ((now_ms - m_calibEmitMs) >= 500u);
		if (cr == MotionManager::CALIB_RESULT_DONE_OK) {
			board_motion_calib_complete(&m_calibState);
			persistCalibrationResult(target);
			sendCommandResult(m_calibRequestId,
			                  board_BoardCommand_Calibrate,
			                  board_BoardResultCode_SUCCESS);
			sendBoardStatus(
				board_BoardStatusCode_BOARD_STATUS_CALIBRATION_PROGRESS,
				board_BoardResultCode_SUCCESS,
				board_ResourceKind_RESOURCE_UNKNOWN, 0u, 1000u, true,
				"calibrate:done");
			m_calibEmitMs = 0;
		} else if (cr == MotionManager::CALIB_RESULT_FAILED) {
			board_motion_calib_complete(&m_calibState);
			sendCommandResult(m_calibRequestId,
			                  board_BoardCommand_Calibrate,
			                  board_BoardResultCode_SENSOR_FAULT);
			sendBoardStatus(
				board_BoardStatusCode_BOARD_STATUS_CALIBRATION_PROGRESS,
				board_BoardResultCode_SENSOR_FAULT,
				board_ResourceKind_RESOURCE_UNKNOWN, 0u, 0u, true,
				"calibrate:failed");
			m_calibEmitMs = 0;
		} else if (emit_progress) {
			m_calibEmitMs = now_ms;
			Message *resp = messagePoolAllocateForDomain(DOMAIN_BOARD);
			if (resp != NULL) {
				board_CommandResult crp;
				memset(&crp, 0, sizeof(crp));
				crp.command = board_BoardCommand_Calibrate;
				crp.result = board_BoardResultCode_SUCCESS;
				crp.terminal = false;
				snprintf(crp.detail, sizeof(crp.detail),
				         "progress=%u%%", (unsigned)progress);
				whad_board_command_result(resp, m_calibRequestId, &crp);
				m_core->pushMessageToQueue(resp);
			}
			sendBoardStatus(
				board_BoardStatusCode_BOARD_STATUS_CALIBRATION_PROGRESS,
				board_BoardResultCode_SUCCESS,
				board_ResourceKind_RESOURCE_UNKNOWN,
				m_calibSensorId, (uint32_t)progress * 10u, false,
				"calibrate:progress");
		}
	}

	/* Active stream sampling (rate-limited internally). */
	emitStreamSamples();

	/* Register dashboard once on first tick when the display is up. */
	if (!m_dashboardRegistered) {
		registerDashboardPages();
	}

	/* === PDM POLL + DRAIN (Todo 9 — RawPcmDiagnostics) ===
	 * poll() pulls one DMA buffer from the ISR-ready queue and feeds
	 * both the metric window (sensor 13 audio level) and, when a raw
	 * PCM session is active, the capture ring. We then drain the ring
	 * in ≤20-sample chunks and push AudioChunk messages until either
	 * the ring empties (wait for next DMA) or the session ends (eof). */
	m_pdm.poll();

	if (m_rawPcmRequestId != 0 && m_pdm.isPcmActive()) {
		int16_t scratch[PDM_PCM_CHUNK_MAX_SAMPLES];

		while (m_pdm.isPcmActive()) {
			uint32_t got = m_pdm.drainPcmSamples(scratch,
			                                     PDM_PCM_CHUNK_MAX_SAMPLES);
			if (got == 0) {
				break;
			}

			/* Trim to remaining session samples so the final chunk
			 * does not overshoot the requested duration. */
			uint32_t remaining = m_pdm.pcmTotalSamples() -
			                     m_pdm.pcmSentSamples();
			if (got > remaining) {
				got = remaining;
			}

			uint32_t offset_samples = m_pdm.pcmSentSamples();
			bool complete = m_pdm.accountPcmDrained(got);

			Message *chunkMsg = messagePoolAllocateForDomain(DOMAIN_BOARD);
			if (chunkMsg == NULL) {
				/* Pool exhausted — try again next tick. */
				break;
			}

			board_AudioChunk chunk;
			memset(&chunk, 0, sizeof(chunk));
			chunk.sequence = m_rawPcmSequence++;
			chunk.offset   = offset_samples * sizeof(int16_t);
			chunk.count    = got * sizeof(int16_t);
			chunk.pcm.size = got * sizeof(int16_t);
			memcpy(chunk.pcm.bytes, scratch, chunk.pcm.size);
			chunk.eof      = complete;
			chunk.total    = m_pdm.pcmTotalSamples() * sizeof(int16_t);
			chunk.result   = board_BoardResultCode_SUCCESS;

			whad_board_audio_chunk(chunkMsg, m_rawPcmRequestId, &chunk);
			m_core->pushMessageToQueue(chunkMsg);

			if (complete) {
				m_rawPcmRequestId = 0;
				m_rawPcmSequence  = 0;
				break;
			}
		}
	}

	/* === STORAGE TICK (Todo 28) ===
	 * Advance the async adoption + journal erase state machines. Both
	 * feed the watchdog between long erase sectors. Erase is only
	 * driven when not IDLE so the journal FSM owns the busy flag. */
	if (m_qspi.isPresent()) {
		m_qspi.tick();
	}
	if (m_qspi.isAdopted() && m_journal.isReady()) {
		m_journal.tick();
		if (m_journal.eraseState() != JOURNAL_ERASE_IDLE) {
			m_journal.tickEraseLog();
		}
	}
#else
	(void)0;
#endif
}

#ifdef BOARD_CLUE

void BoardModule::publishRotationState(rotg_event_t evt)
{
	/* Surface rotation-gesture state transitions to the host via a
	 * BoardStatus event (code=RUNTIME_SWITCHING for switch-related
	 * events, terminal=true for end states). The legacy Verbose
	 * notification is kept for backward-compat with whadup log
	 * scraping; BoardStatus is the structured path consumed by the
	 * client's on_domain_msg handler. */
	const char *msg = nullptr;
	bool terminal = false;
	switch (evt) {
	case ROTG_EVENT_ARMED:          msg = "rot:armed";    break;
	case ROTG_EVENT_INVERTED:       msg = "rot:inverted"; break;
	case ROTG_EVENT_RETURNED:       msg = "rot:returned"; terminal = true; break;
	case ROTG_EVENT_CONFIRM_REQ:    msg = "rot:confirm";  break;
	case ROTG_EVENT_SWITCH_CONFIRM: msg = "rot:switch";   terminal = true; break;
	case ROTG_EVENT_CANCELLED:      msg = "rot:cancelled"; terminal = true; break;
	default: break;
	}
	if (msg != nullptr) {
		m_core->sendVerbose(msg);
		sendBoardStatus(
			board_BoardStatusCode_BOARD_STATUS_RUNTIME_SWITCHING,
			board_BoardResultCode_SUCCESS,
			board_ResourceKind_RESOURCE_UNKNOWN,
			0u, 0u, terminal, msg);
	}
}

void BoardModule::dispatchApdsToProfiles(apds9960_gesture_t g)
{
	if (m_profiles == nullptr) return;
	if (m_inputMode == board_InputMode_INPUT_MODE_DISABLED) return;

	prof_src_t src;
	bool pressed = true;
	switch (g) {
	case APDS9960_GESTURE_UP:    src = PROF_SRC_APDS_UP; break;
	case APDS9960_GESTURE_DOWN:  src = PROF_SRC_APDS_DOWN; break;
	case APDS9960_GESTURE_LEFT:  src = PROF_SRC_APDS_LEFT; break;
	case APDS9960_GESTURE_RIGHT: src = PROF_SRC_APDS_RIGHT; break;
	default: return;
	}

	(void)m_profiles->dispatchInput(pressed, src, false);
	(void)m_profiles->dispatchInput(!pressed, src, false);
}

void BoardModule::persistCalibrationResult(MotionManager::CalibTarget target)
{
	if (target == MotionManager::CALIB_NONE) return;

	uint8_t blob[CALIB_BLOB_MAX];
	uint8_t blob_len = 0;
	if (!m_motion.serializeCalibration(target, blob, sizeof(blob),
	                                   &blob_len)) {
		return;
	}

	if (target == MotionManager::CALIB_IMU) {
		(void)m_calib.storeImu((uint8_t)m_calibSensorId,
		                       blob, blob_len);
	} else if (target == MotionManager::CALIB_MAG) {
		(void)m_calib.storeMag((uint8_t)m_calibSensorId,
		                       blob, blob_len);
	}
}

/* ---- Dashboard widget ----------------------------------------------- */
/*
 * The dashboard page renders the active runtime mode and the latest
 * motion summary (quaternion / accel). It is intentionally minimal — a
 * future UI todo will add real layout. For now we register a single
 * widget that the display compositor includes on page 0.
 */

static void dashboard_render(DisplayModule &display,
                             const DisplayRect &clip,
                             void *context)
{
	(void)clip;
	BoardModule *self = static_cast<BoardModule *>(context);
	if (self == nullptr) return;

	const motion_quat_t *q = self->motion().fusion().getQuaternion();
	int32_t q30[4] = {0};
	if (q != nullptr) {
		(void)motion_quat_to_q30(q, q30);
	}

	char buf[32];
	snprintf(buf, sizeof(buf), "q:%ld %ld %ld %ld",
	         (long)q30[0], (long)q30[1], (long)q30[2], (long)q30[3]);
	display.drawText(4, 60, buf, COLOR_WHITE, COLOR_BLACK);

	rotg_state_t rs = self->motion().rotationGesture().getState();
	const char *rs_name = "idle";
	switch (rs) {
	case ROTG_STATE_IDLE:            rs_name = "idle"; break;
	case ROTG_STATE_ARMING:          rs_name = "arming"; break;
	case ROTG_STATE_ARMED:           rs_name = "armed"; break;
	case ROTG_STATE_INVERTED:        rs_name = "flipped"; break;
	case ROTG_STATE_RETURNED:        rs_name = "returned"; break;
	case ROTG_STATE_CONFIRM_PENDING: rs_name = "confirm?"; break;
	}
	snprintf(buf, sizeof(buf), "rot:%s", rs_name);
	display.drawText(4, 72, buf, COLOR_CYAN, COLOR_BLACK);
}

void BoardModule::registerDashboardPages(void)
{
	m_dashboardRegistered = true;
	DisplayModule *disp = m_core->getDisplayModule();
	if (disp == nullptr) return;

	DisplayWidgetConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.bounds = {0, 50, 240, 40};
	cfg.render = &dashboard_render;
	cfg.context = this;
	cfg.z = 0;
	cfg.page = 0;
	WidgetHandle h = disp->registerWidget(cfg);
	if (h != DISPLAY_WIDGET_INVALID) {
		(void)disp->addWidgetToPage(0, h);
	}
}

#endif /* BOARD_CLUE */
