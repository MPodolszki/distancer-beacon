/*
 * Copyright (c) 2020-2026 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RGB LED control for the distancer board.
 *
 * Adopted from the distancer application (distancer-dev/src/leds.c) so that
 * both firmwares use the same colours for the same meaning. The board has two
 * RGB units (D23-D26 and D3) which are always driven together.
 *
 * Colour scheme (as used by the distancer):
 *   heartbeat / charging  green, full brightness
 *   warn                  red, full brightness
 *   lowbat                red full + green 1/4  -> amber
 *   update                blue, 1/4 brightness
 *   ack                   all channels          -> white
 */

#ifndef LEDS_H
#define LEDS_H

#include <stdint.h>

int leds_init(void);

/** Green, full brightness. Used for "running" and for "charging". */
int leds_heartbeat_set(int value);

int leds_warn_set(int value);
int leds_lowbat_set(int value);
int leds_update_set(int value);
int leds_ack_set(int value, uint32_t dur_ms);

#endif /* LEDS_H */
