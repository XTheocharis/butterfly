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

#ifdef BOARD_CLUE
#include "ble/profiles.h"
#include "ble/profiles_eval.c"
#include "storage/calib.h"
#include "output/buzzer.h"
#include "expert/i2c.h"
#include "expert/spi.h"
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

/* ---- BoardModule class ------------------------------------------------ */

BoardModule::BoardModule(Core *core)
	: m_core(core)
{
	board_motion_stream_init(&m_streamState);
	board_motion_calib_init(&m_calibState);
	m_audioGainReg = PDM_GAIN_DEFAULT;
	m_audioEnabled = false;
	m_rawPcmRequestId = 0;
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
	Message *resp = messagePoolAllocateMessage(NULL);
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
	Message *resp = messagePoolAllocateMessage(NULL);
	if (resp == NULL) {
		return;
	}

	board_RuntimeConfigResponse cfg;
	memset(&cfg, 0, sizeof(cfg));

	cfg.active_runtime = static_cast<board_RuntimeMode>(
		boardmodule_rt_to_proto(runtime_get_selected()));
	cfg.persisted_runtime = BOARD_RT_UNKNOWN;
	cfg.persistence_available = false;
	cfg.ble_advertising = false;
	cfg.ble_pairable = false;
	cfg.ble_connected = false;
	cfg.bond_count = 0;
	cfg.event_log_filter = 0;
	cfg.raw_packet_log_filter = 0;

	whad_board_runtime_config(resp, requestId, &cfg);
	m_core->pushMessageToQueue(resp);
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

	uint32_t code = boardmodule_eval_set_runtime_config(
		req.which_operation, hasPersistedRuntime);

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
	Message *resp = messagePoolAllocateMessage(NULL);
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

	Message *resp = messagePoolAllocateMessage(NULL);
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
	} else if (req.sensor_id == info->sensor_id) {
		/* Non-motion sensors (env/color/proximity/gesture/audio):
		 * no driver wired in this todo — STALE zeros is correct. */
		status = board_SensorStatusFlag_SENSOR_STATUS_STALE;
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

	Message *resp = messagePoolAllocateMessage(NULL);
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
	Message *resp = messagePoolAllocateMessage(NULL);
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

	Message *resp = messagePoolAllocateMessage(NULL);
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
	/* Try loading from persisted storage if adopted (Todo 30).
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
			loaded = false; /* loadImu via manager when wired */
		} else if (kind == CALIB_KIND_MAG) {
			loaded = false; /* loadMag via manager when wired */
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
	Message *resp = messagePoolAllocateMessage(NULL);
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
	Message *resp = messagePoolAllocateMessage(NULL);
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

/* === INPUT/PROFILE HANDLER IMPLEMENTATIONS (Todo 23) === */

void BoardModule::handleGetInputState(uint32_t requestId)
{
	Message *resp = messagePoolAllocateMessage(NULL);
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
	state.mode = (runtime_get_selected() == RUNTIME_BLE_HID)
	             ? board_InputMode_INPUT_MODE_GESTURE
	             : board_InputMode_INPUT_MODE_REMOTE;
#else
	state.buttons = 0;
	state.gesture = board_Gesture_GESTURE_UNKNOWN;
	state.microphone_threshold = false;
	state.mode = board_InputMode_INPUT_MODE_REMOTE;
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
	}

	sendCommandResult(requestId,
	                  board_BoardCommand_ConfigureInput,
	                  code);
}

void BoardModule::handleRemoteProfileGet(uint32_t requestId,
                                         const board_RemoteProfileGetRequest &req)
{
	Message *resp = messagePoolAllocateMessage(NULL);
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

	Message *resp = messagePoolAllocateMessage(NULL);
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

void BoardModule::handleRawPcmDiagnostics(uint32_t requestId,
                                          const board_RawPcmDiagnosticsRequest &req)
{
	runtime_mode_t mode = runtime_get_selected();

	static pdm_pcm_state_t pcmState;
	pdm_pcm_init(&pcmState);

	/* radio_idle: BoardModule has no direct radio-state accessor; pass true
	 * (conservative) so the eval layer gates solely on RAW_WHAD mode. A future
	 * todo wiring Core's Radio state can refine this to reject PCM while the
	 * radio is actively sniffing. */
	uint32_t code = pdm_pcm_start(&pcmState, mode, /*radio_idle=*/true, requestId,
	                              req.duration_ms, req.chunk_size);

	if (code != board_BoardResultCode_SUCCESS) {
		sendCommandResult(requestId,
			board_BoardCommand_RawPcmDiagnostics,
			(board_BoardResultCode)code);
		return;
	}

	m_rawPcmRequestId = requestId;

	/* Send nonterminal "accepted" CommandResult, then chunks.
	 * In this implementation, we have no live PCM capture buffer (PDM
	 * driver is not wired in this todo). Send an empty chunk sequence
	 * with eof=true to indicate the diagnostics completed (no data).
	 * When the PDM firmware driver is integrated, this will stream
	 * real captured PCM in ≤40-byte chunks. */
	Message *accepted = messagePoolAllocateMessage(NULL);
	if (accepted != NULL) {
		board_CommandResult cr;
		memset(&cr, 0, sizeof(cr));
		cr.command = board_BoardCommand_RawPcmDiagnostics;
		cr.result = board_BoardResultCode_SUCCESS;
		cr.terminal = false;
		whad_board_command_result(accepted, requestId, &cr);
		m_core->pushMessageToQueue(accepted);
	}

	/* Final chunk with eof=true, empty pcm, total=0. */
	Message *finalChunk = messagePoolAllocateMessage(NULL);
	if (finalChunk != NULL) {
		board_AudioChunk chunk;
		memset(&chunk, 0, sizeof(chunk));
		chunk.sequence = 0;
		chunk.offset = 0;
		chunk.count = 0;
		chunk.pcm.size = 0;
		chunk.eof = true;
		chunk.total = 0;
		chunk.result = board_BoardResultCode_SUCCESS;

		whad_board_audio_chunk(finalChunk, requestId, &chunk);
		m_core->pushMessageToQueue(finalChunk);
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
	static Buzzer s_buzzer;
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
		Message *resp = messagePoolAllocateMessage(NULL);
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

	Message *resp = messagePoolAllocateMessage(NULL);
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
	Message *resp = messagePoolAllocateMessage(NULL);
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
		Message *resp = messagePoolAllocateMessage(NULL);
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

	Message *resp = messagePoolAllocateMessage(NULL);
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
	Message *resp = messagePoolAllocateMessage(NULL);
	if (resp != NULL) {
		board_SpiTransferResponse r = {};
		r.result = board_BoardResultCode_NOT_IMPLEMENTED;
		whad_board_spi_result(resp, requestId, &r);
		m_core->pushMessageToQueue(resp);
	}
#endif
}

void BoardModule::handleReleasePin(uint32_t requestId,
                                   const board_ReleasePinRequest &req)
{
	(void)req;

	sendCommandResult(requestId,
		board_BoardCommand_ReleasePin,
		board_BoardResultCode_SUCCESS);
}

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
#else
	(void)g;
#endif
}

void BoardModule::injectButtons(bool button_a, bool button_b)
{
#ifdef BOARD_CLUE
	m_motion.feedButtons(button_a, button_b);
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

	/* Advance async calibration if active. */
	if (m_motion.isCalibrationBusy()) {
		uint8_t progress = 0;
		/* Calibration is driven by injected IMU/mag samples already
		 * cached inside MotionManager — pass nullptr to use the most
		 * recent cached samples via the manager's own feed path. */
		MotionManager::CalibResult cr =
		    m_motion.tickCalibration(nullptr, nullptr, &progress);
		uint32_t now_ms = (uint32_t)(now_us / 1000ull);
		bool emit_progress = ((now_ms - m_calibEmitMs) >= 500u);
		if (cr == MotionManager::CALIB_RESULT_DONE_OK) {
			board_motion_calib_complete(&m_calibState);
			sendCommandResult(m_calibRequestId,
			                  board_BoardCommand_Calibrate,
			                  board_BoardResultCode_SUCCESS);
			m_calibEmitMs = 0;
		} else if (cr == MotionManager::CALIB_RESULT_FAILED) {
			board_motion_calib_complete(&m_calibState);
			sendCommandResult(m_calibRequestId,
			                  board_BoardCommand_Calibrate,
			                  board_BoardResultCode_SENSOR_FAULT);
			m_calibEmitMs = 0;
		} else if (emit_progress) {
			m_calibEmitMs = now_ms;
			Message *resp = messagePoolAllocateMessage(NULL);
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
		}
	}

	/* Active stream sampling (rate-limited internally). */
	emitStreamSamples();

	/* Register dashboard once on first tick when the display is up. */
	if (!m_dashboardRegistered) {
		registerDashboardPages();
	}
#else
	(void)0;
#endif
}

#ifdef BOARD_CLUE

void BoardModule::publishRotationState(rotg_event_t evt)
{
	/* For now: surface to the host via a Verbose notification on the
	 * most significant events. A future todo (Todo 14 / 21) routes this
	 * to a proper BoardStatus event and HIDS consumer key. */
	const char *msg = nullptr;
	switch (evt) {
	case ROTG_EVENT_ARMED:        msg = "rot:armed"; break;
	case ROTG_EVENT_INVERTED:     msg = "rot:inverted"; break;
	case ROTG_EVENT_RETURNED:     msg = "rot:returned"; break;
	case ROTG_EVENT_CONFIRM_REQ:  msg = "rot:confirm"; break;
	case ROTG_EVENT_SWITCH_CONFIRM: msg = "rot:switch"; break;
	case ROTG_EVENT_CANCELLED:    msg = "rot:cancelled"; break;
	default: break;
	}
	if (msg != nullptr) {
		m_core->sendVerbose(msg);
	}
}

void BoardModule::dispatchApdsToProfiles(apds9960_gesture_t g)
{
	if (m_profiles == nullptr) return;

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
