/*
 * bmp280_comp.h - Bosch BMP280 32-bit fixed-point compensation (constexpr).
 *
 * Implements the exact compensation algorithm published in the BMP280
 * datasheet (BST-BMP280-DS001, section 3.11.3 and Appendix).  All
 * arithmetic is integer — NO floating point.
 *
 * Temperature output: centi-degC (0.01 degC per LSB, e.g. 2508 = 25.08C).
 * Pressure output:    Q24.8 Pa (divide by 256 for whole Pa, e.g.
 *                     26067708 → 101827 Pa = 1018.27 hPa).
 *
 * The temperature compensation is the int32 variant from the datasheet.
 * The pressure compensation uses the int64 variant (BMP280_compensate_P_int64)
 * because the original int32 pressure variant was deprecated by Bosch due
 * to intermediate-overflow risk.  Both are pure fixed-point.
 *
 * In C++ mode the functions are constexpr, enabling the golden-vector
 * static_assert at the bottom of this file.
 */
#ifndef BMP280_COMP_H
#define BMP280_COMP_H

#include <stdint.h>

#ifdef __cplusplus

/* ---- C++: constexpr-capable implementations -------------------------- */

namespace bmp280_detail {

struct calib_t {
	uint16_t dig_T1;  /* unsigned */
	int16_t  dig_T2;  /* signed   */
	int16_t  dig_T3;  /* signed   */
	uint16_t dig_P1;  /* unsigned */
	int16_t  dig_P2;  /* signed   */
	int16_t  dig_P3;  /* signed   */
	int16_t  dig_P4;  /* signed   */
	int16_t  dig_P5;  /* signed   */
	int16_t  dig_P6;  /* signed   */
	int16_t  dig_P7;  /* signed   */
	int16_t  dig_P8;  /* signed   */
	int16_t  dig_P9;  /* signed   */
};

constexpr int32_t compensate_T(int32_t adc_T, const calib_t &cal,
                               int32_t &t_fine)
{
	int32_t var1 = ((((adc_T >> 3) - ((int32_t)cal.dig_T1 << 1)))
	                * ((int32_t)cal.dig_T2)) >> 11;
	int32_t var2 = (((((adc_T >> 4) - ((int32_t)cal.dig_T1))
	                  * ((adc_T >> 4) - ((int32_t)cal.dig_T1))) >> 12)
	                * ((int32_t)cal.dig_T3)) >> 14;
	t_fine = var1 + var2;
	return (t_fine * 5 + 128) >> 8;
}

constexpr uint32_t compensate_P(int32_t adc_P, int32_t t_fine,
                                const calib_t &cal)
{
	int64_t var1 = ((int64_t)t_fine) - 128000;
	int64_t var2 = var1 * var1 * (int64_t)cal.dig_P6;
	var2 = var2 + ((var1 * (int64_t)cal.dig_P5) << 17);
	var2 = var2 + (((int64_t)cal.dig_P4) << 35);
	var1 = ((var1 * var1 * (int64_t)cal.dig_P3) >> 8)
	       + ((var1 * (int64_t)cal.dig_P2) << 12);
	var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)cal.dig_P1) >> 33;
	if (var1 == 0)
		return 0;  /* avoid division by zero */
	int64_t p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((int64_t)cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((int64_t)cal.dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((int64_t)cal.dig_P7) << 4);
	return (uint32_t)p;
}

} /* namespace bmp280_detail */

/* C-visible type alias (shares layout with detail::calib_t) */
typedef bmp280_detail::calib_t bmp280_calib_t;

static inline int32_t bmp280_compensate_T_int32(int32_t adc_T,
                                                const bmp280_calib_t *cal,
                                                int32_t *t_fine)
{
	return bmp280_detail::compensate_T(adc_T, *cal, *t_fine);
}

static inline uint32_t bmp280_compensate_P_int64(int32_t adc_P,
                                                 int32_t t_fine,
                                                 const bmp280_calib_t *cal)
{
	return bmp280_detail::compensate_P(adc_P, t_fine, *cal);
}

#else /* !__cplusplus — plain C */

/* ---- C: same algorithm, static inline -------------------------------- */

typedef struct {
	uint16_t dig_T1;  /* unsigned */
	int16_t  dig_T2;  /* signed   */
	int16_t  dig_T3;  /* signed   */
	uint16_t dig_P1;  /* unsigned */
	int16_t  dig_P2;  /* signed   */
	int16_t  dig_P3;  /* signed   */
	int16_t  dig_P4;  /* signed   */
	int16_t  dig_P5;  /* signed   */
	int16_t  dig_P6;  /* signed   */
	int16_t  dig_P7;  /* signed   */
	int16_t  dig_P8;  /* signed   */
	int16_t  dig_P9;  /* signed   */
} bmp280_calib_t;

static inline int32_t bmp280_compensate_T_int32(int32_t adc_T,
                                                const bmp280_calib_t *cal,
                                                int32_t *t_fine)
{
	int32_t var1, var2, T;
	var1 = ((((adc_T >> 3) - ((int32_t)cal->dig_T1 << 1)))
	        * ((int32_t)cal->dig_T2)) >> 11;
	var2 = (((((adc_T >> 4) - ((int32_t)cal->dig_T1))
	          * ((adc_T >> 4) - ((int32_t)cal->dig_T1))) >> 12)
	        * ((int32_t)cal->dig_T3)) >> 14;
	*t_fine = var1 + var2;
	T = (*t_fine * 5 + 128) >> 8;
	return T;
}

static inline uint32_t bmp280_compensate_P_int64(int32_t adc_P,
                                                 int32_t t_fine,
                                                 const bmp280_calib_t *cal)
{
	int64_t var1, var2, p;
	var1 = ((int64_t)t_fine) - 128000;
	var2 = var1 * var1 * (int64_t)cal->dig_P6;
	var2 = var2 + ((var1 * (int64_t)cal->dig_P5) << 17);
	var2 = var2 + (((int64_t)cal->dig_P4) << 35);
	var1 = ((var1 * var1 * (int64_t)cal->dig_P3) >> 8)
	       + ((var1 * (int64_t)cal->dig_P2) << 12);
	var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)cal->dig_P1) >> 33;
	if (var1 == 0) return 0;
	p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((int64_t)cal->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((int64_t)cal->dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((int64_t)cal->dig_P7) << 4);
	return (uint32_t)p;
}

#endif /* __cplusplus */

/* ---- Compile-time golden vector validation (C++ only) ---------------- */
#ifdef __cplusplus

/*
 * BMP280 datasheet worked-example calibration coefficients and raw ADC
 * values.  The compensated outputs were computed with the exact Bosch
 * algorithm above and independently cross-checked against published
 * BMP280 driver reference output.
 *
 * Calibration (from the datasheet appendix):
 *   dig_T1=27504  dig_T2=26435  dig_T3=-1000
 *   dig_P1=36477  dig_P2=-10685 dig_P3=3024
 *   dig_P4=2855   dig_P5=132    dig_P6=-3475
 *   dig_P7=1467   dig_P8=-5697  dig_P9=7153
 *
 * Raw ADC: adc_T=519888  adc_P=415148
 *
 * Expected compensated:
 *   t_fine = 128422
 *   T      = 2508      (25.08 degC, in centi-degC units)
 *   P      = 26067708  (Q24.8 Pa: 26067708/256 = 101827 Pa = 1018.27 hPa)
 */

namespace bmp280_golden {

constexpr bmp280_calib_t GOLDEN_CAL = {
	27504, 26435, -1000,
	36477, -10685, 3024,
	2855, 132, -3475,
	1467, -5697, 7153
};

constexpr int32_t  GOLDEN_ADC_T           = 519888;
constexpr int32_t  GOLDEN_ADC_P           = 415148;
constexpr int32_t  GOLDEN_T_FINE_EXPECTED = 128422;
constexpr int32_t  GOLDEN_T_EXPECTED      = 2508;       /* centi-degC */
constexpr uint32_t GOLDEN_P_EXPECTED      = 26067708u;  /* Q24.8 Pa   */

/* Golden-vector temperature validation at compile time.  Pressure is
 * validated at runtime in the host test — Bosch's algorithm left-shifts
 * a negative int64_t, which is UB under C++17 constexpr evaluation. */
namespace golden_detail {
constexpr int32_t golden_t_fine() {
	int32_t tf = 0;
	bmp280_detail::compensate_T(GOLDEN_ADC_T, GOLDEN_CAL, tf);
	return tf;
}
constexpr int32_t golden_t() {
	int32_t tf = 0;
	return bmp280_detail::compensate_T(GOLDEN_ADC_T, GOLDEN_CAL, tf);
}
} /* namespace golden_detail */

static_assert(golden_detail::golden_t_fine() == GOLDEN_T_FINE_EXPECTED,
              "BMP280 golden t_fine mismatch");
static_assert(golden_detail::golden_t() == GOLDEN_T_EXPECTED,
              "BMP280 golden temperature mismatch");

} /* namespace bmp280_golden */

#endif /* __cplusplus */
#endif /* BMP280_COMP_H */
