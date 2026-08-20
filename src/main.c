/*
 * Copyright (c) 2026 PHYTEC Messtechnik GmbH
 * Based on the Zephyr iBeacon sample (c) 2018 Henrik Brix Andersen.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * iBeacon firmware for the distancer board.
 *
 * The device continuously broadcasts an Apple iBeacon advertisement. It does
 * NOT open the door itself: a receiver (e.g. the Home Assistant iOS Companion
 * app monitoring an iBeacon zone) detects this beacon by its UUID/Major/Minor
 * and triggers the lock. Change the identifiers below to match the receiver.
 *
 * The green RGB LEDs flash briefly once a second as a running indicator.
 *
 * Power notes: the device is battery powered, so everything here is arranged
 * to keep the CPU asleep. Zephyr runs tickless, so the core idles in WFI and
 * only wakes for the radio event, the LED pulse and (debug builds only) the
 * status log. Avoid adding periodic work that is not strictly needed.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/poweroff.h>

#include "leds.h"
#include "battery.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ibeacon, LOG_LEVEL_INF);

/* ------------------------------------------------------------------------- *
 *  iBeacon identity - CHANGE THESE to match your door-automation receiver.
 *  Proximity UUID: 18ee1516-016b-4bec-ad96-bcb96d166e97  (16 bytes, MSB first)
 * ------------------------------------------------------------------------- */
#define IBEACON_UUID							\
	0x18, 0xee, 0x15, 0x16, 0x01, 0x6b, 0x4b, 0xec,			\
	0xad, 0x96, 0xbc, 0xb9, 0x6d, 0x16, 0x6e, 0x97

/*
 * These must match the receiver's zone configuration exactly - a scanner
 * matches on UUID *and* major *and* minor, so a mismatched pair looks like no
 * beacon at all rather than like a wrong one. 1122/4455 are the upstream
 * Zephyr sample values; if they ever reappear here, something restored the
 * sample defaults over the real identity.
 */
#define IBEACON_MAJOR	1		/* e.g. site / building id      */
#define IBEACON_MINOR	1		/* e.g. this individual fob id  */

/* Calibrated signal strength (2's complement) measured at 1 m distance.
 * 0xC6 = -58 dBm at the default 0 dBm transmit power. This was briefly raised
 * to 0xCA to match a +4 dBm setting, but that setting turned out to change
 * nothing measurable (see prj.conf), so the original value stands.
 * Re-measure it if the transmit power is ever changed for real, otherwise
 * receivers will estimate the distance incorrectly. */
#define IBEACON_RSSI	0xc6

/*
 * Advertising interval. This is the dominant power consumer: every interval
 * the radio wakes, starts the crystal and transmits on three channels.
 *
 *   fast (100-150 ms, the Zephyr sample default) -> ~10 radio events/s
 *   slow (1.00-1.20 s, used here)                -> ~1 radio event/s
 *
 * Slow advertising cuts the radio duty cycle by roughly 10x at the cost of
 * detection latency: a passively scanning phone needs correspondingly longer
 * to notice the beacon. If the door reacts too sluggishly, lower this.
 */
/* Advertising intervals are expressed in 0.625 ms units. */
#define ADV_INT_UNITS(ms)	((ms) * 1000U / 625U)

#define ADV_INT_MIN	ADV_INT_UNITS(150)		/* 0.15 s */
#define ADV_INT_MAX	ADV_INT_UNITS(200)		/* 0.20 s */

static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_USE_IDENTITY, ADV_INT_MIN, ADV_INT_MAX, NULL);

/*
 * Flags: BT_LE_AD_GENERAL marks the device as generally discoverable. Without
 * it the advertisement is "non-discoverable", and scanners - iOS in particular
 * - are entitled to drop it even though the payload itself is valid. Apple's
 * own iBeacon examples advertise 0x1A, which is the dual-mode equivalent;
 * 0x06 (general discoverable + BR/EDR not supported) is what an LE-only part
 * like the nRF52 should send.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_MANUFACTURER_DATA,
		      0x4c, 0x00,		/* Apple company id      */
		      0x02, 0x15,		/* iBeacon type + length */
		      IBEACON_UUID,		/* 16-byte proximity UUID */
		      (IBEACON_MAJOR >> 8) & 0xff, IBEACON_MAJOR & 0xff,
		      (IBEACON_MINOR >> 8) & 0xff, IBEACON_MINOR & 0xff,
		      IBEACON_RSSI)
};

/* ------------------------------------------------------------------------- *
 *  Battery reporting via BTHome v2
 *
 *  The iBeacon payload above is full: 3 bytes flags + 27 bytes manufacturer
 *  data = 30 of the 31 available bytes, so the battery level cannot ride
 *  along in the same packet. Instead the device briefly switches the
 *  advertising payload to a BTHome v2 frame whenever it has something new to
 *  report, then returns to the iBeacon frame. See BATTERY_INTERVAL below for
 *  how often that is.
 *
 *  Home Assistant discovers BTHome (service data UUID 0xFCD2) natively and
 *  creates a battery sensor automatically - no connection required, which is
 *  what keeps this cheap on a battery powered fob.
 *
 *  BTHome v2 service data layout used here:
 *      0x40        device info: BTHome v2, unencrypted, not trigger based
 *      0x01 <pct>  object id 0x01 = battery, uint8, unit %
 * ------------------------------------------------------------------------- */
#define BTHOME_UUID_LSB		0xd2
#define BTHOME_UUID_MSB		0xfc
#define BTHOME_DEVICE_INFO	0x40
#define BTHOME_OBJ_BATTERY	0x01

/*
 * Measuring and announcing are deliberately decoupled.
 *
 * Announcing is what costs something: for the length of the burst the BTHome
 * frame REPLACES the iBeacon frame, so the door automation is blind while it
 * runs. Measuring is nearly free - one ADC conversion, no radio.
 *
 * So the battery is measured often enough to notice a cell swap quickly, but
 * only announced when the percentage actually changed, plus a keep-alive so a
 * receiver that missed a burst is not left without a value indefinitely. A
 * pack sitting at a steady level therefore costs the same blind time as
 * before, while a change shows up within one measurement interval instead of
 * up to five minutes later.
 *
 * The burst has to be long enough for a scanner to catch at least one packet
 * at the advertising interval - a passive scanner misses most of them.
 */
#define BATTERY_INTERVAL	K_SECONDS(60)
#define BTHOME_KEEPALIVE_MS	(5 * 60 * 1000)
#define BTHOME_BURST_MS		3000

static uint8_t bthome_svc_data[] = {
	BTHOME_UUID_LSB, BTHOME_UUID_MSB,
	BTHOME_DEVICE_INFO,
	BTHOME_OBJ_BATTERY, 0x00,	/* battery percent, filled in at runtime */
};

static const struct bt_data ad_bthome[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_SVC_DATA16, bthome_svc_data, sizeof(bthome_svc_data)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ------------------------------------------------------------------------- *
 *  Battery chemistry
 *
 *  Set this to 1 when running primary alkaline cells instead of rechargeable
 *  NiMH. It picks the discharge curve and the flat-battery threshold below.
 *
 *  The board charges through an LTC4060, which is a NiMH/NiCd charger, so
 *  rechargeable NiMH is what the hardware is built for and what this defaults
 *  to. Alkaline cells work too, but their curve sits roughly 300-400 mV higher
 *  over the whole discharge, so reading one pack against the other chemistry's
 *  curve is what produces a permanent "100 %".
 * ------------------------------------------------------------------------- */
#define BATTERY_ALKALINE	0

/*
 * Discharge curves for the two-cell pack, in millivolts across both cells.
 *
 * battery_level_pptt() walks the curve from the top and interpolates between
 * neighbouring points, so the shape matters: both chemistries spend most of
 * their life on a shallow plateau and then fall off a knee near the end. A
 * two-point straight line - which is what this used to be - maps that plateau
 * onto a handful of percent and the knee onto everything else, so the reading
 * sticks at one end and then collapses. The extra points below follow the
 * actual discharge shape, which is what makes the percentage mean something.
 *
 * The last entry must be 0 %; battery_level_pptt() uses it to terminate.
 */
#if BATTERY_ALKALINE
/*
 * 2x alkaline, 1.55 V fresh down to 0.95 V exhausted per cell. The slope is
 * gentle and nearly linear, which is what makes alkaline readable at all.
 */
static const struct battery_level_point battery_curve[] = {
	{ 100, 3100 },
	{ 80, 2900 },
	{ 60, 2750 },
	{ 40, 2600 },
	{ 25, 2450 },
	{ 10, 2250 },
	{ 5, 2100 },
	{ 0, 1900 },
};
#else
/*
 * 2x NiMH, 1.45 V straight off the charger down to 1.05 V empty per cell.
 *
 * The top point is deliberately set to what a full pack actually measures on
 * this board (2900-2925 mV observed) rather than to the textbook resting
 * voltage. Anything at or above the top point saturates at 100 %, so a top
 * that is too low makes a full pack and a half-full pack look identical -
 * which is exactly what a 2500 mV top did here.
 *
 * The shape has two distinct regions:
 *
 *   2900-2750 mV  surface charge. A freshly charged pack sits here and drops
 *                 through it within the first minutes of use, so it is worth
 *                 only the top 10 % - otherwise the reading would appear to
 *                 collapse right after charging.
 *   2750-2400 mV  the real plateau, where most of the capacity lives. A few
 *                 tens of millivolts are worth a lot of percent here, hence
 *                 the tight spacing.
 *
 * Below 2400 mV the cells fall off the knee and the remaining runtime is
 * short, so the last points are spaced wide.
 */
static const struct battery_level_point battery_curve[] = {
	{ 100, 2900 },
	{ 95, 2820 },
	{ 90, 2750 },
	{ 70, 2680 },
	{ 50, 2620 },
	{ 30, 2560 },
	{ 15, 2480 },
	{ 5, 2350 },
	{ 0, 2100 },
};
#endif

#define BATTERY_CHEMISTRY_NAME	(BATTERY_ALKALINE ? "alkaline" : "NiMH")

/* Top of the curve: at or above this the percentage saturates at 100 %. */
#define BATTERY_CURVE_TOP_MV	(battery_curve[0].lvl_mV)

/*
 * Flat battery: the 0 % point of the curve. Below it the percentage carries no
 * information any more and the orange flash is the only useful signal left.
 * The nRF52 itself runs down to 1.7 V, so both thresholds leave the radio
 * working well past the point where the pack is declared empty.
 */
#define LOWBAT_MV		(battery_curve[ARRAY_SIZE(battery_curve) - 1].lvl_mV)

/* ------------------------------------------------------------------------- *
 *  Green "beacon running" indicator.
 *
 *  One short flash per second rather than a 50 % duty blink: the LEDs draw a
 *  few mA while lit, so keeping the pulse short is what makes them cheap.
 *  At 20 ms on / 1000 ms period the duty cycle is 2 %, i.e. the average LED
 *  current is 1/50th of a permanently lit LED.
 * ------------------------------------------------------------------------- */
#define LED_PERIOD_MS	1000
#define LED_PULSE_MS	20

/*
 * Low battery uses the distancer's timing: a very short flash three times a
 * second, which reads as "urgent" next to the calm one-per-second heartbeat.
 */
#define LED_LOWBAT_PERIOD_MS	300
#define LED_LOWBAT_PULSE_MS	5

/*
 * Indicator states, highest priority first:
 *
 *   CHARGING - green, lit continuously. The device sits in the charger, so the
 *              extra current does not matter and a steady light is the
 *              clearest "still charging" signal. As on the distancer, charging
 *              overrides the low battery indication - the cell is being dealt
 *              with, so there is nothing left to warn about.
 *   OFF      - beacon switched off with the on/off button; LEDs dark so the
 *              device looks and behaves as if it were off.
 *   LOWBAT   - orange, short flash three times a second: the cell is below the
 *              point where the NiMH curve still gives a useful reading.
 *   SILENT   - red, short flash once a second: running, but not advertising
 *              the iBeacon, so the door no longer opens automatically.
 *   BEACON   - green, short flash once a second: advertising, on battery.
 */
enum led_mode {
	LED_MODE_OFF,
	LED_MODE_BEACON,
	LED_MODE_SILENT,
	LED_MODE_LOWBAT,
	LED_MODE_CHARGING,
};

static enum led_mode led_mode = LED_MODE_BEACON;

static void led_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(led_work, led_work_handler);

/*
 * Colours are the distancer's: green (heartbeat) for both "running" and
 * "charging", orange (lowbat) for a flat cell, red (warn) for the suppressed
 * automatic opening. Only one of them is ever lit, so the inactive channels
 * are cleared explicitly - the LEDs share one RGB package per side.
 */
static void leds_set(bool on)
{
	switch (led_mode) {
	case LED_MODE_SILENT:
		leds_heartbeat_set(0);
		leds_lowbat_set(0);
		leds_warn_set(on);
		break;
	case LED_MODE_LOWBAT:
		leds_heartbeat_set(0);
		leds_warn_set(0);
		leds_lowbat_set(on);
		break;
	default:
		leds_warn_set(0);
		leds_lowbat_set(0);
		leds_heartbeat_set(on);
		break;
	}
}

/*
 * Self-rescheduling work item. The blinking modes are on for their pulse
 * length and off for the rest of their period; OFF and CHARGING are static and
 * simply stop rescheduling, so a switched-off or charging device costs no
 * periodic wake-ups at all. Runs in thread context, not in a timer ISR.
 */
static void led_work_handler(struct k_work *work)
{
	static bool on;
	uint32_t period_ms = LED_PERIOD_MS;
	uint32_t pulse_ms = LED_PULSE_MS;

	switch (led_mode) {
	case LED_MODE_OFF:
		on = false;
		leds_set(false);
		return;			/* no reschedule */
	case LED_MODE_CHARGING:
		on = true;
		leds_set(true);
		return;			/* no reschedule: steady light */
	case LED_MODE_LOWBAT:
		period_ms = LED_LOWBAT_PERIOD_MS;
		pulse_ms = LED_LOWBAT_PULSE_MS;
		/* fall through */
	case LED_MODE_BEACON:
	case LED_MODE_SILENT:
	default:
		on = !on;
		leds_set(on);
		k_work_reschedule(&led_work,
				  on ? K_MSEC(pulse_ms)
				     : K_MSEC(period_ms - pulse_ms));
		return;
	}
}

static void led_set_mode(enum led_mode mode)
{
	led_mode = mode;
	k_work_reschedule(&led_work, K_NO_WAIT);
}



/* ------------------------------------------------------------------------- *
 *  Status heartbeat - debug builds only.
 *
 *  In a production (low-power) build CONFIG_LOG is off, so this timer is not
 *  compiled in at all and cannot wake the CPU.
 * ------------------------------------------------------------------------- */
#if defined(CONFIG_LOG)
static void status_log_work_handler(struct k_work *work)
{
	LOG_INF("advertising, uptime %lld s", k_uptime_get() / 1000);
}
K_WORK_DEFINE(status_log_work, status_log_work_handler);

static void status_log(struct k_timer *timer)
{
	k_work_submit(&status_log_work);
}
K_TIMER_DEFINE(status_timer, status_log, NULL);
#endif /* CONFIG_LOG */

/* ------------------------------------------------------------------------- *
 *  Beacon on/off and charge state
 * ------------------------------------------------------------------------- */
static bool beacon_on = true;	/* advertising enabled (on/off button) */
static bool charging;		/* LTC4060 CHRG asserted (micro USB in)  */

/*
 * The gpio-keys driver only reports state *changes*, so a device that boots
 * with the charger already plugged in would never see an event. Read the
 * CHRG line once at start-up to seed the state.
 */
static const struct gpio_dt_spec chrg_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(chrg), gpios);

static void charging_state_init(void)
{
	int val = gpio_pin_get_dt(&chrg_gpio);

	if (val < 0) {
		LOG_ERR("Failed to read CHRG line (%d)", val);
		return;
	}

	charging = (val != 0);
	LOG_INF("charger %s at boot", charging ? "connected" : "disconnected");
}

/*
 * "Silent" mode (mute button): the device stays on and keeps reporting its
 * battery, but stops sending the iBeacon frame, so the door no longer opens
 * automatically. This is separate from beacon_on, which switches the whole
 * device off.
 */
static bool silent;

/* Set by the battery measurement below once the cell drops under LOWBAT_MV. */
static bool low_battery;

/*
 * Charging wins over everything else - as on the distancer, plugging in clears
 * the low battery warning - then the on/off state, then the flat cell, then
 * silent mode.
 *
 * Colours follow the distancer palette: green = running/charging,
 * orange = battery flat, red = automatic opening suppressed.
 */
static void indicator_update(void)
{
	if (charging) {
		led_set_mode(LED_MODE_CHARGING);
	} else if (!beacon_on) {
		led_set_mode(LED_MODE_OFF);
	} else if (low_battery) {
		led_set_mode(LED_MODE_LOWBAT);
	} else if (silent) {
		led_set_mode(LED_MODE_SILENT);
	} else {
		led_set_mode(LED_MODE_BEACON);
	}
}

static void beacon_set(bool on)
{
	int err;

	if (on == beacon_on) {
		return;
	}

	if (on) {
		err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
		if (err) {
			LOG_ERR("Advertising failed to start (err %d)", err);
			return;
		}
	} else {
		err = bt_le_adv_stop();
		if (err) {
			LOG_ERR("Advertising failed to stop (err %d)", err);
			return;
		}
	}

	beacon_on = on;
	LOG_INF("beacon %s", on ? "on" : "off");
	indicator_update();
}

/* ------------------------------------------------------------------------- *
 *  Battery measurement and BTHome announcement
 * ------------------------------------------------------------------------- */
static void bthome_restore_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(bthome_restore, bthome_restore_handler);

/* What was last put on air, so an unchanged value can skip the burst. */
static int bthome_announced_pct = -1;
static int64_t bthome_announced_at;

/* Back to the iBeacon payload after the BTHome burst. */
static void bthome_restore_handler(struct k_work *work)
{
	int err;

	if (!beacon_on || silent) {
		return;
	}

	err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Failed to restore iBeacon payload (err %d)", err);
	}
}

/*
 * One line with the whole measurement chain: ADC counts, the voltage at the
 * pin, the voltage after divider scaling and what the curve makes of it. A
 * percentage on its own cannot distinguish a full pack from a broken sense
 * line or a divider that does not match the populated hardware.
 */
static void battery_log(const struct battery_reading *r, unsigned int pct)
{
	uint32_t full_ohm, output_ohm;

	battery_divider_info(&full_ohm, &output_ohm);

	LOG_INF("battery %d mV (ADC %d counts = %d mV, divider %u/%u) -> %u %% "
		"[%s curve %u-%u mV]%s",
		r->batt_mV, r->raw, r->adc_mV,
		(unsigned int)full_ohm, (unsigned int)output_ohm, pct,
		BATTERY_CHEMISTRY_NAME,
		(unsigned int)LOWBAT_MV, (unsigned int)BATTERY_CURVE_TOP_MV,
		r->batt_mV >= BATTERY_CURVE_TOP_MV
			? "  <- at or above the top of the curve, so the"
			  " percentage saturates; if the pack is not actually"
			  " full, BATTERY_ALKALINE is set wrong"
			: "");
}

static void battery_work_handler(struct k_work *work)
{
	struct battery_reading reading;
	int mv;
	unsigned int pct;
	int err;

	err = battery_sample_full(&reading);
	if (err != 0) {
		LOG_ERR("Battery measurement failed (%d)", err);
		return;
	}
	mv = reading.batt_mV;

	pct = battery_level_pptt((unsigned int)mv, battery_curve);
	if (pct > 100U) {
		pct = 100U;
	}

	bthome_svc_data[4] = (uint8_t)pct;
	battery_log(&reading, pct);

	/*
	 * Same threshold the distancer uses. It is also the 0 % point of the
	 * NiMH curve, so below it the percentage carries no information any
	 * more and the orange flash is the only useful signal left.
	 */
	if ((mv < LOWBAT_MV) != low_battery) {
		low_battery = (mv < LOWBAT_MV);
		LOG_INF("battery %s", low_battery ? "low" : "ok");
		indicator_update();
	}

	if (!beacon_on) {
		return;
	}

	/*
	 * Skip the burst when there is nothing new to say. The keep-alive
	 * bound still gets one out eventually, which matters because a scanner
	 * catches only a fraction of the packets in any single burst.
	 */
	if ((int)pct == bthome_announced_pct &&
	    (k_uptime_get() - bthome_announced_at) < BTHOME_KEEPALIVE_MS) {
		return;
	}

	/*
	 * Swap the advertising payload to the BTHome frame for a moment so a
	 * scanner can pick up the battery value, then switch back. While
	 * silent, the iBeacon frame is not wanted anyway, so the BTHome frame
	 * simply stays up and no restore is scheduled.
	 */
	err = bt_le_adv_update_data(ad_bthome, ARRAY_SIZE(ad_bthome), NULL, 0);
	if (err) {
		LOG_ERR("Failed to set BTHome payload (err %d)", err);
		return;
	}

	bthome_announced_pct = (int)pct;
	bthome_announced_at = k_uptime_get();

	if (!silent) {
		k_work_reschedule(&bthome_restore, K_MSEC(BTHOME_BURST_MS));
	}
}
K_WORK_DEFINE(battery_work, battery_work_handler);

static void battery_timer_handler(struct k_timer *timer)
{
	k_work_submit(&battery_work);
}
K_TIMER_DEFINE(battery_timer, battery_timer_handler, NULL);

#if defined(CONFIG_LOG)
/*
 * Live battery trace over the UART console, debug builds only.
 *
 * The BTHome announcement above measures every five minutes, which is far too
 * slow to watch a voltage while swapping cells or checking whether the pack
 * sags under the radio. This samples much faster and only logs - it does not
 * touch the advertising payload, the BTHome cadence or the low battery
 * indication, so a debug build behaves like production apart from the extra
 * measurements. In a production build CONFIG_LOG is off and none of this is
 * compiled in.
 *
 * Read it with 115200 8N1 on the debug probe's virtual COM port:
 *
 *     west build -b distancer . -- -DEXTRA_CONF_FILE=debug.conf
 *     picocom -b 115200 /dev/ttyACM0
 */
#define BATTERY_TRACE_INTERVAL	K_SECONDS(10)

static void battery_trace_work_handler(struct k_work *work)
{
	struct battery_reading reading;
	int err = battery_sample_full(&reading);

	if (err != 0) {
		LOG_ERR("Battery trace failed (%d)", err);
		return;
	}

	battery_log(&reading,
		    battery_level_pptt((unsigned int)reading.batt_mV, battery_curve));
}
K_WORK_DEFINE(battery_trace_work, battery_trace_work_handler);

static void battery_trace(struct k_timer *timer)
{
	k_work_submit(&battery_trace_work);
}
K_TIMER_DEFINE(battery_trace_timer, battery_trace, NULL);
#endif /* CONFIG_LOG */

/*
 * Mute button: suppress the iBeacon frame so the door stops opening
 * automatically, while the device itself stays on and keeps announcing its
 * battery over BTHome.
 */
static void silent_toggle(void)
{
	int err;

	silent = !silent;

	if (!beacon_on) {
		LOG_INF("silent %s (beacon is off)", silent ? "on" : "off");
		indicator_update();
		return;
	}

	k_work_cancel_delayable(&bthome_restore);

	err = bt_le_adv_update_data(silent ? ad_bthome : ad,
				    silent ? ARRAY_SIZE(ad_bthome)
					   : ARRAY_SIZE(ad),
				    NULL, 0);
	if (err) {
		LOG_ERR("Failed to switch advertising payload (err %d)", err);
	}

	LOG_INF("automatic opening %s", silent ? "disabled" : "enabled");
	indicator_update();
}

/* ------------------------------------------------------------------------- *
 *  Front buttons (Zephyr input subsystem, debounced by the gpio-keys driver)
 *
 *  Codes come from the board devicetree "buttons" node:
 *      INPUT_KEY_0  acknowledge   (P0.27)
 *      INPUT_KEY_1  mute          (P0.02)
 *      INPUT_KEY_2  on/off        (P0.09)
 *      INPUT_KEY_3  LTC4060 CHRG  (P0.08)  - charger status, not a button
 *      INPUT_KEY_4  user button   (P0.10)
 * ------------------------------------------------------------------------- */
#define BUTTON_LONGPRESS_MS	2000

static const struct gpio_dt_spec onoff_gpio =
	GPIO_DT_SPEC_GET(DT_NODELABEL(button_onoff), gpios);

/*
 * Switch the device off for real: nRF52 System OFF, where the CPU, the RAM
 * retention and every peripheral except the wake-up logic are unpowered. This
 * is the only "off" that does not drain the cells over weeks - simply stopping
 * the advertising still leaves the core idling and the regulators up.
 *
 * Waking from System OFF is a reset, so there is nothing to restore here: the
 * device comes back through main() and switches itself on again, which is
 * exactly the behaviour wanted for the charger.
 */
static void poweroff_handler(struct k_work *work)
{
	int err;

	LOG_INF("switching off");

	beacon_set(false);
	k_timer_stop(&battery_timer);
#if defined(CONFIG_LOG)
	k_timer_stop(&battery_trace_timer);
#endif
	k_work_cancel_delayable(&bthome_restore);
	battery_measure_enable(false);
	led_set_mode(LED_MODE_OFF);
	leds_set(false);

	/*
	 * Wake sources: pressing on/off again, or plugging in the charger.
	 * Both lines are active low with a pull-up, so a *level* interrupt on
	 * the active level is what the nRF GPIO driver turns into a SENSE
	 * configuration - edge detection needs GPIOTE, which System OFF powers
	 * down, while SENSE keeps working.
	 */
	err = gpio_pin_interrupt_configure_dt(&onoff_gpio, GPIO_INT_LEVEL_ACTIVE);
	if (err) {
		LOG_ERR("Failed to arm on/off wake-up (err %d)", err);
		return;
	}

	err = gpio_pin_interrupt_configure_dt(&chrg_gpio, GPIO_INT_LEVEL_ACTIVE);
	if (err) {
		LOG_ERR("Failed to arm charger wake-up (err %d)", err);
		return;
	}

	sys_poweroff();
}
static K_WORK_DEFINE(poweroff_work, poweroff_handler);

static bool poweroff_pending;

/*
 * The long press only *arms* the power-off; it is carried out when the button
 * is released. Entering System OFF while the button is still held would arm
 * the level-triggered wake-up on an already-active line, and the device would
 * wake straight back up.
 */
static void onoff_longpress_handler(struct k_work *work)
{
	if (charging) {
		/* Charging keeps the device on: the wake-up line would be
		 * active anyway, so switching off could not stick. */
		LOG_INF("on/off ignored while charging");
		return;
	}

	poweroff_pending = true;
	LOG_INF("power off armed, release the button");
}
static K_WORK_DELAYABLE_DEFINE(onoff_longpress, onoff_longpress_handler);

static void input_cb(struct input_event *evt, void *user_data)
{
	if (evt->type != INPUT_EV_KEY) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_2:
		/*
		 * On/off switches the device off, but only on a long press, so
		 * it cannot be triggered by brushing against the fob in a
		 * pocket. A release before the delay elapses cancels the
		 * pending work. Switching back on is not handled here: the
		 * device is in System OFF by then and a press resets it.
		 */
		if (evt->value) {
			k_work_reschedule(&onoff_longpress,
					  K_MSEC(BUTTON_LONGPRESS_MS));
		} else {
			k_work_cancel_delayable(&onoff_longpress);
			if (poweroff_pending) {
				poweroff_pending = false;
				k_work_submit(&poweroff_work);
			}
		}
		break;

	case INPUT_KEY_3:
		/* Charger status: 1 = charging (CHRG pulled low by LTC4060). */
		charging = (evt->value != 0);
		LOG_INF("charger %s", charging ? "connected" : "disconnected");
		indicator_update();
		break;

	case INPUT_KEY_1:
		/* Mute: toggle automatic opening on the press edge. */
		if (evt->value) {
			silent_toggle();
		}
		break;

	case INPUT_KEY_0:
	case INPUT_KEY_4:
		/*
		 * Ack and the user button are captured but unassigned. The
		 * user button is earmarked for the pairing mode, which is
		 * postponed.
		 */
		LOG_INF("button %u %s", (unsigned int)evt->code,
			evt->value ? "pressed" : "released");
		break;

	default:
		break;
	}
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

static void bt_ready(int err)
{
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	/* Non-connectable beacon advertising at the interval configured above. */
	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}
	beacon_on = true;

	/*
	 * Start the indicator before logging: a blocking console write must
	 * never be what stands between the radio coming up and the LED
	 * actually blinking.
	 */
	indicator_update();

	/* First reading shortly after boot, then periodically. */
	k_timer_start(&battery_timer, K_SECONDS(5), BATTERY_INTERVAL);

#if defined(CONFIG_LOG)
	k_timer_start(&status_timer, K_SECONDS(60), K_SECONDS(60));
	k_timer_start(&battery_trace_timer, K_SECONDS(2),
		      BATTERY_TRACE_INTERVAL);
#endif

	LOG_INF("iBeacon started (UUID 18ee1516-016b-4bec-ad96-bcb96d166e97, "
		"major %u, minor %u, RSSI@1m %d dBm, adv %u-%u ms)",
		(unsigned int)IBEACON_MAJOR, (unsigned int)IBEACON_MINOR,
		(int)(int8_t)IBEACON_RSSI,
		(unsigned int)(ADV_INT_MIN * 625U / 1000U),
		(unsigned int)(ADV_INT_MAX * 625U / 1000U));
}

int main(void)
{
	int err;

	LOG_INF("Starting distancer iBeacon");

	err = leds_init();
	if (err) {
		LOG_ERR("LED setup failed (err %d)", err);
	}

	charging_state_init();

	err = battery_measure_enable(true);
	if (err) {
		LOG_ERR("Battery measurement enable failed (err %d)", err);
	}

	err = bt_enable(bt_ready);
	if (err) {
		LOG_ERR("Bluetooth enable failed (err %d)", err);
	}

	/*
	 * Nothing further to do: main() returns and the idle thread puts the
	 * core to sleep until the next radio event or LED pulse.
	 */
	return 0;
}
