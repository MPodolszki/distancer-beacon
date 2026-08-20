/*
 * Copyright (c) 2018-2019 Peter Bigot Consulting, LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APPLICATION_BATTERY_H_
#define APPLICATION_BATTERY_H_

#include <stdbool.h>
#include <stdint.h>

/** Enable or disable measurement of the battery voltage.
 *
 * @param enable true to enable, false to disable
 *
 * @return zero on success, or a negative error code.
 */
int battery_measure_enable(bool enable);

/** Measure the battery voltage.
 *
 * @return the battery voltage in millivolts, or a negative error code.
 */
int battery_sample(void);

/** Everything one measurement produced, for diagnostics.
 *
 * battery_sample() collapses this to the final millivolt figure. When that
 * figure looks implausible, what is needed is the chain that produced it: a
 * wrong divider in the devicetree, a saturated ADC or a dead sense line all
 * look identical once only the end result is left.
 */
struct battery_reading {
	/** Raw ADC counts. */
	int16_t raw;
	/** Voltage at the ADC pin, before divider scaling. */
	int32_t adc_mV;
	/** Battery voltage, after divider scaling. */
	int32_t batt_mV;
};

/** Measure the battery voltage and report the full measurement chain.
 *
 * @param out filled in on success.
 *
 * @return zero on success, or a negative error code.
 */
int battery_sample_full(struct battery_reading *out);

/** Divider ratio taken from the devicetree, as full_ohms / output_ohms.
 *
 * @param full_ohm receives the total resistance, may be NULL.
 * @param output_ohm receives the sensed leg, may be NULL.
 */
void battery_divider_info(uint32_t *full_ohm, uint32_t *output_ohm);

/** A point in a battery discharge curve sequence. */
struct battery_level_point {
	/** Remaining life at #lvl_mV. */
	uint16_t lvl_pptt;
	/** Battery voltage at #lvl_pptt remaining life. */
	uint16_t lvl_mV;
};

/** Calculate the estimated battery level based on a measured voltage. */
unsigned int battery_level_pptt(unsigned int batt_mV,
				const struct battery_level_point *curve);

#endif /* APPLICATION_BATTERY_H_ */
