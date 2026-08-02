/*
 * test_pass.c - reference suite that exercises the framework and every mock.
 *
 * Proves the harness exits 0 when every assertion holds, and demonstrates
 * the calling convention every future test file should follow. Later
 * todos (CRC32, Madgwick fusion, QSPI journal, message pool, etc.) will
 * copy this skeleton and replace the body of each test function.
 */
#include "test_framework.h"

#include "mocks/mock_time.h"
#include "mocks/mock_transport.h"
#include "mocks/mock_flash.h"
#include "mocks/mock_i2c.h"
#include "mocks/mock_spi.h"
#include "mocks/mock_pdm.h"
#include "mocks/mock_ble.h"

#include <string.h>

static void test_framework_basic(void) {
    TEST_ASSERT(1, "literal true");
    TEST_ASSERT_EQ_INT(42, 6 * 7);
}

static void test_deterministic_prng(void) {
    test_framework_srand(0xDEADBEEFu);
    uint32_t a = test_framework_rand();
    uint32_t b = test_framework_rand();
    TEST_ASSERT(a != b, "PRNG must advance between calls");

    /* Reseeding reproduces the same sequence: determinism check. */
    test_framework_srand(0xDEADBEEFu);
    TEST_ASSERT_EQ_INT((long long)a, (long long)test_framework_rand());
    TEST_ASSERT_EQ_INT((long long)b, (long long)test_framework_rand());
}

static void test_mock_time(void) {
    mock_time_reset();
    TEST_ASSERT_EQ_INT(0, (int)mock_time_now_us());
    mock_time_advance_us(1500);
    TEST_ASSERT_EQ_INT(1, (int)mock_time_now_ms());
    mock_time_set_us(1000000ull);
    TEST_ASSERT_EQ_INT(1000, (int)mock_time_now_ms());
}

static void test_mock_transport(void) {
    mock_transport_reset();
    const uint8_t pkt[] = { 0x01, 0x02, 0x03, 0x04 };
    TEST_ASSERT_EQ_INT(0, mock_transport_send(pkt, sizeof pkt));
    TEST_ASSERT_EQ_INT(1, (int)mock_transport_sent_count());
    const mock_transport_pkt_t *sent = mock_transport_sent(0);
    TEST_ASSERT(sent != NULL, "sent[0] exists");
    TEST_ASSERT_EQ_INT(4, (int)sent->len);
    TEST_ASSERT_EQ_INT(0, (int)memcmp(sent->bytes, pkt, sizeof pkt));

    /* Round-trip: enqueue RX, then receive. */
    mock_transport_enqueue_rx(pkt, sizeof pkt);
    uint8_t rbuf[8];
    size_t  got = 0;
    TEST_ASSERT_EQ_INT(0, mock_transport_recv(rbuf, sizeof rbuf, &got));
    TEST_ASSERT_EQ_INT(4, (int)got);
    TEST_ASSERT_EQ_INT(0, (int)memcmp(rbuf, pkt, sizeof pkt));
}

static void test_mock_flash_erase_program(void) {
    mock_flash_reset();
    uint8_t buf[8];
    TEST_ASSERT_EQ_INT(0, mock_flash_read(0, buf, sizeof buf));
    for (size_t i = 0; i < sizeof buf; ++i) {
        TEST_ASSERT_EQ_INT(0xFF, buf[i]);
    }
    const uint8_t pat[] = { 0xAA, 0x55, 0x12 };
    TEST_ASSERT_EQ_INT(0, mock_flash_program(0x10, pat, sizeof pat));
    TEST_ASSERT_EQ_INT(0, mock_flash_read(0x10, buf, sizeof pat));
    TEST_ASSERT_EQ_INT(0, (int)memcmp(buf, pat, sizeof pat));

    /* NOR semantics: cannot raise a bit without erasing. */
    uint8_t raise = 0xF0; /* 0xAA & 0xF0 = 0xA0, not 0xF0 -> reject */
    TEST_ASSERT_EQ_INT(-1, mock_flash_program(0x10, &raise, 1));

    /* Erase restores 0xFF for the whole sector. */
    TEST_ASSERT_EQ_INT(0, mock_flash_erase_sector(0));
    TEST_ASSERT_EQ_INT(0, mock_flash_read(0x10, buf, sizeof pat));
    TEST_ASSERT_EQ_INT(0xFF, buf[0]);

    /* Misaligned erase address rejected. */
    TEST_ASSERT_EQ_INT(-1, mock_flash_erase_sector(1));
}

static int dummy_i2c_xfer(uint8_t addr, uint8_t *buf, size_t len,
                          int read, void *user) {
    (void)addr; (void)user;
    if (read) {
        for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)(0x40 + i);
        return 0;
    }
    return (len > 0) ? 0 : -1;
}

static void test_mock_i2c(void) {
    mock_i2c_reset();
    mock_i2c_device_t dev = { 0x6A, dummy_i2c_xfer, NULL };
    mock_i2c_install(&dev);

    uint8_t reg = 0x0F;
    TEST_ASSERT_EQ_INT(0, mock_i2c_transfer(0x6A, &reg, 1, 0));

    uint8_t rx[3];
    TEST_ASSERT_EQ_INT(0, mock_i2c_transfer(0x6A, rx, sizeof rx, 1));
    TEST_ASSERT_EQ_INT(0x40, rx[0]);
    TEST_ASSERT_EQ_INT(0x42, rx[2]);

    /* Uninstalled address NACKs. */
    TEST_ASSERT_EQ_INT(-1, mock_i2c_transfer(0x77, rx, 1, 1));
}

static int invert_spi_xfer(uint8_t *buf, size_t len, void *user) {
    (void)user;
    for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)(buf[i] ^ 0xFF);
    return 0;
}

static void test_mock_spi(void) {
    mock_spi_reset();
    mock_spi_device_t dev = { 1, invert_spi_xfer, NULL };
    mock_spi_install(&dev);

    uint8_t buf[] = { 0x00, 0xFF, 0x55 };
    TEST_ASSERT_EQ_INT(0, mock_spi_transfer(1, buf, sizeof buf));
    TEST_ASSERT_EQ_INT(0xFF, buf[0]);
    TEST_ASSERT_EQ_INT(0x00, buf[1]);
    TEST_ASSERT_EQ_INT(0xAA, buf[2]);

    TEST_ASSERT_EQ_INT(-1, mock_spi_transfer(2, buf, 1));
}

static void test_mock_pdm(void) {
    mock_pdm_reset();
    const int16_t samples[] = { 100, -100, 200, -200 };
    size_t pushed = mock_pdm_push(samples, 4);
    TEST_ASSERT_EQ_INT(4, (int)pushed);
    TEST_ASSERT_EQ_INT(4, (int)mock_pdm_pending());

    int16_t out[3];
    size_t  got = mock_pdm_read(out, 3);
    TEST_ASSERT_EQ_INT(3, (int)got);
    TEST_ASSERT_EQ_INT(100, (int)out[0]);
    TEST_ASSERT_EQ_INT(-100, (int)out[1]);
    TEST_ASSERT_EQ_INT(1, (int)mock_pdm_pending());
}

static void test_mock_ble(void) {
    mock_ble_reset();
    mock_ble_emit(MOCK_BLE_EVT_CONNECTED);
    mock_ble_emit(MOCK_BLE_EVT_CONNECTED);
    mock_ble_emit(MOCK_BLE_EVT_DISCONNECTED);

    TEST_ASSERT_EQ_INT(2, (int)mock_ble_event_count(MOCK_BLE_EVT_CONNECTED));
    TEST_ASSERT_EQ_INT(1, (int)mock_ble_event_count(MOCK_BLE_EVT_DISCONNECTED));
    TEST_ASSERT_EQ_INT(3, (int)mock_ble_total_events());
    TEST_ASSERT_EQ_INT(0, (int)mock_ble_event_count(MOCK_BLE_EVT_SECURED));
}

int main(void) {
    test_framework_init();
    RUN_TEST(test_framework_basic);
    RUN_TEST(test_deterministic_prng);
    RUN_TEST(test_mock_time);
    RUN_TEST(test_mock_transport);
    RUN_TEST(test_mock_flash_erase_program);
    RUN_TEST(test_mock_i2c);
    RUN_TEST(test_mock_spi);
    RUN_TEST(test_mock_pdm);
    RUN_TEST(test_mock_ble);
    return test_framework_finish();
}
