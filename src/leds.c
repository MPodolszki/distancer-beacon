/*
 * Copyright (c) 2020-2026 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RGB LED control, adopted unchanged from the distancer application
 * (distancer-dev/src/leds.c) so both firmwares use identical colours.
 * The iBeacon currently only uses leds_heartbeat_set() (green), for the
 * running indicator and for the charging indication; the remaining colours
 * are kept so future states look the same as on the distancer.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include "leds.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ibeacon_leds);

/* Common PWM period used for LED brightness control. */
#define LED_PWM_PERIOD		PWM_USEC(100)

/* RGB LED channels (see board devicetree: pwmleds node). */
static const struct pwm_dt_spec rgb_1_r = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rgb_1_r));
static const struct pwm_dt_spec rgb_1_g = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rgb_1_g));
static const struct pwm_dt_spec rgb_1_b = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rgb_1_b));
static const struct pwm_dt_spec rgb_2_r = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rgb_2_r));
static const struct pwm_dt_spec rgb_2_g = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rgb_2_g));
static const struct pwm_dt_spec rgb_2_b = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rgb_2_b));

/* Set a channel to a duty cycle given as numerator/denominator of the period. */
static int led_set(const struct pwm_dt_spec *led, uint32_t num, uint32_t den)
{
	return pwm_set_dt(led, LED_PWM_PERIOD, (LED_PWM_PERIOD * num) / den);
}

int leds_init(void)
{
	const struct pwm_dt_spec *all[] = {
		&rgb_1_r, &rgb_1_g, &rgb_1_b, &rgb_2_r, &rgb_2_g, &rgb_2_b,
	};

	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		if (!pwm_is_ready_dt(all[i])) {
			LOG_ERR("PWM LED channel %u not ready", (unsigned int)i);
			return -ENODEV;
		}
	}

	return 0;
}

static void led_off_timer_handler(struct k_timer *dummy)
{
	leds_ack_set(0, 0);
}
K_TIMER_DEFINE(led_off_timer, led_off_timer_handler, NULL);

int leds_warn_set(int value)
{
	if (value) {
		led_set(&rgb_1_r, 1, 1);
		led_set(&rgb_2_r, 1, 1);
		return 0;
	}
	led_set(&rgb_1_r, 0, 1);
	led_set(&rgb_2_r, 0, 1);
	return 0;
}

int leds_ack_set(int value, uint32_t dur_ms)
{
	if (value) {
		led_set(&rgb_1_r, 1, 1);
		led_set(&rgb_1_g, 1, 1);
		led_set(&rgb_1_b, 1, 1);
		led_set(&rgb_2_r, 1, 1);
		led_set(&rgb_2_g, 1, 1);
		led_set(&rgb_2_b, 1, 1);
		k_timer_start(&led_off_timer, K_MSEC(dur_ms), K_NO_WAIT);
		return 0;
	}
	led_set(&rgb_1_r, 0, 1);
	led_set(&rgb_1_g, 0, 1);
	led_set(&rgb_1_b, 0, 1);
	led_set(&rgb_2_r, 0, 1);
	led_set(&rgb_2_g, 0, 1);
	led_set(&rgb_2_b, 0, 1);
	return 0;
}

int leds_heartbeat_set(int value)
{
	if (value) {
		led_set(&rgb_1_g, 1, 1);
		led_set(&rgb_2_g, 1, 1);
		return 0;
	}
	led_set(&rgb_1_g, 0, 1);
	led_set(&rgb_2_g, 0, 1);
	return 0;
}

int leds_lowbat_set(int value)
{
	if (value) {
		led_set(&rgb_1_r, 1, 1);
		led_set(&rgb_1_g, 1, 4);
		led_set(&rgb_2_r, 1, 1);
		led_set(&rgb_2_g, 1, 4);
		return 0;
	}
	led_set(&rgb_1_r, 0, 1);
	led_set(&rgb_1_g, 0, 1);
	led_set(&rgb_2_r, 0, 1);
	led_set(&rgb_2_g, 0, 1);
	return 0;
}

int leds_update_set(int value)
{
	LOG_INF("set update led");
	if (value) {
		led_set(&rgb_1_b, 1, 4);
		led_set(&rgb_2_b, 1, 4);
		return 0;
	}
	led_set(&rgb_1_b, 0, 1);
	led_set(&rgb_2_b, 0, 1);
	return 0;
}
