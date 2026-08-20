# Copyright (c) 2020 PHYTEC Messtechnik GmbH
# SPDX-License-Identifier: Apache-2.0

# Suppress "unique_unit_address_if_enabled" to handle overlapping unit
# addresses of the nRF52832 peripherals (e.g. power/clock/bprot @40000000).
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")
