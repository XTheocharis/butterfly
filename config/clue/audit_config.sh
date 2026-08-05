#!/usr/bin/env bash
# Audit CLUE platform sdk_config.h and custom_board.h against plan Todo 1.
#
# Verifies:
#  - sdk_config.h is a real text file (NOT a symlink).
#  - mdk-dongle/sdk_config.h is unchanged relative to git HEAD.
#  - Every plan-mandated NRFX_* driver instance macro is enabled.
#  - NRF_SDH_BLE_PERIPHERAL_LINK_COUNT=1, CENTRAL=0, TOTAL=1.
#  - BLE HIDS, advertising, DIS, Peer Manager, FDS enabled.
#  - PM_LESC_ENABLED + NRF_BLE_LESC_ENABLED.
#  - CC310 backend enabled with secp256r1 + RNG + interrupts + static buffers.
#  - Every other CC310 algorithm/curve disabled.
#  - CC310_BL, Cifra, mbedTLS, micro-ecc, Oberon, nRF-HW-RNG backends disabled.
#  - LFXTAL clock source selected (NRFX_CLOCK_CONFIG_LF_SRC == 1).
#  - USB CDC ACM enabled (Butterfly transport surface).
#  - custom_board.h: every CLUE onboard pin defined, no UART P0.09/P0.10 alias.
#
# Exits 0 on success, 1 on any check failure. Each failure names the bad symbol.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SDK_CONFIG="${SCRIPT_DIR}/sdk_config.h"
CUSTOM_BOARD="${SCRIPT_DIR}/custom_board.h"
MDK_CONFIG="${SCRIPT_DIR}/../mdk-dongle/sdk_config.h"

errors=0

fail() {
    echo "FAIL: $*" >&2
    errors=$((errors + 1))
}

require_eq() {
    local name="$1" expected="$2"
    local actual
    actual=$(grep -E "^#define[[:space:]]+${name}[[:space:]]+" "${SDK_CONFIG}" \
        | awk '{ print $3 }' \
        | head -n1)
    if [[ "${actual}" != "${expected}" ]]; then
        fail "${name}: expected ${expected}, got '${actual}'"
    fi
}

# --- File-kind checks ----------------------------------------------------------
if [[ -L "${SDK_CONFIG}" ]]; then
    fail "sdk_config.h is a symbolic link; standalone file required"
fi
if ! file "${SDK_CONFIG}" | grep -q "ASCII text"; then
    fail "sdk_config.h is not an ASCII text file"
fi
if ! file "${CUSTOM_BOARD}" | grep -q "ASCII text"; then
    fail "custom_board.h is not an ASCII text file"
fi

# --- MDK untouched -------------------------------------------------------------
if ! git -C "${REPO_ROOT}" diff --quiet -- "${MDK_CONFIG}"; then
    fail "mdk-dongle/sdk_config.h is modified relative to git HEAD (must be untouched)"
fi

# --- NRFX peripheral instances -------------------------------------------------
for n in NRFX_PDM_ENABLED \
         NRFX_PWM_ENABLED NRFX_PWM0_ENABLED NRFX_PWM1_ENABLED \
         NRFX_QSPI_ENABLED \
         NRFX_RNG_ENABLED \
         NRFX_RTC_ENABLED NRFX_RTC2_ENABLED \
         NRFX_SAADC_ENABLED \
         NRFX_SPIM_ENABLED NRFX_SPIM2_ENABLED NRFX_SPIM3_ENABLED \
         NRFX_TWIM_ENABLED NRFX_TWIM1_ENABLED \
         NRFX_WDT_ENABLED \
         NRFX_GPIOTE_ENABLED \
         NRFX_POWER_ENABLED; do
    require_eq "${n}" "1"
done

# --- SDH / BLE peripheral ------------------------------------------------------
for n in NRF_SDH_ENABLED \
         NRF_SDH_BLE_ENABLED \
         NRF_SDH_SOC_ENABLED \
         NRF_SDH_BLE_PERIPHERAL_LINK_COUNT \
         NRF_SDH_BLE_TOTAL_LINK_COUNT \
         BLE_HIDS_ENABLED \
         BLE_ADVERTISING_ENABLED \
         BLE_DIS_ENABLED \
         PEER_MANAGER_ENABLED \
         PM_LESC_ENABLED \
         NRF_BLE_LESC_ENABLED \
         NRF_CRYPTO_ENABLED \
         FDS_ENABLED; do
    require_eq "${n}" "1"
done
require_eq NRF_SDH_BLE_CENTRAL_LINK_COUNT "0"

# --- Crypto backend: CC310 + secp256r1/RNG subset ------------------------------
for n in NRF_CRYPTO_BACKEND_CC310_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP256R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_RNG_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_INTERRUPTS_ENABLED \
         NRF_CRYPTO_RNG_STATIC_MEMORY_BUFFERS_ENABLED \
         NRF_CRYPTO_RNG_AUTO_INIT_ENABLED; do
    require_eq "${n}" "1"
done
require_eq NRF_CRYPTO_ALLOCATOR "2"

# Every other CC310 algorithm/curve must be disabled.
for n in NRF_CRYPTO_BACKEND_CC310_AES_CBC_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_AES_CTR_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_AES_ECB_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_AES_CBC_MAC_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_AES_CMAC_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_AES_CCM_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_AES_CCM_STAR_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_CHACHA_POLY_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP160R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP160R2_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP192R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP224R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP384R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP521R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP160K1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP192K1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP224K1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_SECP256K1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_CURVE25519_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_ECC_ED25519_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_HASH_SHA256_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_HASH_SHA512_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_HMAC_SHA256_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_HMAC_SHA512_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_BL_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_BL_ECC_SECP256R1_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_BL_HASH_SHA256_ENABLED \
         NRF_CRYPTO_BACKEND_CC310_BL_INTERRUPTS_ENABLED; do
    require_eq "${n}" "0"
done

# Other backends must be disabled.
for n in NRF_CRYPTO_BACKEND_CIFRA_ENABLED \
         NRF_CRYPTO_BACKEND_MBEDTLS_ENABLED \
         NRF_CRYPTO_BACKEND_MICRO_ECC_ENABLED \
         NRF_CRYPTO_BACKEND_OBERON_ENABLED \
         NRF_CRYPTO_BACKEND_NRF_HW_RNG_ENABLED \
         NRF_CRYPTO_BACKEND_NRF_SW_ENABLED \
         NRF_CRYPTO_BACKEND_OPTIGA_ENABLED; do
    require_eq "${n}" "0"
done

# --- LFXTAL: CLUE has 32.768kHz crystal on XL1/XL2 (P0.00/P0.01) ---------------
# Known-good reference firmware uses XTAL on this exact hardware and boots.
# RC (LF_SRC=0) was wrong — caused boot crash in SoftDevice clock init.
require_eq NRFX_CLOCK_CONFIG_LF_SRC "1"
require_eq CLOCK_CONFIG_LF_SRC "1"
require_eq NRF_SDH_CLOCK_LF_SRC "1"
require_eq NRF_SDH_CLOCK_LF_ACCURACY "7"

# --- USB CDC ACM (Butterfly shared transport surface) --------------------------
for n in USBD_ENABLED APP_USBD_ENABLED APP_USBD_CDC_ACM_ENABLED NRF_CLI_ENABLED; do
    require_eq "${n}" "1"
done

# --- custom_board.h: every CLUE onboard pin defined ----------------------------
for pin in CLUE_I2C_SDA CLUE_I2C_SCL CLUE_I2C_INSTANCE CLUE_I2C_FREQUENCY \
           CLUE_PDM_DATA CLUE_PDM_CLK CLUE_PDM_INSTANCE CLUE_PDM_GAIN_DEFAULT \
           CLUE_QSPI_SCK CLUE_QSPI_CSN CLUE_QSPI_IO0 CLUE_QSPI_IO1 CLUE_QSPI_IO2 CLUE_QSPI_IO3 \
           CLUE_NEOPIXEL \
           CLUE_SPEAKER CLUE_SPEAKER_PWM_INSTANCE \
           CLUE_LSM6DS33_IRQ CLUE_APDS9960_IRQ \
           CLUE_LSM6DS33_ADDR CLUE_LIS3MDL_ADDR CLUE_APDS9960_ADDR CLUE_SHT31D_ADDR CLUE_BMP280_ADDR \
           CLUE_TFT_SCK CLUE_TFT_MOSI CLUE_TFT_DC CLUE_TFT_CS CLUE_TFT_RST CLUE_TFT_BL \
           CLUE_TFT_SPIM_INSTANCE; do
    if ! grep -qE "^#define[[:space:]]+${pin}([[:space:]]|$)" "${CUSTOM_BOARD}"; then
        fail "custom_board.h missing required pin: ${pin}"
    fi
done

# --- custom_board.h: value assertions for critical macros ---------------------
# Presence-check above only verifies pin names exist. These four instance macros
# MUST be the right value or peripherals collide (e.g. PWM0 is NeoPixel, PWM1 is
# speaker — swapping them corrupts both drivers).
assert_value() {
    local macro="$1" expected="$2"
    local actual
    actual=$(grep -oP "^#define[[:space:]]+${macro}[[:space:]]+\K[0-9]+" "${CUSTOM_BOARD}" 2>/dev/null || echo "")
    if [ "$actual" != "$expected" ]; then
        fail "custom_board.h: ${macro}=${actual:-MISSING}, expected ${expected}"
    fi
}
assert_value CLUE_SPEAKER_PWM_INSTANCE "1"
assert_value CLUE_I2C_INSTANCE "1"
assert_value CLUE_PDM_INSTANCE "0"
assert_value CLUE_TFT_SPIM_INSTANCE "2"

# --- custom_board.h: no UART P0.09/P0.10 alias ---------------------------------
# RX_PIN_NUMBER=9 / TX_PIN_NUMBER=10 must NOT be defined (collide with APDS9960 IRQ + LED_2).
if grep -qE "^#define[[:space:]]+RX_PIN_NUMBER[[:space:]]+9" "${CUSTOM_BOARD}"; then
    fail "custom_board.h: RX_PIN_NUMBER=9 still aliases APDS9960 IRQ (P0.09)"
fi
if grep -qE "^#define[[:space:]]+TX_PIN_NUMBER[[:space:]]+10" "${CUSTOM_BOARD}"; then
    fail "custom_board.h: TX_PIN_NUMBER=10 still aliases LED_2 (P0.10)"
fi
# Even better: no RX/TX/CTS/RTS defines should be present at all on CLUE.
for sym in RX_PIN_NUMBER TX_PIN_NUMBER CTS_PIN_NUMBER RTS_PIN_NUMBER HWFC; do
    if grep -qE "^#define[[:space:]]+${sym}" "${CUSTOM_BOARD}"; then
        fail "custom_board.h: legacy UART symbol ${sym} still defined (CLUE uses USB CDC only)"
    fi
done

# --- QSPI pin correctness ------------------------------------------------------
# Verify the corrected QSPI pinout (SCK=P0.19, CSN=P0.20, IO0=P0.17, IO1=P0.22, IO2=P0.23, IO3=P0.21).
declare -A QSPI_EXPECTED=(
    [CLUE_QSPI_SCK]="NRF_GPIO_PIN_MAP(0,19)"
    [CLUE_QSPI_CSN]="NRF_GPIO_PIN_MAP(0,20)"
    [CLUE_QSPI_IO0]="NRF_GPIO_PIN_MAP(0,17)"
    [CLUE_QSPI_IO1]="NRF_GPIO_PIN_MAP(0,22)"
    [CLUE_QSPI_IO2]="NRF_GPIO_PIN_MAP(0,23)"
    [CLUE_QSPI_IO3]="NRF_GPIO_PIN_MAP(0,21)"
)
for pin in "${!QSPI_EXPECTED[@]}"; do
    # Use grep -F because NRF_GPIO_PIN_MAP(0,NN) contains regex-significant parentheses.
    line=$(awk -v n="${pin}" '$1 == "#define" && $2 == n { $1=""; $2=""; sub(/^[ \t]+/,""); print; exit }' "${CUSTOM_BOARD}")
    if [[ "${line}" != "${QSPI_EXPECTED[${pin}]}" ]]; then
        fail "custom_board.h: ${pin} must be ${QSPI_EXPECTED[${pin}]}"
    fi
done

# --- Sensor address correctness ------------------------------------------------
declare -A ADDR_EXPECTED=(
    [CLUE_LSM6DS33_ADDR]="0x6AU"
    [CLUE_LIS3MDL_ADDR]="0x1CU"
    [CLUE_APDS9960_ADDR]="0x39U"
    [CLUE_SHT31D_ADDR]="0x44U"
    [CLUE_BMP280_ADDR]="0x77U"
)
for pin in "${!ADDR_EXPECTED[@]}"; do
    if ! grep -qE "^#define[[:space:]]+${pin}[[:space:]]+${ADDR_EXPECTED[${pin}]}" "${CUSTOM_BOARD}"; then
        fail "custom_board.h: ${pin} must be ${ADDR_EXPECTED[${pin}]}"
    fi
done

# --- Result --------------------------------------------------------------------
if (( errors > 0 )); then
    echo "AUDIT FAILED: ${errors} check(s) failed" >&2
    exit 1
fi

echo "AUDIT OK: sdk_config.h standalone, all required modules enabled, custom_board.h pins correct."
exit 0
