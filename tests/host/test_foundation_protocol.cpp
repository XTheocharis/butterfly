/*
 * test_foundation_protocol.cpp - Board domain nanopb round-trip foundation tests.
 *
 * Category (a): encode → decode → C-API parse for representative messages
 * across all Board categories (requests, responses, results, events), plus
 * manifest integrity, request_id correlation, encoded-size bounds, validator
 * rejection, and domain routing.
 *
 * Links nanopb core + generated protobuf + whad-lib Board C API directly.
 * Provides a local whad_get_message_domain stub to avoid the transport chain.
 */
#include "test_framework.h"

#include <string.h>

/* whad-lib sub-headers only — avoid whad.h which pulls cpp/whad.hpp. */
#include "types.h"
#include "transport.h"
#include "discovery.h"
#include "domains/board.h"

/* ---- Local stub: whad_get_message_domain -------------------------------
 * board.c's unpack path calls whad_get_message_domain. We provide a minimal
 * implementation so we don't need to link the full whad.c → transport chain. */
extern "C" whad_domain_t whad_get_message_domain(Message *p_msg) {
    switch (p_msg->which_msg) {
        case Message_board_tag:   return DOMAIN_BOARD;
        case Message_ble_tag:     return DOMAIN_BTLE;
        case Message_esb_tag:     return DOMAIN_ESB;
        case Message_phy_tag:     return DOMAIN_PHY;
        case Message_dot15d4_tag: return DOMAIN_DOT15D4;
        default:                  return DOMAIN_NONE;
    }
}

/* ---- Encode/decode helpers -------------------------------------------- */

static const size_t ENC_BUF_SIZE = 2048;

static size_t encode_msg(Message *msg, uint8_t *buf, size_t bufsize) {
    pb_ostream_t os = pb_ostream_from_buffer(buf, bufsize);
    if (!pb_encode(&os, Message_fields, msg)) return 0;
    return os.bytes_written;
}

static bool decode_msg(const uint8_t *buf, size_t len, Message *out) {
    pb_istream_t is = pb_istream_from_buffer(buf, len);
    return pb_decode(&is, Message_fields, out);
}

/* Round-trip helper: populate → encode → decode → parse.
 * Verifies encoded size fits transport and request_id survives. */
#define ROUND_TRIP(name, req_id, src_value, dst_value) \
    do { \
        Message msg = {}; \
        Message decoded = {}; \
        uint8_t buf[ENC_BUF_SIZE]; \
        TEST_ASSERT_EQ_INT(WHAD_SUCCESS, \
            (int)whad_board_##name(&msg, (req_id), &(src_value))); \
        size_t sz = encode_msg(&msg, buf, ENC_BUF_SIZE); \
        TEST_ASSERT(sz > 0, "encode produced bytes"); \
        TEST_ASSERT(sz <= WHAD_MAX_ENCODED_MESSAGE_SIZE, \
                    "encoded message fits framed payload"); \
        TEST_ASSERT(decode_msg(buf, sz, &decoded), "decode succeeds"); \
        uint32_t out_id = 0xDEAD; \
        TEST_ASSERT_EQ_INT(WHAD_SUCCESS, \
            (int)whad_board_##name##_parse(&decoded, &out_id, &(dst_value))); \
        TEST_ASSERT_EQ_INT((int)(req_id), (int)out_id); \
    } while (0)

/* ---- Tests: manifest integrity ---------------------------------------- */

static void test_root_message_tag_is_8(void) {
    TEST_ASSERT_EQ_INT(8, (int)Message_board_tag);
}

static void test_domain_board_value_is_0x0C000000(void) {
    TEST_ASSERT_EQ_INT(0x0C000000, (int)DOMAIN_BOARD);
    TEST_ASSERT_EQ_INT(201326592, (int)DOMAIN_BOARD);
}

static void test_board_command_enum_has_28_values(void) {
    /* _board_BoardCommand_MAX == RawPcmDiagnostics == 27, so 28 values 0..27 */
    TEST_ASSERT_EQ_INT(0, (int)_board_BoardCommand_MIN);
    TEST_ASSERT_EQ_INT(27, (int)_board_BoardCommand_MAX);
    TEST_ASSERT_EQ_INT(28, (int)_board_BoardCommand_ARRAYSIZE);
}

static void test_board_result_code_includes_not_implemented(void) {
    TEST_ASSERT_EQ_INT(14, (int)board_BoardResultCode_NOT_IMPLEMENTED);
    TEST_ASSERT_EQ_INT(14, (int)_board_BoardResultCode_MAX);
}

static void test_transport_constants_imported_not_hardcoded(void) {
    /* WHAD_RINGBUF_MAX_SIZE=1024, capacity=1023, header=4, max_msg=1019 */
    TEST_ASSERT_EQ_INT(1019, (int)WHAD_MAX_ENCODED_MESSAGE_SIZE);
    TEST_ASSERT_EQ_INT(4, (int)WHAD_TRANSPORT_FRAME_HEADER_SIZE);
    TEST_ASSERT_EQ_INT(1023, (int)WHAD_RINGBUF_CAPACITY);
}

/* ---- Tests: round-trip for representative requests -------------------- */

static void test_get_board_info_request_round_trip(void) {
    board_GetBoardInfoRequest src = {};
    board_GetBoardInfoRequest dst = {};
    ROUND_TRIP(get_board_info, 42, src, dst);
}

static void test_read_sensor_request_round_trip(void) {
    board_ReadSensorRequest src = {};
    board_ReadSensorRequest dst = {};
    src.sensor_id = 3;
    ROUND_TRIP(read_sensor, 7, src, dst);
    TEST_ASSERT_EQ_INT(3, (int)dst.sensor_id);
}

static void test_configure_stream_request_round_trip(void) {
    board_ConfigureStreamRequest src = {};
    board_ConfigureStreamRequest dst = {};
    src.sensor_id = 1;
    src.rate_millihz = 50000;
    src.flags = 0x01;
    ROUND_TRIP(configure_stream, 100, src, dst);
    TEST_ASSERT_EQ_INT(1, (int)dst.sensor_id);
    TEST_ASSERT_EQ_INT(50000, (int)dst.rate_millihz);
    TEST_ASSERT_EQ_INT(1, (int)dst.flags);
}

static void test_set_runtime_mode_request_round_trip(void) {
    board_SetRuntimeModeRequest src = {};
    board_SetRuntimeModeRequest dst = {};
    src.runtime = board_RuntimeMode_RUNTIME_BLE_HID;
    src.persist = true;
    src.reboot = true;
    ROUND_TRIP(set_runtime_mode, 99, src, dst);
    TEST_ASSERT_EQ_INT((int)board_RuntimeMode_RUNTIME_BLE_HID,
                       (int)dst.runtime);
    TEST_ASSERT(dst.persist, "persist survives round-trip");
    TEST_ASSERT(dst.reboot, "reboot survives round-trip");
}

static void test_calibrate_request_round_trip(void) {
    board_CalibrateRequest src = {};
    board_CalibrateRequest dst = {};
    src.sensor_id = 5;
    src.flags = 0x02;
    src.persist = true;
    ROUND_TRIP(calibrate, 55, src, dst);
    TEST_ASSERT_EQ_INT(5, (int)dst.sensor_id);
    TEST_ASSERT_EQ_INT(2, (int)dst.flags);
    TEST_ASSERT(dst.persist, "persist survives");
}

/* ---- Tests: round-trip for representative responses ------------------ */

static void test_board_info_response_round_trip(void) {
    board_GetBoardInfoResponse src = {};
    board_GetBoardInfoResponse dst = {};
    strcpy(src.board_name, "ButteRFly CLUE");
    strcpy(src.hardware_revision, "rev B");
    strcpy(src.firmware_version, "1.0.0-clue");
    strcpy(src.protocol_variant, "whad-board");
    src.device_id.size = 4;
    memcpy(src.device_id.bytes, "\xDE\xAD\xBE\xEF", 4);
    src.active_runtime = board_RuntimeMode_RUNTIME_RAW_WHAD;
    src.implemented_sensor_count = 14;
    ROUND_TRIP(board_info, 200, src, dst);
    TEST_ASSERT(strcmp(dst.board_name, "ButteRFly CLUE") == 0,
                "board_name matches");
    TEST_ASSERT(strcmp(dst.firmware_version, "1.0.0-clue") == 0,
                "firmware_version matches");
    TEST_ASSERT_EQ_INT(4, (int)dst.device_id.size);
    TEST_ASSERT_EQ_INT(0xDE, (int)dst.device_id.bytes[0]);
    TEST_ASSERT_EQ_INT((int)board_RuntimeMode_RUNTIME_RAW_WHAD,
                       (int)dst.active_runtime);
    TEST_ASSERT_EQ_INT(14, (int)dst.implemented_sensor_count);
}

static void test_command_result_round_trip(void) {
    board_CommandResult src = {};
    board_CommandResult dst = {};
    src.command = board_BoardCommand_ReadSensor;
    src.result = board_BoardResultCode_SUCCESS;
    src.terminal = true;
    strcpy(src.detail, "sensor read ok");
    ROUND_TRIP(command_result, 77, src, dst);
    TEST_ASSERT_EQ_INT((int)board_BoardCommand_ReadSensor,
                       (int)dst.command);
    TEST_ASSERT_EQ_INT((int)board_BoardResultCode_SUCCESS,
                       (int)dst.result);
    TEST_ASSERT(dst.terminal, "terminal flag survives");
    TEST_ASSERT(strcmp(dst.detail, "sensor read ok") == 0,
                "detail string matches");
}

static void test_adc_value_response_round_trip(void) {
    board_AdcReadResponse src = {};
    board_AdcReadResponse dst = {};
    src.result = board_BoardResultCode_SUCCESS;
    src.channel = 2;
    src.millivolts = 3300;
    src.raw = 0x0ABC;
    ROUND_TRIP(adc_value, 33, src, dst);
    TEST_ASSERT_EQ_INT(2, (int)dst.channel);
    TEST_ASSERT_EQ_INT(3300, (int)dst.millivolts);
    TEST_ASSERT_EQ_INT(0x0ABC, (int)dst.raw);
}

/* ---- Tests: round-trip for events ------------------------------------ */

static void test_sensor_sample_round_trip(void) {
    board_SensorSample src = {};
    board_SensorSample dst = {};
    src.sensor_id = 1;
    src.sequence = 42;
    src.timestamp_us = 1234567890ULL;
    src.status = board_SensorStatusFlag_SENSOR_STATUS_CALIBRATED;
    src.values_count = 3;
    src.values[0] = 100;
    src.values[1] = -200;
    src.values[2] = 300;
    ROUND_TRIP(sensor_sample, 0, src, dst);
    TEST_ASSERT_EQ_INT(1, (int)dst.sensor_id);
    TEST_ASSERT_EQ_INT(42, (int)dst.sequence);
    TEST_ASSERT_EQ_INT(3, (int)dst.values_count);
    TEST_ASSERT_EQ_INT(100, (int)dst.values[0]);
    TEST_ASSERT_EQ_INT(-200, (int)dst.values[1]);
    TEST_ASSERT_EQ_INT(1234567890LL, (long long)dst.timestamp_us);
}

static void test_board_status_event_round_trip(void) {
    board_BoardStatus src = {};
    board_BoardStatus dst = {};
    src.timestamp_us = 9999999ULL;
    src.code = board_BoardStatusCode_BOARD_STATUS_RESOURCE_DISPLACED;
    src.result = board_BoardResultCode_BUSY;
    src.resource = board_ResourceKind_RESOURCE_GPIO_PIN;
    src.instance = 17;
    src.progress_per_mille = 500;
    src.terminal = false;
    strcpy(src.detail, "pin displaced by BLE");
    ROUND_TRIP(board_status, 0, src, dst);
    TEST_ASSERT_EQ_INT(
        (int)board_BoardStatusCode_BOARD_STATUS_RESOURCE_DISPLACED,
        (int)dst.code);
    TEST_ASSERT_EQ_INT(17, (int)dst.instance);
    TEST_ASSERT_EQ_INT(500, (int)dst.progress_per_mille);
    TEST_ASSERT(strcmp(dst.detail, "pin displaced by BLE") == 0,
                "detail survives");
}

static void test_audio_chunk_event_round_trip(void) {
    board_AudioChunk src = {};
    board_AudioChunk dst = {};
    src.sequence = 5;
    src.offset = 0;
    src.count = 64;
    src.pcm.size = 64;
    memset(src.pcm.bytes, 0xAA, 64);
    src.eof = false;
    src.total = 1024;
    src.result = board_BoardResultCode_SUCCESS;
    ROUND_TRIP(audio_chunk, 0, src, dst);
    TEST_ASSERT_EQ_INT(5, (int)dst.sequence);
    TEST_ASSERT_EQ_INT(64, (int)dst.pcm.size);
    TEST_ASSERT_EQ_INT(0xAA, (int)dst.pcm.bytes[0]);
    TEST_ASSERT_EQ_INT(0xAA, (int)dst.pcm.bytes[63]);
    TEST_ASSERT(!dst.eof, "eof false survives");
}

/* ---- Tests: largest encoded message fits transport -------------------- */

static void test_max_audio_chunk_fits_transport(void) {
    board_AudioChunk chunk = {};
    chunk.sequence = 1;
    chunk.count = 800;
    chunk.pcm.size = 800;
    memset(chunk.pcm.bytes, 0xFF, 800);
    chunk.eof = true;
    chunk.total = 800;
    chunk.result = board_BoardResultCode_SUCCESS;

    Message msg = {};
    TEST_ASSERT_EQ_INT(WHAD_SUCCESS,
        (int)whad_board_audio_chunk(&msg, 0, &chunk));
    uint8_t buf[ENC_BUF_SIZE];
    size_t sz = encode_msg(&msg, buf, ENC_BUF_SIZE);
    TEST_ASSERT(sz > 0, "800-byte AudioChunk encodes");
    TEST_ASSERT(sz <= WHAD_MAX_ENCODED_MESSAGE_SIZE,
                "max AudioChunk fits within framed payload");
}

static void test_max_log_chunk_fits_transport(void) {
    board_LogChunk chunk = {};
    chunk.sequence = 1;
    chunk.count = 900;
    chunk.data.size = 900;
    memset(chunk.data.bytes, 0xBB, 900);
    chunk.eof = true;
    chunk.total = 900;
    chunk.result = board_BoardResultCode_SUCCESS;

    Message msg = {};
    TEST_ASSERT_EQ_INT(WHAD_SUCCESS,
        (int)whad_board_log_chunk(&msg, 0, &chunk));
    uint8_t buf[ENC_BUF_SIZE];
    size_t sz = encode_msg(&msg, buf, ENC_BUF_SIZE);
    TEST_ASSERT(sz > 0, "900-byte LogChunk encodes");
    TEST_ASSERT(sz <= WHAD_MAX_ENCODED_MESSAGE_SIZE,
                "max LogChunk fits within framed payload");
}

/* ---- Tests: request_id correlation across interleaved messages ------- */

static void test_request_id_correlation_survives_interleave(void) {
    /* Two different board messages with distinct request_ids — verify
     * each round-trips independently without cross-contamination. */
    board_ReadSensorRequest r1 = {};
    r1.sensor_id = 1;
    board_CommandResult r2 = {};
    r2.command = board_BoardCommand_ReadSensor;
    r2.result = board_BoardResultCode_SUCCESS;
    r2.terminal = true;

    Message m1 = {}, m2 = {};
    whad_board_read_sensor(&m1, 10, &r1);
    whad_board_command_result(&m2, 20, &r2);

    uint8_t b1[ENC_BUF_SIZE], b2[ENC_BUF_SIZE];
    size_t s1 = encode_msg(&m1, b1, ENC_BUF_SIZE);
    size_t s2 = encode_msg(&m2, b2, ENC_BUF_SIZE);
    TEST_ASSERT(s1 > 0 && s2 > 0, "both encode");

    Message d1 = {}, d2 = {};
    TEST_ASSERT(decode_msg(b1, s1, &d1), "decode m1");
    TEST_ASSERT(decode_msg(b2, s2, &d2), "decode m2");

    uint32_t id1 = 0, id2 = 0;
    board_ReadSensorRequest out1 = {};
    board_CommandResult out2 = {};
    TEST_ASSERT_EQ_INT(WHAD_SUCCESS,
        (int)whad_board_read_sensor_parse(&d1, &id1, &out1));
    TEST_ASSERT_EQ_INT(WHAD_SUCCESS,
        (int)whad_board_command_result_parse(&d2, &id2, &out2));
    TEST_ASSERT_EQ_INT(10, (int)id1);
    TEST_ASSERT_EQ_INT(20, (int)id2);
    TEST_ASSERT_EQ_INT(1, (int)out1.sensor_id);
}

/* ---- Tests: domain routing ------------------------------------------- */

static void test_board_message_routes_to_domain_board(void) {
    board_ReadSensorRequest req = {};
    req.sensor_id = 1;
    Message msg = {};
    whad_board_read_sensor(&msg, 1, &req);
    TEST_ASSERT_EQ_INT((int)DOMAIN_BOARD,
                       (int)whad_get_message_domain(&msg));
}

static void test_non_board_message_does_not_route_to_board(void) {
    Message msg = {};
    msg.which_msg = Message_ble_tag;
    TEST_ASSERT((int)whad_get_message_domain(&msg) != (int)DOMAIN_BOARD,
                "BLE message is not DOMAIN_BOARD");
}

/* ---- Tests: validator rejection -------------------------------------- */

static void test_validator_rejects_unknown_command_enum(void) {
    board_CommandResult bad = {};
    bad.command = (board_BoardCommand)99;
    bad.result = board_BoardResultCode_SUCCESS;
    bad.terminal = true;
    bad.detail[0] = '\0';
    Message msg = {};
    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_board_command_result(&msg, 1, &bad));
}

static void test_validator_rejects_unknown_result_enum(void) {
    board_CommandResult bad = {};
    bad.command = board_BoardCommand_ReadSensor;
    bad.result = (board_BoardResultCode)99;
    bad.terminal = true;
    bad.detail[0] = '\0';
    Message msg = {};
    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_board_command_result(&msg, 1, &bad));
}

static void test_validator_rejects_runtime_mode_enum(void) {
    board_SetRuntimeModeRequest bad = {};
    bad.runtime = (board_RuntimeMode)99;
    bad.persist = false;
    bad.reboot = false;
    Message msg = {};
    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_board_set_runtime_mode(&msg, 1, &bad));
}

static void test_unpack_rejects_wrong_tag(void) {
    /* Populate a ReadSensor request, try to parse as GetBoardInfoResponse. */
    board_ReadSensorRequest req = {};
    req.sensor_id = 1;
    Message msg = {};
    whad_board_read_sensor(&msg, 5, &req);

    board_GetBoardInfoResponse out = {};
    uint32_t id = 0;
    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_board_board_info_parse(&msg, &id, &out));
}

static void test_unpack_rejects_null_message(void) {
    board_ReadSensorRequest out = {};
    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_board_read_sensor_parse(NULL, NULL, &out));
}

/* ---- Tests: command-to-message-type mapping -------------------------- */

static void test_command_from_message_type_mapping(void) {
    /* Spot-check several representative command mappings. */
    board_BoardCommand cmd = (board_BoardCommand)0xFF;

    TEST_ASSERT_EQ_INT(WHAD_SUCCESS,
        (int)whad_board_command_from_message_type(
            WHAD_BOARD_READ_SENSOR, &cmd));
    TEST_ASSERT_EQ_INT((int)board_BoardCommand_ReadSensor, (int)cmd);

    TEST_ASSERT_EQ_INT(WHAD_SUCCESS,
        (int)whad_board_command_from_message_type(
            WHAD_BOARD_SET_RUNTIME_MODE, &cmd));
    TEST_ASSERT_EQ_INT((int)board_BoardCommand_SetRuntimeMode, (int)cmd);

    TEST_ASSERT_EQ_INT(WHAD_ERROR,
        (int)whad_board_command_from_message_type(
            WHAD_BOARD_SENSOR_SAMPLE, &cmd));
}

/* ---- Tests: capability definitions present --------------------------- */

static void test_board_capabilities_defined(void) {
    /* Verify the 4 Board capability aliases resolve to discovery values. */
    TEST_ASSERT_EQ_INT((int)CAP_READ, (int)CAP_BOARD_READ);
    TEST_ASSERT_EQ_INT((int)CAP_WRITE, (int)CAP_BOARD_WRITE);
    TEST_ASSERT_EQ_INT((int)CAP_STREAM, (int)CAP_BOARD_STREAM);
    TEST_ASSERT_EQ_INT((int)CAP_STORE, (int)CAP_BOARD_STORE);
}

/* ---- Tests: transport frame boundary conditions ---------------------- */
/* Exercise whad-lib transport.c + ringbuf.c at the frame edge cases that
 * a malformed or malicious peer could trigger. */

static void transport_test_init(void)
{
    /* Reset the static transport with a NULL send callback; tests don't
     * actually emit bytes over UART. */
    whad_transport_cfg_t cfg;
    cfg.max_txbuf_size = WHAD_RINGBUF_MAX_SIZE;
    cfg.pfn_data_send_buffer = NULL;
    whad_transport_init(&cfg);
}

static void test_transport_max_size_frame_round_trips(void)
{
    /* Frame at exactly WHAD_MAX_ENCODED_MESSAGE_SIZE (1019) must encode.
     * 4-byte header + 1019-byte payload = 1023 = full ring buffer capacity. */
    transport_test_init();

    static uint8_t payload[WHAD_MAX_ENCODED_MESSAGE_SIZE];
    memset(payload, 0xAB, sizeof(payload));

    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_send_message(payload, WHAD_MAX_ENCODED_MESSAGE_SIZE));

    TEST_ASSERT_EQ_INT(WHAD_RINGBUF_CAPACITY,
        whad_transport_get_txbuf_size());
}

static void test_transport_oversized_frame_rejected(void)
{
    /* Frame at WHAD_MAX_ENCODED_MESSAGE_SIZE + 1 (1020) must be rejected
     * by the encoder before any byte touches the TX ring buffer. */
    transport_test_init();

    static uint8_t payload[WHAD_MAX_ENCODED_MESSAGE_SIZE + 1];
    memset(payload, 0xCD, sizeof(payload));

    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_transport_send_message(
            payload, WHAD_MAX_ENCODED_MESSAGE_SIZE + 1));

    /* TX buffer must remain untouched. */
    TEST_ASSERT_EQ_INT(0, whad_transport_get_txbuf_size());
}

static void test_transport_partial_frame_waits(void)
{
    /* Magic + length header only, no payload: parser must wait, not
     * treat it as a complete frame and not corrupt the buffer. */
    transport_test_init();

    uint8_t header[WHAD_TRANSPORT_FRAME_HEADER_SIZE] =
        {0xAC, 0xBE, 10, 0};
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_data_received(header, sizeof(header)));

    uint8_t out[64];
    int sz = sizeof(out);
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_get_message(out, &sz));
    TEST_ASSERT_EQ_INT(0, sz);

    /* Header stays in RX buffer waiting for the rest of the frame. */
    TEST_ASSERT_EQ_INT(WHAD_TRANSPORT_FRAME_HEADER_SIZE,
        whad_transport_get_rxbuf_size());
}

static void test_transport_invalid_magic_skipped(void)
{
    /* Two non-magic bytes followed by a valid frame. The parser must
     * skip the garbage bytes via the bad-magic resync path and decode
     * the valid frame on a subsequent call. */
    transport_test_init();

    uint8_t buf[] = {
        0x00, 0x00,        /* bad magic: header[1] != 0xAC → skip 2 */
        0xAC, 0xBE, 2, 0,  /* good magic, length = 2 */
        0xAA, 0xBB         /* payload */
    };
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_data_received(buf, sizeof(buf)));

    uint8_t out[16] = {0};
    int sz = sizeof(out);
    /* First call sees the bad magic, skips 2 bytes, falls through. */
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_get_message(out, &sz));
    TEST_ASSERT_EQ_INT(0, sz);

    /* Second call lands on the good magic and decodes the frame. */
    sz = sizeof(out);
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_get_message(out, &sz));
    TEST_ASSERT_EQ_INT(2, sz);
    TEST_ASSERT_EQ_INT(0xAA, (int)out[0]);
    TEST_ASSERT_EQ_INT(0xBB, (int)out[1]);

    /* RX buffer is drained. */
    TEST_ASSERT_EQ_INT(0, whad_transport_get_rxbuf_size());
}

static void test_transport_ringbuf_wraparound_preserves_data(void)
{
    /* Drive the underlying ring buffer through a wrap boundary and verify
     * that whad_ringbuf_copy (used by get_message to extract a frame)
     * preserves byte order across the wrap. */
    whad_ringbuf_t rb;
    whad_ringbuf_init(&rb);

    /* Phase 1: push near MAX_SIZE, then drain to advance tail near end. */
    for (int i = 0; i < WHAD_RINGBUF_MAX_SIZE - 2; ++i) {
        TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
            (int)whad_ringbuf_push(&rb, (uint8_t)(i & 0xFF)));
    }
    uint8_t sink;
    for (int i = 0; i < WHAD_RINGBUF_MAX_SIZE - 2; ++i) {
        TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
            (int)whad_ringbuf_pull(&rb, &sink));
    }

    /* Phase 2: a 5-byte write must wrap from data[1022..1023] to data[0..2]. */
    uint8_t expect[5] = {0xD0, 0xD1, 0xD2, 0xD3, 0xD4};
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
            (int)whad_ringbuf_push(&rb, expect[i]));
    }

    /* Phase 3: copy (non-destructive read) must return them in order. */
    uint8_t out[5] = {0};
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_ringbuf_copy(&rb, out, 5));
    TEST_ASSERT_EQ_INT(0, memcmp(expect, out, 5));
}

static void test_transport_malicious_length_rejected(void)
{
    /* 0xFFFF length is rejected; resync skips 1 byte and the parser
     * eventually re-locks on the next valid magic without buffer overrun. */
    transport_test_init();

    uint8_t bad[] = {0xAC, 0xBE, 0xFF, 0xFF};
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_data_received(bad, sizeof(bad)));

    uint8_t out[16] = {0};
    int sz = sizeof(out);
    TEST_ASSERT_EQ_INT((int)WHAD_ERROR,
        (int)whad_transport_get_message(out, &sz));
    TEST_ASSERT_EQ_INT(0, sz);

    /* Resync skipped exactly 1 byte (0xAC). No overrun occurred. */
    TEST_ASSERT_EQ_INT(3, whad_transport_get_rxbuf_size());

    /* Feed a follow-up valid frame; the parser must eventually find it
     * by scanning for the next 0xAC 0xBE pair. */
    uint8_t next[] = {0xAC, 0xBE, 1, 0, 0x42};
    TEST_ASSERT_EQ_INT((int)WHAD_SUCCESS,
        (int)whad_transport_data_received(next, sizeof(next)));

    int decoded = 0;
    for (int i = 0; i < 16; ++i) {
        sz = sizeof(out);
        whad_result_t r = whad_transport_get_message(out, &sz);
        if (r == WHAD_SUCCESS && sz > 0) {
            TEST_ASSERT_EQ_INT(1, sz);
            TEST_ASSERT_EQ_INT(0x42, (int)out[0]);
            decoded = 1;
            break;
        }
        if (whad_transport_get_rxbuf_size() <
            WHAD_TRANSPORT_FRAME_HEADER_SIZE) {
            break;
        }
    }
    TEST_ASSERT_EQ_INT(1, decoded);
}

/* ---- Test runner ------------------------------------------------------ */

int main(void) {
    test_framework_init();

    /* Manifest */
    RUN_TEST(test_root_message_tag_is_8);
    RUN_TEST(test_domain_board_value_is_0x0C000000);
    RUN_TEST(test_board_command_enum_has_28_values);
    RUN_TEST(test_board_result_code_includes_not_implemented);
    RUN_TEST(test_transport_constants_imported_not_hardcoded);

    /* Round-trip: requests */
    RUN_TEST(test_get_board_info_request_round_trip);
    RUN_TEST(test_read_sensor_request_round_trip);
    RUN_TEST(test_configure_stream_request_round_trip);
    RUN_TEST(test_set_runtime_mode_request_round_trip);
    RUN_TEST(test_calibrate_request_round_trip);

    /* Round-trip: responses */
    RUN_TEST(test_board_info_response_round_trip);
    RUN_TEST(test_command_result_round_trip);
    RUN_TEST(test_adc_value_response_round_trip);

    /* Round-trip: events */
    RUN_TEST(test_sensor_sample_round_trip);
    RUN_TEST(test_board_status_event_round_trip);
    RUN_TEST(test_audio_chunk_event_round_trip);

    /* Transport size bounds */
    RUN_TEST(test_max_audio_chunk_fits_transport);
    RUN_TEST(test_max_log_chunk_fits_transport);

    /* request_id correlation */
    RUN_TEST(test_request_id_correlation_survives_interleave);

    /* Domain routing */
    RUN_TEST(test_board_message_routes_to_domain_board);
    RUN_TEST(test_non_board_message_does_not_route_to_board);

    /* Validator rejection */
    RUN_TEST(test_validator_rejects_unknown_command_enum);
    RUN_TEST(test_validator_rejects_unknown_result_enum);
    RUN_TEST(test_validator_rejects_runtime_mode_enum);
    RUN_TEST(test_unpack_rejects_wrong_tag);
    RUN_TEST(test_unpack_rejects_null_message);

    /* Command mapping */
    RUN_TEST(test_command_from_message_type_mapping);

    /* Capabilities */
    RUN_TEST(test_board_capabilities_defined);

    /* Transport frame boundary conditions */
    RUN_TEST(test_transport_max_size_frame_round_trips);
    RUN_TEST(test_transport_oversized_frame_rejected);
    RUN_TEST(test_transport_partial_frame_waits);
    RUN_TEST(test_transport_invalid_magic_skipped);
    RUN_TEST(test_transport_ringbuf_wraparound_preserves_data);
    RUN_TEST(test_transport_malicious_length_rejected);

    return test_framework_finish();
}
