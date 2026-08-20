/*
 * Copyright (c) 2018-2019 Peter Bigot Consulting, LLC
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>

#include "battery.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ibeacon_battery);

#define VBATT		DT_PATH(vbatt)

static const struct adc_dt_spec adc = ADC_DT_SPEC_GET_BY_IDX(VBATT, 0);
static const struct gpio_dt_spec power_gpio =
	GPIO_DT_SPEC_GET_OR(VBATT, power_gpios, {0});

/* Resistor divider parameters from the devicetree. */
static const uint32_t output_ohm = DT_PROP(VBATT, output_ohms);
static const uint32_t full_ohm = DT_PROP(VBATT, full_ohms);

static struct adc_sequence adc_seq;
static int16_t raw;
static bool battery_ok;

static int divider_setup(void)
{
	int rc;

	if (!adc_is_ready_dt(&adc)) {
		LOG_ERR("ADC controller not ready");
		return -ENODEV;
	}

	if (power_gpio.port) {
		if (!gpio_is_ready_dt(&power_gpio)) {
			LOG_ERR("Divider power GPIO not ready");
			return -ENODEV;
		}
		rc = gpio_pin_configure_dt(&power_gpio, GPIO_OUTPUT_INACTIVE);
		if (rc != 0) {
			LOG_ERR("Failed to configure divider GPIO: %d", rc);
			return rc;
		}
	}

	rc = adc_channel_setup_dt(&adc);
	if (rc != 0) {
		LOG_ERR("ADC channel setup failed: %d", rc);
		return rc;
	}

	rc = adc_sequence_init_dt(&adc, &adc_seq);
	if (rc != 0) {
		LOG_ERR("ADC sequence init failed: %d", rc);
		return rc;
	}
	adc_seq.buffer = &raw;
	adc_seq.buffer_size = sizeof(raw);
	adc_seq.calibrate = true;

	return 0;
}

static int battery_setup(void)
{
	int rc = divider_setup();

	battery_ok = (rc == 0);
	LOG_DBG("Battery setup: %d %d", rc, battery_ok);
	return rc;
}
SYS_INIT(battery_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int battery_measure_enable(bool enable)
{
	if (!battery_ok) {
		return -ENOENT;
	}
	if (power_gpio.port) {
		return gpio_pin_set_dt(&power_gpio, enable);
	}
	return 0;
}

int battery_sample_full(struct battery_reading *out)
{
	int rc;
	int32_t val;

	if (!battery_ok) {
		return -ENOENT;
	}

	rc = adc_read_dt(&adc, &adc_seq);
	adc_seq.calibrate = false;
	if (rc != 0) {
		return rc;
	}

	val = raw;
	rc = adc_raw_to_millivolts_dt(&adc, &val);
	if (rc != 0) {
		return rc;
	}

	out->raw = raw;
	out->adc_mV = val;

	/* Scale the divided voltage back to the full battery voltage. */
	if (output_ohm != 0) {
		val = val * (uint64_t)full_ohm / output_ohm;
	}
	out->batt_mV = val;

	LOG_DBG("raw %d ~ %d mV at the pin ~ %d mV battery",
		out->raw, out->adc_mV, out->batt_mV);

	return 0;
}

int battery_sample(void)
{
	struct battery_reading reading;
	int rc = battery_sample_full(&reading);

	if (rc != 0) {
		return rc;
	}

	return reading.batt_mV;
}

void battery_divider_info(uint32_t *full, uint32_t *output)
{
	if (full != NULL) {
		*full = full_ohm;
	}
	if (output != NULL) {
		*output = output_ohm;
	}
}

unsigned int battery_level_pptt(unsigned int batt_mV,
				const struct battery_level_point *curve)
{
	const struct battery_level_point *pb = curve;

	if (batt_mV >= pb->lvl_mV) {
		return pb->lvl_pptt;
	}
	while ((pb->lvl_pptt > 0) && (batt_mV < pb->lvl_mV)) {
		++pb;
	}
	if (batt_mV < pb->lvl_mV) {
		return pb->lvl_pptt;
	}

	const struct battery_level_point *pa = pb - 1;

	return pb->lvl_pptt
	       + ((pa->lvl_pptt - pb->lvl_pptt)
		  * (batt_mV - pb->lvl_mV)
		  / (pa->lvl_mV - pb->lvl_mV));
}
